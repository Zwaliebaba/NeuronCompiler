/* cv.c — CodeView emission. Layouts follow cvinfo.h (microsoft-pdb). */
#include "cv.h"

static const u32 DEBUG_SEC_FLAGS = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_DISCARDABLE;

/* ---- record helpers ----------------------------------------------------- */

static u32 SymBegin(Buf* _b, u16 _kind)
{
  u32 at = _b->len;
  BufU16(_b, 0); /* length, patched */
  BufU16(_b, _kind);
  return at;
}

static void SymEnd(Buf* _b, u32 _at)
{
  BufAlign(_b, 4, 0);
  BufPutU16(_b, _at, (u16)(_b->len - _at - 2));
}

static u32 SubBegin(Buf* _b, u32 _kind)
{
  BufU32(_b, _kind);
  u32 at = _b->len;
  BufU32(_b, 0);
  return at;
}

static void SubEnd(Buf* _b, u32 _at)
{
  BufPutU32(_b, _at, _b->len - _at - 4);
  BufAlign(_b, 4, 0);
}

static u32 TypeBegin(CvModule* _cv, u16 _leaf)
{
  u32 at = _cv->types.len;
  BufU16(&_cv->types, 0);
  BufU16(&_cv->types, _leaf);
  return at;
}

static u32 TypeEnd(CvModule* _cv, u32 _at)
{
  Buf* b = &_cv->types;
  /* Type records are padded with LF_PAD bytes (0xF0 | bytes remaining). */
  u32 pad = (4 - (b->len & 3)) & 3;
  for (u32 i = pad; i > 0; i--)
    BufU8(b, (u8)(0xF0 | i));
  BufPutU16(b, _at, (u16)(b->len - _at - 2));
  return _cv->nextType++;
}

static u32 StringAdd(CvModule* _cv, const char* _s)
{
  u32 off = _cv->strings.len;
  BufStr0(&_cv->strings, _s);
  return off;
}

/* ---- module ------------------------------------------------------------- */

void CvInit(CvModule* _cv, Arena* _arena, const char* _objName, const char* _sourceName)
{
  memset(_cv, 0, sizeof *_cv);
  _cv->arena = _arena;
  _cv->objName = ArenaStrdup(_arena, _objName);
  _cv->nextType = 0x1000;
  BufU8(&_cv->strings, 0); /* the string table starts with an empty string */

  /* One source file without a checksum (CHKSUM_TYPE_NONE) in phase 0; the
     * front end will record SHA-256 of the file as read. */
  _cv->fileId = _cv->checksums.len;
  BufU32(&_cv->checksums, StringAdd(_cv, _sourceName));
  BufU8(&_cv->checksums, 0); /* checksum size */
  BufU8(&_cv->checksums, 0); /* CHKSUM_TYPE_NONE */
  BufAlign(&_cv->checksums, 4, 0);

  /* int (void) */
  u32 at = TypeBegin(_cv, LF_ARGLIST);
  BufU32(&_cv->types, 0);
  u32 arglist = TypeEnd(_cv, at);
  at = TypeBegin(_cv, LF_PROCEDURE);
  BufU32(&_cv->types, T_INT4);
  BufU8(&_cv->types, 0);  /* CV_CALL_NEAR_C */
  BufU8(&_cv->types, 0);  /* function attributes */
  BufU16(&_cv->types, 0); /* parameter count */
  BufU32(&_cv->types, arglist);
  _cv->procType = TypeEnd(_cv, at);
}

void CvFree(CvModule* _cv)
{
  BufFree(&_cv->types);
  BufFree(&_cv->strings);
  BufFree(&_cv->checksums);
}

