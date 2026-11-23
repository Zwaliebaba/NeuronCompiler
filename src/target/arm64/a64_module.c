/* a64_module.c — function finalisation and object emission. */
#include "a64_module.h"

#include <stdarg.h>
#include <stdio.h>

static const char* SymNameCb(void* _ctx, u32 _sym)
{
  A64Module* m = _ctx;
  return _sym < m->symNames.len ? m->symNames.data[_sym] : "<badsym>";
}

void A64ModuleInit(A64Module* _m, Arena* _arena, const char* _objName, const char* _sourceName, bool _debug)
{
  memset(_m, 0, sizeof *_m);
  _m->arena = _arena;
  _m->debug = _debug;
  _m->verifyUnwind = true;
  CoffInit(&_m->obj, _arena);
  if (_debug)
    CvInit(&_m->cv, _arena, _objName, _sourceName);
  _m->chkstkSym = A64ModuleSym(_m, "__chkstk");
}

void A64ModuleFree(A64Module* _m)
{
  CoffFree(&_m->obj);
  if (_m->debug)
    CvFree(&_m->cv);
  VEC_FREE(&_m->symNames);
  VEC_FREE(&_m->symCoff);
  BufFree(&_m->listing);
}

/* Linear lookup is adequate for phase-0 object sizes; the front end will use
 * the interned-symbol hash map (Design/02 §3) instead. */
u32 A64ModuleSym(A64Module* _m, const char* _name)
{
  for (u32 i = 0; i < _m->symNames.len; i++)
    if (strcmp(_m->symNames.data[i], _name) == 0)
      return i;
  VEC_PUSH(&_m->symNames, ArenaStrdup(_m->arena, _name));
  VEC_PUSH(&_m->symCoff, CoffSymbolNamed(&_m->obj, _name));
  return _m->symNames.len - 1;
}

void A64ModuleDirective(A64Module* _m, const char* _d)
{
  CoffAddDirective(&_m->obj, _d);
}

static void ListingPrintf(A64Module* _m, const char* _fmt, ...)
{
  char tmp[512];
  va_list ap;
  va_start(ap, _fmt);
  int n = vsnprintf(tmp, sizeof tmp, _fmt, ap);
  va_end(ap);
  if (n > 0)
    BufBytes(&_m->listing, tmp, (u32)NC_MIN((size_t)n, sizeof tmp - 1));
}

/* ---- branch relaxation -------------------------------------------------- */

static u32 InstSize(const FramePlan* _p, const MirInst* _mi)
{
  switch (_mi->op)
  {
  case A64OpPseudoLabel:
    return 0;
  case A64OpPseudoPrologue:
    return 4 * FramePrologueLen(_p);
  case A64OpPseudoEpilogue:
    return 4 * FrameEpilogueLen(_p);
  default:
    return 4;
  }
}

static void ComputeLayout(const FramePlan* _p, const MirFunc* _f, u32* _offs, u32* _labelOff)
{
  u32 pos = 0;
  for (u32 i = 0; i < _f->insts.len; i++)
  {
    const MirInst* mi = &_f->insts.data[i];
    _offs[i] = pos;
    if (mi->op == A64OpPseudoLabel)
      _labelOff[mi->target] = pos;
    pos += InstSize(_p, mi);
  }
  _offs[_f->insts.len] = pos;
}

static bool IsCondBranch(u16 _op)
{
  if (_op >= A64OpRealCount)
    return false;
  A64Form fm = A64_INSN_INFO[_op].form;
  return fm == A64FormBcond || fm == A64FormCb || fm == A64FormTb;
}

static u16 InvertBranch(u16 _op)
{
  switch (_op)
  {
  case A64OpBcc:
    return A64OpBcc;
  case A64OpCBZX:
    return A64OpCBNZX;
  case A64OpCBNZX:
    return A64OpCBZX;
  case A64OpCBZW:
    return A64OpCBNZW;
  case A64OpCBNZW:
    return A64OpCBZW;
  case A64OpTBZ:
    return A64OpTBNZ;
  case A64OpTBNZ:
    return A64OpTBZ;
  default:
    break;
  }
  NC_UNREACHABLE();
}

/* Replaces out-of-range conditional branches by an inverted short branch
 * around an unconditional b. Offsets only grow, so this terminates. */
