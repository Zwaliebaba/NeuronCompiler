/* diag.c — diagnostics engine. */
#include "diag.h"

#include "buf.h"

#include <inttypes.h>
#include <stdio.h>

typedef struct DiagInfo
{
  u16 number;
  DiagSeverity severity;
  u8 level;
  const char* message;
} DiagInfo;

static const DiagInfo DIAG_INFO[DiagIdCount] = {
#define DIAG(name, number, severity, level, message) [DiagId##name] = {number, DiagSeverity##severity, level, message},
#include "diag.def"
#undef DIAG
};

struct DiagEngine
{
  SourceManager* sources;
  const SymTable* syms;
  DiagSinkFn sink;
  void* sinkContext;
  u32 warningLevel;
  bool warningsAsErrors;
  u8 levelOf[DiagIdCount]; /* a warning's /W level; 0 when disabled */
  bool asError[DiagIdCount];
  u32 errorCount;
  u32 warningCount;
  bool hadFatal;
  Buf line;
};

static void StderrSink(void* _context, const char* _line)
{
  fflush(stdout);
  fprintf(stderr, "%s\n", _line);
}

DiagEngine* DiagEngineCreate(Arena* _arena, SourceManager* _sources, const SymTable* _syms)
{
  DiagEngine* e = ARENA_NEW(_arena, DiagEngine);
  e->sources = _sources;
  e->syms = _syms;
  e->sink = StderrSink;
  e->warningLevel = 1;
  for (u32 i = 0; i < DiagIdCount; i++)
    e->levelOf[i] = DIAG_INFO[i].level;
  return e;
}

void DiagEngineDestroy(DiagEngine* _engine)
{
  BufFree(&_engine->line);
}

void DiagSetSink(DiagEngine* _engine, DiagSinkFn _sink, void* _context)
{
  _engine->sink = _sink;
  _engine->sinkContext = _context;
}

u32 DiagNumber(DiagId _id)
{
  NC_ASSERT(_id < DiagIdCount);
  return DIAG_INFO[_id].number;
}

DiagSeverity DiagSeverityOf(DiagId _id)
{
  NC_ASSERT(_id < DiagIdCount);
  return DIAG_INFO[_id].severity;
}

bool DiagFindNumber(u32 _number, DiagId* _id)
{
  if (_number == 0)
    return false; /* notes have no number */
  for (u32 i = 0; i < DiagIdCount; i++)
    if (DIAG_INFO[i].number == _number)
    {
      *_id = (DiagId)i;
      return true;
    }
  return false;
}

static bool FindWarning(u32 _number, DiagId* _id)
{
  return DiagFindNumber(_number, _id) && DIAG_INFO[*_id].severity == DiagSeverityWarning;
}

void DiagSetWarningLevel(DiagEngine* _engine, u32 _level)
{
  NC_ASSERT(_level <= 4);
  _engine->warningLevel = _level;
}

void DiagSetWarningsAsErrors(DiagEngine* _engine, bool _on)
{
  _engine->warningsAsErrors = _on;
}

bool DiagDisableWarning(DiagEngine* _engine, u32 _number)
{
  DiagId id;
  if (!FindWarning(_number, &id))
    return false;
  _engine->levelOf[id] = 0;
  _engine->asError[id] = false;
  return true;
}

/* As /we: an error from now on, reported at any warning level. */
bool DiagWarningAsError(DiagEngine* _engine, u32 _number)
{
  DiagId id;
  if (!FindWarning(_number, &id))
    return false;
  if (_engine->levelOf[id] == 0)
    _engine->levelOf[id] = DIAG_INFO[id].level;
  _engine->asError[id] = true;
  return true;
}

bool DiagSetLevelOf(DiagEngine* _engine, u32 _number, u32 _level)
{
  DiagId id;
  if (!FindWarning(_number, &id) || _level < 1 || _level > 4)
    return false;
  _engine->levelOf[id] = (u8)_level;
  return true;
}

u32 DiagErrorCount(const DiagEngine* _engine)
{
  return _engine->errorCount;
}

