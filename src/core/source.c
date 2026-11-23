/* source.c — source manager and the location space. */
#include "source.h"

#include "os.h"
#include "vec.h"

typedef enum SourceEntryKind : u8
{
  SourceEntryKindFile,
  SourceEntryKindExpansion
} SourceEntryKind;

/* One range of the location space: a file inclusion or a macro expansion.
 * Both take their length plus one locations, so the position just past the
 * last byte (where an end-of-file token sits) has a location too. */
typedef struct SourceEntry
{
  u32 base; /* first location */
  u32 span; /* number of locations */
  SourceEntryKind kind;
  bool isSystem;
  /* file */
  Sym fileName;
  const u8* text; /* NUL-terminated; span - 1 bytes */
  SourceLoc includedFrom;
  u32* lineStarts; /* byte offset of each line's first byte, built on first use */
  u32 lineCount;
  /* expansion */
  SourceLoc spelling;
  SourceLoc expansionStart;
  SourceLoc expansionEnd;
} SourceEntry;

struct SourceManager
{
  Arena* arena;
  VEC(SourceEntry) entries; /* in location order: bases increase */
  u32 next;                 /* first unassigned location */
  u32 limit;                /* one past the last assignable location */
  u32 lastIndex;            /* lookup cache: consecutive queries are usually close */
};

SourceManager* SourceManagerCreate(Arena* _arena)
{
  SourceManager* m = ARENA_NEW(_arena, SourceManager);
  m->arena = _arena;
  m->next = 1; /* 0 is SOURCE_LOC_NONE */
  m->limit = UINT32_MAX;
  return m;
}

void SourceManagerDestroy(SourceManager* _manager)
{
  VEC_FREE(&_manager->entries);
}

void SourceManagerSetLimit(SourceManager* _manager, u32 _limit)
{
  _manager->limit = _limit;
}

/* Claims `_span` locations, or returns false if they do not fit. */
static bool ClaimRange(SourceManager* _manager, u32 _span, SourceEntry* _entry)
{
  if ((u64)_manager->next + _span > _manager->limit)
    return false;
  _entry->base = _manager->next;
  _entry->span = _span;
  _manager->next += _span;
  return true;
}

static SourceLoadStatus AddFileEntry(SourceManager* _manager, Sym _fileName, const u8* _text, u32 _sizeBytes, SourceLoc _includedFrom,
                                     bool _isSystem, SourceLoc* _start)
{
  if (_sizeBytes == UINT32_MAX)
    return SourceLoadStatusTooLarge;
  SourceEntry e = {.kind = SourceEntryKindFile, .isSystem = _isSystem, .fileName = _fileName, .text = _text, .includedFrom = _includedFrom};
  if (!ClaimRange(_manager, _sizeBytes + 1, &e))
    return SourceLoadStatusTooLarge;
  VEC_PUSH(&_manager->entries, e);
  *_start = (SourceLoc){e.base};
  return SourceLoadStatusOk;
}

/* Copies `_sizeBytes` bytes into the arena with a NUL after them. */
static const u8* CopyText(Arena* _arena, const u8* _bytes, size_t _sizeBytes)
{
  u8* copy = ArenaAlloc(_arena, _sizeBytes + 1, 1);
  if (_sizeBytes)
    memcpy(copy, _bytes, _sizeBytes);
  copy[_sizeBytes] = 0;
  return copy;
}

static u8* PutUtf8(u8* _out, u32 _codePoint)
{
  if (_codePoint < 0x80)
    *_out++ = (u8)_codePoint;
  else if (_codePoint < 0x800)
  {
    *_out++ = (u8)(0xC0 | _codePoint >> 6);
    *_out++ = (u8)(0x80 | (_codePoint & 0x3F));
  }
  else if (_codePoint < 0x10000)
  {
    *_out++ = (u8)(0xE0 | _codePoint >> 12);
    *_out++ = (u8)(0x80 | ((_codePoint >> 6) & 0x3F));
    *_out++ = (u8)(0x80 | (_codePoint & 0x3F));
  }
  else
  {
    *_out++ = (u8)(0xF0 | _codePoint >> 18);
    *_out++ = (u8)(0x80 | ((_codePoint >> 12) & 0x3F));
    *_out++ = (u8)(0x80 | ((_codePoint >> 6) & 0x3F));
    *_out++ = (u8)(0x80 | (_codePoint & 0x3F));
  }
  return _out;
}

