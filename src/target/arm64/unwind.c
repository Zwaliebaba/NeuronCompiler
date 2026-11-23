/* unwind.c — frame plans, prologues/epilogues, .pdata/.xdata and the
 * unwind verifier. Reference: Microsoft Learn, "ARM64 exception handling". */
#include "unwind.h"

#include <stdarg.h>
#include <stdio.h>

/* ======================================================================== */
/* Frame plans                                                              */
/* ======================================================================== */

static void add_step(FramePlan *p, FrameStepKind k, u32 reg, u32 off) {
    NC_ASSERT(p->nsteps < FRAME_MAX_STEPS);
    p->steps[p->nsteps++] = (FrameStep){k, (u8)reg, off};
}

void frame_plan(const FrameDesc *d, FramePlan *p) {
    memset(p, 0, sizeof *p);
    NC_ASSERT(d->n_gpr <= 10 && d->n_fpr <= 8);
    p->n_gpr = d->n_gpr;
    p->n_fpr = d->n_fpr;
    p->home_args = d->home_args;
    p->dynamic = d->dynamic;
    u32 locals = NC_ALIGN_UP(d->locals, 16u);
    u32 out = NC_ALIGN_UP(d->outgoing, 16u);
    p->locals_size = locals;

    if (d->frameless) {
        NC_ASSERT(!d->n_gpr && !d->n_fpr && !d->home_args && !d->dynamic && !locals && !out);
        p->shape = SHAPE_NONE;
        return;
    }

    u32 intsz = 8u * d->n_gpr, fpsz = 8u * d->n_fpr;
    u32 savsz = NC_ALIGN_UP(intsz + fpsz + (d->home_args ? 64u : 0u), 16u);
    p->save_size = savsz;

    /* Register save area, in the canonical order: int pairs, FP pairs, home. */
    bool first = true;
    if (d->n_gpr == 0 && d->n_fpr == 0 && savsz) {
        add_step(p, FS_ALLOC, 0, savsz);
        first = false;
    }
    for (u32 i = 0; i < d->n_gpr; i += 2, first = false) {
        bool pair = i + 1 < d->n_gpr;
        if (first) add_step(p, pair ? FS_STP_X_PRE : FS_STR_X_PRE, 19 + i, savsz);
        else add_step(p, pair ? FS_STP_X : FS_STR_X, 19 + i, 8 * i);
    }
    for (u32 j = 0; j < d->n_fpr; j += 2, first = false) {
        bool pair = j + 1 < d->n_fpr;
        if (first) add_step(p, pair ? FS_STP_D_PRE : FS_STR_D_PRE, 8 + j, savsz);
        else add_step(p, pair ? FS_STP_D : FS_STR_D, 8 + j, intsz + 8 * j);
    }
    if (d->home_args) {
        /* Home area at the top of the save area, contiguous with the incoming
         * stack arguments so va_list can walk both. */
        for (u32 k = 0; k < 4; k++) add_step(p, FS_HOME, 2 * k, savsz - 64 + 16 * k);
    }

    if (!d->dynamic && out == 0 && 16 + locals <= 4080) {
        /* Shape A: {x29, lr} at the bottom of the locals, x29 == sp. */
        u32 locsz = 16 + locals;
        /* The packed-unwind table allows the pre-indexed form up to 512, but
         * the mirrored `ldp x29, x30, [sp], #512` is not encodable (imm7
         * tops out at 504), so 512 uses the sub/stp form instead. */
        if (locsz <= 504) {
            add_step(p, FS_FPLR_PRE, 0, locsz);
        } else {
            add_step(p, FS_ALLOC, 0, locsz);
            add_step(p, FS_FPLR, 0, 0);
        }
        add_step(p, FS_SET_FP, 0, 0);
        p->shape = SHAPE_A;
        p->locals_sp_offset = 16;
        p->fp_sp_offset = 0;
        p->frame_size = savsz + locsz;
    } else {
        /* Shape B: {x29, lr} directly below the save area; the rest of the
         * frame is allocated below x29 and released with mov sp, x29. */
        add_step(p, FS_FPLR_PRE, 0, 16);
        add_step(p, FS_SET_FP, 0, 0);
        u32 rest = locals + out;
        if (rest >= 4096) {
            u32 units = rest / 16;
            if (rest >= ((u32)1 << 28)) nc_fatal("stack frame of %u bytes is too large", rest);
            add_step(p, FS_CHK_MOVZ, 15, units & 0xFFFF);
            if (units > 0xFFFF) add_step(p, FS_CHK_MOVK, 15, units >> 16);
            add_step(p, FS_CHK_BL, 0, 0);
            add_step(p, FS_CHK_SUB, 0, rest);
        } else if (rest) {
            add_step(p, FS_ALLOC, 0, rest);
        }
        p->shape = SHAPE_B;
        p->locals_sp_offset = out;
        p->fp_sp_offset = rest;
        p->frame_size = savsz + 16 + rest;
    }
    p->home_sp_offset = p->frame_size - 64;
}

/* ---- instruction generation --------------------------------------------- */

static MInst mi_pair(A64Op op, u32 rt, u32 rt2, i64 off) {
    return (MInst){.op = op, .rd = (u8)rt, .ra = (u8)rt2, .rn = A64_SP, .imm = off};
}
static MInst mi_single(A64Op op, u32 rt, i64 off) {
    return (MInst){.op = op, .rd = (u8)rt, .rn = A64_SP, .imm = off};
}

static MInst step_insn(const FrameStep *s, u32 chkstk_sym) {
    switch (s->kind) {
    case FS_STP_X_PRE: return mi_pair(A64_STPXpre, s->reg, s->reg + 1u, -(i64)s->off);
    case FS_STP_X: return mi_pair(A64_STPXi, s->reg, s->reg + 1u, s->off);
    case FS_STR_X_PRE: return mi_single(A64_STRXpre, s->reg, -(i64)s->off);
    case FS_STR_X: return mi_single(A64_STRXui, s->reg, s->off);
    case FS_STP_D_PRE: return mi_pair(A64_STPDpre, s->reg, s->reg + 1u, -(i64)s->off);
    case FS_STP_D: return mi_pair(A64_STPDi, s->reg, s->reg + 1u, s->off);
    case FS_STR_D_PRE: return mi_single(A64_STRDpre, s->reg, -(i64)s->off);
    case FS_STR_D: return mi_single(A64_STRDui, s->reg, s->off);
    case FS_HOME: return mi_pair(A64_STPXi, s->reg, s->reg + 1u, s->off);
    case FS_FPLR_PRE: return mi_pair(A64_STPXpre, A64_FP, A64_LR, -(i64)s->off);
    case FS_FPLR: return mi_pair(A64_STPXi, A64_FP, A64_LR, s->off);
    case FS_SET_FP: return (MInst){.op = A64_MOVXsp, .rd = A64_FP, .rn = A64_SP};
    case FS_ALLOC: return (MInst){.op = A64_SUBXri, .rd = A64_SP, .rn = A64_SP, .imm = s->off};
    case FS_CHK_MOVZ: return (MInst){.op = A64_MOVZX, .rd = 15, .imm = s->off};
    case FS_CHK_MOVK: return (MInst){.op = A64_MOVKX, .rd = 15, .imm = s->off, .amount = 16};
    case FS_CHK_BL: return (MInst){.op = A64_BL, .tkind = TK_SYM, .target = chkstk_sym};
    case FS_CHK_SUB:
        return (MInst){.op = A64_SUBXrx, .rd = A64_SP, .rn = A64_SP, .rm = 15,
                       .shift = EX_UXTX, .amount = 4};
    }
    NC_UNREACHABLE();
}