void CvEmitFunction(CvModule* _cv, CoffObj* _obj, const CvFunc* _f)
{
  u32 at = TypeBegin(_cv, LF_FUNC_ID);
  BufU32(&_cv->types, 0); /* parent scope */
  BufU32(&_cv->types, _cv->procType);
  BufStr0(&_cv->types, _f->name);
  u32 funcId = TypeEnd(_cv, at);

  CoffSec ds = CoffAddSection(_obj, ".debug$S", DEBUG_SEC_FLAGS, 1);
  CoffSetComdat(_obj, ds, IMAGE_COMDAT_SELECT_ASSOCIATIVE, _f->text);
  Buf* b = &CoffSectionGet(_obj, ds)->data;
  BufU32(b, CV_SIGNATURE_C13);

  u32 sub = SubBegin(b, DEBUG_S_SYMBOLS);
  u32 rec = SymBegin(b, _f->isStatic ? S_LPROC32_ID : S_GPROC32_ID);
  BufU32(b, 0); /* pParent */
  BufU32(b, 0); /* pEnd */
  BufU32(b, 0); /* pNext */
  BufU32(b, _f->length);
  BufU32(b, _f->prologEnd);
  BufU32(b, _f->epilogStart);
  BufU32(b, funcId);
  CoffAddReloc(_obj, ds, b->len, _f->sym, IMAGE_REL_ARM64_SECREL);
  BufU32(b, 0);
  CoffAddReloc(_obj, ds, b->len, _f->sym, IMAGE_REL_ARM64_SECTION);
  BufU16(b, 0);
  BufU8(b, 0); /* flags */
  BufStr0(b, _f->name);
  SymEnd(b, rec);

  rec = SymBegin(b, S_FRAMEPROC);
  BufU32(b, _f->frameSize);     /* cbFrame */
  BufU32(b, 0);                 /* cbPad */
  BufU32(b, 0);                 /* offPad */
  BufU32(b, _f->savedRegsSize); /* cbSaveRegs */
  BufU32(b, 0);                 /* offExHdlr */
  BufU16(b, 0);                 /* sectExHdlr */
  /* encodedLocalBasePointer and encodedParamBasePointer (bits 14-17):
     * 2 = frame pointer (x29). */
  BufU32(b, 2u << 14 | 2u << 16);
  SymEnd(b, rec);

  rec = SymBegin(b, S_PROC_ID_END);
  SymEnd(b, rec);
  SubEnd(b, sub);

  if (_f->nlines)
  {
    sub = SubBegin(b, DEBUG_S_LINES);
    CoffAddReloc(_obj, ds, b->len, _f->sym, IMAGE_REL_ARM64_SECREL);
    BufU32(b, 0); /* offCon */
    CoffAddReloc(_obj, ds, b->len, _f->sym, IMAGE_REL_ARM64_SECTION);
    BufU16(b, 0); /* segCon */
    BufU16(b, 0); /* flags: no columns */
    BufU32(b, _f->length);
    BufU32(b, _cv->fileId);
    BufU32(b, _f->nlines);
    BufU32(b, 12 + 8 * _f->nlines);
    for (u32 i = 0; i < _f->nlines; i++)
    {
      BufU32(b, _f->lines[i].offset);
      BufU32(b, (_f->lines[i].line & 0xFFFFFF) | 0x80000000u); /* is_statement */
    }
    SubEnd(b, sub);
  }
}

void CvFinish(CvModule* _cv, CoffObj* _obj)
{
  CoffSec ds = CoffAddSection(_obj, ".debug$S", DEBUG_SEC_FLAGS, 1);
  Buf* b = &CoffSectionGet(_obj, ds)->data;
  BufU32(b, CV_SIGNATURE_C13);

  u32 sub = SubBegin(b, DEBUG_S_SYMBOLS);
  u32 rec = SymBegin(b, S_OBJNAME);
  BufU32(b, 0);
  BufStr0(b, _cv->objName);
  SymEnd(b, rec);

  rec = SymBegin(b, S_COMPILE3);
  BufU32(b, CV_CFL_C);
  BufU16(b, CV_CFL_ARM64);
  for (int i = 0; i < 4; i++)
    BufU16(b, i == 1 ? 1 : 0); /* front-end version 0.1.0.0 */
  for (int i = 0; i < 4; i++)
    BufU16(b, i == 1 ? 1 : 0); /* back-end version */
  BufStr0(b, "NeuronCompiler 0.1");
  SymEnd(b, rec);
  SubEnd(b, sub);

  sub = SubBegin(b, DEBUG_S_FILECHKSMS);
  BufBytes(b, _cv->checksums.data, _cv->checksums.len);
  SubEnd(b, sub);

  sub = SubBegin(b, DEBUG_S_STRINGTABLE);
  BufBytes(b, _cv->strings.data, _cv->strings.len);
  SubEnd(b, sub);

  CoffSec ts = CoffAddSection(_obj, ".debug$T", DEBUG_SEC_FLAGS, 1);
  Buf* t = &CoffSectionGet(_obj, ts)->data;
  BufU32(t, CV_SIGNATURE_C13);
  BufBytes(t, _cv->types.data, _cv->types.len);
}
