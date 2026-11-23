/* a64_enc.c — ARM64 instruction encoder. */
#include "a64.h"

#include "../../obj/coff/coff.h"

#include <math.h>
#include <stdio.h>

const A64InsnInfo a64_insn_info[A64_NUM_REAL] = {
#define INSN(id, mn, form, bits, kd, kn, km, sz) [A64_##id] = {mn, bits, form, kd, kn, km, sz},
#include "arm64.def"
#undef INSN
};

const char *const a64_op_names[A64_NUM_OPS] = {
#define INSN(id, mn, form, bits, kd, kn, km, sz) [A64_##id] = #id,
#include "arm64.def"
#undef INSN
    [A64_P_LABEL] = "P_LABEL",
    [A64_P_PROLOGUE] = "P_PROLOGUE",
    [A64_P_EPILOGUE] = "P_EPILOGUE",
};

[[noreturn]] static void enc_fail(const MInst *mi, const char *what, i64 v) {
    nc_fatal("internal error: cannot encode %s: %s (value %lld)", a64_op_names[mi->op], what,
             (long long)v);
}

#define ENC_CHECK(mi, cond, what, v) \
    do { if (!(cond)) enc_fail((mi), (what), (i64)(v)); } while (0)

/* ---- logical immediates ------------------------------------------------- */

static bool is_mask(u64 v) { return v && ((v + 1) & v) == 0; }
static bool is_shifted_mask(u64 v) { return v && is_mask((v - 1) | v); }
static u32 count_trailing_ones(u64 v) { return nc_ctz64(~v); }
static u32 count_leading_ones(u64 v) { return nc_clz64(~v); }

bool a64_encode_logical_imm(u64 imm, u32 width, u32 *n, u32 *immr, u32 *imms) {
    NC_ASSERT(width == 32 || width == 64);
    if (width == 32) {
        if (imm >> 32) return false;
        if (imm == 0 || imm == 0xFFFFFFFFu) return false;
        imm |= imm << 32;
    } else if (imm == 0 || imm == ~(u64)0) {
        return false;
    }
    /* Find the smallest repeating element size. */
    u32 size = 64;
    do {
        size /= 2;
        u64 mask = ((u64)1 << size) - 1;
        if ((imm & mask) != ((imm >> size) & mask)) {
            size *= 2;
            break;
        }
    } while (size > 2);

    u64 mask = ~(u64)0 >> (64 - size);
    u32 rot, ones;
    imm &= mask;
    if (is_shifted_mask(imm)) {
        rot = nc_ctz64(imm);
        ones = count_trailing_ones(imm >> rot);
    } else {
        imm |= ~mask;
        if (!is_shifted_mask(~imm)) return false;
        u32 clo = count_leading_ones(imm);
        rot = 64 - clo;
        ones = clo + count_trailing_ones(imm) - (64 - size);
    }
    u32 r = (size - rot) & (size - 1);
    u64 nimms = ~(u64)(size - 1) << 1;
    nimms |= (ones - 1);
    u32 nbit = (u32)(((nimms >> 6) & 1) ^ 1);
    if (width == 32 && nbit) return false;
    *n = nbit;
    *immr = r;
    *imms = (u32)(nimms & 0x3F);
    return true;
}

u64 a64_decode_logical_imm(u32 n, u32 immr, u32 imms, u32 width) {
    u32 combined = (n << 6) | (~imms & 0x3F);
    NC_ASSERT(combined != 0);
    u32 len = 31 - (u32)__builtin_clz(combined);
    u32 size = 1u << len;
    u32 r = immr & (size - 1);
    u32 s = imms & (size - 1);
    NC_ASSERT(s != size - 1);
    u64 pattern = (s + 1 == 64) ? ~(u64)0 : (((u64)1 << (s + 1)) - 1);
    if (r) {
        u64 emask = size == 64 ? ~(u64)0 : (((u64)1 << size) - 1);
        pattern = ((pattern >> r) | (pattern << (size - r))) & emask;
    }
    u64 v = 0;
    for (u32 i = 0; i < width; i += size) v |= pattern << i;
    return width == 64 ? v : (v & 0xFFFFFFFFu);
}