/* The epilogue instruction undoing step `s`, or false if the step has no
 * epilogue counterpart. */
static bool step_epilog_insn(const FramePlan *p, const FrameStep *s, MInst *out) {
    switch (s->kind) {
    case FS_STP_X_PRE: *out = mi_pair(A64_LDPXpost, s->reg, s->reg + 1u, s->off); return true;
    case FS_STP_X: *out = mi_pair(A64_LDPXi, s->reg, s->reg + 1u, s->off); return true;
    case FS_STR_X_PRE: *out = mi_single(A64_LDRXpost, s->reg, s->off); return true;
    case FS_STR_X: *out = mi_single(A64_LDRXui, s->reg, s->off); return true;
    case FS_STP_D_PRE: *out = mi_pair(A64_LDPDpost, s->reg, s->reg + 1u, s->off); return true;
    case FS_STP_D: *out = mi_pair(A64_LDPDi, s->reg, s->reg + 1u, s->off); return true;
    case FS_STR_D_PRE: *out = mi_single(A64_LDRDpost, s->reg, s->off); return true;
    case FS_STR_D: *out = mi_single(A64_LDRDui, s->reg, s->off); return true;
    case FS_FPLR_PRE: *out = mi_pair(A64_LDPXpost, A64_FP, A64_LR, s->off); return true;
    case FS_FPLR: *out = mi_pair(A64_LDPXi, A64_FP, A64_LR, s->off); return true;
    case FS_SET_FP:
        if (p->shape != SHAPE_B) return false;
        *out = (MInst){.op = A64_MOVXsp, .rd = A64_SP, .rn = A64_FP};
        return true;
    case FS_ALLOC:
        /* In shape B the allocation below x29 is undone by mov sp, x29. */
        if (p->shape == SHAPE_B && s != &p->steps[0]) {
            bool after_fp = false;
            for (const FrameStep *q = p->steps; q < s; q++) after_fp |= q->kind == FS_SET_FP;
            if (after_fp) return false;
        }
        *out = (MInst){.op = A64_ADDXri, .rd = A64_SP, .rn = A64_SP, .imm = s->off};
        return true;
    case FS_HOME:
    case FS_CHK_MOVZ:
    case FS_CHK_MOVK:
    case FS_CHK_BL:
    case FS_CHK_SUB: return false;
    }
    NC_UNREACHABLE();
}

u32 frame_prologue_len(const FramePlan *p) { return p->nsteps; }

u32 frame_prologue(const FramePlan *p, MInst *out, u32 chkstk_sym) {
    for (u32 i = 0; i < p->nsteps; i++) out[i] = step_insn(&p->steps[i], chkstk_sym);
    return p->nsteps;
}

u32 frame_epilogue(const FramePlan *p, MInst *out) {
    u32 n = 0;
    for (u32 i = p->nsteps; i-- > 0;)
        if (step_epilog_insn(p, &p->steps[i], &out[n])) n++;
    out[n++] = (MInst){.op = A64_RET, .rn = A64_LR};
    return n;
}

u32 frame_epilogue_len(const FramePlan *p) {
    MInst tmp[FRAME_MAX_STEPS + 1];
    return frame_epilogue(p, tmp);
}

/* ======================================================================== */
/* Unwind codes                                                             */
/* ======================================================================== */

typedef enum UwKind : u8 {
    UW_ALLOC, UW_SAVE_R19R20_X, UW_SAVE_FPLR, UW_SAVE_FPLR_X, UW_SAVE_REGP, UW_SAVE_REGP_X,
    UW_SAVE_REG, UW_SAVE_REG_X, UW_SAVE_LRPAIR, UW_SAVE_FREGP, UW_SAVE_FREGP_X, UW_SAVE_FREG,
    UW_SAVE_FREG_X, UW_SET_FP, UW_ADD_FP, UW_NOP, UW_END, UW_END_C, UW_SAVE_NEXT,
    UW_PAC_SIGN_LR, UW_UNKNOWN,
} UwKind;

static const char *const uw_names[] = {
    "alloc", "save_r19r20_x", "save_fplr", "save_fplr_x", "save_regp", "save_regp_x",
    "save_reg", "save_reg_x", "save_lrpair", "save_fregp", "save_fregp_x", "save_freg",
    "save_freg_x", "set_fp", "add_fp", "nop", "end", "end_c", "save_next", "pac_sign_lr",
    "unknown",
};

typedef struct UwOp {
    UwKind kind;
    u8 reg; /* absolute register number (x19.., d8..) */
    u32 off; /* byte offset, or allocation size */
} UwOp;

typedef struct UwCodes {
    u8 b[512];
    u32 len;
} UwCodes;

static void uw_put(UwCodes *c, u32 n, u32 v) {
    NC_ASSERT(c->len + n <= sizeof c->b);
    for (u32 i = n; i-- > 0;) c->b[c->len++] = (u8)(v >> (8 * i));
}

static void uw_encode(UwCodes *c, UwOp op) {
    u32 r = op.reg, z = op.off / 8;
    switch (op.kind) {
    case UW_ALLOC: {
        u32 units = op.off / 16;
        NC_ASSERT(op.off % 16 == 0);
        if (units < 32) uw_put(c, 1, units);
        else if (units < 2048) uw_put(c, 2, 0xC000u | units);
        else { NC_ASSERT(units < (1u << 24)); uw_put(c, 4, 0xE0000000u | units); }
        return;
    }
    case UW_SAVE_R19R20_X: NC_ASSERT(z < 32); uw_put(c, 1, 0x20u | z); return;
    case UW_SAVE_FPLR: NC_ASSERT(z < 64); uw_put(c, 1, 0x40u | z); return;
    case UW_SAVE_FPLR_X: NC_ASSERT(z >= 1 && z <= 64); uw_put(c, 1, 0x80u | (z - 1)); return;
    case UW_SAVE_REGP: NC_ASSERT(z < 64); uw_put(c, 2, 0xC800u | (r - 19) << 6 | z); return;
    case UW_SAVE_REGP_X:
        NC_ASSERT(z >= 1 && z <= 64);
        uw_put(c, 2, 0xCC00u | (r - 19) << 6 | (z - 1));
        return;
    case UW_SAVE_REG: NC_ASSERT(z < 64); uw_put(c, 2, 0xD000u | (r - 19) << 6 | z); return;
    case UW_SAVE_REG_X:
        NC_ASSERT(z >= 1 && z <= 32);
        uw_put(c, 2, 0xD400u | (r - 19) << 5 | (z - 1));
        return;
    case UW_SAVE_LRPAIR: uw_put(c, 2, 0xD600u | ((r - 19) / 2) << 6 | z); return;
    case UW_SAVE_FREGP: NC_ASSERT(z < 64); uw_put(c, 2, 0xD800u | (r - 8) << 6 | z); return;
    case UW_SAVE_FREGP_X:
        NC_ASSERT(z >= 1 && z <= 64);
        uw_put(c, 2, 0xDA00u | (r - 8) << 6 | (z - 1));
        return;
    case UW_SAVE_FREG: NC_ASSERT(z < 64); uw_put(c, 2, 0xDC00u | (r - 8) << 6 | z); return;
    case UW_SAVE_FREG_X:
        NC_ASSERT(z >= 1 && z <= 32);
        uw_put(c, 2, 0xDE00u | (r - 8) << 5 | (z - 1));
        return;
    case UW_SET_FP: uw_put(c, 1, 0xE1); return;
    case UW_ADD_FP: NC_ASSERT(z < 256); uw_put(c, 2, 0xE200u | z); return;
    case UW_NOP: uw_put(c, 1, 0xE3); return;
    case UW_END: uw_put(c, 1, 0xE4); return;
    case UW_END_C: uw_put(c, 1, 0xE5); return;
    case UW_SAVE_NEXT: uw_put(c, 1, 0xE6); return;
    case UW_PAC_SIGN_LR: uw_put(c, 1, 0xFC); return;
    case UW_UNKNOWN: break;
    }
    NC_UNREACHABLE();
}

