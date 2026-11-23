/* a64_enc.c — ARM64 instruction encoder. */
#include "a64.h"

#include "../../obj/coff/coff.h"

#include <math.h>
#include <stdio.h>

const A64InsnInfo A64_INSN_INFO[A64OpRealCount] = {
#define INSN(id, mn, form, bits, kd, kn, km, sz) [A64Op##id] = {mn, bits, form, kd, kn, km, sz},
#include "arm64.def"
#undef INSN
};

const char* const A64_OP_NAMES[A64OpCount] = {
#define INSN(id, mn, form, bits, kd, kn, km, sz) [A64Op##id] = #id,
#include "arm64.def"
#undef INSN
  [A64OpPseudoLabel] = "P_LABEL",
  [A64OpPseudoPrologue] = "P_PROLOGUE",
  [A64OpPseudoEpilogue] = "P_EPILOGUE",
};

[[noreturn]] static void enc_fail(const MirInst* _mi, const char* _what, i64 _v)
{
  NcFatal("internal error: cannot encode %s: %s (value %lld)", A64_OP_NAMES[_mi->op], _what, (long long)_v);
}

#define ENC_CHECK(mi, cond, what, v)    \
  do                                    \
  {                                     \
    if (!(cond))                        \
      enc_fail((mi), (what), (i64)(v)); \
  } while (0)

/* ---- logical immediates ------------------------------------------------- */

static bool IsMask(u64 _v)
{
  return _v && ((_v + 1) & _v) == 0;
}
static bool IsShiftedMask(u64 _v)
{
  return _v && IsMask((_v - 1) | _v);
}
static u32 CountTrailingOnes(u64 _v)
{
  return NcCtz64(~_v);
}
static u32 CountLeadingOnes(u64 _v)
{
  return NcClz64(~_v);
}

bool A64EncodeLogicalImm(u64 _imm, u32 _width, u32* _n, u32* _immr, u32* _imms)
{
  NC_ASSERT(_width == 32 || _width == 64);
  if (_width == 32)
  {
    if (_imm >> 32)
      return false;
    if (_imm == 0 || _imm == 0xFFFFFFFFu)
      return false;
    _imm |= _imm << 32;
  }
  else if (_imm == 0 || _imm == ~(u64)0)
  {
    return false;
  }
  /* Find the smallest repeating element size. */
  u32 size = 64;
  do
  {
    size /= 2;
    u64 mask = ((u64)1 << size) - 1;
    if ((_imm & mask) != ((_imm >> size) & mask))
    {
      size *= 2;
      break;
    }
  } while (size > 2);

  u64 mask = ~(u64)0 >> (64 - size);
  u32 rot, ones;
  _imm &= mask;
  if (IsShiftedMask(_imm))
  {
    rot = NcCtz64(_imm);
    ones = CountTrailingOnes(_imm >> rot);
  }
  else
  {
    _imm |= ~mask;
    if (!IsShiftedMask(~_imm))
      return false;
    u32 clo = CountLeadingOnes(_imm);
    rot = 64 - clo;
    ones = clo + CountTrailingOnes(_imm) - (64 - size);
  }
  u32 r = (size - rot) & (size - 1);
  u64 nimms = ~(u64)(size - 1) << 1;
  nimms |= (ones - 1);
  u32 nbit = (u32)(((nimms >> 6) & 1) ^ 1);
  if (_width == 32 && nbit)
    return false;
  *_n = nbit;
  *_immr = r;
  *_imms = (u32)(nimms & 0x3F);
  return true;
}

u64 A64DecodeLogicalImm(u32 _n, u32 _immr, u32 _imms, u32 _width)
{
  NC_ASSERT(_width == 32 || _width == 64);
  u32 combined = (_n << 6) | (~_imms & 0x3F);
  NC_ASSERT(combined != 0);
  u32 len = 31 - NC_CLZ32_NONZERO(combined);
  u32 size = 1u << len;
  u32 r = _immr & (size - 1);
  u32 s = _imms & (size - 1);
  NC_ASSERT(s != size - 1);
  u64 pattern = (s + 1 == 64) ? ~(u64)0 : (((u64)1 << (s + 1)) - 1);
  if (r)
  {
    u64 emask = size == 64 ? ~(u64)0 : (((u64)1 << size) - 1);
    pattern = ((pattern >> r) | (pattern << (size - r))) & emask;
  }
  /* Replicate the element across the register by doubling: every shift
     * amount is an element size below the register width, so below 64. */
  for (u32 filled = size; filled < _width; filled *= 2)
    pattern |= pattern << filled;
  return _width == 64 ? pattern : (pattern & 0xFFFFFFFFu);
}

