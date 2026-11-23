/* coff.c — COFF object serialisation, regular and bigobj formats. */
#include "coff.h"

#include "../../core/crc32.h"

static const u8 BIGOBJ_CLASS_ID[16] = {0xC7, 0xA1, 0xBA, 0xD1, 0xEE, 0xBA, 0xA9, 0x4B, 0xAF, 0x20, 0xFA, 0xF6, 0x6A, 0xA4, 0xDC, 0xB8};

u32 CoffAlignFlags(u32 _align)
{
  NC_ASSERT(NC_IS_POW2(_align) && _align <= 8192);
  u32 log = NcCtz64(_align);
  return (log + 1) << 20;
}

void CoffInit(CoffObj* _o, Arena* _arena)
{
  memset(_o, 0, sizeof *_o);
  _o->arena = _arena;
  /* MSVC objects always carry @feat.00; we start with no feature bits. */
  CoffAddSymbol(_o, "@feat.00", -1, 0, 0, IMAGE_SYM_CLASS_STATIC);
}

void CoffFree(CoffObj* _o)
{
  for (u32 i = 0; i < _o->sections.len; i++)
  {
    BufFree(&_o->sections.data[i].data);
    VEC_FREE(&_o->sections.data[i].relocs);
  }
  VEC_FREE(&_o->sections);
  VEC_FREE(&_o->symbols);
  BufFree(&_o->directives);
}

CoffSection* CoffSectionGet(CoffObj* _o, CoffSec _s)
{
  NC_ASSERT(_s >= 1 && _s <= _o->sections.len);
  return &_o->sections.data[_s - 1];
}

CoffSec CoffAddSection(CoffObj* _o, const char* _name, u32 _characteristics, u32 _align)
{
  CoffSection* sec = VEC_PUSH_ZERO(&_o->sections);
  sec->name = ArenaStrdup(_o->arena, _name);
  sec->characteristics = _characteristics | CoffAlignFlags(_align);
  CoffSec id = _o->sections.len;
  CoffSym sym = CoffAddSymbol(_o, _name, (i32)id, 0, 0, IMAGE_SYM_CLASS_STATIC);
  _o->symbols.data[sym].isSectionSym = true;
  CoffSectionGet(_o, id)->secSym = sym;
  return id;
}

void CoffSetComdat(CoffObj* _o, CoffSec _s, u8 _selection, CoffSec _assoc)
{
  CoffSection* sec = CoffSectionGet(_o, _s);
  NC_ASSERT(_selection >= IMAGE_COMDAT_SELECT_NODUPLICATES && _selection <= IMAGE_COMDAT_SELECT_LARGEST);
  NC_ASSERT((_selection == IMAGE_COMDAT_SELECT_ASSOCIATIVE) == (_assoc != 0));
  if (_assoc)
    NC_ASSERT(CoffSectionGet(_o, _assoc)->comdatSel != 0);
  sec->characteristics |= IMAGE_SCN_LNK_COMDAT;
  sec->comdatSel = _selection;
  sec->assoc = _assoc;
}

CoffSym CoffAddSymbol(CoffObj* _o, const char* _name, i32 _section, u32 _value, u16 _type, u8 _storageClass)
{
  CoffSymbol* s = VEC_PUSH_ZERO(&_o->symbols);
  s->name = ArenaStrdup(_o->arena, _name);
  s->section = _section;
  s->value = _value;
  s->type = _type;
  s->storageClass = _storageClass;
  return _o->symbols.len - 1;
}

CoffSym CoffSymbolNamed(CoffObj* _o, const char* _name)
{
  /* Linear search is fine for phase-0 object sizes; the code generator keeps
     * its own symbol map and calls this once per symbol. */
  for (u32 i = 0; i < _o->symbols.len; i++)
  {
    CoffSymbol* s = &_o->symbols.data[i];
    if (!s->isSectionSym && strcmp(s->name, _name) == 0)
      return i;
  }
  return CoffAddSymbol(_o, _name, 0, 0, 0, IMAGE_SYM_CLASS_EXTERNAL);
}

void CoffDefine(CoffObj* _o, CoffSym _sym, CoffSec _sec, u32 _value, u16 _type, u8 _storageClass)
{
  CoffSymbol* s = &_o->symbols.data[_sym];
  NC_ASSERT(s->section == 0 && !s->isSectionSym);
  s->section = (i32)_sec;
  s->value = _value;
  s->type = _type;
  s->storageClass = _storageClass;
}

CoffSym CoffSectionSymbol(CoffObj* _o, CoffSec _s)
{
  return CoffSectionGet(_o, _s)->secSym;
}