/* Decodes one code; returns its length in bytes (0 if truncated). */
static u32 uw_decode(const u8 *p, u32 avail, UwOp *op) {
    if (!avail) return 0;
    u32 b = p[0];
    u32 v = avail >= 2 ? (b << 8 | p[1]) : 0;
    *op = (UwOp){UW_UNKNOWN, 0, 0};
    if (b < 0x20) { *op = (UwOp){UW_ALLOC, 0, (b & 0x1F) * 16}; return 1; }
    if (b < 0x40) { *op = (UwOp){UW_SAVE_R19R20_X, 19, (b & 0x1F) * 8}; return 1; }
    if (b < 0x80) { *op = (UwOp){UW_SAVE_FPLR, 29, (b & 0x3F) * 8}; return 1; }
    if (b < 0xC0) { *op = (UwOp){UW_SAVE_FPLR_X, 29, ((b & 0x3F) + 1) * 8}; return 1; }
    if (b <= 0xDE && avail < 2) return 0;
    if (b < 0xC8) { *op = (UwOp){UW_ALLOC, 0, (v & 0x7FF) * 16}; return 2; }
    if (b < 0xCC) { *op = (UwOp){UW_SAVE_REGP, (u8)(19 + ((v >> 6) & 0xF)), (v & 0x3F) * 8}; return 2; }
    if (b < 0xD0) { *op = (UwOp){UW_SAVE_REGP_X, (u8)(19 + ((v >> 6) & 0xF)), ((v & 0x3F) + 1) * 8}; return 2; }
    if (b < 0xD4) { *op = (UwOp){UW_SAVE_REG, (u8)(19 + ((v >> 6) & 0xF)), (v & 0x3F) * 8}; return 2; }
    if (b < 0xD6) { *op = (UwOp){UW_SAVE_REG_X, (u8)(19 + ((v >> 5) & 0xF)), ((v & 0x1F) + 1) * 8}; return 2; }
    if (b < 0xD8) { *op = (UwOp){UW_SAVE_LRPAIR, (u8)(19 + 2 * ((v >> 6) & 7)), (v & 0x3F) * 8}; return 2; }
    if (b < 0xDA) { *op = (UwOp){UW_SAVE_FREGP, (u8)(8 + ((v >> 6) & 7)), (v & 0x3F) * 8}; return 2; }
    if (b < 0xDC) { *op = (UwOp){UW_SAVE_FREGP_X, (u8)(8 + ((v >> 6) & 7)), ((v & 0x3F) + 1) * 8}; return 2; }
    if (b < 0xDE) { *op = (UwOp){UW_SAVE_FREG, (u8)(8 + ((v >> 6) & 7)), (v & 0x3F) * 8}; return 2; }
    if (b == 0xDE) { *op = (UwOp){UW_SAVE_FREG_X, (u8)(8 + ((v >> 5) & 7)), ((v & 0x1F) + 1) * 8}; return 2; }
    if (b == 0xDF) return avail >= 2 ? 2 : 0; /* alloc_z (SVE): unsupported */
    if (b == 0xE0) {
        if (avail < 4) return 0;
        u32 units = (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
        *op = (UwOp){UW_ALLOC, 0, units * 16};
        return 4;
    }
    if (b == 0xE1) { op->kind = UW_SET_FP; return 1; }
    if (b == 0xE2) { if (avail < 2) return 0; *op = (UwOp){UW_ADD_FP, 0, p[1] * 8u}; return 2; }
    if (b == 0xE3) { op->kind = UW_NOP; return 1; }
    if (b == 0xE4) { op->kind = UW_END; return 1; }
    if (b == 0xE5) { op->kind = UW_END_C; return 1; }
    if (b == 0xE6) { op->kind = UW_SAVE_NEXT; return 1; }
    if (b == 0xE7) return avail >= 3 ? 3 : 0;  /* save_any_*: not generated */
    if (b == 0xFC) { op->kind = UW_PAC_SIGN_LR; return 1; }
    if (b >= 0xF8 && b <= 0xFB) return b - 0xF8 + 2 <= avail ? b - 0xF8 + 2 : 0;
    return 1;
}

static UwOp step_code(const FrameStep *s) {
    switch (s->kind) {
    case FS_STP_X_PRE: return (UwOp){UW_SAVE_REGP_X, s->reg, s->off};
    case FS_STP_X: return (UwOp){UW_SAVE_REGP, s->reg, s->off};
    case FS_STR_X_PRE: return (UwOp){UW_SAVE_REG_X, s->reg, s->off};
    case FS_STR_X: return (UwOp){UW_SAVE_REG, s->reg, s->off};
    case FS_STP_D_PRE: return (UwOp){UW_SAVE_FREGP_X, s->reg, s->off};
    case FS_STP_D: return (UwOp){UW_SAVE_FREGP, s->reg, s->off};
    case FS_STR_D_PRE: return (UwOp){UW_SAVE_FREG_X, s->reg, s->off};
    case FS_STR_D: return (UwOp){UW_SAVE_FREG, s->reg, s->off};
    case FS_FPLR_PRE: return (UwOp){UW_SAVE_FPLR_X, 29, s->off};
    case FS_FPLR: return (UwOp){UW_SAVE_FPLR, 29, s->off};
    case FS_SET_FP: return (UwOp){UW_SET_FP, 0, 0};
    case FS_ALLOC: return (UwOp){UW_ALLOC, 0, s->off};
    case FS_CHK_SUB: return (UwOp){UW_ALLOC, 0, s->off};
    case FS_HOME:
    case FS_CHK_MOVZ:
    case FS_CHK_MOVK:
    case FS_CHK_BL: return (UwOp){UW_NOP, 0, 0};
    }
    NC_UNREACHABLE();
}

/* Prologue codes: steps in reverse execution order, then end. */
static void prolog_codes(const FramePlan *p, UwCodes *c) {
    c->len = 0;
    for (u32 i = p->nsteps; i-- > 0;) uw_encode(c, step_code(&p->steps[i]));
    uw_encode(c, (UwOp){UW_END, 0, 0});
}

/* Epilogue codes: one per epilogue instruction, in execution order, then end
 * (which stands for the ret). */
static void epilog_codes(const FramePlan *p, UwCodes *c) {
    c->len = 0;
    for (u32 i = p->nsteps; i-- > 0;) {
        MInst tmp;
        if (step_epilog_insn(p, &p->steps[i], &tmp)) uw_encode(c, step_code(&p->steps[i]));
    }
    uw_encode(c, (UwOp){UW_END, 0, 0});
}

/* Returns the byte offset at which `e` occurs as a suffix of `p` on a code
 * boundary, or -1. */
static i32 suffix_offset(const UwCodes *p, const UwCodes *e) {
    if (e->len > p->len) return -1;
    u32 start = p->len - e->len;
    if (memcmp(p->b + start, e->b, e->len) != 0) return -1;
    for (u32 pos = 0; pos < p->len;) {
        if (pos == start) return (i32)start;
        UwOp op;
        u32 n = uw_decode(p->b + pos, p->len - pos, &op);
        if (!n) break;
        pos += n;
    }
    return -1;
}

void unwind_format_codes(const u8 *codes, u32 len, Buf *out) {
    char line[96];
    for (u32 pos = 0; pos < len;) {
        UwOp op;
        u32 n = uw_decode(codes + pos, len - pos, &op);
        if (!n) { buf_bytes(out, "<truncated>\n", 12); break; }
        int k = snprintf(line, sizeof line, "  %s", uw_names[op.kind]);
        if (op.kind >= UW_SAVE_R19R20_X && op.kind <= UW_SAVE_FREG_X)
            k += snprintf(line + k, sizeof line - (size_t)k, " %c%u, %u",
                          op.kind >= UW_SAVE_FREGP ? 'd' : 'x', op.reg, op.off);
        else if (op.kind == UW_ALLOC || op.kind == UW_ADD_FP)
            k += snprintf(line + k, sizeof line - (size_t)k, " %u", op.off);
        line[k++] = '\n';
        buf_bytes(out, line, (u32)k);
        pos += n;
    }
}

/* ---- packed unwind data ------------------------------------------------- */

typedef struct PackedFields {
    u32 func_len, reg_f, reg_i, h, cr, frame_size;
} PackedFields;

static PackedFields packed_decode(u32 w) {
    return (PackedFields){((w >> 2) & 0x7FF) * 4, (w >> 13) & 7, (w >> 16) & 0xF,
                          (w >> 20) & 1, (w >> 21) & 3, (w >> 23) * 16};
}

/* Whether a step can be represented by its unwind code and instruction.
 * Packed words from an image may be arbitrary, so their expansion is
 * validated rather than asserted. */
static bool step_valid(const FrameStep *s) {
    u32 o = s->off;
    switch (s->kind) {
    case FS_STP_X_PRE: case FS_STP_D_PRE: case FS_FPLR_PRE: return o >= 8 && o <= 512 && o % 8 == 0;
    case FS_STR_X_PRE: case FS_STR_D_PRE: return o >= 8 && o <= 256 && o % 8 == 0;
    case FS_STP_X: case FS_STR_X: case FS_STP_D: case FS_STR_D: case FS_FPLR: case FS_HOME:
        return o <= 504 && o % 8 == 0;
    case FS_ALLOC: case FS_CHK_SUB: return o % 16 == 0 && o < (1u << 28);
    default: return true;
    }
}

/* Expands packed fields into the canonical prologue, following the step
 * table in Microsoft's documentation. Independent of frame_plan() on
 * purpose: the two must agree for packed data to be used. Only CR == 3
 * (chained, stp x29/lr) is supported. */
static bool packed_expand(PackedFields f, FramePlan *x) {
    memset(x, 0, sizeof *x);
    if (f.cr != 3 || f.reg_i > 10) return false;
    u32 nfp = f.reg_f ? f.reg_f + 1 : 0;
    u32 intsz = f.reg_i * 8, fpsz = nfp * 8;
    u32 savsz = NC_ALIGN_UP(intsz + fpsz + 64 * f.h, 16u);
    if (f.frame_size < savsz) return false;
    u32 locsz = f.frame_size - savsz;
    if (f.h && f.reg_i == 0 && nfp == 0) return false;
    for (u32 i = 0; i < f.reg_i; i += 2) {
        bool pair = i + 1 < f.reg_i;
        if (i == 0) add_step(x, pair ? FS_STP_X_PRE : FS_STR_X_PRE, 19, savsz);
        else add_step(x, pair ? FS_STP_X : FS_STR_X, 19 + i, 8 * i);
    }
    for (u32 j = 0; j < nfp; j += 2) {
        bool pair = j + 1 < nfp, first = f.reg_i == 0 && j == 0;
        if (first) add_step(x, pair ? FS_STP_D_PRE : FS_STR_D_PRE, 8, savsz);
        else add_step(x, pair ? FS_STP_D : FS_STR_D, 8 + j, intsz + 8 * j);
    }
    if (f.h)
        for (u32 k = 0; k < 4; k++) add_step(x, FS_HOME, 2 * k, intsz + fpsz + 16 * k);
    if (locsz <= 512) {
        add_step(x, FS_FPLR_PRE, 0, locsz);
    } else if (locsz <= 4080) {
        add_step(x, FS_ALLOC, 0, locsz);
        add_step(x, FS_FPLR, 0, 0);
    } else {
        add_step(x, FS_ALLOC, 0, 4080);
        add_step(x, FS_ALLOC, 0, locsz - 4080);
        add_step(x, FS_FPLR, 0, 0);
    }
    add_step(x, FS_SET_FP, 0, 0);
    x->shape = SHAPE_A;
    if (locsz < 16) return false; /* a chained frame always saves {x29, lr} */
    for (u32 i = 0; i < x->nsteps; i++)
        if (!step_valid(&x->steps[i])) return false;
    return true;
}

static bool steps_equal(const FramePlan *a, const FramePlan *b) {
    if (a->nsteps != b->nsteps) return false;
    for (u32 i = 0; i < a->nsteps; i++)
        if (a->steps[i].kind != b->steps[i].kind || a->steps[i].reg != b->steps[i].reg ||
            a->steps[i].off != b->steps[i].off)
            return false;
    return true;
}

static bool try_pack(const FramePlan *p, u32 func_bytes, const u32 *epilogs, u32 nepilogs,
                     u32 *word) {
    if (p->shape != SHAPE_A || nepilogs != 1) return false;
    if (func_bytes > 2047 * 4 || p->frame_size / 16 > 511 || p->n_fpr == 1) return false;
    if (epilogs[0] != func_bytes - 4 * frame_epilogue_len(p)) return false;
    u32 reg_f = p->n_fpr ? p->n_fpr - 1u : 0u;
    u32 w = 1u | (func_bytes / 4) << 2 | reg_f << 13 | (u32)p->n_gpr << 16 |
            (u32)p->home_args << 20 | 3u << 21 | (p->frame_size / 16) << 23;
    FramePlan x;
    if (!packed_expand(packed_decode(w), &x) || !steps_equal(p, &x)) return false;
    *word = w;
    return true;
}

/* ---- full .xdata records ------------------------------------------------ */

void unwind_build(const FramePlan *p, u32 func_bytes, const u32 *epilogs, u32 nepilogs,
                  UnwindRecord *out) {
    memset(out, 0, sizeof *out);
    if (p->shape == SHAPE_NONE) {
        out->none = true;
        return;
    }
    if (try_pack(p, func_bytes, epilogs, nepilogs, &out->packed_word)) {
        out->packed = true;
        return;
    }
    if (func_bytes / 4 >= (1u << 18))
        nc_fatal("function of %u bytes exceeds the 1 MiB unwind limit (fragments are not "
                 "supported yet)", func_bytes);

    UwCodes codes, epi;
    prolog_codes(p, &codes);
    epilog_codes(p, &epi);
    i32 epi_index = suffix_offset(&codes, &epi);
    if (epi_index < 0) {
        epi_index = (i32)codes.len;
        memcpy(codes.b + codes.len, epi.b, epi.len);
        codes.len += epi.len;
    }
    while (codes.len % 4) codes.b[codes.len++] = 0xE4; /* pad with end */
    u32 words = codes.len / 4;
    NC_ASSERT(epi_index < 1024);

    Buf *x = &out->xdata;
    u32 flen = func_bytes / 4;
    if (nepilogs > 31 || words > 31) {
        NC_ASSERT(nepilogs <= 0xFFFF && words <= 0xFF);
        buf_u32(x, flen);
        buf_u32(x, nepilogs | words << 16);
    } else {
        buf_u32(x, flen | nepilogs << 22 | words << 27);
    }
    for (u32 i = 0; i < nepilogs; i++) {
        NC_ASSERT(epilogs[i] % 4 == 0 && epilogs[i] < func_bytes);
        buf_u32(x, epilogs[i] / 4 | (u32)epi_index << 22);
    }
    buf_bytes(x, codes.b, codes.len);
}

void unwind_record_free(UnwindRecord *r) { buf_free(&r->xdata); }

/* ======================================================================== */
/* Verifier                                                                 */
/* ======================================================================== */

enum { MEM_SLOTS = 256 }; /* prologues store at most ~30 words */

typedef struct MemSlot {
    u64 addr, val;
    bool used;
} MemSlot;

typedef struct EmuState {
    u64 x[31];
    u64 sp;
    u64 d[32];
    u64 pc; /* function-relative byte offset; UINT64_MAX after ret */
    MemSlot mem[MEM_SLOTS];
} EmuState;

static const u64 POISON = 0xDEADDEADDEADDEADull;

static MemSlot *mem_slot(EmuState *s, u64 addr, bool create) {
    u32 h = (u32)((addr >> 3) * 2654435761u) & (MEM_SLOTS - 1);
    for (u32 i = 0; i < MEM_SLOTS; i++, h = (h + 1) & (MEM_SLOTS - 1)) {
        if (s->mem[h].used && s->mem[h].addr == addr) return &s->mem[h];
        if (!s->mem[h].used) {
            if (!create) return nullptr;
            s->mem[h] = (MemSlot){addr, 0, true};
            return &s->mem[h];
        }
    }
    nc_fatal("unwind verifier: emulated memory full");
}

static void mem_store(EmuState *s, u64 addr, u64 v) { mem_slot(s, addr, true)->val = v; }
static u64 mem_load(EmuState *s, u64 addr) {
    MemSlot *m = mem_slot(s, addr, false);
    return m ? m->val : POISON;
}

typedef struct VErr {
    char *buf;
    size_t size;
    bool failed;
    bool skipped; /* failure means "outside what the verifier models", not "wrong" */
} VErr;

static bool vfail(VErr *e, const char *fmt, ...) {
    if (e->failed) return false;
    e->failed = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->buf, e->size, fmt, ap);
    va_end(ap);
    return false;
}