u32 DiagWarningCount(const DiagEngine* _engine)
{
  return _engine->warningCount;
}

bool DiagHadFatal(const DiagEngine* _engine)
{
  return _engine->hadFatal;
}

static void PutText(Buf* _b, const char* _text)
{
  size_t n = strlen(_text);
  NC_ASSERT(n < UINT32_MAX);
  BufBytes(_b, _text, (u32)n);
}

static void PutArg(DiagEngine* _engine, const DiagArg* _arg)
{
  char digits[32];
  switch (_arg->kind)
  {
  case DiagArgKindText:
    PutText(&_engine->line, _arg->text);
    return;
  case DiagArgKindInt:
    snprintf(digits, sizeof digits, "%" PRId64, _arg->intValue);
    PutText(&_engine->line, digits);
    return;
  case DiagArgKindUint:
    snprintf(digits, sizeof digits, "%" PRIu64, _arg->uintValue);
    PutText(&_engine->line, digits);
    return;
  case DiagArgKindSym:
    BufBytes(&_engine->line, SymText(_engine->syms, _arg->sym), SymLength(_engine->syms, _arg->sym));
    return;
  }
  NC_UNREACHABLE();
}

static void PutMessage(DiagEngine* _engine, const char* _message, const DiagArg* _args, u32 _argCount)
{
  for (const char* p = _message; *p; p++)
  {
    if (*p != '%')
    {
      BufU8(&_engine->line, (u8)*p);
      continue;
    }
    p++;
    if (*p == '%')
    {
      BufU8(&_engine->line, '%');
      continue;
    }
    NC_ASSERT(*p >= '0' && *p <= '9'); /* the table is malformed, not the input */
    u32 index = (u32)(*p - '0');
    NC_ASSERT(index < _argCount);
    PutArg(_engine, &_args[index]);
  }
}

void DiagReport(DiagEngine* _engine, SourceLoc _loc, DiagId _id, const DiagArg* _args, u32 _argCount)
{
  NC_ASSERT(_id < DiagIdCount);
  const DiagInfo* info = &DIAG_INFO[_id];
  DiagSeverity severity = info->severity;
  bool hasPosition = !SourceLocIsNone(_loc);
  SourcePosition pos = {};
  if (hasPosition)
    pos = SourceGetPosition(_engine->sources, _loc);

  if (severity == DiagSeverityWarning)
  {
    /* Warnings in system headers are never reported (Design/03 §5.8). */
    if (pos.isSystem)
      return;
    u32 level = _engine->levelOf[_id];
    if (level == 0)
      return;
    /* /we reports a warning at any level; otherwise /W decides, and /WX
     * turns what is reported into an error. */
    if (level > _engine->warningLevel && !_engine->asError[_id])
      return;
    if (_engine->asError[_id] || _engine->warningsAsErrors)
      severity = DiagSeverityError;
  }

  Buf* b = &_engine->line;
  b->len = 0;
  if (hasPosition)
  {
    char where[48];
    PutText(b, SymText(_engine->syms, pos.fileName));
    snprintf(where, sizeof where, "(%u,%u): ", pos.line, pos.column);
    PutText(b, where);
  }
  else
    PutText(b, "ncc: ");
  static const char* const SEVERITY_WORDS[] = {"note", "warning", "error", "fatal error"};
  PutText(b, SEVERITY_WORDS[severity]);
  if (info->number)
  {
    char number[16];
    snprintf(number, sizeof number, " NC%04u", (unsigned)info->number);
    PutText(b, number);
  }
  PutText(b, ": ");
  PutMessage(_engine, info->message, _args, _argCount);
  BufU8(b, 0);
  _engine->sink(_engine->sinkContext, (const char*)b->data);

  switch (severity)
  {
  case DiagSeverityNote:
    break;
  case DiagSeverityWarning:
    _engine->warningCount++;
    break;
  case DiagSeverityFatal:
    _engine->hadFatal = true;
    _engine->errorCount++;
    break;
  case DiagSeverityError:
    _engine->errorCount++;
    break;
  }
}
