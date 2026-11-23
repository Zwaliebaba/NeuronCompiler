/* unwind.c — frame plans, prologues/epilogues, .pdata/.xdata and the
 * unwind verifier. Reference: Microsoft Learn, "ARM64 exception handling". */
#include "unwind.h"

#include <stdarg.h>
#include <stdio.h>

/* ======================================================================== */
/* Frame plans                                                              */
/* ======================================================================== */

static void AddStep(FramePlan* _p, FrameStepKind _k, u32 _reg, u32 _off)
{
  NC_ASSERT(_p->nsteps < FRAME_MAX_STEPS);
  _p->steps[_p->nsteps++] = (FrameStep){_k, (u8)_reg, _off};
}

void FramePlanBuild(const FrameDesc* _d, FramePlan* _p)
{
  memset(_p, 0, sizeof *_p);
  NC_ASSERT(_d->nGpr <= 10 && _d->nFpr <= 8);
  _p->nGpr = _d->nGpr;
  _p->nFpr = _d->nFpr;
  _p->homeArgs = _d->homeArgs;
  _p->dynamic = _d->dynamic;
  u32 locals = NC_ALIGN_UP(_d->locals, 16u);
  u32 out = NC_ALIGN_UP(_d->outgoing, 16u);
  _p->localsSize = locals;

  if (_d->frameless)
  {
    NC_ASSERT(!_d->nGpr && !_d->nFpr && !_d->homeArgs && !_d->dynamic && !locals && !out);
    _p->shape = FrameShapeNone;
    return;
  }

  u32 intsz = 8u * _d->nGpr, fpsz = 8u * _d->nFpr;
  u32 savsz = NC_ALIGN_UP(intsz + fpsz + (_d->homeArgs ? 64u : 0u), 16u);
  _p->saveSize = savsz;

  /* Register save area, in the canonical order: int pairs, FP pairs, home. */
  bool first = true;
  if (_d->nGpr == 0 && _d->nFpr == 0 && savsz)
  {
    AddStep(_p, FrameStepKindAlloc, 0, savsz);
    first = false;
  }
  for (u32 i = 0; i < _d->nGpr; i += 2, first = false)
  {
    bool pair = i + 1 < _d->nGpr;
    if (first)
      AddStep(_p, pair ? FrameStepKindStpXPre : FrameStepKindStrXPre, 19 + i, savsz);
    else
      AddStep(_p, pair ? FrameStepKindStpX : FrameStepKindStrX, 19 + i, 8 * i);
  }
  for (u32 j = 0; j < _d->nFpr; j += 2, first = false)
  {
    bool pair = j + 1 < _d->nFpr;
    if (first)
      AddStep(_p, pair ? FrameStepKindStpDPre : FrameStepKindStrDPre, 8 + j, savsz);
    else
      AddStep(_p, pair ? FrameStepKindStpD : FrameStepKindStrD, 8 + j, intsz + 8 * j);
  }
  if (_d->homeArgs)
  {
    /* Home area at the top of the save area, contiguous with the incoming
         * stack arguments so va_list can walk both. */
    for (u32 k = 0; k < 4; k++)
      AddStep(_p, FrameStepKindHome, 2 * k, savsz - 64 + 16 * k);
  }

  if (!_d->dynamic && out == 0 && 16 + locals <= 4080)
  {
    /* Shape A: {x29, lr} at the bottom of the locals, x29 == sp. */
    u32 locsz = 16 + locals;
    /* The packed-unwind table allows the pre-indexed form up to 512, but
         * the mirrored `ldp x29, x30, [sp], #512` is not encodable (imm7
         * tops out at 504), so 512 uses the sub/stp form instead. */
    if (locsz <= 504)
    {
      AddStep(_p, FrameStepKindFplrPre, 0, locsz);
    }
    else
    {
      AddStep(_p, FrameStepKindAlloc, 0, locsz);
      AddStep(_p, FrameStepKindFplr, 0, 0);
    }
    AddStep(_p, FrameStepKindSetFp, 0, 0);
    _p->shape = FrameShapeA;
    _p->localsSpOffset = 16;
    _p->fpSpOffset = 0;
    _p->frameSize = savsz + locsz;
  }
  else
  {
    /* Shape B: {x29, lr} directly below the save area; the rest of the
         * frame is allocated below x29 and released with mov sp, x29. */
    AddStep(_p, FrameStepKindFplrPre, 0, 16);
    AddStep(_p, FrameStepKindSetFp, 0, 0);
    u32 rest = locals + out;
    if (rest >= 4096)
    {
      u32 units = rest / 16;
      if (rest >= ((u32)1 << 28))
        NcFatal("stack frame of %u bytes is too large", rest);
      AddStep(_p, FrameStepKindChkMovz, 15, units & 0xFFFF);
      if (units > 0xFFFF)
        AddStep(_p, FrameStepKindChkMovk, 15, units >> 16);
      AddStep(_p, FrameStepKindChkBl, 0, 0);
      AddStep(_p, FrameStepKindChkSub, 0, rest);
    }
    else if (rest)
    {
      AddStep(_p, FrameStepKindAlloc, 0, rest);
    }
    _p->shape = FrameShapeB;
    _p->localsSpOffset = out;
    _p->fpSpOffset = rest;
    _p->frameSize = savsz + 16 + rest;
  }
  _p->homeSpOffset = _p->frameSize - 64;
}

/* ---- instruction generation --------------------------------------------- */

static MirInst MiPair(A64Op _op, u32 _rt, u32 _rt2, i64 _off)
{
  return (MirInst){.op = _op, .rd = (u8)_rt, .ra = (u8)_rt2, .rn = A64_SP, .imm = _off};
}
static MirInst MiSingle(A64Op _op, u32 _rt, i64 _off)
{
  return (MirInst){.op = _op, .rd = (u8)_rt, .rn = A64_SP, .imm = _off};
}

static MirInst StepInsn(const FrameStep* _s, u32 _chkstkSym)
{
  switch (_s->kind)
  {
  case FrameStepKindStpXPre:
    return MiPair(A64OpSTPXpre, _s->reg, _s->reg + 1u, -(i64)_s->off);
  case FrameStepKindStpX:
    return MiPair(A64OpSTPXi, _s->reg, _s->reg + 1u, _s->off);
  case FrameStepKindStrXPre:
    return MiSingle(A64OpSTRXpre, _s->reg, -(i64)_s->off);
  case FrameStepKindStrX:
    return MiSingle(A64OpSTRXui, _s->reg, _s->off);
  case FrameStepKindStpDPre:
    return MiPair(A64OpSTPDpre, _s->reg, _s->reg + 1u, -(i64)_s->off);
  case FrameStepKindStpD:
    return MiPair(A64OpSTPDi, _s->reg, _s->reg + 1u, _s->off);
  case FrameStepKindStrDPre:
    return MiSingle(A64OpSTRDpre, _s->reg, -(i64)_s->off);
  case FrameStepKindStrD:
    return MiSingle(A64OpSTRDui, _s->reg, _s->off);
  case FrameStepKindHome:
    return MiPair(A64OpSTPXi, _s->reg, _s->reg + 1u, _s->off);
  case FrameStepKindFplrPre:
    return MiPair(A64OpSTPXpre, A64_FP, A64_LR, -(i64)_s->off);
  case FrameStepKindFplr:
    return MiPair(A64OpSTPXi, A64_FP, A64_LR, _s->off);
  case FrameStepKindSetFp:
    return (MirInst){.op = A64OpMOVXsp, .rd = A64_FP, .rn = A64_SP};
  case FrameStepKindAlloc:
    return (MirInst){.op = A64OpSUBXri, .rd = A64_SP, .rn = A64_SP, .imm = _s->off};
  case FrameStepKindChkMovz:
    return (MirInst){.op = A64OpMOVZX, .rd = 15, .imm = _s->off};
  case FrameStepKindChkMovk:
    return (MirInst){.op = A64OpMOVKX, .rd = 15, .imm = _s->off, .amount = 16};
  case FrameStepKindChkBl:
    return (MirInst){.op = A64OpBL, .tkind = A64TargetKindSym, .target = _chkstkSym};
  case FrameStepKindChkSub:
    return (MirInst){.op = A64OpSUBXrx, .rd = A64_SP, .rn = A64_SP, .rm = 15, .shift = A64ExtendUxtx, .amount = 4};
  }
  NC_UNREACHABLE();
}