/* Like vfail, but for constructs the verifier does not model (instructions
 * ncc never emits in frame code, unwind codes it never generates). For ncc's
 * own functions this is still a failure; for foreign code in a linked image
 * (e.g. the CRT) the function is reported as skipped. */
static bool vunsupported(VErr *e, const char *fmt, ...) {
    if (e->failed) return false;
    e->failed = e->skipped = true;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e->buf, e->size, fmt, ap);
    va_end(ap);
    return false;
}

static i64 sext(u64 v, u32 bits) { return (i64)(v << (64 - bits)) >> (64 - bits); }

static u64 *xreg(EmuState *s, u32 r) { return r == 31 ? &s->sp : &s->x[r]; }

/* Executes one prologue/epilogue instruction. Only the instruction forms the
 * frame lowering emits are supported; anything else is a verifier error. */
static bool emu_exec(EmuState *s, u32 w, VErr *e) {
    u32 rt = w & 31, rn = (w >> 5) & 31;
    if ((w & 0x3A000000) == 0x28000000) { /* load/store pair */
        u32 opc = w >> 30, v = (w >> 26) & 1, idx = (w >> 23) & 3, l = (w >> 22) & 1;
        u32 rt2 = (w >> 10) & 31;
        if (!((v == 0 && opc == 2) || (v == 1 && opc == 1)) || idx == 0)
            return vunsupported(e, "unsupported pair instruction %08x", w);
        i64 imm = sext((w >> 15) & 0x7F, 7) * 8;
        u64 base = *xreg(s, rn);
        u64 addr = idx == 1 ? base : base + (u64)imm;
        if (l) {
            u64 a = mem_load(s, addr), b = mem_load(s, addr + 8);
            if (v) { s->d[rt] = a; s->d[rt2] = b; }
            else { s->x[rt] = a; s->x[rt2] = b; } /* rt/rt2 == 31 would be xzr: not emitted */
        } else {
            mem_store(s, addr, v ? s->d[rt] : (rt == 31 ? 0 : s->x[rt]));
            mem_store(s, addr + 8, v ? s->d[rt2] : (rt2 == 31 ? 0 : s->x[rt2]));
        }
        if (idx != 2) *xreg(s, rn) = base + (u64)imm;
    } else if ((w & 0x3B000000) == 0x39000000) { /* ldr/str unsigned offset */
        u32 size = w >> 30, v = (w >> 26) & 1, opc = (w >> 22) & 3;
        if (size != 3 || opc > 1) return vunsupported(e, "unsupported load/store %08x", w);
        u64 addr = *xreg(s, rn) + (((w >> 10) & 0xFFF) << 3);
        if (opc) { if (v) s->d[rt] = mem_load(s, addr); else s->x[rt] = mem_load(s, addr); }
        else mem_store(s, addr, v ? s->d[rt] : s->x[rt]);
    } else if ((w & 0x3B200000) == 0x38000000) { /* ldr/str imm9 */
        u32 size = w >> 30, v = (w >> 26) & 1, opc = (w >> 22) & 3, idx = (w >> 10) & 3;
        if (size != 3 || opc > 1 || (idx != 1 && idx != 3))
            return vunsupported(e, "unsupported load/store %08x", w);
        i64 imm = sext((w >> 12) & 0x1FF, 9);
        u64 base = *xreg(s, rn);
        u64 addr = idx == 1 ? base : base + (u64)imm;
        if (opc) { if (v) s->d[rt] = mem_load(s, addr); else s->x[rt] = mem_load(s, addr); }
        else mem_store(s, addr, v ? s->d[rt] : s->x[rt]);
        *xreg(s, rn) = base + (u64)imm;
    } else if ((w & 0x9F000000) == 0x91000000) { /* add/sub immediate, 64-bit */
        if (w & (1u << 29)) return vunsupported(e, "flag-setting add/sub in frame code %08x", w);
        u64 imm = ((w >> 10) & 0xFFF) << ((w >> 22) & 1 ? 12 : 0);
        u64 src = *xreg(s, rn);
        *xreg(s, rt) = (w & (1u << 30)) ? src - imm : src + imm;
    } else if ((w & 0x9FE00000) == 0x8B200000) { /* add/sub extended, 64-bit */
        u32 rm = (w >> 16) & 31, opt = (w >> 13) & 7, sh = (w >> 10) & 7;
        if (opt != EX_UXTX || (w & (1u << 29))) return vunsupported(e, "unsupported extend %08x", w);
        u64 src = *xreg(s, rn), m = (rm == 31 ? 0 : s->x[rm]) << sh;
        *xreg(s, rt) = (w & (1u << 30)) ? src - m : src + m;
    } else if ((w & 0x9F800000) == 0x92800000) { /* movn/movz/movk, 64-bit */
        u32 opc = (w >> 29) & 3, hw = (w >> 21) & 3;
        u64 imm = (u64)((w >> 5) & 0xFFFF) << (16 * hw);
        if (opc == 0) s->x[rt] = ~imm;
        else if (opc == 2) s->x[rt] = imm;
        else if (opc == 3) s->x[rt] = (s->x[rt] & ~((u64)0xFFFF << (16 * hw))) | imm;
        else return vunsupported(e, "bad move-wide %08x", w);
    } else if ((w & 0xFC000000) == 0x94000000) { /* bl: only __chkstk in frame code */
        s->x[30] = s->pc + 4;
        s->x[16] = s->x[17] = POISON; /* __chkstk clobbers x16/x17, preserves x15 */
    } else if ((w & 0xFFFFFC1F) == 0xD65F0000) { /* ret */
        s->pc = s->x[rn];
        return true;
    } else if ((w & 0xFFFFF01F) == 0xD503201F) { /* hint space: nop, pacibsp */
    } else {
        return vunsupported(e, "unexpected instruction %08x in frame code", w);
    }
    s->pc += 4;
    return true;
}