/* ---- floating-point immediates ------------------------------------------ */

double A64DecodeFpImm(u32 _imm8)
{
  u64 a = (_imm8 >> 7) & 1, b = (_imm8 >> 6) & 1, cd = (_imm8 >> 4) & 3, efgh = _imm8 & 0xF;
  u64 exp = b ? (0x3FCu | cd) : (0x400u | cd);
  u64 bits = (a << 63) | (exp << 52) | (efgh << 48);
  double d;
  memcpy(&d, &bits, sizeof d);
  return d;
}

bool A64EncodeFpImm(double _v, u32* _imm8)
{
  for (u32 i = 0; i < 256; i++)
  {
    if (A64DecodeFpImm(i) == _v && signbit(A64DecodeFpImm(i)) == signbit(_v))
    {
      *_imm8 = i;
      return true;
    }
  }
  return false;
}

/* ---- branch ranges ------------------------------------------------------ */

i64 A64BranchRange(A64Op _op)
{
  switch (A64_INSN_INFO[_op].form)
  {
  case A64FormB:
    return (i64)1 << 27;
  case A64FormBcond:
  case A64FormCb:
  case A64FormLdrLit:
    return (i64)1 << 20;
  case A64FormAdr:
    return _op == A64OpADR ? (i64)1 << 20 : (i64)1 << 32;
  case A64FormTb:
    return (i64)1 << 15;
  default:
    return 0;
  }
}

/* ---- encoder ------------------------------------------------------------ */

static u32 EncPcrel(const MirInst* _mi, i64 _pcrel, u32 _bits, u32 _shift)
{
  ENC_CHECK(_mi, (_pcrel & 3) == 0, "misaligned branch offset", _pcrel);
  i64 words = _pcrel >> 2;
  ENC_CHECK(_mi, NcFitsSigned(words, _bits), "branch offset out of range", _pcrel);
  return ((u32)words & ((1u << _bits) - 1)) << _shift;
}