/* The epilogue instruction undoing step `s`, or false if the step has no
 * epilogue counterpart. */
static bool StepEpilogInsn(const FramePlan* _p, const FrameStep* _s, MirInst* _out)
{
  switch (_s->kind)
  {
  case FrameStepKindStpXPre:
    *_out = MiPair(A64OpLDPXpost, _s->reg, _s->reg + 1u, _s->off);
    return true;
  case FrameStepKindStpX:
    *_out = MiPair(A64OpLDPXi, _s->reg, _s->reg + 1u, _s->off);
    return true;
  case FrameStepKindStrXPre:
    *_out = MiSingle(A64OpLDRXpost, _s->reg, _s->off);
    return true;
  case FrameStepKindStrX:
    *_out = MiSingle(A64OpLDRXui, _s->reg, _s->off);
    return true;
  case FrameStepKindStpDPre:
    *_out = MiPair(A64OpLDPDpost, _s->reg, _s->reg + 1u, _s->off);
    return true;
  case FrameStepKindStpD:
    *_out = MiPair(A64OpLDPDi, _s->reg, _s->reg + 1u, _s->off);
    return true;
  case FrameStepKindStrDPre:
    *_out = MiSingle(A64OpLDRDpost, _s->reg, _s->off);
    return true;
  case FrameStepKindStrD:
    *_out = MiSingle(A64OpLDRDui, _s->reg, _s->off);
    return true;
  case FrameStepKindFplrPre:
    *_out = MiPair(A64OpLDPXpost, A64_FP, A64_LR, _s->off);
    return true;
  case FrameStepKindFplr:
    *_out = MiPair(A64OpLDPXi, A64_FP, A64_LR, _s->off);
    return true;
  case FrameStepKindSetFp:
    if (_p->shape != FrameShapeB)
      return false;
    *_out = (MirInst){.op = A64OpMOVXsp, .rd = A64_SP, .rn = A64_FP};
    return true;
  case FrameStepKindAlloc:
    /* In shape B the allocation below x29 is undone by mov sp, x29. */
    if (_p->shape == FrameShapeB && _s != &_p->steps[0])
    {
      bool afterFp = false;
      for (const FrameStep* q = _p->steps; q < _s; q++)
        afterFp |= q->kind == FrameStepKindSetFp;
      if (afterFp)
        return false;
    }
    *_out = (MirInst){.op = A64OpADDXri, .rd = A64_SP, .rn = A64_SP, .imm = _s->off};
    return true;
  case FrameStepKindHome:
  case FrameStepKindChkMovz:
  case FrameStepKindChkMovk:
  case FrameStepKindChkBl:
  case FrameStepKindChkSub:
    return false;
  }
  NC_UNREACHABLE();
}

u32 FramePrologueLen(const FramePlan* _p)
{
  return _p->nsteps;
}

u32 FramePrologue(const FramePlan* _p, MirInst* _out, u32 _chkstkSym)
{
  for (u32 i = 0; i < _p->nsteps; i++)
    _out[i] = StepInsn(&_p->steps[i], _chkstkSym);
  return _p->nsteps;
}

u32 FrameEpilogue(const FramePlan* _p, MirInst* _out)
{
  u32 n = 0;
  for (u32 i = _p->nsteps; i-- > 0;)
    if (StepEpilogInsn(_p, &_p->steps[i], &_out[n]))
      n++;
  _out[n++] = (MirInst){.op = A64OpRET, .rn = A64_LR};
  return n;
}

u32 FrameEpilogueLen(const FramePlan* _p)
{
  MirInst tmp[FRAME_MAX_STEPS + 1];
  return FrameEpilogue(_p, tmp);
}

/* ======================================================================== */
/* Unwind codes                                                             */
/* ======================================================================== */

typedef enum UwKind : u8
{
  UwKindAlloc,
  UwKindSaveR19r20X,
  UwKindSaveFplr,
  UwKindSaveFplrX,
  UwKindSaveRegp,
  UwKindSaveRegpX,
  UwKindSaveReg,
  UwKindSaveRegX,
  UwKindSaveLrpair,
  UwKindSaveFregp,
  UwKindSaveFregpX,
  UwKindSaveFreg,
  UwKindSaveFregX,
  UwKindSetFp,
  UwKindAddFp,
  UwKindNop,
  UwKindEnd,
  UwKindEndC,
  UwKindSaveNext,
  UwKindPacSignLr,
  UwKindUnknown,
} UwKind;

static const char* const UW_NAMES[] = {
  "alloc",      "save_r19r20_x", "save_fplr",  "save_fplr_x",  "save_regp", "save_regp_x", "save_reg",
  "save_reg_x", "save_lrpair",   "save_fregp", "save_fregp_x", "save_freg", "save_freg_x", "set_fp",
  "add_fp",     "nop",           "end",        "end_c",        "save_next", "pac_sign_lr", "unknown",
};

typedef struct UwOp
{
  UwKind kind;
  u8 reg;  /* absolute register number (x19.., d8..) */
  u32 off; /* byte offset, or allocation size */
} UwOp;

typedef struct UwCodes
{
  u8 b[512];
  u32 len;
} UwCodes;

static void UwPut(UwCodes* _c, u32 _n, u32 _v)
{
  NC_ASSERT(_c->len + _n <= sizeof _c->b);
  for (u32 i = _n; i-- > 0;)
    _c->b[_c->len++] = (u8)(_v >> (8 * i));
}

static void UwEncode(UwCodes* _c, UwOp _op)
{
  u32 r = _op.reg, z = _op.off / 8;
  switch (_op.kind)
  {
  case UwKindAlloc:
  {
    u32 units = _op.off / 16;
    NC_ASSERT(_op.off % 16 == 0);
    if (units < 32)
      UwPut(_c, 1, units);
    else if (units < 2048)
      UwPut(_c, 2, 0xC000u | units);
    else
    {
      NC_ASSERT(units < (1u << 24));
      UwPut(_c, 4, 0xE0000000u | units);
    }
    return;
  }
  case UwKindSaveR19r20X:
    NC_ASSERT(z < 32);
    UwPut(_c, 1, 0x20u | z);
    return;
  case UwKindSaveFplr:
    NC_ASSERT(z < 64);
    UwPut(_c, 1, 0x40u | z);
    return;
  case UwKindSaveFplrX:
    NC_ASSERT(z >= 1 && z <= 64);
    UwPut(_c, 1, 0x80u | (z - 1));
    return;
  case UwKindSaveRegp:
    NC_ASSERT(z < 64);
    UwPut(_c, 2, 0xC800u | (r - 19) << 6 | z);
    return;
  case UwKindSaveRegpX:
    NC_ASSERT(z >= 1 && z <= 64);
    UwPut(_c, 2, 0xCC00u | (r - 19) << 6 | (z - 1));
    return;
  case UwKindSaveReg:
    NC_ASSERT(z < 64);
    UwPut(_c, 2, 0xD000u | (r - 19) << 6 | z);
    return;
  case UwKindSaveRegX:
    NC_ASSERT(z >= 1 && z <= 32);
    UwPut(_c, 2, 0xD400u | (r - 19) << 5 | (z - 1));
    return;
  case UwKindSaveLrpair:
    UwPut(_c, 2, 0xD600u | ((r - 19) / 2) << 6 | z);
    return;
  case UwKindSaveFregp:
    NC_ASSERT(z < 64);
    UwPut(_c, 2, 0xD800u | (r - 8) << 6 | z);
    return;
  case UwKindSaveFregpX:
    NC_ASSERT(z >= 1 && z <= 64);
    UwPut(_c, 2, 0xDA00u | (r - 8) << 6 | (z - 1));
    return;
  case UwKindSaveFreg:
    NC_ASSERT(z < 64);
    UwPut(_c, 2, 0xDC00u | (r - 8) << 6 | z);
    return;
  case UwKindSaveFregX:
    NC_ASSERT(z >= 1 && z <= 32);
    UwPut(_c, 2, 0xDE00u | (r - 8) << 5 | (z - 1));
    return;
  case UwKindSetFp:
    UwPut(_c, 1, 0xE1);
    return;
  case UwKindAddFp:
    NC_ASSERT(z < 256);
    UwPut(_c, 2, 0xE200u | z);
    return;
  case UwKindNop:
    UwPut(_c, 1, 0xE3);
    return;
  case UwKindEnd:
    UwPut(_c, 1, 0xE4);
    return;
  case UwKindEndC:
    UwPut(_c, 1, 0xE5);
    return;
  case UwKindSaveNext:
    UwPut(_c, 1, 0xE6);
    return;
  case UwKindPacSignLr:
    UwPut(_c, 1, 0xFC);
    return;
  case UwKindUnknown:
    break;
  }
  NC_UNREACHABLE();
}