/* A decoded unwind program: prologue ops plus epilogue scopes. */
typedef struct UwProgram {
    UwOp ops[160];
    u32 nops;
    u32 op_at_byte[512]; /* byte index -> op index */
    u32 prolog_count;    /* instruction-mapping ops before the first end */
    u32 nscopes;
    u32 scope_off[64];   /* bytes */
    u32 scope_op[64];    /* op index */
    u32 func_bytes;
    bool header_epilog;  /* E bit: one epilogue, described in the header */
} UwProgram;

static bool program_from_codes(UwProgram *u, const u8 *codes, u32 len, VErr *e) {
    u->nops = 0;
    for (u32 pos = 0; pos < len;) {
        UwOp op;
        u32 n = uw_decode(codes + pos, len - pos, &op);
        if (!n) return vfail(e, "truncated unwind code at byte %u", pos);
        if (op.kind == UW_UNKNOWN) return vunsupported(e, "unsupported unwind code %02x", codes[pos]);
        if (u->nops >= NC_ARRAY_LEN(u->ops) || pos >= NC_ARRAY_LEN(u->op_at_byte))
            return vfail(e, "too many unwind codes");
        u->op_at_byte[pos] = u->nops;
        u->ops[u->nops++] = op;
        pos += n;
    }
    u->prolog_count = 0;
    while (u->prolog_count < u->nops && u->ops[u->prolog_count].kind != UW_END)
        u->prolog_count++;
    if (u->prolog_count == u->nops) return vfail(e, "unwind codes lack an end");
    return true;
}

