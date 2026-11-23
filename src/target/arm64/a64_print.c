/* a64_print.c — ARM64 instruction printer (LLVM/GNU syntax). The output is
 * fed back through llvm-mc by the differential encoding test, so it must be
 * exactly assemblable, not merely readable. */
#include "a64.h"

#include <stdarg.h>
#include <stdio.h>

typedef struct Out {
    char *p;
    size_t left;
} Out;

static void put(Out *o, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(o->p, o->left, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    size_t adv = (size_t)n < o->left ? (size_t)n : (o->left ? o->left - 1 : 0);
    o->p += adv;
    o->left -= adv;
}

static void reg(Out *o, A64Kind k, u32 r) {
    r &= 31;
    switch (k) {
    case K_X: r == 31 ? put(o, "xzr") : put(o, "x%u", r); break;
    case K_XSP: r == 31 ? put(o, "sp") : put(o, "x%u", r); break;
    case K_W: r == 31 ? put(o, "wzr") : put(o, "w%u", r); break;
    case K_WSP: r == 31 ? put(o, "wsp") : put(o, "w%u", r); break;
    case K_S: put(o, "s%u", r); break;
    case K_D: put(o, "d%u", r); break;
    case K_Q: put(o, "q%u", r); break;
    case K_: put(o, "<noreg>"); break;
    }
}

static const char *const cond_names[16] = {"eq", "ne", "hs", "lo", "mi", "pl", "vs", "vc",
                                           "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
static const char *const shift_names[4] = {"lsl", "lsr", "asr", "ror"};
static const char *const ext_names[8] = {"uxtb", "uxth", "uxtw", "uxtx",
                                         "sxtb", "sxth", "sxtw", "sxtx"};

static void target(Out *o, const MInst *mi, i64 pcrel, A64SymNameFn symname, void *ctx) {
    switch (mi->tkind) {
    case TK_LABEL: put(o, ".L%u", mi->target); break;
    case TK_SYM: put(o, "%s", symname ? symname(ctx, mi->target) : "<sym>"); break;
    case TK_OFFSET: put(o, "#%lld", (long long)pcrel); break;
    default: put(o, "<notarget>"); break;
    }
}

static void lo12(Out *o, const MInst *mi, A64SymNameFn symname, void *ctx) {
    const char *mod = mi->smod == SM_SECREL_HI12   ? ":secrel_hi12:"
                      : mi->smod == SM_SECREL_LO12 ? ":secrel_lo12:"
                                                   : ":lo12:";
    put(o, "%s%s", mod, symname ? symname(ctx, mi->target) : "<sym>");
}

static void shifted(Out *o, const MInst *mi) {
    if (mi->amount || (mi->shift & 3) != SH_LSL)
        put(o, ", %s #%u", shift_names[mi->shift & 3], mi->amount);
}

void a64_format(char *buf, size_t bufsz, const MInst *mi, i64 pcrel, A64SymNameFn symname,
                void *ctx) {
    Out o = {buf, bufsz};
    if (bufsz) buf[0] = 0;
    if (mi->op >= A64_NUM_REAL) {
        if (mi->op == A64_P_LABEL) put(&o, ".L%u:", mi->target);
        else put(&o, "// %s", a64_op_names[mi->op]);
        return;
    }
    const A64InsnInfo *ii = &a64_insn_info[mi->op];
    bool sym = mi->tkind == TK_SYM;

    if (ii->form == F_BCOND) {
        put(&o, "b.%s\t", cond_names[mi->shift & 15]);
        target(&o, mi, pcrel, symname, ctx);
        return;
    }
    put(&o, "%s", ii->mnemonic);
    if (ii->form != F_SYS && !(ii->form == F_RET && mi->rn == A64_LR)) put(&o, "\t");

    switch (ii->form) {
    case F_PSEUDO: break;
    case F_ADDSUB_IMM:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        if (sym) lo12(&o, mi, symname, ctx);
        else if (mi->imm > 0xFFF) put(&o, "#%lld, lsl #12", (long long)(mi->imm >> 12));
        else put(&o, "#%lld", (long long)mi->imm);
        break;
    case F_CMP_IMM:
        reg(&o, ii->kn, mi->rn);
        if (mi->imm > 0xFFF) put(&o, ", #%lld, lsl #12", (long long)(mi->imm >> 12));
        else put(&o, ", #%lld", (long long)mi->imm);
        break;
    case F_MOV_SP: reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); break;
    case F_LOGIC_IMM:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn);
        put(&o, ", #0x%llx", (unsigned long long)mi->imm);
        break;
    case F_TST_IMM:
        reg(&o, ii->kn, mi->rn); put(&o, ", #0x%llx", (unsigned long long)mi->imm);
        break;
    case F_MOVW:
        reg(&o, ii->kd, mi->rd); put(&o, ", #%lld", (long long)mi->imm);
        if (mi->amount) put(&o, ", lsl #%u", mi->amount);
        break;
    case F_ADR:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); target(&o, mi, pcrel, symname, ctx);
        break;
    case F_BITFIELD:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn);
        put(&o, ", #%lld, #%u", (long long)mi->imm, mi->amount);
        break;
    case F_LSL_IMM:
    case F_LSR_IMM:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn);
        put(&o, ", #%u", mi->amount);
        break;
    case F_EXTEND:
    case F_DP1:
    case F_FP1: reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); break;
    case F_LOGIC_REG:
    case F_ADDSUB_REG:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, ii->km, mi->rm); shifted(&o, mi);
        break;
    case F_MOV_REG:
    case F_NEG:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->km, mi->rm); shifted(&o, mi);
        break;
    case F_CMP_REG:
        reg(&o, ii->kn, mi->rn); put(&o, ", "); reg(&o, ii->km, mi->rm); shifted(&o, mi);
        break;
    case F_ADDSUB_EXT: {
        A64Extend ex = (A64Extend)(mi->shift & 7);
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, (ex & 3) == 3 ? K_X : K_W, mi->rm);
        bool sp_form = (mi->rd & 31) == 31 || (mi->rn & 31) == 31;
        if (ex == EX_UXTX && sp_form) {
            if (mi->amount) put(&o, ", lsl #%u", mi->amount);
        } else {
            put(&o, ", %s", ext_names[ex]);
            if (mi->amount) put(&o, " #%u", mi->amount);
        }
        break;
    }
    case F_DP3:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, ii->km, mi->rm); put(&o, ", "); reg(&o, ii->kd, mi->ra);
        break;
    case F_DP3_NOA:
    case F_DP2:
    case F_FP2:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, ii->km, mi->rm);
        break;
    case F_CSEL:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, ii->km, mi->rm); put(&o, ", %s", cond_names[mi->shift & 15]);
        break;
    case F_CSET: reg(&o, ii->kd, mi->rd); put(&o, ", %s", cond_names[mi->shift & 15]); break;
    case F_LDST_UIMM:
        reg(&o, ii->kd, mi->rd); put(&o, ", ["); reg(&o, ii->kn, mi->rn);
        if (sym) { put(&o, ", "); lo12(&o, mi, symname, ctx); }
        else if (mi->imm) put(&o, ", #%lld", (long long)mi->imm);
        put(&o, "]");
        break;
    case F_LDST_PRE:
        reg(&o, ii->kd, mi->rd); put(&o, ", ["); reg(&o, ii->kn, mi->rn);
        put(&o, ", #%lld]!", (long long)mi->imm);
        break;
    case F_LDST_POST:
        reg(&o, ii->kd, mi->rd); put(&o, ", ["); reg(&o, ii->kn, mi->rn);
        put(&o, "], #%lld", (long long)mi->imm);
        break;
    case F_LDST_UNSCALED:
        reg(&o, ii->kd, mi->rd); put(&o, ", ["); reg(&o, ii->kn, mi->rn);
        put(&o, ", #%lld]", (long long)mi->imm);
        break;
    case F_LDST_REG: {
        A64Extend ex = (A64Extend)(mi->shift & 7);
        reg(&o, ii->kd, mi->rd); put(&o, ", ["); reg(&o, ii->kn, mi->rn); put(&o, ", ");
        reg(&o, (ex & 3) == 3 ? K_X : K_W, mi->rm);
        if (ex == EX_UXTX) {
            if (mi->amount) put(&o, ", lsl #%u", ii->sz);
        } else {
            put(&o, ", %s", ext_names[ex]);
            if (mi->amount) put(&o, " #%u", ii->sz);
        }
        put(&o, "]");
        break;
    }
    case F_LDST_PAIR:
    case F_LDST_PAIR_PRE:
    case F_LDST_PAIR_POST:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); reg(&o, ii->kd, mi->ra); put(&o, ", [");
        reg(&o, ii->kn, mi->rn);
        if (ii->form == F_LDST_PAIR_POST) put(&o, "], #%lld", (long long)mi->imm);
        else if (ii->form == F_LDST_PAIR_PRE) put(&o, ", #%lld]!", (long long)mi->imm);
        else if (mi->imm) put(&o, ", #%lld]", (long long)mi->imm);
        else put(&o, "]");
        break;
    case F_LDR_LIT:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); target(&o, mi, pcrel, symname, ctx);
        break;
    case F_B: target(&o, mi, pcrel, symname, ctx); break;
    case F_BCOND: break; /* handled above */
    case F_CB:
        reg(&o, ii->kd, mi->rd); put(&o, ", "); target(&o, mi, pcrel, symname, ctx);
        break;
    case F_TB:
        reg(&o, mi->imm < 32 ? K_W : K_X, mi->rd);
        put(&o, ", #%lld, ", (long long)mi->imm);
        target(&o, mi, pcrel, symname, ctx);
        break;
    case F_BR: reg(&o, K_X, mi->rn); break;
    case F_RET: if (mi->rn != A64_LR) reg(&o, K_X, mi->rn); break;
    case F_SYS: break;
    case F_BRK: put(&o, "#0x%llx", (unsigned long long)mi->imm); break;
    case F_FCMP: reg(&o, ii->kn, mi->rn); put(&o, ", "); reg(&o, ii->km, mi->rm); break;
    case F_FMOV_IMM: {
        reg(&o, ii->kd, mi->rd);
        double v = a64_decode_fp_imm((u32)mi->imm);
        char num[64];
        snprintf(num, sizeof num, "%.17g", v);
        bool has_point = strchr(num, '.') || strchr(num, 'e');
        put(&o, ", #%s%s", num, has_point ? "" : ".0");
        break;
    }
    }
}