void CoffAddReloc(CoffObj* _o, CoffSec _s, u32 _offset, CoffSym _sym, u16 _type)
{
  CoffSection* sec = CoffSectionGet(_o, _s);
  NC_ASSERT(_sym < _o->symbols.len);
  VEC_PUSH(&sec->relocs, ((CoffReloc){_offset, _sym, _type}));
}

void CoffAddDirective(CoffObj* _o, const char* _directive)
{
  BufU8(&_o->directives, ' ');
  BufBytes(&_o->directives, _directive, (u32)strlen(_directive));
}

/* ---- serialisation ------------------------------------------------------ */

typedef struct StrTab
{
  Buf b;
} StrTab;

static u32 StrtabAdd(StrTab* _t, const char* _s)
{
  if (_t->b.len == 0)
    BufU32(&_t->b, 0); /* size, patched at the end */
  u32 off = _t->b.len;
  BufStr0(&_t->b, _s);
  return off;
}

static void PutName8(Buf* _out, StrTab* _st, const char* _name, bool _sectionHeader)
{
  size_t n = strlen(_name);
  u8 field[8] = {};
  if (n <= 8)
  {
    /* A short name fills the 8-byte field and is not NUL-terminated when
         * it is exactly 8 bytes long (PE/COFF spec, section headers). */
    for (size_t i = 0; i < n; i++)
      field[i] = (u8)_name[i];
  }
  else if (_sectionHeader)
  {
    u32 off = StrtabAdd(_st, _name);
    char tmp[16];
    int len = 0;
    tmp[len++] = '/';
    char digits[12];
    int nd = 0;
    do
    {
      digits[nd++] = (char)('0' + off % 10);
      off /= 10;
    } while (off);
    if (nd > 7)
      NcFatal("COFF string table too large for section names");
    while (nd)
      tmp[len++] = digits[--nd];
    memcpy(field, tmp, (size_t)len);
  }
  else
  {
    u32 off = StrtabAdd(_st, _name);
    field[4] = (u8)off;
    field[5] = (u8)(off >> 8);
    field[6] = (u8)(off >> 16);
    field[7] = (u8)(off >> 24);
  }
  BufBytes(_out, field, 8);
}

