/* a64_print.c — ARM64 instruction printer (LLVM/GNU syntax). The output is
 * fed back through llvm-mc by the differential encoding test, so it must be
 * exactly assemblable, not merely readable. */
#include "a64.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct Out
{
  char* p;
  size_t left;
} Out;

static void Put(Out* _o, const char* _fmt, ...)
{
  va_list ap;
  va_start(ap, _fmt);
  int n = vsnprintf(_o->p, _o->left, _fmt, ap);
  va_end(ap);
  if (n < 0)
    return;
  size_t adv = (size_t)n < _o->left ? (size_t)n : (_o->left ? _o->left - 1 : 0);
  _o->p += adv;
  _o->left -= adv;
}

static void Reg(Out* _o, A64Kind _k, u32 _r)
{
  _r &= 31;
  switch (_k)
  {
  case A64KindX:
    _r == 31 ? Put(_o, "xzr") : Put(_o, "x%u", _r);
    break;
  case A64KindXsp:
    _r == 31 ? Put(_o, "sp") : Put(_o, "x%u", _r);
    break;
  case A64KindW:
    _r == 31 ? Put(_o, "wzr") : Put(_o, "w%u", _r);
    break;
  case A64KindWsp:
    _r == 31 ? Put(_o, "wsp") : Put(_o, "w%u", _r);
    break;
  case A64KindS:
    Put(_o, "s%u", _r);
    break;
  case A64KindD:
    Put(_o, "d%u", _r);
    break;
  case A64KindQ:
    Put(_o, "q%u", _r);
    break;
  case A64KindNone:
    Put(_o, "<noreg>");
    break;
  }
}

static const char* const COND_NAMES[16] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
static const char* const SHIFT_NAMES[4] = {"lsl", "lsr", "asr", "ror"};
static const char* const EXT_NAMES[8] = {"uxtb", "uxth", "uxtw", "uxtx", "sxtb", "sxth", "sxtw", "sxtx"};

static void Target(Out* _o, const MirInst* _mi, i64 _pcrel, A64SymNameFn _symname, void* _ctx)
{
  switch (_mi->tkind)
  {
  case A64TargetKindLabel:
    Put(_o, ".L%u", _mi->target);
    break;
  case A64TargetKindSym:
    Put(_o, "%s", _symname ? _symname(_ctx, _mi->target) : "<sym>");
    break;
  case A64TargetKindOffset:
    Put(_o, "#%lld", (long long)_pcrel);
    break;
  default:
    Put(_o, "<notarget>");
    break;
  }
}

static void Lo12(Out* _o, const MirInst* _mi, A64SymNameFn _symname, void* _ctx)
{
  const char* mod = _mi->smod == A64SymModSecrelHi12 ? ":secrel_hi12:" : _mi->smod == A64SymModSecrelLo12 ? ":secrel_lo12:" : ":lo12:";
  Put(_o, "%s%s", mod, _symname ? _symname(_ctx, _mi->target) : "<sym>");
}

static void Shifted(Out* _o, const MirInst* _mi)
{
  if (_mi->amount || (_mi->shift & 3) != A64ShiftLsl)
    Put(_o, ", %s #%u", SHIFT_NAMES[_mi->shift & 3], _mi->amount);
}