static void RelaxBranches(const FramePlan* _p, MirFunc* _f)
{
  for (;;)
  {
    u32* offs = NcXrealloc(nullptr, sizeof(u32) * (_f->insts.len + 1));
    u32* lab = NcXrealloc(nullptr, sizeof(u32) * (_f->nlabels + 1));
    ComputeLayout(_p, _f, offs, lab);
    i32 fix = -1;
    for (u32 i = 0; i < _f->insts.len && fix < 0; i++)
    {
      MirInst* mi = &_f->insts.data[i];
      if (mi->tkind != A64TargetKindLabel)
        continue;
      i64 d = (i64)lab[mi->target] - (i64)offs[i];
      i64 range = A64BranchRange((A64Op)mi->op);
      if (d >= -range && d < range)
        continue;
      if (!IsCondBranch(mi->op))
        NcFatal("branch from %s+%u is out of range", _f->name, offs[i]);
      fix = (i32)i;
    }
    NcXfree(offs);
    NcXfree(lab);
    if (fix < 0)
      return;

    MirInst orig = _f->insts.data[fix];
    u32 skip = _f->nlabels++;
    MirInst inv = orig;
    inv.op = InvertBranch(orig.op);
    if (orig.op == A64OpBcc)
      inv.shift = A64CondInvert((A64Cond)orig.shift);
    inv.target = skip;
    MirInst far = {.op = A64OpB, .tkind = A64TargetKindLabel, .target = orig.target, .line = orig.line};
    MirInst lbl = {.op = A64OpPseudoLabel, .target = skip, .line = orig.line};
    VEC_RESERVE(&_f->insts, _f->insts.len + 2);
    memmove(&_f->insts.data[fix + 3], &_f->insts.data[fix + 1], sizeof(MirInst) * (_f->insts.len - (u32)fix - 1));
    _f->insts.data[fix] = inv;
    _f->insts.data[fix + 1] = far;
    _f->insts.data[fix + 2] = lbl;
    _f->insts.len += 2;
  }
}

/* ---- function emission -------------------------------------------------- */