/* Transcodes UTF-16 (after its BOM) to NUL-terminated UTF-8 in the arena. A
 * surrogate pair (2 units) becomes 4 bytes and any other unit at most 3, so
 * 3 bytes per unit always suffices. */
static bool TranscodeUtf16(Arena* _arena, const u8* _bytes, size_t _sizeBytes, bool _bigEndian, const u8** _text, size_t* _textBytes)
{
  if (_sizeBytes % 2)
    return false;
  size_t units = _sizeBytes / 2;
  u8* out = ArenaAlloc(_arena, units * 3 + 1, 1);
  u8* p = out;
  for (size_t i = 0; i < units; i++)
  {
    const u8* u = _bytes + 2 * i;
    u32 c = _bigEndian ? (u32)u[0] << 8 | u[1] : (u32)u[1] << 8 | u[0];
    if (c >= 0xDC00 && c <= 0xDFFF)
      return false; /* a low surrogate with no high surrogate before it */
    if (c >= 0xD800 && c <= 0xDBFF)
    {
      if (i + 1 == units)
        return false;
      const u8* v = _bytes + 2 * (i + 1);
      u32 low = _bigEndian ? (u32)v[0] << 8 | v[1] : (u32)v[1] << 8 | v[0];
      if (low < 0xDC00 || low > 0xDFFF)
        return false;
      c = 0x10000 + ((c - 0xD800) << 10) + (low - 0xDC00);
      i++;
    }
    p = PutUtf8(p, c);
  }
  *p = 0;
  *_text = out;
  *_textBytes = (size_t)(p - out);
  return true;
}

SourceLoadStatus SourceLoadFile(SourceManager* _manager, Sym _fileName, const char* _path, SourceLoc _includedFrom, bool _isSystem,
                                SourceLoc* _start)
{
  u8* data = nullptr;
  size_t size = 0;
  if (!OsReadFile(_path, &data, &size))
    return SourceLoadStatusCannotOpen;
  const u8* text = nullptr;
  size_t textBytes = 0;
  SourceLoadStatus status = SourceLoadStatusOk;
  if (size >= 3 && data[0] == 0xEF && data[1] == 0xBB && data[2] == 0xBF)
  {
    text = CopyText(_manager->arena, data + 3, size - 3);
    textBytes = size - 3;
  }
  else if (size >= 2 && ((data[0] == 0xFF && data[1] == 0xFE) || (data[0] == 0xFE && data[1] == 0xFF)))
  {
    if (!TranscodeUtf16(_manager->arena, data + 2, size - 2, data[0] == 0xFE, &text, &textBytes))
      status = SourceLoadStatusInvalidUtf16;
  }
  else
  {
    text = CopyText(_manager->arena, data, size);
    textBytes = size;
  }
  NcXfree(data);
  if (status != SourceLoadStatusOk)
    return status;
  if (textBytes >= UINT32_MAX)
    return SourceLoadStatusTooLarge;
  return AddFileEntry(_manager, _fileName, text, (u32)textBytes, _includedFrom, _isSystem, _start);
}

SourceLoadStatus SourceAddBuffer(SourceManager* _manager, Sym _fileName, const u8* _text, u32 _sizeBytes, SourceLoc _includedFrom,
                                 bool _isSystem, SourceLoc* _start)
{
  if (_sizeBytes == UINT32_MAX)
    return SourceLoadStatusTooLarge;
  return AddFileEntry(_manager, _fileName, CopyText(_manager->arena, _text, _sizeBytes), _sizeBytes, _includedFrom, _isSystem, _start);
}