void A64Format(char* _buf, size_t _bufsz, const MirInst* _mi, i64 _pcrel, A64SymNameFn _symname, void* _ctx)
{
  Out o = {_buf, _bufsz};
  if (_bufsz)
    _buf[0] = 0;
  if (_mi->op >= A64OpRealCount)
  {
    if (_mi->op == A64OpPseudoLabel)
      Put(&o, ".L%u:", _mi->target);
    else
      Put(&o, "// %s", A64_OP_NAMES[_mi->op]);
    return;
  }
  const A64InsnInfo* ii = &A64_INSN_INFO[_mi->op];
  bool sym = _mi->tkind == A64TargetKindSym;

  if (ii->form == A64FormBcond)
  {
    Put(&o, "b.%s\t", COND_NAMES[_mi->shift & 15]);
    Target(&o, _mi, _pcrel, _symname, _ctx);
    return;
  }
  Put(&o, "%s", ii->mnemonic);
  if (ii->form != A64FormSys && !(ii->form == A64FormRet && _mi->rn == A64_LR))
    Put(&o, "\t");

  switch (ii->form)
  {
  case A64FormPseudo:
    break;
  case A64FormAddsubImm:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    if (sym)
      Lo12(&o, _mi, _symname, _ctx);
    else if (_mi->imm > 0xFFF)
      Put(&o, "#%lld, lsl #12", (long long)(_mi->imm >> 12));
    else
      Put(&o, "#%lld", (long long)_mi->imm);
    break;
  case A64FormCmpImm:
    Reg(&o, ii->kn, _mi->rn);
    if (_mi->imm > 0xFFF)
      Put(&o, ", #%lld, lsl #12", (long long)(_mi->imm >> 12));
    else
      Put(&o, ", #%lld", (long long)_mi->imm);
    break;
  case A64FormMovSp:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    break;
  case A64FormLogicImm:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #0x%llx", (unsigned long long)_mi->imm);
    break;
  case A64FormTstImm:
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #0x%llx", (unsigned long long)_mi->imm);
    break;
  case A64FormMovw:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", #%lld", (long long)_mi->imm);
    if (_mi->amount)
      Put(&o, ", lsl #%u", _mi->amount);
    break;
  case A64FormAdr:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Target(&o, _mi, _pcrel, _symname, _ctx);
    break;
  case A64FormBitfield:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #%lld, #%u", (long long)_mi->imm, _mi->amount);
    break;
  case A64FormLslImm:
  case A64FormLsrImm:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #%u", _mi->amount);
    break;
  case A64FormExtend:
  case A64FormDp1:
  case A64FormFp1:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    break;
  case A64FormLogicReg:
  case A64FormAddsubReg:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    Shifted(&o, _mi);
    break;
  case A64FormMovReg:
  case A64FormNeg:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    Shifted(&o, _mi);
    break;
  case A64FormCmpReg:
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    Shifted(&o, _mi);
    break;
  case A64FormAddsubExt:
  {
    A64Extend ex = (A64Extend)(_mi->shift & 7);
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, (ex & 3) == 3 ? A64KindX : A64KindW, _mi->rm);
    bool spForm = (_mi->rd & 31) == 31 || (_mi->rn & 31) == 31;
    if (ex == A64ExtendUxtx && spForm)
    {
      if (_mi->amount)
        Put(&o, ", lsl #%u", _mi->amount);
    }
    else
    {
      Put(&o, ", %s", EXT_NAMES[ex]);
      if (_mi->amount)
        Put(&o, " #%u", _mi->amount);
    }
    break;
  }
  case A64FormDp3:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    Put(&o, ", ");
    Reg(&o, ii->kd, _mi->ra);
    break;
  case A64FormDp3Noa:
  case A64FormDp2:
  case A64FormFp2:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    break;
  case A64FormCsel:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    Put(&o, ", %s", COND_NAMES[_mi->shift & 15]);
    break;
  case A64FormCset:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", %s", COND_NAMES[_mi->shift & 15]);
    break;
  case A64FormLdstUimm:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    if (sym)
    {
      Put(&o, ", ");
      Lo12(&o, _mi, _symname, _ctx);
    }
    else if (_mi->imm)
      Put(&o, ", #%lld", (long long)_mi->imm);
    Put(&o, "]");
    break;
  case A64FormLdstPre:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #%lld]!", (long long)_mi->imm);
    break;
  case A64FormLdstPost:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, "], #%lld", (long long)_mi->imm);
    break;
  case A64FormLdstUnscaled:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", #%lld]", (long long)_mi->imm);
    break;
  case A64FormLdstReg:
  {
    A64Extend ex = (A64Extend)(_mi->shift & 7);
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, (ex & 3) == 3 ? A64KindX : A64KindW, _mi->rm);
    if (ex == A64ExtendUxtx)
    {
      if (_mi->amount)
        Put(&o, ", lsl #%u", ii->accessSizeLog2);
    }
    else
    {
      Put(&o, ", %s", EXT_NAMES[ex]);
      if (_mi->amount)
        Put(&o, " #%u", ii->accessSizeLog2);
    }
    Put(&o, "]");
    break;
  }
  case A64FormLdstPair:
  case A64FormLdstPairPre:
  case A64FormLdstPairPost:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Reg(&o, ii->kd, _mi->ra);
    Put(&o, ", [");
    Reg(&o, ii->kn, _mi->rn);
    if (ii->form == A64FormLdstPairPost)
      Put(&o, "], #%lld", (long long)_mi->imm);
    else if (ii->form == A64FormLdstPairPre)
      Put(&o, ", #%lld]!", (long long)_mi->imm);
    else if (_mi->imm)
      Put(&o, ", #%lld]", (long long)_mi->imm);
    else
      Put(&o, "]");
    break;
  case A64FormLdrLit:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Target(&o, _mi, _pcrel, _symname, _ctx);
    break;
  case A64FormB:
    Target(&o, _mi, _pcrel, _symname, _ctx);
    break;
  case A64FormBcond:
    break; /* handled above */
  case A64FormCb:
    Reg(&o, ii->kd, _mi->rd);
    Put(&o, ", ");
    Target(&o, _mi, _pcrel, _symname, _ctx);
    break;
  case A64FormTb:
    Reg(&o, _mi->imm < 32 ? A64KindW : A64KindX, _mi->rd);
    Put(&o, ", #%lld, ", (long long)_mi->imm);
    Target(&o, _mi, _pcrel, _symname, _ctx);
    break;
  case A64FormBr:
    Reg(&o, A64KindX, _mi->rn);
    break;
  case A64FormRet:
    if (_mi->rn != A64_LR)
      Reg(&o, A64KindX, _mi->rn);
    break;
  case A64FormSys:
    break;
  case A64FormBrk:
    Put(&o, "#0x%llx", (unsigned long long)_mi->imm);
    break;
  case A64FormFcmp:
    Reg(&o, ii->kn, _mi->rn);
    Put(&o, ", ");
    Reg(&o, ii->km, _mi->rm);
    break;
  case A64FormFmovImm:
  {
    Reg(&o, ii->kd, _mi->rd);
    double v = A64DecodeFpImm((u32)_mi->imm);
    char num[64];
    snprintf(num, sizeof num, "%.17g", v);
    bool hasPoint = strchr(num, '.') || strchr(num, 'e');
    Put(&o, ", #%s%s", num, hasPoint ? "" : ".0");
    break;
  }
  }
}