static bool program_from_record(UwProgram *u, const UnwindRecord *r, u32 func_bytes,
                                VErr *e) {
    memset(u, 0, sizeof *u);
    u->func_bytes = func_bytes;
    if (r->packed) {
        PackedFields f = packed_decode(r->packed_word);
        if ((r->packed_word & 3) != 1) return vfail(e, "packed flag is not 1");
        if (f.func_len != func_bytes)
            return vfail(e, "packed function length %u != %u", f.func_len, func_bytes);
        FramePlan x;
        if (!packed_expand(f, &x)) return vfail(e, "packed data does not expand");
        UwCodes pc, ec;
        prolog_codes(&x, &pc);
        /* Packed epilogue: reverse prologue without the home stores and
         * without set_fp, at the end of the function. */
        ec.len = 0;
        u32 epi_insns = 1;
        for (u32 i = x.nsteps; i-- > 0;) {
            if (x.steps[i].kind == FS_HOME || x.steps[i].kind == FS_SET_FP) continue;
            uw_encode(&ec, step_code(&x.steps[i]));
            epi_insns++;
        }
        uw_encode(&ec, (UwOp){UW_END, 0, 0});
        u8 all[1024];
        memcpy(all, pc.b, pc.len);
        memcpy(all + pc.len, ec.b, ec.len);
        if (!program_from_codes(u, all, pc.len + ec.len, e)) return false;
        u->nscopes = 1;
        u->scope_off[0] = func_bytes - 4 * epi_insns;
        u->scope_op[0] = u->op_at_byte[pc.len];
        return true;
    }
    const Buf *x = &r->xdata;
    if (x->len < 4) return vfail(e, "xdata too short");
    u32 w0 = buf_get_u32(x, 0);
    u32 flen = (w0 & 0x3FFFF) * 4, ebit = (w0 >> 21) & 1, xbit = (w0 >> 20) & 1;
    u32 nep = (w0 >> 22) & 0x1F, words = (w0 >> 27) & 0x1F, pos = 4;
    if ((w0 >> 18) & 3) return vfail(e, "xdata version is not 0");
    if (flen != func_bytes) return vfail(e, "xdata function length %u != %u", flen, func_bytes);
    if (nep == 0 && words == 0) {
        u32 w1 = buf_get_u32(x, 4);
        nep = w1 & 0xFFFF;
        words = (w1 >> 16) & 0xFF;
        pos = 8;
    }
    if (ebit) {
        /* One epilogue whose codes start at index `nep`; ncc never sets E, and
         * the epilogue's location is not recorded, so only prologue and body
         * can be checked for such functions. */
        u32 code_bytes = words * 4;
        if (pos + code_bytes + (xbit ? 4u : 0u) != x->len)
            return vfail(e, "xdata size mismatch (%u code words, %u bytes total)", words, x->len);
        if (!program_from_codes(u, x->data + pos, code_bytes, e)) return false;
        u->header_epilog = true;
        u->nscopes = 0;
        return true;
    }
    if (nep > NC_ARRAY_LEN(u->scope_off)) return vfail(e, "too many epilogue scopes");
    u32 scope_index[64];
    for (u32 i = 0; i < nep; i++) {
        u32 sw = buf_get_u32(x, pos);
        pos += 4;
        u->scope_off[i] = (sw & 0x3FFFF) * 4;
        scope_index[i] = sw >> 22;
        if ((sw >> 18) & 0xF) return vfail(e, "epilogue scope reserved bits set");
        if (i && u->scope_off[i] <= u->scope_off[i - 1])
            return vfail(e, "epilogue scopes not in increasing order");
    }
    u->nscopes = nep;
    u32 code_bytes = words * 4;
    if (pos + code_bytes + (xbit ? 4u : 0u) != x->len)
        return vfail(e, "xdata size mismatch (%u codes words, %u bytes total)", words, x->len);
    if (!program_from_codes(u, x->data + pos, code_bytes, e)) return false;
    for (u32 i = 0; i < nep; i++) {
        if (scope_index[i] >= code_bytes) return vfail(e, "epilogue index out of range");
        u->scope_op[i] = u->op_at_byte[scope_index[i]];
    }
    return true;
}