/* Decodes one code; returns its length in bytes (0 if truncated). */
static u32 UwDecode(const u8* _p, u32 _avail, UwOp* _op)
{
  if (!_avail)
    return 0;
  u32 b = _p[0];
  u32 v = _avail >= 2 ? (b << 8 | _p[1]) : 0;
  *_op = (UwOp){UwKindUnknown, 0, 0};
  if (b < 0x20)
  {
    *_op = (UwOp){UwKindAlloc, 0, (b & 0x1F) * 16};
    return 1;
  }
  if (b < 0x40)
  {
    *_op = (UwOp){UwKindSaveR19r20X, 19, (b & 0x1F) * 8};
    return 1;
  }
  if (b < 0x80)
  {
    *_op = (UwOp){UwKindSaveFplr, 29, (b & 0x3F) * 8};
    return 1;
  }
  if (b < 0xC0)
  {
    *_op = (UwOp){UwKindSaveFplrX, 29, ((b & 0x3F) + 1) * 8};
    return 1;
  }
  if (b <= 0xDE && _avail < 2)
    return 0;
  if (b < 0xC8)
  {
    *_op = (UwOp){UwKindAlloc, 0, (v & 0x7FF) * 16};
    return 2;
  }
  if (b < 0xCC)
  {
    *_op = (UwOp){UwKindSaveRegp, (u8)(19 + ((v >> 6) & 0xF)), (v & 0x3F) * 8};
    return 2;
  }
  if (b < 0xD0)
  {
    *_op = (UwOp){UwKindSaveRegpX, (u8)(19 + ((v >> 6) & 0xF)), ((v & 0x3F) + 1) * 8};
    return 2;
  }
  if (b < 0xD4)
  {
    *_op = (UwOp){UwKindSaveReg, (u8)(19 + ((v >> 6) & 0xF)), (v & 0x3F) * 8};
    return 2;
  }
  if (b < 0xD6)
  {
    *_op = (UwOp){UwKindSaveRegX, (u8)(19 + ((v >> 5) & 0xF)), ((v & 0x1F) + 1) * 8};
    return 2;
  }
  if (b < 0xD8)
  {
    *_op = (UwOp){UwKindSaveLrpair, (u8)(19 + 2 * ((v >> 6) & 7)), (v & 0x3F) * 8};
    return 2;
  }
  if (b < 0xDA)
  {
    *_op = (UwOp){UwKindSaveFregp, (u8)(8 + ((v >> 6) & 7)), (v & 0x3F) * 8};
    return 2;
  }
  if (b < 0xDC)
  {
    *_op = (UwOp){UwKindSaveFregpX, (u8)(8 + ((v >> 6) & 7)), ((v & 0x3F) + 1) * 8};
    return 2;
  }
  if (b < 0xDE)
  {
    *_op = (UwOp){UwKindSaveFreg, (u8)(8 + ((v >> 6) & 7)), (v & 0x3F) * 8};
    return 2;
  }
  if (b == 0xDE)
  {
    *_op = (UwOp){UwKindSaveFregX, (u8)(8 + ((v >> 5) & 7)), ((v & 0x1F) + 1) * 8};
    return 2;
  }
  if (b == 0xDF)
    return _avail >= 2 ? 2 : 0; /* alloc_z (SVE): unsupported */
  if (b == 0xE0)
  {
    if (_avail < 4)
      return 0;
    u32 units = (u32)_p[1] << 16 | (u32)_p[2] << 8 | _p[3];
    *_op = (UwOp){UwKindAlloc, 0, units * 16};
    return 4;
  }
  if (b == 0xE1)
  {
    _op->kind = UwKindSetFp;
    return 1;
  }
  if (b == 0xE2)
  {
    if (_avail < 2)
      return 0;
    *_op = (UwOp){UwKindAddFp, 0, _p[1] * 8u};
    return 2;
  }
  if (b == 0xE3)
  {
    _op->kind = UwKindNop;
    return 1;
  }
  if (b == 0xE4)
  {
    _op->kind = UwKindEnd;
    return 1;
  }
  if (b == 0xE5)
  {
    _op->kind = UwKindEndC;
    return 1;
  }
  if (b == 0xE6)
  {
    _op->kind = UwKindSaveNext;
    return 1;
  }
  if (b == 0xE7)
    return _avail >= 3 ? 3 : 0; /* save_any_*: not generated */
  if (b == 0xFC)
  {
    _op->kind = UwKindPacSignLr;
    return 1;
  }
  if (b >= 0xF8 && b <= 0xFB)
    return b - 0xF8 + 2 <= _avail ? b - 0xF8 + 2 : 0;
  return 1;
}

static UwOp StepCode(const FrameStep* _s)
{
  switch (_s->kind)
  {
  case FrameStepKindStpXPre:
    return (UwOp){UwKindSaveRegpX, _s->reg, _s->off};
  case FrameStepKindStpX:
    return (UwOp){UwKindSaveRegp, _s->reg, _s->off};
  case FrameStepKindStrXPre:
    return (UwOp){UwKindSaveRegX, _s->reg, _s->off};
  case FrameStepKindStrX:
    return (UwOp){UwKindSaveReg, _s->reg, _s->off};
  case FrameStepKindStpDPre:
    return (UwOp){UwKindSaveFregpX, _s->reg, _s->off};
  case FrameStepKindStpD:
    return (UwOp){UwKindSaveFregp, _s->reg, _s->off};
  case FrameStepKindStrDPre:
    return (UwOp){UwKindSaveFregX, _s->reg, _s->off};
  case FrameStepKindStrD:
    return (UwOp){UwKindSaveFreg, _s->reg, _s->off};
  case FrameStepKindFplrPre:
    return (UwOp){UwKindSaveFplrX, 29, _s->off};
  case FrameStepKindFplr:
    return (UwOp){UwKindSaveFplr, 29, _s->off};
  case FrameStepKindSetFp:
    return (UwOp){UwKindSetFp, 0, 0};
  case FrameStepKindAlloc:
  case FrameStepKindChkSub:
    return (UwOp){UwKindAlloc, 0, _s->off};
  case FrameStepKindHome:
  case FrameStepKindChkMovz:
  case FrameStepKindChkMovk:
  case FrameStepKindChkBl:
    return (UwOp){UwKindNop, 0, 0};
  }
  NC_UNREACHABLE();
}

/* Prologue codes: steps in reverse execution order, then end. */
static void PrologCodes(const FramePlan* _p, UwCodes* _c)
{
  _c->len = 0;
  for (u32 i = _p->nsteps; i-- > 0;)
    UwEncode(_c, StepCode(&_p->steps[i]));
  UwEncode(_c, (UwOp){UwKindEnd, 0, 0});
}

/* Epilogue codes: one per epilogue instruction, in execution order, then end
 * (which stands for the ret). */
static void EpilogCodes(const FramePlan* _p, UwCodes* _c)
{
  _c->len = 0;
  for (u32 i = _p->nsteps; i-- > 0;)
  {
    MirInst tmp;
    if (StepEpilogInsn(_p, &_p->steps[i], &tmp))
      UwEncode(_c, StepCode(&_p->steps[i]));
  }
  UwEncode(_c, (UwOp){UwKindEnd, 0, 0});
}

/* Returns the byte offset at which `e` occurs as a suffix of `p` on a code
 * boundary, or -1. */
