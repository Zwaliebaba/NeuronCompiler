/* a64.h — ARM64 machine instructions (MIR), encoder and printer. */
#ifndef NC_A64_H
#define NC_A64_H

#include "../../core/core.h"

/* Register kinds used by arm64.def. Register 31 means SP for the *SP kinds
 * and the zero register otherwise. */
typedef enum A64Kind : u8 { K_, K_X, K_XSP, K_W, K_WSP, K_S, K_D, K_Q } A64Kind;

typedef enum A64Form : u8 {
    F_PSEUDO,
    F_ADDSUB_IMM, F_CMP_IMM, F_MOV_SP,
    F_LOGIC_IMM, F_TST_IMM,
    F_MOVW, F_ADR,
    F_BITFIELD, F_LSL_IMM, F_LSR_IMM, F_EXTEND,
    F_LOGIC_REG, F_MOV_REG, F_CMP_REG, F_ADDSUB_REG, F_NEG, F_ADDSUB_EXT,
    F_DP3, F_DP3_NOA, F_DP2, F_CSEL, F_CSET, F_DP1,
    F_LDST_UIMM, F_LDST_PRE, F_LDST_POST, F_LDST_UNSCALED, F_LDST_REG,
    F_LDST_PAIR, F_LDST_PAIR_PRE, F_LDST_PAIR_POST, F_LDR_LIT,
    F_B, F_BCOND, F_CB, F_TB, F_BR, F_RET,
    F_SYS, F_BRK,
    F_FP1, F_FP2, F_FCMP, F_FMOV_IMM,
} A64Form;

typedef enum A64Op : u16 {
#define INSN(id, mn, form, bits, kd, kn, km, sz) A64_##id,
#include "arm64.def"
#undef INSN
    A64_NUM_REAL,
    /* Pseudo-instructions, expanded before encoding. */
    A64_P_LABEL = A64_NUM_REAL, /* defines label `target` */
    A64_P_PROLOGUE,             /* expanded from the function's frame plan */
    A64_P_EPILOGUE,             /* epilogue + ret */
    A64_NUM_OPS,
} A64Op;

typedef struct A64InsnInfo {
    const char *mnemonic;
    u32 bits;
    A64Form form;
    A64Kind kd, kn, km;
    u8 sz;
} A64InsnInfo;

extern const A64InsnInfo a64_insn_info[A64_NUM_REAL];
extern const char *const a64_op_names[A64_NUM_OPS];

/* Registers: plain numbers 0..31; interpretation of 31 comes from the kind. */
enum {
    A64_FP = 29, A64_LR = 30, A64_ZR = 31, A64_SP = 31,
    A64_IP0 = 16, A64_IP1 = 17, A64_X18 = 18,
};

typedef enum A64Cond : u8 {
    CC_EQ, CC_NE, CC_HS, CC_LO, CC_MI, CC_PL, CC_VS, CC_VC,
    CC_HI, CC_LS, CC_GE, CC_LT, CC_GT, CC_LE, CC_AL, CC_NV,
} A64Cond;
static inline A64Cond a64_cond_invert(A64Cond c) { return (A64Cond)(c ^ 1); }

typedef enum A64Shift : u8 { SH_LSL, SH_LSR, SH_ASR, SH_ROR } A64Shift;
typedef enum A64Extend : u8 {
    EX_UXTB, EX_UXTH, EX_UXTW, EX_UXTX, EX_SXTB, EX_SXTH, EX_SXTW, EX_SXTX,
} A64Extend;

/* What MInst.target refers to. */
typedef enum A64TargetKind : u8 {
    TK_NONE,
    TK_LABEL,  /* function-local label id */
    TK_SYM,    /* module symbol id: becomes a relocation */
    TK_OFFSET, /* raw PC-relative byte offset in MInst.imm (tests, hand-written code) */
} A64TargetKind;

/* Symbol-operand modifier for TK_SYM on add/ldr/str. */
typedef enum A64SymMod : u8 {
    SM_DEFAULT,    /* adrp: page; add/ldr/str: :lo12:; b/bl: branch26 */
    SM_SECREL_HI12,
    SM_SECREL_LO12,
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
typedef struct MInst {
    u16 op;
    u8 rd, rn, rm, ra;
    u8 shift;
    u8 amount;
    u8 tkind;
    u8 smod;
    i64 imm;
    u32 target;
    u32 line;
} MInst;

/* Relocation produced by the encoder (COFF IMAGE_REL_ARM64_* type). */
typedef struct A64Reloc {
    u32 offset;
    u32 sym;
    u16 type;
} A64Reloc;

/* Encodes `mi`. `pcrel` is the resolved PC-relative byte offset for TK_LABEL
 * and TK_OFFSET targets. For TK_SYM targets the immediate is left zero and
 * `*reloc_type` receives the relocation to emit (0 if none). Encoding errors
 * (out-of-range immediates) are internal errors. */
u32 a64_encode(const MInst *mi, i64 pcrel, u16 *reloc_type);

/* Formats `mi` in LLVM/GNU assembler syntax (accepted by llvm-mc).
 * `symname` maps TK_SYM targets to names; labels print as ".L<n>". */
typedef const char *(*A64SymNameFn)(void *ctx, u32 sym);
void a64_format(char *out, size_t outsz, const MInst *mi, i64 pcrel, A64SymNameFn symname,
                void *ctx);

/* Logical (bitmask) immediates. */
bool a64_encode_logical_imm(u64 imm, u32 width, u32 *n, u32 *immr, u32 *imms);
u64  a64_decode_logical_imm(u32 n, u32 immr, u32 imms, u32 width);

/* 8-bit floating-point immediates for fmov. */
bool   a64_encode_fp_imm(double v, u32 *imm8);
double a64_decode_fp_imm(u32 imm8);

/* Branch reach (in bytes, symmetric) of a branch op with a label target. */
i64 a64_branch_range(A64Op op);

#endif