/* ---- floating-point immediates ------------------------------------------ */

double a64_decode_fp_imm(u32 imm8) {
    u64 a = (imm8 >> 7) & 1, b = (imm8 >> 6) & 1, cd = (imm8 >> 4) & 3, efgh = imm8 & 0xF;
    u64 exp = b ? (0x3FCu | cd) : (0x400u | cd);
    u64 bits = (a << 63) | (exp << 52) | (efgh << 48);
    double d;
    memcpy(&d, &bits, sizeof d);
    return d;
}

bool a64_encode_fp_imm(double v, u32 *imm8) {
    for (u32 i = 0; i < 256; i++) {
        if (a64_decode_fp_imm(i) == v && signbit(a64_decode_fp_imm(i)) == signbit(v)) {
            *imm8 = i;
            return true;
        }
    }
    return false;
}

/* ---- branch ranges ------------------------------------------------------ */

i64 a64_branch_range(A64Op op) {
    switch (a64_insn_info[op].form) {
    case F_B: return (i64)1 << 27;
    case F_BCOND:
    case F_CB:
    case F_LDR_LIT: return (i64)1 << 20;
    case F_ADR: return op == A64_ADR ? (i64)1 << 20 : (i64)1 << 32;
    case F_TB: return (i64)1 << 15;
    default: return 0;
    }
}

/* ---- encoder ------------------------------------------------------------ */

static u32 enc_pcrel(const MInst *mi, i64 pcrel, u32 bits, u32 shift) {
    ENC_CHECK(mi, (pcrel & 3) == 0, "misaligned branch offset", pcrel);
    i64 words = pcrel >> 2;
    ENC_CHECK(mi, nc_fits_signed(words, bits), "branch offset out of range", pcrel);
    return ((u32)words & ((1u << bits) - 1)) << shift;
}