static i32 SuffixOffset(const UwCodes* _p, const UwCodes* _e)
{
  if (_e->len > _p->len)
    return -1;
  u32 start = _p->len - _e->len;
  if (memcmp(_p->b + start, _e->b, _e->len) != 0)
    return -1;
  for (u32 pos = 0; pos < _p->len;)
  {
    if (pos == start)
      return (i32)start;
    UwOp op;
    u32 n = UwDecode(_p->b + pos, _p->len - pos, &op);
    if (!n)
      break;
    pos += n;
  }
  return -1;
}

void UnwindFormatCodes(const u8* _codes, u32 _len, Buf* _out)
{
  char line[96];
  for (u32 pos = 0; pos < _len;)
  {
    UwOp op;
    u32 n = UwDecode(_codes + pos, _len - pos, &op);
    if (!n)
    {
      BufBytes(_out, "<truncated>\n", 12);
      break;
    }
    int k = snprintf(line, sizeof line, "  %s", UW_NAMES[op.kind]);
    if (op.kind >= UwKindSaveR19r20X && op.kind <= UwKindSaveFregX)
      k += snprintf(line + k, sizeof line - (size_t)k, " %c%u, %u", op.kind >= UwKindSaveFregp ? 'd' : 'x', op.reg, op.off);
    else if (op.kind == UwKindAlloc || op.kind == UwKindAddFp)
      k += snprintf(line + k, sizeof line - (size_t)k, " %u", op.off);
    line[k++] = '\n';
    BufBytes(_out, line, (u32)k);
    pos += n;
  }
}

/* ---- packed unwind data ------------------------------------------------- */

typedef struct PackedFields
{
  u32 funcLen, regF, regI, h, cr, frameSize;
} PackedFields;

static PackedFields PackedDecode(u32 _w)
{
  return (PackedFields){((_w >> 2) & 0x7FF) * 4, (_w >> 13) & 7, (_w >> 16) & 0xF, (_w >> 20) & 1, (_w >> 21) & 3, (_w >> 23) * 16};
}

/* Whether a step can be represented by its unwind code and instruction.
 * Packed words from an image may be arbitrary, so their expansion is
 * validated rather than asserted. */
static bool StepValid(const FrameStep* _s)
{
  u32 o = _s->off;
  switch (_s->kind)
  {
  case FrameStepKindStpXPre:
  case FrameStepKindStpDPre:
  case FrameStepKindFplrPre:
    return o >= 8 && o <= 512 && o % 8 == 0;
  case FrameStepKindStrXPre:
  case FrameStepKindStrDPre:
    return o >= 8 && o <= 256 && o % 8 == 0;
  case FrameStepKindStpX:
  case FrameStepKindStrX:
  case FrameStepKindStpD:
  case FrameStepKindStrD:
  case FrameStepKindFplr:
  case FrameStepKindHome:
    return o <= 504 && o % 8 == 0;
  case FrameStepKindAlloc:
  case FrameStepKindChkSub:
    return o % 16 == 0 && o < (1u << 28);
  default:
    return true;
  }
}

/* Expands packed fields into the canonical prologue, following the step
 * table in Microsoft's documentation. Independent of FramePlanBuild() on
 * purpose: the two must agree for packed data to be used. Only CR == 3
 * (chained, stp x29/lr) is supported. */
/* Why a packed word could not be expanded (for the image checker's verdict). */
static const char* g_packedUnsupportedReason;

static bool PackedExpand(PackedFields _f, FramePlan* _x)
{
  g_packedUnsupportedReason = nullptr;
  memset(_x, 0, sizeof *_x);
  if (_f.cr != 3)
  {
    /* CR 0/1: unchained frames; CR 2: pacibsp-signed. Valid Windows forms
         * (MSVC emits them), but ncc never does and the expander does not
         * model them. */
    g_packedUnsupportedReason =
      _f.cr == 2 ? "packed CR=2 (signed return address) not modelled" : "packed CR=0/1 (unchained frame) not modelled";
    return false;
  }
  if (_f.regI > 10)
    return false;
  u32 nfp = _f.regF ? _f.regF + 1 : 0;
  u32 intsz = _f.regI * 8, fpsz = nfp * 8;
  u32 savsz = NC_ALIGN_UP(intsz + fpsz + 64 * _f.h, 16u);
  if (_f.frameSize < savsz)
    return false;
  u32 locsz = _f.frameSize - savsz;
  if (_f.h && _f.regI == 0 && nfp == 0)
  {
    g_packedUnsupportedReason = "packed H=1 without saved registers not modelled";
    return false;
  }
  for (u32 i = 0; i < _f.regI; i += 2)
  {
    bool pair = i + 1 < _f.regI;
    if (i == 0)
      AddStep(_x, pair ? FrameStepKindStpXPre : FrameStepKindStrXPre, 19, savsz);
    else
      AddStep(_x, pair ? FrameStepKindStpX : FrameStepKindStrX, 19 + i, 8 * i);
  }
  for (u32 j = 0; j < nfp; j += 2)
  {
    bool pair = j + 1 < nfp, first = _f.regI == 0 && j == 0;
    if (first)
      AddStep(_x, pair ? FrameStepKindStpDPre : FrameStepKindStrDPre, 8, savsz);
    else
      AddStep(_x, pair ? FrameStepKindStpD : FrameStepKindStrD, 8 + j, intsz + 8 * j);
  }
  if (_f.h)
    for (u32 k = 0; k < 4; k++)
      AddStep(_x, FrameStepKindHome, 2 * k, intsz + fpsz + 16 * k);
  if (locsz <= 512)
  {
    AddStep(_x, FrameStepKindFplrPre, 0, locsz);
  }
  else if (locsz <= 4080)
  {
    AddStep(_x, FrameStepKindAlloc, 0, locsz);
    AddStep(_x, FrameStepKindFplr, 0, 0);
  }
  else
  {
    AddStep(_x, FrameStepKindAlloc, 0, 4080);
    AddStep(_x, FrameStepKindAlloc, 0, locsz - 4080);
    AddStep(_x, FrameStepKindFplr, 0, 0);
  }
  AddStep(_x, FrameStepKindSetFp, 0, 0);
  _x->shape = FrameShapeA;
  if (locsz < 16)
    return false; /* a chained frame always saves {x29, lr} */
  for (u32 i = 0; i < _x->nsteps; i++)
    if (!StepValid(&_x->steps[i]))
      return false;
  return true;
}

static bool StepsEqual(const FramePlan* _a, const FramePlan* _b)
{
  if (_a->nsteps != _b->nsteps)
    return false;
  for (u32 i = 0; i < _a->nsteps; i++)
    if (_a->steps[i].kind != _b->steps[i].kind || _a->steps[i].reg != _b->steps[i].reg || _a->steps[i].off != _b->steps[i].off)
      return false;
  return true;
}

static bool TryPack(const FramePlan* _p, u32 _funcBytes, const u32* _epilogs, u32 _nepilogs, u32* _word)
{
  if (_p->shape != FrameShapeA || _nepilogs != 1)
    return false;
  if (_funcBytes > 2047 * 4 || _p->frameSize / 16 > 511 || _p->nFpr == 1)
    return false;
  if (_epilogs[0] != _funcBytes - 4 * FrameEpilogueLen(_p))
    return false;
  u32 regF = _p->nFpr ? _p->nFpr - 1u : 0u;
  u32 w = 1u | (_funcBytes / 4) << 2 | regF << 13 | (u32)_p->nGpr << 16 | (u32)_p->homeArgs << 20 | 3u << 21 | (_p->frameSize / 16) << 23;
  FramePlan x;
  if (!PackedExpand(PackedDecode(w), &x) || !StepsEqual(_p, &x))
    return false;
  *_word = w;
  return true;
}

/* ---- full .xdata records ------------------------------------------------ */