/* Unwinds one frame from function-relative byte offset `off`, mutating `s`
 * the way RtlVirtualUnwind would. */
static bool uw_run(const UwProgram *u, u32 off, EmuState *s, VErr *e) {
    u32 start = 0;
    u32 insn = off / 4;
    if (insn < u->prolog_count) {
        start = u->prolog_count - insn;
    } else {
        for (u32 i = u->nscopes; i-- > 0;) {
            if (off < u->scope_off[i]) continue;
            u32 k = (off - u->scope_off[i]) / 4, n = 0;
            while (u->scope_op[i] + n < u->nops && u->ops[u->scope_op[i] + n].kind != UW_END) n++;
            if (k <= n) start = u->scope_op[i] + k;
            break;
        }
    }
    for (u32 i = start; i < u->nops; i++) {
        UwOp op = u->ops[i];
        u64 sp = s->sp;
        switch (op.kind) {
        case UW_ALLOC: s->sp += op.off; break;
        case UW_SAVE_R19R20_X:
            s->x[19] = mem_load(s, sp); s->x[20] = mem_load(s, sp + 8); s->sp += op.off;
            break;
        case UW_SAVE_FPLR:
            s->x[29] = mem_load(s, sp + op.off); s->x[30] = mem_load(s, sp + op.off + 8);
            break;
        case UW_SAVE_FPLR_X:
            s->x[29] = mem_load(s, sp); s->x[30] = mem_load(s, sp + 8); s->sp += op.off;
            break;
        case UW_SAVE_REGP:
            s->x[op.reg] = mem_load(s, sp + op.off); s->x[op.reg + 1] = mem_load(s, sp + op.off + 8);
            break;
        case UW_SAVE_REGP_X:
            s->x[op.reg] = mem_load(s, sp); s->x[op.reg + 1] = mem_load(s, sp + 8); s->sp += op.off;
            break;
        case UW_SAVE_REG: s->x[op.reg] = mem_load(s, sp + op.off); break;
        case UW_SAVE_REG_X: s->x[op.reg] = mem_load(s, sp); s->sp += op.off; break;
        case UW_SAVE_LRPAIR:
            s->x[op.reg] = mem_load(s, sp + op.off); s->x[30] = mem_load(s, sp + op.off + 8);
            break;
        case UW_SAVE_FREGP:
            s->d[op.reg] = mem_load(s, sp + op.off); s->d[op.reg + 1] = mem_load(s, sp + op.off + 8);
            break;
        case UW_SAVE_FREGP_X:
            s->d[op.reg] = mem_load(s, sp); s->d[op.reg + 1] = mem_load(s, sp + 8); s->sp += op.off;
            break;
        case UW_SAVE_FREG: s->d[op.reg] = mem_load(s, sp + op.off); break;
        case UW_SAVE_FREG_X: s->d[op.reg] = mem_load(s, sp); s->sp += op.off; break;
        case UW_SET_FP: s->sp = s->x[29]; break;
        case UW_ADD_FP: s->sp = s->x[29] - op.off; break;
        case UW_NOP:
        case UW_PAC_SIGN_LR: break;
        case UW_END: s->pc = s->x[30]; return true;
        case UW_END_C:
        case UW_SAVE_NEXT:
        case UW_UNKNOWN: return vunsupported(e, "unwind code %s not supported by the verifier",
                                      uw_names[op.kind]);
        }
    }
    return vfail(e, "unwind ran off the end of the codes");
}

static bool check_unwound(const EmuState *got, const EmuState *entry, const char *where, u32 off,
                          VErr *e) {
    if (got->sp != entry->sp)
        return vfail(e, "%s +%u: unwound sp %llx, expected %llx", where, off,
                     (unsigned long long)got->sp, (unsigned long long)entry->sp);
    if (got->pc != entry->x[30])
        return vfail(e, "%s +%u: unwound return address %llx, expected %llx", where, off,
                     (unsigned long long)got->pc, (unsigned long long)entry->x[30]);
    for (u32 r = 19; r <= 29; r++)
        if (got->x[r] != entry->x[r])
            return vfail(e, "%s +%u: x%u not restored", where, off, r);
    for (u32 r = 8; r <= 15; r++)
        if (got->d[r] != entry->d[r]) return vfail(e, "%s +%u: d%u not restored", where, off, r);
    return true;
}

/* ---- the sweep -------------------------------------------------------- */

typedef struct Sweep {
    const UwProgram *u;
    const u8 *code;
    u32 func_bytes;
    u32 plen;           /* prologue instructions */
    u32 nepi;
    const u32 *epi_off; /* bytes */
    const u32 *epi_len; /* instructions, including ret */
    u64 entry_sp;
    bool sim_dynamic;   /* move sp in the body as a dynamic allocation would */
    i64 expect_fp_sp;   /* x29 - sp after the prologue, or -1 if unknown */
    UwOsUnwindFn os;
    void *os_ctx;
    u32 checks;
} Sweep;

static void to_machine(const EmuState *s, UwMachine *m) {
    memcpy(m->x, s->x, sizeof m->x);
    memcpy(m->d, s->d, sizeof m->d);
    m->sp = s->sp;
    m->pc = s->pc;
}

static void entry_state(EmuState *s, u64 sp) {
    memset(s, 0, sizeof *s);
    for (u32 i = 0; i < 31; i++) s->x[i] = 0xA5A5000000000000ull | (u64)i << 32 | 0x1111u * i;
    for (u32 i = 0; i < 32; i++) s->d[i] = 0xD0D0000000000000ull | (u64)i << 8;
    s->x[30] = 0x0000700000001230ull; /* return address */
    s->sp = sp;
}

/* Unwinds from `off` in state `st` with our unwinder and, if present, the
 * external one, and checks both recover the entry state. */
static bool check_at(Sweep *w, const EmuState *st, const EmuState *entry, u32 off,
                     const char *where, VErr *e) {
    static EmuState tmp;
    tmp = *st;
    if (!uw_run(w->u, off, &tmp, e)) return false;
    if (!check_unwound(&tmp, entry, where, off, e)) return false;
    w->checks++;
    if (!w->os) return true;

    static u64 addr[MEM_SLOTS], val[MEM_SLOTS];
    u32 n = 0;
    for (u32 i = 0; i < MEM_SLOTS; i++)
        if (st->mem[i].used) { addr[n] = st->mem[i].addr; val[n] = st->mem[i].val; n++; }
    UwMachine in, out;
    to_machine(st, &in);
    if (!w->os(w->os_ctx, off, &in, addr, val, n, &out))
        return vfail(e, "%s +%u: external unwinder failed", where, off);
    memcpy(tmp.x, out.x, sizeof tmp.x);
    memcpy(tmp.d, out.d, sizeof tmp.d);
    tmp.sp = out.sp;
    tmp.pc = out.pc;
    char label[64];
    snprintf(label, sizeof label, "%s (external unwinder)", where);
    if (!check_unwound(&tmp, entry, label, off, e)) return false;
    w->checks++;
    return true;
}