void CoffSerialize(CoffObj* _o, Buf* _out)
{
  /* The .drectve section is materialised last so directives can be added
     * at any time. */
  if (_o->directives.len)
  {
    CoffSec d = CoffAddSection(_o, ".drectve", IMAGE_SCN_LNK_INFO | IMAGE_SCN_LNK_REMOVE, 1);
    BufBytes(&CoffSectionGet(_o, d)->data, _o->directives.data, _o->directives.len);
    BufU8(&CoffSectionGet(_o, d)->data, ' ');
  }

  u32 nsec = _o->sections.len;
  bool big = _o->forceBigobj || nsec > COFF_MAX_REGULAR_SECTIONS;
  u32 symSize = big ? 20 : 18;

  /* Symbol file indices: section symbols first (1 aux each), then the rest. */
  u32* fileIndex = NcXrealloc(nullptr, sizeof(u32) * (_o->symbols.len + 1));
  u32 next = 0;
  for (u32 s = 1; s <= nsec; s++)
  {
    fileIndex[CoffSectionGet(_o, s)->secSym] = next;
    next += 2;
  }
  for (u32 i = 0; i < _o->symbols.len; i++)
    if (!_o->symbols.data[i].isSectionSym)
      fileIndex[i] = next++;
  u32 nsymsFile = next;

  u32 headerSize = big ? 56 : 20;
  u32 pos = headerSize + 40 * nsec;
  u32* rawPtr = NcXrealloc(nullptr, sizeof(u32) * (nsec + 1));
  u32* relPtr = NcXrealloc(nullptr, sizeof(u32) * (nsec + 1));
  for (u32 s = 1; s <= nsec; s++)
  {
    CoffSection* sec = CoffSectionGet(_o, s);
    bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    rawPtr[s] = (!bss && sec->data.len) ? pos : 0;
    if (!bss)
      pos += sec->data.len;
    u32 nrel = sec->relocs.len + (sec->relocs.len > 0xFFFF ? 1 : 0);
    relPtr[s] = nrel ? pos : 0;
    pos += 10 * nrel;
  }
  u32 symtabPtr = pos;

  StrTab st = {};
  BufU32(&st.b, 0);
  _out->len = 0;

  /* File header. */
  if (big)
  {
    BufU16(_out, 0);
    BufU16(_out, 0xFFFF);
    BufU16(_out, 2);
    BufU16(_out, IMAGE_FILE_MACHINE_ARM64);
    BufU32(_out, 0); /* TimeDateStamp: 0 for reproducibility */
    BufBytes(_out, BIGOBJ_CLASS_ID, 16);
    BufU32(_out, 0);
    BufU32(_out, 0);
    BufU32(_out, 0);
    BufU32(_out, 0);
    BufU32(_out, nsec);
    BufU32(_out, symtabPtr);
    BufU32(_out, nsymsFile);
  }
  else
  {
    BufU16(_out, IMAGE_FILE_MACHINE_ARM64);
    BufU16(_out, (u16)nsec);
    BufU32(_out, 0);
    BufU32(_out, symtabPtr);
    BufU32(_out, nsymsFile);
    BufU16(_out, 0);
    BufU16(_out, 0);
  }

  /* Section headers. */
  for (u32 s = 1; s <= nsec; s++)
  {
    CoffSection* sec = CoffSectionGet(_o, s);
    bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    u32 nrel = sec->relocs.len;
    u32 ch = sec->characteristics;
    if (nrel > 0xFFFF)
      ch |= IMAGE_SCN_LNK_NRELOC_OVFL;
    PutName8(_out, &st, sec->name, true);
    BufU32(_out, 0); /* VirtualSize */
    BufU32(_out, 0); /* VirtualAddress */
    BufU32(_out, bss ? sec->bssSize : sec->data.len);
    BufU32(_out, rawPtr[s]);
    BufU32(_out, relPtr[s]);
    BufU32(_out, 0); /* PointerToLinenumbers */
    BufU16(_out, (u16)(nrel > 0xFFFF ? 0xFFFF : nrel));
    BufU16(_out, 0);
    BufU32(_out, ch);
  }

  /* Raw data and relocations. */
  for (u32 s = 1; s <= nsec; s++)
  {
    CoffSection* sec = CoffSectionGet(_o, s);
    bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    if (!bss)
    {
      NC_ASSERT(rawPtr[s] == 0 || _out->len == rawPtr[s]);
      BufBytes(_out, sec->data.data, sec->data.len);
    }
    if (sec->relocs.len)
    {
      NC_ASSERT(_out->len == relPtr[s]);
      if (sec->relocs.len > 0xFFFF)
      {
        BufU32(_out, sec->relocs.len + 1);
        BufU32(_out, 0);
        BufU16(_out, 0);
      }
      for (u32 r = 0; r < sec->relocs.len; r++)
      {
        CoffReloc* rl = &sec->relocs.data[r];
        BufU32(_out, rl->offset);
        BufU32(_out, fileIndex[rl->sym]);
        BufU16(_out, rl->type);
      }
    }
  }

  /* Symbol table. */
  NC_ASSERT(_out->len == symtabPtr);
  for (u32 s = 1; s <= nsec; s++)
  {
    CoffSection* sec = CoffSectionGet(_o, s);
    bool bss = sec->characteristics & IMAGE_SCN_CNT_UNINITIALIZED_DATA;
    PutName8(_out, &st, sec->name, false);
    BufU32(_out, 0);
    if (big)
      BufU32(_out, s);
    else
      BufU16(_out, (u16)s);
    BufU16(_out, 0);
    BufU8(_out, IMAGE_SYM_CLASS_STATIC);
    BufU8(_out, 1);
    /* Section-definition auxiliary record. */
    u32 len = bss ? sec->bssSize : sec->data.len;
    u32 crc = (sec->comdatSel && !bss) ? Crc32Jam(sec->data.data, sec->data.len) : 0;
    BufU32(_out, len);
    BufU16(_out, (u16)(sec->relocs.len > 0xFFFF ? 0xFFFF : sec->relocs.len));
    BufU16(_out, 0);
    BufU32(_out, crc);
    BufU16(_out, (u16)sec->assoc);
    BufU8(_out, sec->comdatSel);
    if (big)
    {
      BufU8(_out, 0);
      BufU16(_out, (u16)(sec->assoc >> 16));
      BufU16(_out, 0);
    }
    else
    {
      BufZeros(_out, 3);
    }
  }
  for (u32 i = 0; i < _o->symbols.len; i++)
  {
    CoffSymbol* sym = &_o->symbols.data[i];
    if (sym->isSectionSym)
      continue;
    PutName8(_out, &st, sym->name, false);
    BufU32(_out, sym->value);
    if (big)
      BufU32(_out, (u32)sym->section);
    else
      BufU16(_out, (u16)(i16)sym->section);
    BufU16(_out, sym->type);
    BufU8(_out, sym->storageClass);
    BufU8(_out, 0);
  }
  NC_ASSERT(_out->len == symtabPtr + nsymsFile * symSize);

  /* String table. */
  BufPutU32(&st.b, 0, st.b.len);
  BufBytes(_out, st.b.data, st.b.len);

  BufFree(&st.b);
  NcXfree(fileIndex);
  NcXfree(rawPtr);
  NcXfree(relPtr);
}