void UnwindBuild(const FramePlan* _p, u32 _funcBytes, const u32* _epilogs, u32 _nepilogs, UnwindRecord* _out)
{
  memset(_out, 0, sizeof *_out);
  if (_p->shape == FrameShapeNone)
  {
    _out->none = true;
    return;
  }
  if (TryPack(_p, _funcBytes, _epilogs, _nepilogs, &_out->packedWord))
  {
    _out->packed = true;
    return;
  }
  if (_funcBytes / 4 >= (1u << 18))
    NcFatal("function of %u bytes exceeds the 1 MiB unwind limit (fragments are not "
            "supported yet)",
            _funcBytes);

  UwCodes codes, epi;
  PrologCodes(_p, &codes);
  EpilogCodes(_p, &epi);
  i32 epiIndex = SuffixOffset(&codes, &epi);
  if (epiIndex < 0)
  {
    epiIndex = (i32)codes.len;
    memcpy(codes.b + codes.len, epi.b, epi.len);
    codes.len += epi.len;
  }
  while (codes.len % 4)
    codes.b[codes.len++] = 0xE4; /* pad with end */
  u32 words = codes.len / 4;
  NC_ASSERT(epiIndex < 1024);

  Buf* x = &_out->xdata;
  u32 flen = _funcBytes / 4;
  if (_nepilogs > 31 || words > 31)
  {
    NC_ASSERT(_nepilogs <= 0xFFFF && words <= 0xFF);
    BufU32(x, flen);
    BufU32(x, _nepilogs | words << 16);
  }
  else
  {
    BufU32(x, flen | _nepilogs << 22 | words << 27);
  }
  for (u32 i = 0; i < _nepilogs; i++)
  {
    NC_ASSERT(_epilogs[i] % 4 == 0 && _epilogs[i] < _funcBytes);
    BufU32(x, _epilogs[i] / 4 | (u32)epiIndex << 22);
  }
  BufBytes(x, codes.b, codes.len);
}

void UnwindRecordFree(UnwindRecord* _r)
{
  BufFree(&_r->xdata);
}

/* ======================================================================== */
/* Verifier                                                                 */
/* ======================================================================== */

enum
{
  MEM_SLOTS = 256
}; /* prologues store at most ~30 words */

typedef struct MemSlot
{
  u64 addr, val;
  bool used;
} MemSlot;

typedef struct EmuState
{
  u64 x[31];
  u64 sp;
  u64 d[32];
  u64 pc; /* function-relative byte offset; UINT64_MAX after ret */
  MemSlot mem[MEM_SLOTS];
} EmuState;

static const u64 POISON = 0xDEADDEADDEADDEADull;

static MemSlot* MemSlotFind(EmuState* _s, u64 _addr, bool _create)
{
  u32 h = (u32)((_addr >> 3) * 2654435761u) & (MEM_SLOTS - 1);
  for (u32 i = 0; i < MEM_SLOTS; i++, h = (h + 1) & (MEM_SLOTS - 1))
  {
    if (_s->mem[h].used && _s->mem[h].addr == _addr)
      return &_s->mem[h];
    if (!_s->mem[h].used)
    {
      if (!_create)
        return nullptr;
      _s->mem[h] = (MemSlot){_addr, 0, true};
      return &_s->mem[h];
    }
  }
  NcFatal("unwind verifier: emulated memory full");
}

static void MemStore(EmuState* _s, u64 _addr, u64 _v)
{
  MemSlotFind(_s, _addr, true)->val = _v;
}
static u64 MemLoad(EmuState* _s, u64 _addr)
{
  MemSlot* m = MemSlotFind(_s, _addr, false);
  return m ? m->val : POISON;
}

typedef struct VErr
{
  char* buf;
  size_t size;
  bool failed;
  bool skipped; /* failure means "outside what the verifier models", not "wrong" */
} VErr;

static bool Vfail(VErr* _e, const char* _fmt, ...)
{
  if (_e->failed)
    return false;
  _e->failed = true;
  va_list ap;
  va_start(ap, _fmt);
  vsnprintf(_e->buf, _e->size, _fmt, ap);
  va_end(ap);
  return false;
}

/* Like vfail, but for constructs the verifier does not model (instructions
 * ncc never emits in frame code, unwind codes it never generates). For ncc's
 * own functions this is still a failure; for foreign code in a linked image
 * (e.g. the CRT) the function is reported as skipped. */
static bool Vunsupported(VErr* _e, const char* _fmt, ...)
{
  if (_e->failed)
    return false;
  _e->failed = _e->skipped = true;
  va_list ap;
  va_start(ap, _fmt);
  vsnprintf(_e->buf, _e->size, _fmt, ap);
  va_end(ap);
  return false;
}

static i64 Sext(u64 _v, u32 _bits)
{
  return (i64)(_v << (64 - _bits)) >> (64 - _bits);
}

static u64* Xreg(EmuState* _s, u32 _r)
{
  return _r == 31 ? &_s->sp : &_s->x[_r];
}

/* Executes one prologue/epilogue instruction. Only the instruction forms the
 * frame lowering emits are supported; anything else is a verifier error. */
static bool EmuExec(EmuState* _s, u32 _w, VErr* _e)
{
  u32 rt = _w & 31, rn = (_w >> 5) & 31;
  if ((_w & 0x3A000000) == 0x28000000)
  { /* load/store pair */
    u32 opc = _w >> 30, v = (_w >> 26) & 1, idx = (_w >> 23) & 3, l = (_w >> 22) & 1;
    u32 rt2 = (_w >> 10) & 31;
    if (!((v == 0 && opc == 2) || (v == 1 && opc == 1)) || idx == 0)
      return Vunsupported(_e, "unsupported pair instruction %08x", _w);
    i64 imm = Sext((_w >> 15) & 0x7F, 7) * 8;
    u64 base = *Xreg(_s, rn);
    u64 addr = idx == 1 ? base : base + (u64)imm;
    if (l)
    {
      u64 a = MemLoad(_s, addr), b = MemLoad(_s, addr + 8);
      if (v)
      {
        _s->d[rt] = a;
        _s->d[rt2] = b;
      }
      else
      {
        _s->x[rt] = a;
        _s->x[rt2] = b;
      } /* rt/rt2 == 31 would be xzr: not emitted */
    }
    else
    {
      MemStore(_s, addr, v ? _s->d[rt] : (rt == 31 ? 0 : _s->x[rt]));
      MemStore(_s, addr + 8, v ? _s->d[rt2] : (rt2 == 31 ? 0 : _s->x[rt2]));
    }
    if (idx != 2)
      *Xreg(_s, rn) = base + (u64)imm;
  }
  else if ((_w & 0x3B000000) == 0x39000000)
  { /* ldr/str unsigned offset */
    u32 size = _w >> 30, v = (_w >> 26) & 1, opc = (_w >> 22) & 3;
    if (size != 3 || opc > 1)
      return Vunsupported(_e, "unsupported load/store %08x", _w);
    u64 addr = *Xreg(_s, rn) + (((_w >> 10) & 0xFFF) << 3);
    if (opc)
    {
      if (v)
        _s->d[rt] = MemLoad(_s, addr);
      else
        _s->x[rt] = MemLoad(_s, addr);
    }
    else
      MemStore(_s, addr, v ? _s->d[rt] : _s->x[rt]);
  }
  else if ((_w & 0x3B200000) == 0x38000000)
  { /* ldr/str imm9 */
    u32 size = _w >> 30, v = (_w >> 26) & 1, opc = (_w >> 22) & 3, idx = (_w >> 10) & 3;
    if (size != 3 || opc > 1 || (idx != 1 && idx != 3))
      return Vunsupported(_e, "unsupported load/store %08x", _w);
    i64 imm = Sext((_w >> 12) & 0x1FF, 9);
    u64 base = *Xreg(_s, rn);
    u64 addr = idx == 1 ? base : base + (u64)imm;
    if (opc)
    {
      if (v)
        _s->d[rt] = MemLoad(_s, addr);
      else
        _s->x[rt] = MemLoad(_s, addr);
    }
    else
      MemStore(_s, addr, v ? _s->d[rt] : _s->x[rt]);
    *Xreg(_s, rn) = base + (u64)imm;
  }
  else if ((_w & 0x9F000000) == 0x91000000)
  { /* add/sub immediate, 64-bit */
    if (_w & (1u << 29))
      return Vunsupported(_e, "flag-setting add/sub in frame code %08x", _w);
    u64 imm = ((_w >> 10) & 0xFFF) << ((_w >> 22) & 1 ? 12 : 0);
    u64 src = *Xreg(_s, rn);
    *Xreg(_s, rt) = (_w & (1u << 30)) ? src - imm : src + imm;
  }
  else if ((_w & 0x9FE00000) == 0x8B200000)
  { /* add/sub extended, 64-bit */
    u32 rm = (_w >> 16) & 31, opt = (_w >> 13) & 7, sh = (_w >> 10) & 7;
    if (opt != A64ExtendUxtx || (_w & (1u << 29)))
      return Vunsupported(_e, "unsupported extend %08x", _w);
    u64 src = *Xreg(_s, rn), m = (rm == 31 ? 0 : _s->x[rm]) << sh;
    *Xreg(_s, rt) = (_w & (1u << 30)) ? src - m : src + m;
  }
  else if ((_w & 0x9F800000) == 0x92800000)
  { /* movn/movz/movk, 64-bit */
    u32 opc = (_w >> 29) & 3, hw = (_w >> 21) & 3;
    u64 imm = (u64)((_w >> 5) & 0xFFFF) << (16 * hw);
    if (opc == 0)
      _s->x[rt] = ~imm;
    else if (opc == 2)
      _s->x[rt] = imm;
    else if (opc == 3)
      _s->x[rt] = (_s->x[rt] & ~((u64)0xFFFF << (16 * hw))) | imm;
    else
      return Vunsupported(_e, "bad move-wide %08x", _w);
  }
  else if ((_w & 0xFC000000) == 0x94000000)
  { /* bl: only __chkstk in frame code */
    _s->x[30] = _s->pc + 4;
    _s->x[16] = _s->x[17] = POISON; /* __chkstk clobbers x16/x17, preserves x15 */
  }
  else if ((_w & 0xFFFFFC1F) == 0xD65F0000)
  { /* ret */
    _s->pc = _s->x[rn];
    return true;
  }
  else if ((_w & 0xFFFFF01F) == 0xD503201F)
  { /* hint space: nop, pacibsp */
  }
  else
  {
    return Vunsupported(_e, "unexpected instruction %08x in frame code", _w);
  }
  _s->pc += 4;
  return true;
}