u32 a64_encode(const MInst *mi, i64 pcrel, u16 *reloc_type) {
    NC_ASSERT(mi->op < A64_NUM_REAL);
    const A64InsnInfo *ii = &a64_insn_info[mi->op];
    u32 w = ii->bits;
    u32 rd = mi->rd & 31, rn = mi->rn & 31, rm = mi->rm & 31, ra = mi->ra & 31;
    bool sym = mi->tkind == TK_SYM;
    *reloc_type = 0;

    switch (ii->form) {
    case F_PSEUDO: NC_UNREACHABLE();

    case F_ADDSUB_IMM:
    case F_CMP_IMM: {
        u32 dst = ii->form == F_CMP_IMM ? 0 : rd;
        if (sym) {
            ENC_CHECK(mi, mi->imm == 0, "symbol addend must be zero", mi->imm);
            *reloc_type = mi->smod == SM_SECREL_HI12   ? IMAGE_REL_ARM64_SECREL_HIGH12A
                          : mi->smod == SM_SECREL_LO12 ? IMAGE_REL_ARM64_SECREL_LOW12A
                                                       : IMAGE_REL_ARM64_PAGEOFFSET_12A;
            return w | rn << 5 | dst;
        }
        u64 imm = (u64)mi->imm;
        u32 sh = 0;
        if (imm > 0xFFF) {
            ENC_CHECK(mi, (imm & 0xFFF) == 0 && imm <= 0xFFF000, "add/sub immediate", mi->imm);
            imm >>= 12;
            sh = 1;
        }
        return w | sh << 22 | (u32)imm << 10 | rn << 5 | dst;
    }
    case F_MOV_SP: return w | rn << 5 | rd;

    case F_LOGIC_IMM:
    case F_TST_IMM: {
        u32 width = (w >> 31) ? 64 : 32, n, immr, imms;
        ENC_CHECK(mi, a64_encode_logical_imm((u64)mi->imm, width, &n, &immr, &imms),
                  "not a logical immediate", mi->imm);
        u32 dst = ii->form == F_TST_IMM ? 0 : rd;
        return w | n << 22 | immr << 16 | imms << 10 | rn << 5 | dst;
    }

    case F_MOVW: {
        ENC_CHECK(mi, mi->imm >= 0 && mi->imm <= 0xFFFF, "movw immediate", mi->imm);
        u32 max_shift = (w >> 31) ? 48 : 16;
        ENC_CHECK(mi, mi->amount % 16 == 0 && mi->amount <= max_shift, "movw shift", mi->amount);
        return w | (u32)(mi->amount / 16) << 21 | (u32)mi->imm << 5 | rd;
    }

    case F_ADR: {
        if (sym) {
            ENC_CHECK(mi, mi->imm == 0, "symbol addend must be zero", mi->imm);
            *reloc_type =
                mi->op == A64_ADRP ? IMAGE_REL_ARM64_PAGEBASE_REL21 : IMAGE_REL_ARM64_REL21;
            return w | rd;
        }
        i64 v = pcrel;
        if (mi->op == A64_ADRP) {
            ENC_CHECK(mi, (v & 0xFFF) == 0, "adrp offset not page aligned", v);
            v >>= 12;
        }
        ENC_CHECK(mi, nc_fits_signed(v, 21), "adr offset out of range", pcrel);
        u32 u = (u32)v & 0x1FFFFF;
        return w | (u & 3) << 29 | (u >> 2) << 5 | rd;
    }

    case F_BITFIELD: {
        u32 lim = (w >> 31) ? 63 : 31;
        ENC_CHECK(mi, mi->imm >= 0 && mi->imm <= lim, "bitfield immr", mi->imm);
        ENC_CHECK(mi, mi->amount <= lim, "bitfield imms", mi->amount);
        return w | (u32)mi->imm << 16 | (u32)mi->amount << 10 | rn << 5 | rd;
    }
    case F_LSL_IMM:
    case F_LSR_IMM: {
        u32 width = (w >> 31) ? 64 : 32;
        ENC_CHECK(mi, mi->amount < width, "shift amount", mi->amount);
        u32 s = mi->amount, immr, imms;
        if (ii->form == F_LSL_IMM) {
            immr = (width - s) & (width - 1);
            imms = width - 1 - s;
        } else {
            immr = s;
            imms = width - 1;
        }
        return w | immr << 16 | imms << 10 | rn << 5 | rd;
    }
    case F_EXTEND: return w | rn << 5 | rd;

    case F_LOGIC_REG:
    case F_ADDSUB_REG:
    case F_CMP_REG:
    case F_MOV_REG:
    case F_NEG: {
        u32 width = (w >> 31) ? 64 : 32;
        ENC_CHECK(mi, mi->amount < width, "shift amount", mi->amount);
        bool arith = ii->form == F_ADDSUB_REG || ii->form == F_NEG ||
                     (ii->form == F_CMP_REG && mi->op != A64_TSTXrs && mi->op != A64_TSTWrs);
        ENC_CHECK(mi, !arith || mi->shift != SH_ROR, "ror not allowed", mi->shift);
        u32 dst = ii->form == F_CMP_REG ? 0 : rd;
        u32 src1 = (ii->form == F_MOV_REG || ii->form == F_NEG) ? 0 : rn;
        return w | (u32)mi->shift << 22 | rm << 16 | (u32)mi->amount << 10 | src1 << 5 | dst;
    }
    case F_ADDSUB_EXT:
        ENC_CHECK(mi, mi->amount <= 4, "extend shift", mi->amount);
        return w | rm << 16 | (u32)(mi->shift & 7) << 13 | (u32)mi->amount << 10 | rn << 5 | rd;

    case F_DP3: return w | rm << 16 | ra << 10 | rn << 5 | rd;
    case F_DP3_NOA:
    case F_DP2:
    case F_FP2: return w | rm << 16 | rn << 5 | rd;
    case F_CSEL: return w | rm << 16 | (u32)(mi->shift & 15) << 12 | rn << 5 | rd;
    case F_CSET: {
        ENC_CHECK(mi, (mi->shift & 14) != 14, "cset with al/nv", mi->shift);
        return w | (u32)a64_cond_invert((A64Cond)mi->shift) << 12 | rd;
    }
    case F_DP1:
    case F_FP1: return w | rn << 5 | rd;

    case F_LDST_UIMM: {
        if (sym) {
            ENC_CHECK(mi, mi->imm == 0, "symbol addend must be zero", mi->imm);
            *reloc_type = mi->smod == SM_SECREL_LO12 ? IMAGE_REL_ARM64_SECREL_LOW12L
                                                     : IMAGE_REL_ARM64_PAGEOFFSET_12L;
            return w | rn << 5 | rd;
        }
        i64 off = mi->imm;
        ENC_CHECK(mi, off >= 0 && (off & ((1 << ii->sz) - 1)) == 0, "scaled offset", off);
        ENC_CHECK(mi, (off >> ii->sz) <= 0xFFF, "scaled offset range", off);
        return w | (u32)(off >> ii->sz) << 10 | rn << 5 | rd;
    }
    case F_LDST_PRE:
    case F_LDST_POST:
    case F_LDST_UNSCALED:
        ENC_CHECK(mi, nc_fits_signed(mi->imm, 9), "9-bit offset", mi->imm);
        return w | ((u32)mi->imm & 0x1FF) << 12 | rn << 5 | rd;
    case F_LDST_REG: {
        A64Extend ex = (A64Extend)mi->shift;
        ENC_CHECK(mi, ex == EX_UXTW || ex == EX_UXTX || ex == EX_SXTW || ex == EX_SXTX,
                  "register-offset extend", ex);
        ENC_CHECK(mi, mi->amount <= 1, "register-offset S bit", mi->amount);
        return w | rm << 16 | (u32)ex << 13 | (u32)mi->amount << 12 | rn << 5 | rd;
    }
    case F_LDST_PAIR:
    case F_LDST_PAIR_PRE:
    case F_LDST_PAIR_POST: {
        i64 off = mi->imm;
        i64 scale = (i64)1 << ii->sz;
        ENC_CHECK(mi, off % scale == 0, "pair offset alignment", off);
        ENC_CHECK(mi, nc_fits_signed(off / scale, 7), "pair offset range", off);
        return w | ((u32)(off / scale) & 0x7F) << 15 | ra << 10 | rn << 5 | rd;
    }
    case F_LDR_LIT:
        ENC_CHECK(mi, !sym, "ldr literal to a symbol", 0);
        return w | enc_pcrel(mi, pcrel, 19, 5) | rd;

    case F_B:
        if (sym) { *reloc_type = IMAGE_REL_ARM64_BRANCH26; return w; }
        return w | enc_pcrel(mi, pcrel, 26, 0);
    case F_BCOND:
        if (sym) { *reloc_type = IMAGE_REL_ARM64_BRANCH19; return w | (mi->shift & 15); }
        return w | enc_pcrel(mi, pcrel, 19, 5) | (mi->shift & 15);
    case F_CB:
        if (sym) { *reloc_type = IMAGE_REL_ARM64_BRANCH19; return w | rd; }
        return w | enc_pcrel(mi, pcrel, 19, 5) | rd;
    case F_TB: {
        ENC_CHECK(mi, mi->imm >= 0 && mi->imm < 64, "tbz bit", mi->imm);
        u32 b = (u32)mi->imm;
        w |= (b >> 5) << 31 | (b & 31) << 19 | rd;
        if (sym) { *reloc_type = IMAGE_REL_ARM64_BRANCH14; return w; }
        return w | enc_pcrel(mi, pcrel, 14, 5);
    }
    case F_BR:
    case F_RET: return w | rn << 5;
    case F_SYS: return w;
    case F_BRK:
        ENC_CHECK(mi, mi->imm >= 0 && mi->imm <= 0xFFFF, "brk immediate", mi->imm);
        return w | (u32)mi->imm << 5;
    case F_FCMP: return w | rm << 16 | rn << 5;
    case F_FMOV_IMM:
        ENC_CHECK(mi, mi->imm >= 0 && mi->imm <= 0xFF, "fp imm8", mi->imm);
        return w | (u32)mi->imm << 13 | rd;
    }
    NC_UNREACHABLE();
}