SourceLoc SourceAddExpansion(SourceManager* _manager, SourceLoc _spelling, u32 _lengthBytes, SourceLoc _expansionStart,
                             SourceLoc _expansionEnd)
{
  if (_lengthBytes == UINT32_MAX)
    return SOURCE_LOC_NONE;
  SourceEntry e = {
    .kind = SourceEntryKindExpansion, .spelling = _spelling, .expansionStart = _expansionStart, .expansionEnd = _expansionEnd};
  if (!ClaimRange(_manager, _lengthBytes + 1, &e))
    return SOURCE_LOC_NONE;
  VEC_PUSH(&_manager->entries, e);
  return (SourceLoc){e.base};
}

static SourceEntry* FindEntry(SourceManager* _manager, SourceLoc _loc)
{
  NC_ASSERT(_loc.offset != 0 && _loc.offset < _manager->next);
  SourceEntry* entries = _manager->entries.data; /* non-empty: the location was assigned */
  SourceEntry* last = &entries[_manager->lastIndex];
  if (_loc.offset >= last->base && _loc.offset - last->base < last->span)
    return last;
  /* the last entry whose base is <= the location */
  u32 lo = 0, hi = _manager->entries.len;
  while (hi - lo > 1)
  {
    u32 mid = lo + (hi - lo) / 2;
    if (entries[mid].base <= _loc.offset)
      lo = mid;
    else
      hi = mid;
  }
  NC_ASSERT(_loc.offset - entries[lo].base < entries[lo].span);
  _manager->lastIndex = lo;
  return &entries[lo];
}

SourceLoc SourceSpellingLoc(SourceManager* _manager, SourceLoc _loc)
{
  for (;;)
  {
    const SourceEntry* e = FindEntry(_manager, _loc);
    if (e->kind == SourceEntryKindFile)
      return _loc;
    _loc = SourceLocAdvance(e->spelling, _loc.offset - e->base);
  }
}

SourceLoc SourceExpansionLoc(SourceManager* _manager, SourceLoc _loc)
{
  for (;;)
  {
    const SourceEntry* e = FindEntry(_manager, _loc);
    if (e->kind == SourceEntryKindFile)
      return _loc;
    _loc = e->expansionStart;
  }
}

const u8* SourceText(SourceManager* _manager, SourceLoc _loc)
{
  _loc = SourceSpellingLoc(_manager, _loc);
  const SourceEntry* e = FindEntry(_manager, _loc);
  return e->text + (_loc.offset - e->base);
}

static void BuildLineTable(Arena* _arena, SourceEntry* _entry)
{
  u32 sizeBytes = _entry->span - 1;
  u32 lines = 1;
  for (u32 i = 0; i < sizeBytes; i++)
    lines += _entry->text[i] == '\n';
  u32* starts = ArenaAlloc(_arena, (size_t)lines * sizeof *starts, alignof(u32));
  u32 n = 0;
  starts[n++] = 0;
  for (u32 i = 0; i < sizeBytes; i++)
    if (_entry->text[i] == '\n')
      starts[n++] = i + 1;
  _entry->lineStarts = starts;
  _entry->lineCount = lines;
}

SourcePosition SourceGetPosition(SourceManager* _manager, SourceLoc _loc)
{
  _loc = SourceExpansionLoc(_manager, _loc);
  SourceEntry* e = FindEntry(_manager, _loc);
  if (!e->lineStarts)
    BuildLineTable(_manager->arena, e);
  u32 offset = _loc.offset - e->base;
  /* the last line that starts at or before the offset */
  u32 lo = 0, hi = e->lineCount;
  while (hi - lo > 1)
  {
    u32 mid = lo + (hi - lo) / 2;
    if (e->lineStarts[mid] <= offset)
      lo = mid;
    else
      hi = mid;
  }
  /* columns count code points: every byte that is not a UTF-8 continuation */
  u32 column = 1;
  for (u32 i = e->lineStarts[lo]; i < offset; i++)
    column += (e->text[i] & 0xC0) != 0x80;
  return (SourcePosition){
    .fileName = e->fileName, .line = lo + 1, .column = column, .isSystem = e->isSystem, .includedFrom = e->includedFrom};
}