/* A decoded unwind program: prologue ops plus epilogue scopes. */
typedef struct UwProgram
{
  UwOp ops[160];
  u32 nops;
  u32 opAtByte[512]; /* byte index -> op index */
  u32 prologCount;   /* instruction-mapping ops before the first end */
  u32 nscopes;
  u32 scopeOff[64]; /* bytes */
  u32 scopeOp[64];  /* op index */
  u32 funcBytes;
  bool headerEpilog; /* E bit: one epilogue, described in the header */
} UwProgram;

static bool ProgramFromCodes(UwProgram* _u, const u8* _codes, u32 _len, VErr* _e)
{
  _u->nops = 0;
  for (u32 pos = 0; pos < _len;)
  {
    UwOp op;
    u32 n = UwDecode(_codes + pos, _len - pos, &op);
    if (!n)
      return Vfail(_e, "truncated unwind code at byte %u", pos);
    if (op.kind == UwKindUnknown)
      return Vunsupported(_e, "unsupported unwind code %02x", _codes[pos]);
    /* end_c marks a function fragment: its prologue codes describe a
         * phantom prologue in the parent region, so "instructions before the
         * first end" no longer equals the fragment's own prologue length.
         * ncc never emits fragments (Q-16) and the RunSweep does not model them. */
    if (op.kind == UwKindEndC)
      return Vunsupported(_e, "function fragment (end_c) not modelled");
    if (_u->nops >= NC_ARRAY_LEN(_u->ops) || pos >= NC_ARRAY_LEN(_u->opAtByte))
      return Vfail(_e, "too many unwind codes");
    _u->opAtByte[pos] = _u->nops;
    _u->ops[_u->nops++] = op;
    pos += n;
  }
  _u->prologCount = 0;
  while (_u->prologCount < _u->nops && _u->ops[_u->prologCount].kind != UwKindEnd)
    _u->prologCount++;
  if (_u->prologCount == _u->nops)
    return Vfail(_e, "unwind codes lack an end");
  return true;
}

static bool ProgramFromRecord(UwProgram* _u, const UnwindRecord* _r, u32 _funcBytes, VErr* _e)
{
  memset(_u, 0, sizeof *_u);
  _u->funcBytes = _funcBytes;
  if (_r->packed)
  {
    PackedFields f = PackedDecode(_r->packedWord);
    if ((_r->packedWord & 3) != 1)
      return Vfail(_e, "packed flag is not 1");
    if (f.funcLen != _funcBytes)
      return Vfail(_e, "packed function length %u != %u", f.funcLen, _funcBytes);
    FramePlan x;
    if (!PackedExpand(f, &x))
    {
      if (g_packedUnsupportedReason)
        return Vunsupported(_e, "%s", g_packedUnsupportedReason);
      return Vfail(_e, "packed data does not expand to a valid frame");
    }
    UwCodes pc, ec;
    PrologCodes(&x, &pc);
    /* Packed epilogue: reverse prologue without the home stores and
         * without set_fp, at the end of the function. */
    ec.len = 0;
    u32 epiInsns = 1;
    for (u32 i = x.nsteps; i-- > 0;)
    {
      if (x.steps[i].kind == FrameStepKindHome || x.steps[i].kind == FrameStepKindSetFp)
        continue;
      UwEncode(&ec, StepCode(&x.steps[i]));
      epiInsns++;
    }
    UwEncode(&ec, (UwOp){UwKindEnd, 0, 0});
    u8 all[1024];
    memcpy(all, pc.b, pc.len);
    memcpy(all + pc.len, ec.b, ec.len);
    if (!ProgramFromCodes(_u, all, pc.len + ec.len, _e))
      return false;
    _u->nscopes = 1;
    _u->scopeOff[0] = _funcBytes - 4 * epiInsns;
    _u->scopeOp[0] = _u->opAtByte[pc.len];
    return true;
  }
  const Buf* x = &_r->xdata;
  if (x->len < 4)
    return Vfail(_e, "xdata too short");
  u32 w0 = BufGetU32(x, 0);
  u32 flen = (w0 & 0x3FFFF) * 4, ebit = (w0 >> 21) & 1, xbit = (w0 >> 20) & 1;
  u32 nep = (w0 >> 22) & 0x1F, words = (w0 >> 27) & 0x1F, pos = 4;
  if ((w0 >> 18) & 3)
    return Vfail(_e, "xdata version is not 0");
  if (flen != _funcBytes)
    return Vfail(_e, "xdata function length %u != %u", flen, _funcBytes);
  if (nep == 0 && words == 0)
  {
    u32 w1 = BufGetU32(x, 4);
    nep = w1 & 0xFFFF;
    words = (w1 >> 16) & 0xFF;
    pos = 8;
  }
  if (ebit)
  {
    /* One epilogue whose codes start at index `nep`; ncc never sets E, and
         * the epilogue's location is not recorded, so only prologue and body
         * can be checked for such functions. */
    u32 codeBytes = words * 4;
    if (pos + codeBytes + (xbit ? 4u : 0u) != x->len)
      return Vfail(_e, "xdata size mismatch (%u code words, %u bytes total)", words, x->len);
    if (!ProgramFromCodes(_u, x->data + pos, codeBytes, _e))
      return false;
    _u->headerEpilog = true;
    _u->nscopes = 0;
    return true;
  }
  if (nep > NC_ARRAY_LEN(_u->scopeOff))
    return Vfail(_e, "too many epilogue scopes");
  u32 scopeIndex[64];
  for (u32 i = 0; i < nep; i++)
  {
    u32 sw = BufGetU32(x, pos);
    pos += 4;
    _u->scopeOff[i] = (sw & 0x3FFFF) * 4;
    scopeIndex[i] = sw >> 22;
    if ((sw >> 18) & 0xF)
      return Vfail(_e, "epilogue scope reserved bits set");
    if (i && _u->scopeOff[i] <= _u->scopeOff[i - 1])
      return Vfail(_e, "epilogue scopes not in increasing order");
  }
  _u->nscopes = nep;
  u32 codeBytes = words * 4;
  if (pos + codeBytes + (xbit ? 4u : 0u) != x->len)
    return Vfail(_e, "xdata size mismatch (%u codes words, %u bytes total)", words, x->len);
  if (!ProgramFromCodes(_u, x->data + pos, codeBytes, _e))
    return false;
  for (u32 i = 0; i < nep; i++)
  {
    if (scopeIndex[i] >= codeBytes)
      return Vfail(_e, "epilogue index out of range");
    _u->scopeOp[i] = _u->opAtByte[scopeIndex[i]];
  }
  return true;
}