static bool value_stored(const EmuState *s, u64 v) {
    for (u32 i = 0; i < MEM_SLOTS; i++)
        if (s->mem[i].used && s->mem[i].val == v) return true;
    return false;
}

static bool sweep(Sweep *w, VErr *e) {
    static EmuState entry, cur, epi;
    entry_state(&entry, w->entry_sp);
    if (4 * w->plen > w->func_bytes) return vfail(e, "prologue longer than the function");

    /* Every prefix of the prologue. */
    cur = entry;
    for (u32 k = 0; k <= w->plen; k++) {
        if (!check_at(w, &cur, &entry, 4 * k, "prologue", e)) return false;
        if (k < w->plen && !emu_exec(&cur, rd_u32(w->code + 4 * k), e)) return false;
    }
    if (w->expect_fp_sp >= 0 && cur.x[29] != cur.sp + (u64)w->expect_fp_sp)
        return vfail(e, "x29 - sp after prologue is %lld, plan says %lld",
                     (long long)(cur.x[29] - cur.sp), (long long)w->expect_fp_sp);

    /* The body may overwrite every callee-saved register the prologue saved
     * (and lr, by calling). Clobber them, so that from here on an unwinder
     * only recovers them if it really restores them from the stack. Without
     * this, a record that omits a register would still "restore" it, because
     * nothing ever changed it. x29 keeps its frame-pointer value. */
    for (u32 r = 19; r <= 30; r++)
        if (r != 29 && value_stored(&cur, entry.x[r])) cur.x[r] = 0xB0D7000000000000ull | r;
    for (u32 r = 8; r <= 15; r++)
        if (value_stored(&cur, entry.d[r])) cur.d[r] = 0xB0D7D00000000000ull | r;

    /* One body instruction, if there is one before the first epilogue. */
    u32 body_off = 4 * w->plen;
    bool is_epilog = false;
    for (u32 i = 0; i < w->nepi; i++) is_epilog |= w->epi_off[i] == body_off;
    if (!is_epilog && body_off < w->func_bytes && !check_at(w, &cur, &entry, body_off, "body", e))
        return false;

    /* Every instruction boundary of every epilogue. */
    EmuState *start = &cur;
    if (w->sim_dynamic) start->sp -= 0x1230;
    for (u32 i = 0; i < w->nepi; i++) {
        u32 off = w->epi_off[i], len = w->epi_len[i];
        if (off + 4 * len > w->func_bytes) return vfail(e, "epilogue past function end");
        epi = *start;
        epi.pc = off;
        for (u32 j = 0; j < len; j++) {
            if (!check_at(w, &epi, &entry, off + 4 * j, "epilogue", e)) return false;
            if (!emu_exec(&epi, rd_u32(w->code + off + 4 * j), e)) return false;
        }
        /* After ret the machine state itself must be the entry state. */
        if (!check_unwound(&epi, &entry, "after ret", off + 4 * len, e)) return false;
    }
    return true;
}

bool unwind_verify(const FramePlan *p, const u8 *code, u32 func_bytes, const u32 *epilogs,
                   u32 nepilogs, const UnwindRecord *r, char *err, size_t errsz) {
    VErr e = {err, errsz, false, false};
    if (errsz) err[0] = 0;
    if (p->shape == SHAPE_NONE) return r->none ? true : vfail(&e, "frameless function has unwind data");
    if (r->none) return vfail(&e, "function with a frame has no unwind data");

    static UwProgram u; /* large; not reentrant, verification is single-threaded */
    if (!program_from_record(&u, r, func_bytes, &e)) return false;
    u32 plen = frame_prologue_len(p), elen = frame_epilogue_len(p);
    if (u.prolog_count != plen)
        return vfail(&e, "unwind codes describe %u prologue instructions, prologue has %u",
                     u.prolog_count, plen);
    if (nepilogs > 64) return vfail(&e, "too many epilogues for the verifier");
    u32 lens[64];
    for (u32 i = 0; i < nepilogs; i++) lens[i] = elen;
    Sweep w = {.u = &u, .code = code, .func_bytes = func_bytes, .plen = plen, .nepi = nepilogs,
               .epi_off = epilogs, .epi_len = lens, .entry_sp = 0x0000000010000000ull,
               .sim_dynamic = p->dynamic, .expect_fp_sp = p->fp_sp_offset};
    return sweep(&w, &e);
}

/* ---- records read back from a linked image ----------------------------- */

bool unwind_record_from_pdata(u32 word2, const u8 *xdata, size_t avail, UnwindRecord *out,
                              u32 *func_bytes) {
    memset(out, 0, sizeof *out);
    u32 flag = word2 & 3;
    if (flag == 1) {
        out->packed = true;
        out->packed_word = word2;
        *func_bytes = packed_decode(word2).func_len;
        return true;
    }
    if (flag != 0 || !xdata || avail < 4) return false; /* flag 2: no prologue; 3: link record */
    u32 w0 = rd_u32(xdata), size = 4, scopes = (w0 >> 22) & 0x1F, words = (w0 >> 27) & 0x1F;
    if ((w0 >> 22) == 0) { /* extension word */
        if (avail < 8) return false;
        u32 w1 = rd_u32(xdata + 4);
        size = 8;
        scopes = w1 & 0xFFFF;
        words = (w1 >> 16) & 0xFF;
    }
    if (!((w0 >> 21) & 1)) size += 4 * scopes;
    size += 4 * words;
    if ((w0 >> 20) & 1) size += 4; /* exception handler RVA; handler data is not needed */
    if (size > avail) return false;
    buf_bytes(&out->xdata, xdata, size);
    *func_bytes = (w0 & 0x3FFFF) * 4;
    return true;
}

UwCheckResult unwind_crosscheck(const u8 *code, u32 func_bytes, const UnwindRecord *r,
                                u64 entry_sp, UwOsUnwindFn os, void *os_ctx, u32 *checks,
                                char *err, size_t errsz) {
    VErr e = {err, errsz, false, false};
    if (errsz) err[0] = 0;
    *checks = 0;
    static UwProgram u;
    if (!program_from_record(&u, r, func_bytes, &e))
        return e.skipped ? UWC_SKIPPED : UWC_FAIL;
    u32 lens[64];
    for (u32 i = 0; i < u.nscopes; i++) {
        u32 n = 0;
        while (u.scope_op[i] + n < u.nops && u.ops[u.scope_op[i] + n].kind != UW_END) n++;
        lens[i] = n + 1; /* the codes, plus ret for the end */
    }
    Sweep w = {.u = &u, .code = code, .func_bytes = func_bytes, .plen = u.prolog_count,
               .nepi = u.nscopes, .epi_off = u.scope_off, .epi_len = lens, .entry_sp = entry_sp,
               .expect_fp_sp = -1, .os = os, .os_ctx = os_ctx};
    bool ok = sweep(&w, &e);
    *checks = w.checks;
    if (!ok) return e.skipped ? UWC_SKIPPED : UWC_FAIL;
    return u.header_epilog ? UWC_PARTIAL : UWC_OK;
}