void A64ModuleEmitFunction(A64Module* _m, MirFunc* _f)
{
  FramePlan plan;
  FramePlanBuild(&_f->frame, &plan);
  RelaxBranches(&plan, _f);

  /* Expand pseudos. */
  MirInstVec fin = {};
  VEC(u32) epilogs = {};
  u32 pos = 0;
  MirInst seq[FRAME_MAX_STEPS + 1];
  for (u32 i = 0; i < _f->insts.len; i++)
  {
    MirInst mi = _f->insts.data[i];
    if (mi.op == A64OpPseudoPrologue || mi.op == A64OpPseudoEpilogue)
    {
      NC_ASSERT(mi.op != A64OpPseudoPrologue || pos == 0);
      if (mi.op == A64OpPseudoEpilogue)
        VEC_PUSH(&epilogs, pos);
      u32 n = mi.op == A64OpPseudoPrologue ? FramePrologue(&plan, seq, _m->chkstkSym) : FrameEpilogue(&plan, seq);
      for (u32 k = 0; k < n; k++)
      {
        seq[k].line = mi.line;
        VEC_PUSH(&fin, seq[k]);
      }
      pos += 4 * n;
    }
    else
    {
      VEC_PUSH(&fin, mi);
      if (mi.op != A64OpPseudoLabel)
        pos += 4;
    }
  }
  u32 funcBytes = pos;

  /* Label offsets. */
  u32* lab = NcXrealloc(nullptr, sizeof(u32) * (_f->nlabels + 1));
  for (u32 i = 0, p = 0; i < fin.len; i++)
  {
    if (fin.data[i].op == A64OpPseudoLabel)
      lab[fin.data[i].target] = p;
    else
      p += 4;
  }

  /* Encode. */
  CoffSec text = CoffAddSection(&_m->obj, ".text$mn", IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ, 4);
  CoffSetComdat(&_m->obj, text, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
  u32 fsym = A64ModuleSym(_m, _f->name);
  CoffSym fcoff = _m->symCoff.data[fsym];
  CoffDefine(&_m->obj, fcoff, text, 0, IMAGE_SYM_DTYPE_FUNCTION, _f->isStatic ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL);
  Buf* code = &CoffSectionGet(&_m->obj, text)->data;

  VEC(CvLine) lines = {};
  ListingPrintf(_m, "\n%s:%s\n", _f->name, _f->isStatic ? "  // static" : "");
  char textBuf[256];
  for (u32 i = 0, p = 0; i < fin.len; i++)
  {
    MirInst* mi = &fin.data[i];
    if (mi->op == A64OpPseudoLabel)
    {
      ListingPrintf(_m, ".L%u:\n", mi->target);
      continue;
    }
    i64 pcrel = 0;
    if (mi->tkind == A64TargetKindLabel)
      pcrel = (i64)lab[mi->target] - (i64)p;
    else if (mi->tkind == A64TargetKindOffset)
      pcrel = mi->imm;
    u16 rtype;
    u32 w = A64Encode(mi, pcrel, &rtype);
    if (mi->tkind == A64TargetKindSym)
    {
      NC_ASSERT(rtype != 0);
      CoffAddReloc(&_m->obj, text, p, _m->symCoff.data[mi->target], rtype);
    }
    BufU32(code, w);
    if (mi->line && (lines.len == 0 || VEC_LAST(&lines)->line != mi->line))
      VEC_PUSH(&lines, ((CvLine){p, mi->line}));
    A64Format(textBuf, sizeof textBuf, mi, pcrel, SymNameCb, _m);
    ListingPrintf(_m, "  %06x  %08x   %s\n", p, w, textBuf);
    p += 4;
  }
  NC_ASSERT(code->len == funcBytes);

  /* Unwind data. */
  UnwindRecord rec;
  UnwindBuild(&plan, funcBytes, epilogs.data, epilogs.len, &rec);
  if (_m->verifyUnwind)
  {
    char err[256];
    if (!UnwindVerify(&plan, code->data, funcBytes, epilogs.data, epilogs.len, &rec, err, sizeof err))
      NcFatal("internal error: unwind verification failed for %s: %s", _f->name, err);
  }
  _m->nfuncs++;
  if (!rec.none)
  {
    /* Note: adding a section may reallocate the section table, so section
         * buffers are only looked up after all sections exist. */
    CoffSec pdata = CoffAddSection(&_m->obj, ".pdata", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ, 4);
    CoffSetComdat(&_m->obj, pdata, IMAGE_COMDAT_SELECT_ASSOCIATIVE, text);
    CoffAddReloc(&_m->obj, pdata, 0, fcoff, IMAGE_REL_ARM64_ADDR32NB);
    u32 second = 0;
    if (rec.packed)
    {
      second = rec.packedWord;
      _m->npacked++;
      ListingPrintf(_m, "  // unwind: packed %08x\n", rec.packedWord);
    }
    else
    {
      CoffSec xdata = CoffAddSection(&_m->obj, ".xdata", IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ, 4);
      CoffSetComdat(&_m->obj, xdata, IMAGE_COMDAT_SELECT_ASSOCIATIVE, text);
      BufBytes(&CoffSectionGet(&_m->obj, xdata)->data, rec.xdata.data, rec.xdata.len);
      char* uname = ArenaPrintf(_m->arena, "$unwind$%s", _f->name);
      CoffSym usym = CoffAddSymbol(&_m->obj, uname, (i32)xdata, 0, 0, IMAGE_SYM_CLASS_STATIC);
      CoffAddReloc(&_m->obj, pdata, 4, usym, IMAGE_REL_ARM64_ADDR32NB);
      _m->nxdata++;
      ListingPrintf(_m, "  // unwind: xdata, %u bytes, %u epilogue(s)\n", rec.xdata.len, epilogs.len);
    }
    Buf* pd = &CoffSectionGet(&_m->obj, pdata)->data;
    BufU32(pd, 0);
    BufU32(pd, second);
  }
  else
  {
    ListingPrintf(_m, "  // unwind: none (frameless leaf)\n");
  }

  if (_m->debug)
  {
    CvFunc cf = {
      .name = _f->name,
      .isStatic = _f->isStatic,
      .text = text,
      .sym = fcoff,
      .length = funcBytes,
      .prologEnd = 4 * FramePrologueLen(&plan),
      .epilogStart = epilogs.len ? epilogs.data[0] : funcBytes,
      .frameSize = plan.frameSize,
      .savedRegsSize = plan.saveSize,
      .lines = lines.data,
      .nlines = lines.len,
    };
    CvEmitFunction(&_m->cv, &_m->obj, &cf);
  }

  UnwindRecordFree(&rec);
  VEC_FREE(&lines);
  VEC_FREE(&epilogs);
  VEC_FREE(&fin);
  NcXfree(lab);
}

/* ---- data --------------------------------------------------------------- */

void A64ModuleEmitData(A64Module* _m, const char* _name, bool _isStatic, A64DataKind _kind, const void* _init, u32 _size, u32 _align,
                       const A64DataReloc* _relocs, u32 _nrelocs)
{
  static const char* const NAMES[] = {".data", ".rdata", ".bss", ".tls$"};
  u32 ch = IMAGE_SCN_MEM_READ;
  switch (_kind)
  {
  case A64DataKindRw:
    ch |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE;
    break;
  case A64DataKindRo:
    ch |= IMAGE_SCN_CNT_INITIALIZED_DATA;
    break;
  case A64DataKindBss:
    ch |= IMAGE_SCN_CNT_UNINITIALIZED_DATA | IMAGE_SCN_MEM_WRITE;
    break;
  case A64DataKindTls:
    ch |= IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_WRITE;
    break;
  }
  CoffSec s = CoffAddSection(&_m->obj, NAMES[_kind], ch, _align);
  /* One section per object (/Gw, Q-18) so /OPT:REF can drop unused data. */
  CoffSetComdat(&_m->obj, s, IMAGE_COMDAT_SELECT_NODUPLICATES, 0);
  CoffSection* sec = CoffSectionGet(&_m->obj, s);
  if (_kind == A64DataKindBss)
  {
    NC_ASSERT(!_init && !_nrelocs);
    sec->bssSize = _size;
  }
  else if (_init)
  {
    BufBytes(&sec->data, _init, _size);
  }
  else
  {
    BufZeros(&sec->data, _size);
  }
  u32 sym = A64ModuleSym(_m, _name);
  CoffDefine(&_m->obj, _m->symCoff.data[sym], s, 0, 0, _isStatic ? IMAGE_SYM_CLASS_STATIC : IMAGE_SYM_CLASS_EXTERNAL);
  for (u32 i = 0; i < _nrelocs; i++)
  {
    NC_ASSERT(_relocs[i].offset + 8 <= _size);
    CoffAddReloc(&_m->obj, s, _relocs[i].offset, _m->symCoff.data[_relocs[i].sym], IMAGE_REL_ARM64_ADDR64);
  }
  ListingPrintf(_m, "\n%s:  // %s, %u bytes, align %u\n", _name, NAMES[_kind], _size, _align);
}

void A64ModuleFinish(A64Module* _m, Buf* _out)
{
  if (_m->debug)
    CvFinish(&_m->cv, &_m->obj);
  CoffSerialize(&_m->obj, _out);
}

/* ---- builder helpers ---------------------------------------------------- */

void MirFuncInit(MirFunc* _f, const char* _name, bool _isStatic, FrameDesc _frame)
{
  memset(_f, 0, sizeof *_f);
  _f->name = _name;
  _f->isStatic = _isStatic;
  _f->frame = _frame;
}

void MirFuncFree(MirFunc* _f)
{
  VEC_FREE(&_f->insts);
}

u32 MirFuncNewLabel(MirFunc* _f)
{
  return _f->nlabels++;
}

void MirFuncEmit(MirFunc* _f, MirInst _mi)
{
  if (!_mi.line)
    _mi.line = _f->curLine;
  VEC_PUSH(&_f->insts, _mi);
}

void MirFuncLabel(MirFunc* _f, u32 _label)
{
  MirFuncEmit(_f, (MirInst){.op = A64OpPseudoLabel, .target = _label});
}

void MirFuncLine(MirFunc* _f, u32 _line)
{
  _f->curLine = _line;
}

void MirFuncMovImm(MirFunc* _f, u32 _rd, u64 _value, bool _w)
{
  u32 width = _w ? 32 : 64, chunks = width / 16, zeros = 0, ones = 0;
  if (_w)
    _value &= 0xFFFFFFFFu;
  for (u32 i = 0; i < chunks; i++)
  {
    u32 c = (u32)(_value >> (16 * i)) & 0xFFFF;
    zeros += c == 0;
    ones += c == 0xFFFF;
  }
  bool inverted = ones > zeros;
  u32 fill = inverted ? 0xFFFF : 0;
  u32 needed = chunks - (inverted ? ones : zeros); /* chunks that need an instruction */
  u32 n, immr, imms;
  if (needed > 1 && A64EncodeLogicalImm(_value, width, &n, &immr, &imms))
  {
    MirFuncEmit(_f, (MirInst){.op = _w ? A64OpORRWri : A64OpORRXri, .rd = (u8)_rd, .rn = A64_ZR, .imm = (i64)_value});
    return;
  }
  bool first = true;
  for (u32 i = 0; i < chunks; i++)
  {
    u32 c = (u32)(_value >> (16 * i)) & 0xFFFF;
    bool lastChance = first && i == chunks - 1; /* every chunk equals the fill */
    if (c == fill && !lastChance)
      continue;
    if (first)
    {
      A64Op op = inverted ? (_w ? A64OpMOVNW : A64OpMOVNX) : (_w ? A64OpMOVZW : A64OpMOVZX);
      u32 imm16 = inverted ? (~c & 0xFFFF) : c;
      u32 sh = lastChance && c == fill ? 0 : 16 * i;
      MirFuncEmit(_f, (MirInst){.op = op, .rd = (u8)_rd, .imm = imm16, .amount = (u8)sh});
      first = false;
    }
    else
    {
      MirFuncEmit(_f, (MirInst){.op = _w ? A64OpMOVKW : A64OpMOVKX, .rd = (u8)_rd, .imm = c, .amount = (u8)(16 * i)});
    }
  }
}
