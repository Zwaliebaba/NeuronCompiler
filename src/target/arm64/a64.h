/* a64.h — ARM64 machine instructions (MIR), encoder and printer. */
#ifndef NC_A64_H
#define NC_A64_H

#include "../../core/core.h"

/* Register kinds used by arm64.def. Register 31 means SP for the *SP kinds
 * and the zero register otherwise. */
typedef enum A64Kind : u8
{
  A64KindNone,
  A64KindX,
  A64KindXsp,
  A64KindW,
  A64KindWsp,
  A64KindS,
  A64KindD,
  A64KindQ
} A64Kind;

typedef enum A64Form : u8
{
  A64FormPseudo,
  A64FormAddsubImm,
  A64FormCmpImm,
  A64FormMovSp,
  A64FormLogicImm,
  A64FormTstImm,
  A64FormMovw,
  A64FormAdr,
  A64FormBitfield,
  A64FormLslImm,
  A64FormLsrImm,
  A64FormExtend,
  A64FormLogicReg,
  A64FormMovReg,
  A64FormCmpReg,
  A64FormAddsubReg,
  A64FormNeg,
  A64FormAddsubExt,
  A64FormDp3,
  A64FormDp3Noa,
  A64FormDp2,
  A64FormCsel,
  A64FormCset,
  A64FormDp1,
  A64FormLdstUimm,
  A64FormLdstPre,
  A64FormLdstPost,
  A64FormLdstUnscaled,
  A64FormLdstReg,
  A64FormLdstPair,
  A64FormLdstPairPre,
  A64FormLdstPairPost,
  A64FormLdrLit,
  A64FormB,
  A64FormBcond,
  A64FormCb,
  A64FormTb,
  A64FormBr,
  A64FormRet,
  A64FormSys,
  A64FormBrk,
  A64FormFp1,
  A64FormFp2,
  A64FormFcmp,
  A64FormFmovImm,
} A64Form;

typedef enum A64Op : u16
{
#define INSN(id, mn, form, bits, kd, kn, km, sz) A64Op##id,
#include "arm64.def"
#undef INSN
  A64OpRealCount,
  /* Pseudo-instructions, expanded before encoding. */
  A64OpPseudoLabel = A64OpRealCount, /* defines label `target` */
  A64OpPseudoPrologue,               /* expanded from the function's frame plan */
  A64OpPseudoEpilogue,               /* epilogue + ret */
  A64OpCount,
} A64Op;

typedef struct A64InsnInfo
{
  const char* mnemonic;
  u32 bits;
  A64Form form;
  A64Kind kd, kn, km;
  u8 accessSizeLog2; /* log2 of the memory access size */
} A64InsnInfo;

extern const A64InsnInfo A64_INSN_INFO[A64OpRealCount];
extern const char* const A64_OP_NAMES[A64OpCount];

/* Registers: plain numbers 0..31; interpretation of 31 comes from the kind. */
enum
{
  A64_FP = 29,
  A64_LR = 30,
  A64_ZR = 31,
  A64_SP = 31,
  A64_IP0 = 16,
  A64_IP1 = 17,
  A64_X18 = 18,
};

typedef enum A64Cond : u8
{
  A64CondEq,
  A64CondNe,
  A64CondHs,
  A64CondLo,
  A64CondMi,
  A64CondPl,
  A64CondVs,
  A64CondVc,
  A64CondHi,
  A64CondLs,
  A64CondGe,
  A64CondLt,
  A64CondGt,
  A64CondLe,
  A64CondAl,
  A64CondNv,
} A64Cond;
static inline A64Cond A64CondInvert(A64Cond _c)
{
  return (A64Cond)(_c ^ 1);
}

typedef enum A64Shift : u8
{
  A64ShiftLsl,
  A64ShiftLsr,
  A64ShiftAsr,
  A64ShiftRor
} A64Shift;
typedef enum A64Extend : u8
{
  A64ExtendUxtb,
  A64ExtendUxth,
  A64ExtendUxtw,
  A64ExtendUxtx,
  A64ExtendSxtb,
  A64ExtendSxth,
  A64ExtendSxtw,
  A64ExtendSxtx,
} A64Extend;

/* What MirInst.target refers to. */
typedef enum A64TargetKind : u8
{
  A64TargetKindNone,
  A64TargetKindLabel,  /* function-local label id */
  A64TargetKindSym,    /* module symbol id: becomes a relocation */
  A64TargetKindOffset, /* raw PC-relative byte offset in MirInst.imm (tests, hand-written code) */
} A64TargetKind;

/* Symbol-operand modifier for A64TargetKindSym on add/ldr/str. */
typedef enum A64SymMod : u8
{
  A64SymModDefault, /* adrp: page; add/ldr/str: :lo12:; b/bl: branch26 */
  A64SymModSecrelHi12,
  A64SymModSecrelLo12,
} A64SymMod;

/* One machine instruction. Field meaning depends on the form:
 *   rd    destination / Rt (loads, stores, cbz, tbz)
 *   rn    first source / base register
 *   rm    second source / index register
 *   ra    third source (madd) / Rt2 (pairs)
 *   shift shift type, extend option, condition (csel, b.cond), or S bit
 *   amount shift amount, movw shift, bitfield imms
 *   imm   immediate, memory offset, bitfield immr, tbz bit, label offset
 */
typedef struct MirInst
{
  u16 op;
  u8 rd, rn, rm, ra;
  u8 shift;
  u8 amount;
  u8 tkind;
  u8 smod;
  i64 imm;
  u32 target;
  u32 line;
} MirInst;

/* Relocation produced by the encoder (COFF IMAGE_REL_ARM64_* type). */
typedef struct A64Reloc
{
  u32 offset;
  u32 sym;
  u16 type;
} A64Reloc;

/* Encodes `mi`. `pcrel` is the resolved PC-relative byte offset for A64TargetKindLabel
 * and A64TargetKindOffset targets. For A64TargetKindSym targets the immediate is left zero and
 * `*reloc_type` receives the relocation to emit (0 if none). Encoding errors
 * (out-of-range immediates) are internal errors. */
u32 A64Encode(const MirInst* _mi, i64 _pcrel, u16* _relocType);

/* Formats `mi` in LLVM/GNU assembler syntax (accepted by llvm-mc).
 * `symname` maps A64TargetKindSym targets to names; labels print as ".L<n>". */
typedef const char* (*A64SymNameFn)(void* _ctx, u32 _sym);
void A64Format(char* _out, size_t _outsz, const MirInst* _mi, i64 _pcrel, A64SymNameFn _symname, void* _ctx);

/* Logical (bitmask) immediates. */
bool A64EncodeLogicalImm(u64 _imm, u32 _width, u32* _n, u32* _immr, u32* _imms);
u64 A64DecodeLogicalImm(u32 _n, u32 _immr, u32 _imms, u32 _width);

/* 8-bit floating-point immediates for fmov. */
bool A64EncodeFpImm(double _v, u32* _imm8);
double A64DecodeFpImm(u32 _imm8);

/* Branch reach (in bytes, symmetric) of a branch op with a label target. */
i64 A64BranchRange(A64Op _op);

#endif