/* Unwinds one frame from function-relative byte offset `off`, mutating `s`
 * the way RtlVirtualUnwind would. */
static bool UwRun(const UwProgram* _u, u32 _off, EmuState* _s, VErr* _e)
{
  u32 start = 0;
  u32 insn = _off / 4;
  if (insn < _u->prologCount)
  {
    start = _u->prologCount - insn;
  }
  else
  {
    for (u32 i = _u->nscopes; i-- > 0;)
    {
      if (_off < _u->scopeOff[i])
        continue;
      u32 k = (_off - _u->scopeOff[i]) / 4, n = 0;
      while (_u->scopeOp[i] + n < _u->nops && _u->ops[_u->scopeOp[i] + n].kind != UwKindEnd)
        n++;
      if (k <= n)
        start = _u->scopeOp[i] + k;
      break;
    }
  }
  for (u32 i = start; i < _u->nops; i++)
  {
    UwOp op = _u->ops[i];
    u64 sp = _s->sp;
    switch (op.kind)
    {
    case UwKindAlloc:
      _s->sp += op.off;
      break;
    case UwKindSaveR19r20X:
      _s->x[19] = MemLoad(_s, sp);
      _s->x[20] = MemLoad(_s, sp + 8);
      _s->sp += op.off;
      break;
    case UwKindSaveFplr:
      _s->x[29] = MemLoad(_s, sp + op.off);
      _s->x[30] = MemLoad(_s, sp + op.off + 8);
      break;
    case UwKindSaveFplrX:
      _s->x[29] = MemLoad(_s, sp);
      _s->x[30] = MemLoad(_s, sp + 8);
      _s->sp += op.off;
      break;
    case UwKindSaveRegp:
      _s->x[op.reg] = MemLoad(_s, sp + op.off);
      _s->x[op.reg + 1] = MemLoad(_s, sp + op.off + 8);
      break;
    case UwKindSaveRegpX:
      _s->x[op.reg] = MemLoad(_s, sp);
      _s->x[op.reg + 1] = MemLoad(_s, sp + 8);
      _s->sp += op.off;
      break;
    case UwKindSaveReg:
      _s->x[op.reg] = MemLoad(_s, sp + op.off);
      break;
    case UwKindSaveRegX:
      _s->x[op.reg] = MemLoad(_s, sp);
      _s->sp += op.off;
      break;
    case UwKindSaveLrpair:
      _s->x[op.reg] = MemLoad(_s, sp + op.off);
      _s->x[30] = MemLoad(_s, sp + op.off + 8);
      break;
    case UwKindSaveFregp:
      _s->d[op.reg] = MemLoad(_s, sp + op.off);
      _s->d[op.reg + 1] = MemLoad(_s, sp + op.off + 8);
      break;
    case UwKindSaveFregpX:
      _s->d[op.reg] = MemLoad(_s, sp);
      _s->d[op.reg + 1] = MemLoad(_s, sp + 8);
      _s->sp += op.off;
      break;
    case UwKindSaveFreg:
      _s->d[op.reg] = MemLoad(_s, sp + op.off);
      break;
    case UwKindSaveFregX:
      _s->d[op.reg] = MemLoad(_s, sp);
      _s->sp += op.off;
      break;
    case UwKindSetFp:
      _s->sp = _s->x[29];
      break;
    case UwKindAddFp:
      _s->sp = _s->x[29] - op.off;
      break;
    case UwKindNop:
    case UwKindPacSignLr:
      break;
    case UwKindEnd:
      _s->pc = _s->x[30];
      return true;
    case UwKindEndC:
    case UwKindSaveNext:
    case UwKindUnknown:
      return Vunsupported(_e, "unwind code %s not supported by the verifier", UW_NAMES[op.kind]);
    }
  }
  return Vfail(_e, "unwind ran off the end of the codes");
}

static bool CheckUnwound(const EmuState* _got, const EmuState* _entry, const char* _where, u32 _off, VErr* _e)
{
  if (_got->sp != _entry->sp)
    return Vfail(_e, "%s +%u: unwound sp %llx, expected %llx", _where, _off, (unsigned long long)_got->sp, (unsigned long long)_entry->sp);
  if (_got->pc != _entry->x[30])
    return Vfail(_e, "%s +%u: unwound return address %llx, expected %llx", _where, _off, (unsigned long long)_got->pc,
                 (unsigned long long)_entry->x[30]);
  for (u32 r = 19; r <= 29; r++)
    if (_got->x[r] != _entry->x[r])
      return Vfail(_e, "%s +%u: x%u not restored", _where, _off, r);
  for (u32 r = 8; r <= 15; r++)
    if (_got->d[r] != _entry->d[r])
      return Vfail(_e, "%s +%u: d%u not restored", _where, _off, r);
  return true;
}

/* ---- the RunSweep -------------------------------------------------------- */

typedef struct Sweep
{
  const UwProgram* u;
  const u8* code;
  u32 funcBytes;
  u32 plen; /* prologue instructions */
  u32 nepi;
  const u32* epiOff; /* bytes */
  const u32* epiLen; /* instructions, including ret */
  u64 entrySp;
  bool simDynamic; /* move sp in the body as a dynamic allocation would */
  i64 expectFpSp;  /* x29 - sp after the prologue, or -1 if unknown */
  UwOsUnwindFn os;
  void* osCtx;
  u32 checks;
} Sweep;

static void ToMachine(const EmuState* _s, UwMachine* _m)
{
  memcpy(_m->x, _s->x, sizeof _m->x);
  memcpy(_m->d, _s->d, sizeof _m->d);
  _m->sp = _s->sp;
  _m->pc = _s->pc;
}

static void EntryState(EmuState* _s, u64 _sp)
{
  memset(_s, 0, sizeof *_s);
  for (u32 i = 0; i < 31; i++)
    _s->x[i] = 0xA5A5000000000000ull | (u64)i << 32 | 0x1111ull * i;
  for (u32 i = 0; i < 32; i++)
    _s->d[i] = 0xD0D0000000000000ull | (u64)i << 8;
  _s->x[30] = 0x0000700000001230ull; /* return address */
  _s->sp = _sp;
}

/* Unwinds from `off` in state `st` with our unwinder and, if present, the
 * external one, and checks both recover the entry state. */
static bool CheckAt(Sweep* _w, const EmuState* _st, const EmuState* _entry, u32 _off, const char* _where, VErr* _e)
{
  static EmuState g_tmp;
  g_tmp = *_st;
  if (!UwRun(_w->u, _off, &g_tmp, _e))
    return false;
  if (!CheckUnwound(&g_tmp, _entry, _where, _off, _e))
    return false;
  _w->checks++;
  if (!_w->os)
    return true;

  static u64 g_addr[MEM_SLOTS], g_val[MEM_SLOTS];
  u32 n = 0;
  for (u32 i = 0; i < MEM_SLOTS; i++)
    if (_st->mem[i].used)
    {
      g_addr[n] = _st->mem[i].addr;
      g_val[n] = _st->mem[i].val;
      n++;
    }
  UwMachine in, out;
  ToMachine(_st, &in);
  if (!_w->os(_w->osCtx, _off, &in, g_addr, g_val, n, &out))
    return Vfail(_e, "%s +%u: external unwinder failed", _where, _off);
  memcpy(g_tmp.x, out.x, sizeof g_tmp.x);
  memcpy(g_tmp.d, out.d, sizeof g_tmp.d);
  g_tmp.sp = out.sp;
  g_tmp.pc = out.pc;
  char label[64];
  snprintf(label, sizeof label, "%s (external unwinder)", _where);
  if (!CheckUnwound(&g_tmp, _entry, label, _off, _e))
    return false;
  _w->checks++;
  return true;
}