u32 A64Encode(const MirInst* _mi, i64 _pcrel, u16* _relocType)
{
  NC_ASSERT(_mi->op < A64OpRealCount);
  const A64InsnInfo* ii = &A64_INSN_INFO[_mi->op];
  u32 w = ii->bits;
  u32 rd = _mi->rd & 31, rn = _mi->rn & 31, rm = _mi->rm & 31, ra = _mi->ra & 31;
  bool sym = _mi->tkind == A64TargetKindSym;
  *_relocType = 0;

  switch (ii->form)
  {
  case A64FormPseudo:
    NC_UNREACHABLE();

  case A64FormAddsubImm:
  case A64FormCmpImm:
  {
    u32 dst = ii->form == A64FormCmpImm ? 0 : rd;
    if (sym)
    {
      ENC_CHECK(_mi, _mi->imm == 0, "symbol addend must be zero", _mi->imm);
      *_relocType = _mi->smod == A64SymModSecrelHi12   ? IMAGE_REL_ARM64_SECREL_HIGH12A
                    : _mi->smod == A64SymModSecrelLo12 ? IMAGE_REL_ARM64_SECREL_LOW12A
                                                       : IMAGE_REL_ARM64_PAGEOFFSET_12A;
      /* SECREL_HIGH12A patches bits 12-23 of the section offset, so the
             * add must be the shifted form (lsl #12). link.exe rejects the
             * relocation on an unshifted add (LNK2040); lld-link silently
             * patches it, producing a wrong address. */
      u32 sh = _mi->smod == A64SymModSecrelHi12 ? 1u : 0u;
      return w | sh << 22 | rn << 5 | dst;
    }
    u64 imm = (u64)_mi->imm;
    u32 sh = 0;
    if (imm > 0xFFF)
    {
      ENC_CHECK(_mi, (imm & 0xFFF) == 0 && imm <= 0xFFF000, "add/sub immediate", _mi->imm);
      imm >>= 12;
      sh = 1;
    }
    return w | sh << 22 | (u32)imm << 10 | rn << 5 | dst;
  }
  case A64FormMovSp:
    return w | rn << 5 | rd;

  case A64FormLogicImm:
  case A64FormTstImm:
  {
    u32 width = (w >> 31) ? 64 : 32, n, immr, imms;
    ENC_CHECK(_mi, A64EncodeLogicalImm((u64)_mi->imm, width, &n, &immr, &imms), "not a logical immediate", _mi->imm);
    u32 dst = ii->form == A64FormTstImm ? 0 : rd;
    return w | n << 22 | immr << 16 | imms << 10 | rn << 5 | dst;
  }

  case A64FormMovw:
  {
    ENC_CHECK(_mi, _mi->imm >= 0 && _mi->imm <= 0xFFFF, "movw immediate", _mi->imm);
    u32 maxShift = (w >> 31) ? 48 : 16;
    ENC_CHECK(_mi, _mi->amount % 16 == 0 && _mi->amount <= maxShift, "movw shift", _mi->amount);
    return w | (u32)(_mi->amount / 16) << 21 | (u32)_mi->imm << 5 | rd;
  }

  case A64FormAdr:
  {
    if (sym)
    {
      ENC_CHECK(_mi, _mi->imm == 0, "symbol addend must be zero", _mi->imm);
      *_relocType = _mi->op == A64OpADRP ? IMAGE_REL_ARM64_PAGEBASE_REL21 : IMAGE_REL_ARM64_REL21;
      return w | rd;
    }
    i64 v = _pcrel;
    if (_mi->op == A64OpADRP)
    {
      ENC_CHECK(_mi, (v & 0xFFF) == 0, "adrp offset not page aligned", v);
      v >>= 12;
    }
    ENC_CHECK(_mi, NcFitsSigned(v, 21), "adr offset out of range", _pcrel);
    u32 u = (u32)v & 0x1FFFFF;
    return w | (u & 3) << 29 | (u >> 2) << 5 | rd;
  }

  case A64FormBitfield:
  {
    u32 lim = (w >> 31) ? 63 : 31;
    ENC_CHECK(_mi, _mi->imm >= 0 && _mi->imm <= lim, "bitfield immr", _mi->imm);
    ENC_CHECK(_mi, _mi->amount <= lim, "bitfield imms", _mi->amount);
    return w | (u32)_mi->imm << 16 | (u32)_mi->amount << 10 | rn << 5 | rd;
  }
  case A64FormLslImm:
  case A64FormLsrImm:
  {
    u32 width = (w >> 31) ? 64 : 32;
    ENC_CHECK(_mi, _mi->amount < width, "shift amount", _mi->amount);
    u32 s = _mi->amount, immr, imms;
    if (ii->form == A64FormLslImm)
    {
      immr = (width - s) & (width - 1);
      imms = width - 1 - s;
    }
    else
    {
      immr = s;
      imms = width - 1;
    }
    return w | immr << 16 | imms << 10 | rn << 5 | rd;
  }
  case A64FormExtend:
    return w | rn << 5 | rd;

  case A64FormLogicReg:
  case A64FormAddsubReg:
  case A64FormCmpReg:
  case A64FormMovReg:
  case A64FormNeg:
  {
    u32 width = (w >> 31) ? 64 : 32;
    ENC_CHECK(_mi, _mi->amount < width, "shift amount", _mi->amount);
    bool arith = ii->form == A64FormAddsubReg || ii->form == A64FormNeg ||
                 (ii->form == A64FormCmpReg && _mi->op != A64OpTSTXrs && _mi->op != A64OpTSTWrs);
    ENC_CHECK(_mi, !arith || _mi->shift != A64ShiftRor, "ror not allowed", _mi->shift);
    u32 dst = ii->form == A64FormCmpReg ? 0 : rd;
    u32 src1 = (ii->form == A64FormMovReg || ii->form == A64FormNeg) ? 0 : rn;
    return w | (u32)_mi->shift << 22 | rm << 16 | (u32)_mi->amount << 10 | src1 << 5 | dst;
  }
  case A64FormAddsubExt:
    ENC_CHECK(_mi, _mi->amount <= 4, "extend shift", _mi->amount);
    return w | rm << 16 | (u32)(_mi->shift & 7) << 13 | (u32)_mi->amount << 10 | rn << 5 | rd;

  case A64FormDp3:
    return w | rm << 16 | ra << 10 | rn << 5 | rd;
  case A64FormDp3Noa:
  case A64FormDp2:
  case A64FormFp2:
    return w | rm << 16 | rn << 5 | rd;
  case A64FormCsel:
    return w | rm << 16 | (u32)(_mi->shift & 15) << 12 | rn << 5 | rd;
  case A64FormCset:
  {
    ENC_CHECK(_mi, (_mi->shift & 14) != 14, "cset with al/nv", _mi->shift);
    return w | (u32)A64CondInvert((A64Cond)_mi->shift) << 12 | rd;
  }
  case A64FormDp1:
  case A64FormFp1:
    return w | rn << 5 | rd;

  case A64FormLdstUimm:
  {
    if (sym)
    {
      ENC_CHECK(_mi, _mi->imm == 0, "symbol addend must be zero", _mi->imm);
      *_relocType = _mi->smod == A64SymModSecrelLo12 ? IMAGE_REL_ARM64_SECREL_LOW12L : IMAGE_REL_ARM64_PAGEOFFSET_12L;
      return w | rn << 5 | rd;
    }
    i64 off = _mi->imm;
    ENC_CHECK(_mi, off >= 0 && (off & ((1 << ii->accessSizeLog2) - 1)) == 0, "scaled offset", off);
    ENC_CHECK(_mi, (off >> ii->accessSizeLog2) <= 0xFFF, "scaled offset range", off);
    return w | (u32)(off >> ii->accessSizeLog2) << 10 | rn << 5 | rd;
  }
  case A64FormLdstPre:
  case A64FormLdstPost:
  case A64FormLdstUnscaled:
    ENC_CHECK(_mi, NcFitsSigned(_mi->imm, 9), "9-bit offset", _mi->imm);
    return w | ((u32)_mi->imm & 0x1FF) << 12 | rn << 5 | rd;
  case A64FormLdstReg:
  {
    A64Extend ex = (A64Extend)_mi->shift;
    ENC_CHECK(_mi, ex == A64ExtendUxtw || ex == A64ExtendUxtx || ex == A64ExtendSxtw || ex == A64ExtendSxtx, "register-offset extend", ex);
    ENC_CHECK(_mi, _mi->amount <= 1, "register-offset S bit", _mi->amount);
    return w | rm << 16 | (u32)ex << 13 | (u32)_mi->amount << 12 | rn << 5 | rd;
  }
  case A64FormLdstPair:
  case A64FormLdstPairPre:
  case A64FormLdstPairPost:
  {
    i64 off = _mi->imm;
    i64 scale = (i64)1 << ii->accessSizeLog2;
    ENC_CHECK(_mi, off % scale == 0, "pair offset alignment", off);
    ENC_CHECK(_mi, NcFitsSigned(off / scale, 7), "pair offset range", off);
    return w | ((u32)(off / scale) & 0x7F) << 15 | ra << 10 | rn << 5 | rd;
  }
  case A64FormLdrLit:
    ENC_CHECK(_mi, !sym, "ldr literal to a symbol", 0);
    return w | EncPcrel(_mi, _pcrel, 19, 5) | rd;

  case A64FormB:
    if (sym)
    {
      *_relocType = IMAGE_REL_ARM64_BRANCH26;
      return w;
    }
    return w | EncPcrel(_mi, _pcrel, 26, 0);
  case A64FormBcond:
    if (sym)
    {
      *_relocType = IMAGE_REL_ARM64_BRANCH19;
      return w | (_mi->shift & 15);
    }
    return w | EncPcrel(_mi, _pcrel, 19, 5) | (_mi->shift & 15);
  case A64FormCb:
    if (sym)
    {
      *_relocType = IMAGE_REL_ARM64_BRANCH19;
      return w | rd;
    }
    return w | EncPcrel(_mi, _pcrel, 19, 5) | rd;
  case A64FormTb:
  {
    ENC_CHECK(_mi, _mi->imm >= 0 && _mi->imm < 64, "tbz bit", _mi->imm);
    u32 b = (u32)_mi->imm;
    w |= (b >> 5) << 31 | (b & 31) << 19 | rd;
    if (sym)
    {
      *_relocType = IMAGE_REL_ARM64_BRANCH14;
      return w;
    }
    return w | EncPcrel(_mi, _pcrel, 14, 5);
  }
  case A64FormBr:
  case A64FormRet:
    return w | rn << 5;
  case A64FormSys:
    return w;
  case A64FormBrk:
    ENC_CHECK(_mi, _mi->imm >= 0 && _mi->imm <= 0xFFFF, "brk immediate", _mi->imm);
    return w | (u32)_mi->imm << 5;
  case A64FormFcmp:
    return w | rm << 16 | rn << 5;
  case A64FormFmovImm:
    ENC_CHECK(_mi, _mi->imm >= 0 && _mi->imm <= 0xFF, "fp imm8", _mi->imm);
    return w | (u32)_mi->imm << 13 | rd;
  }
  NC_UNREACHABLE();
}