static bool ValueStored(const EmuState* _s, u64 _v)
{
  for (u32 i = 0; i < MEM_SLOTS; i++)
    if (_s->mem[i].used && _s->mem[i].val == _v)
      return true;
  return false;
}

static bool RunSweep(Sweep* _w, VErr* _e)
{
  static EmuState g_entry, g_cur, g_epi;
  EntryState(&g_entry, _w->entrySp);
  if (4 * _w->plen > _w->funcBytes)
    return Vfail(_e, "prologue longer than the function");

  /* Every prefix of the prologue. */
  g_cur = g_entry;
  for (u32 k = 0; k <= _w->plen; k++)
  {
    if (!CheckAt(_w, &g_cur, &g_entry, 4 * k, "prologue", _e))
      return false;
    if (k < _w->plen && !EmuExec(&g_cur, BufReadU32(_w->code + (size_t)4 * k), _e))
      return false;
  }
  if (_w->expectFpSp >= 0 && g_cur.x[29] != g_cur.sp + (u64)_w->expectFpSp)
    return Vfail(_e, "x29 - sp after prologue is %lld, plan says %lld", (long long)(g_cur.x[29] - g_cur.sp), (long long)_w->expectFpSp);

  /* The body may overwrite every callee-saved register the prologue saved
     * (and lr, by calling). Clobber them, so that from here on an unwinder
     * only recovers them if it really restores them from the stack. Without
     * this, a record that omits a register would still "restore" it, because
     * nothing ever changed it. x29 keeps its frame-pointer value. */
  for (u32 r = 19; r <= 30; r++)
    if (r != 29 && ValueStored(&g_cur, g_entry.x[r]))
      g_cur.x[r] = 0xB0D7000000000000ull | r;
  for (u32 r = 8; r <= 15; r++)
    if (ValueStored(&g_cur, g_entry.d[r]))
      g_cur.d[r] = 0xB0D7D00000000000ull | r;

  /* One body instruction, if there is one before the first epilogue. */
  u32 bodyOff = 4 * _w->plen;
  bool isEpilog = false;
  for (u32 i = 0; i < _w->nepi; i++)
    isEpilog |= _w->epiOff[i] == bodyOff;
  if (!isEpilog && bodyOff < _w->funcBytes && !CheckAt(_w, &g_cur, &g_entry, bodyOff, "body", _e))
    return false;

  /* Every instruction boundary of every epilogue. */
  EmuState* start = &g_cur;
  if (_w->simDynamic)
    start->sp -= 0x1230;
  for (u32 i = 0; i < _w->nepi; i++)
  {
    u32 off = _w->epiOff[i], len = _w->epiLen[i];
    if (off + 4 * len > _w->funcBytes)
      return Vfail(_e, "epilogue past function end");
    g_epi = *start;
    g_epi.pc = off;
    for (u32 j = 0; j < len; j++)
    {
      if (!CheckAt(_w, &g_epi, &g_entry, off + 4 * j, "epilogue", _e))
        return false;
      if (!EmuExec(&g_epi, BufReadU32(_w->code + off + (size_t)4 * j), _e))
        return false;
    }
    /* After ret the machine state itself must be the entry state. */
    if (!CheckUnwound(&g_epi, &g_entry, "after ret", off + 4 * len, _e))
      return false;
  }
  return true;
}

bool UnwindVerify(const FramePlan* _p, const u8* _code, u32 _funcBytes, const u32* _epilogs, u32 _nepilogs, const UnwindRecord* _r,
                  char* _err, size_t _errsz)
{
  VErr e = {_err, _errsz, false, false};
  if (_errsz)
    _err[0] = 0;
  if (_p->shape == FrameShapeNone)
    return _r->none ? true : Vfail(&e, "frameless function has unwind data");
  if (_r->none)
    return Vfail(&e, "function with a frame has no unwind data");

  static UwProgram g_program; /* large; not reentrant, verification is single-threaded */
  if (!ProgramFromRecord(&g_program, _r, _funcBytes, &e))
    return false;
  u32 plen = FramePrologueLen(_p), elen = FrameEpilogueLen(_p);
  if (g_program.prologCount != plen)
    return Vfail(&e, "unwind codes describe %u prologue instructions, prologue has %u", g_program.prologCount, plen);
  if (_nepilogs > 64)
    return Vfail(&e, "too many epilogues for the verifier");
  u32 lens[64];
  for (u32 i = 0; i < _nepilogs; i++)
    lens[i] = elen;
  Sweep w = {.u = &g_program,
             .code = _code,
             .funcBytes = _funcBytes,
             .plen = plen,
             .nepi = _nepilogs,
             .epiOff = _epilogs,
             .epiLen = lens,
             .entrySp = 0x0000000010000000ull,
             .simDynamic = _p->dynamic,
             .expectFpSp = _p->fpSpOffset};
  return RunSweep(&w, &e);
}

/* ---- records read back from a linked image ----------------------------- */

bool UnwindRecordFromPdata(u32 _word2, const u8* _xdata, size_t _avail, UnwindRecord* _out, u32* _funcBytes)
{
  memset(_out, 0, sizeof *_out);
  u32 flag = _word2 & 3;
  if (flag == 1)
  {
    _out->packed = true;
    _out->packedWord = _word2;
    *_funcBytes = PackedDecode(_word2).funcLen;
    return true;
  }
  if (flag != 0 || !_xdata || _avail < 4)
    return false; /* flag 2: no prologue; 3: link record */
  u32 w0 = BufReadU32(_xdata), size = 4, scopes = (w0 >> 22) & 0x1F, words = (w0 >> 27) & 0x1F;
  if ((w0 >> 22) == 0)
  { /* extension word */
    if (_avail < 8)
      return false;
    u32 w1 = BufReadU32(_xdata + 4);
    size = 8;
    scopes = w1 & 0xFFFF;
    words = (w1 >> 16) & 0xFF;
  }
  if (!((w0 >> 21) & 1))
    size += 4 * scopes;
  size += 4 * words;
  if ((w0 >> 20) & 1)
    size += 4; /* exception handler RVA; handler data is not needed */
  if (size > _avail)
    return false;
  BufBytes(&_out->xdata, _xdata, size);
  *_funcBytes = (w0 & 0x3FFFF) * 4;
  return true;
}

UwCheckResult UnwindCrosscheck(const u8* _code, u32 _funcBytes, const UnwindRecord* _r, u64 _entrySp, UwOsUnwindFn _os, void* _osCtx,
                               u32* _checks, char* _err, size_t _errsz)
{
  VErr e = {_err, _errsz, false, false};
  if (_errsz)
    _err[0] = 0;
  *_checks = 0;
  static UwProgram g_program;
  if (!ProgramFromRecord(&g_program, _r, _funcBytes, &e))
    return e.skipped ? UwCheckResultSkipped : UwCheckResultFail;
  if (4 * g_program.prologCount > _funcBytes)
  {
    /* Only possible for fragments (the prologue lives in the parent
         * region); for ncc's own functions UnwindVerify rejects it. */
    snprintf(_err, _errsz, "prologue longer than the function (fragment?) not modelled");
    return UwCheckResultSkipped;
  }
  u32 lens[64];
  for (u32 i = 0; i < g_program.nscopes; i++)
  {
    u32 n = 0;
    while (g_program.scopeOp[i] + n < g_program.nops && g_program.ops[g_program.scopeOp[i] + n].kind != UwKindEnd)
      n++;
    lens[i] = n + 1; /* the codes, plus ret for the end */
  }
  Sweep w = {.u = &g_program,
             .code = _code,
             .funcBytes = _funcBytes,
             .plen = g_program.prologCount,
             .nepi = g_program.nscopes,
             .epiOff = g_program.scopeOff,
             .epiLen = lens,
             .entrySp = _entrySp,
             .expectFpSp = -1,
             .os = _os,
             .osCtx = _osCtx};
  bool ok = RunSweep(&w, &e);
  *_checks = w.checks;
  if (!ok)
    return e.skipped ? UwCheckResultSkipped : UwCheckResultFail;
  return g_program.headerEpilog ? UwCheckResultPartial : UwCheckResultOk;
}
