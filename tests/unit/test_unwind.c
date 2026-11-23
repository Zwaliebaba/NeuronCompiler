/* test_unwind.c — frame plans, unwind records and the unwind verifier.
 *
 * Sweeps the frame-shape space (saved registers, home area, dynamic
 * allocation, locals and outgoing sizes, one or two epilogues), builds the
 * code and unwind record for each, and requires the verifier to accept it.
 * Then mutates prologues and records and requires the verifier to reject
 * them, so a verifier that accepts everything cannot pass. */
#include "test.h"

#include "../../src/core/buf.h"
#include "../../src/target/arm64/unwind.h"

typedef struct Built {
    FramePlan plan;
    u8 code[1024];
    u32 len;
    u32 epilogs[2];
    u32 nepilogs;
    UnwindRecord rec;
} Built;

static void emit(Built *b, const MInst *mi) {
    u16 rt;
    u32 w = a64_encode(mi, 0, &rt);
    for (int i = 0; i < 4; i++) b->code[b->len++] = (u8)(w >> (8 * i));
}

static void build(Built *b, const FrameDesc *d, u32 nepilogs) {
    memset(b, 0, sizeof *b);
    frame_plan(d, &b->plan);
    MInst seq[FRAME_MAX_STEPS + 1];
    u32 n = frame_prologue(&b->plan, seq, 0);
    for (u32 i = 0; i < n; i++) emit(b, &seq[i]);
    MInst nop = {.op = A64_NOP};
    emit(b, &nop);
    for (u32 e = 0; e < nepilogs; e++) {
        if (e) emit(b, &nop);
        b->epilogs[b->nepilogs++] = b->len;
        u32 m = frame_epilogue(&b->plan, seq);
        for (u32 i = 0; i < m; i++) emit(b, &seq[i]);
    }
    unwind_build(&b->plan, b->len, b->epilogs, b->nepilogs, &b->rec);
}

static bool verify(Built *b, char *err, size_t n) {
    return unwind_verify(&b->plan, b->code, b->len, b->epilogs, b->nepilogs, &b->rec, err, n);
}

void test_unwind(void) {
    static const u32 locals_v[] = {0, 8, 48, 480, 496, 500, 3000, 4064, 4080, 5000, 70000, 2000000};
    static const u32 out_v[] = {0, 16, 64, 600};
    char err[256];
    static Built b;
    u32 total = 0, packed = 0, full = 0, failures = 0;

    for (u32 ng = 0; ng <= 10; ng++)
        for (u32 nf = 0; nf <= 8; nf++)
            for (u32 home = 0; home <= 1; home++)
                for (u32 dyn = 0; dyn <= 1; dyn++)
                    for (u32 li = 0; li < NC_ARRAY_LEN(locals_v); li++)
                        for (u32 oi = 0; oi < NC_ARRAY_LEN(out_v); oi++)
                            for (u32 ne = 1; ne <= 2; ne++) {
                                FrameDesc d = {.n_gpr = (u8)ng, .n_fpr = (u8)nf,
                                               .home_args = home, .dynamic = dyn,
                                               .locals = locals_v[li], .outgoing = out_v[oi]};
                                build(&b, &d, ne);
                                total++;
                                if (b.rec.packed) packed++;
                                else full++;
                                if (!verify(&b, err, sizeof err)) {
                                    if (failures++ < 10)
                                        fprintf(stderr,
                                                "unwind: gpr=%u fpr=%u home=%u dyn=%u locals=%u "
                                                "out=%u epilogs=%u: %s\n",
                                                ng, nf, home, dyn, locals_v[li], out_v[oi], ne, err);
                                }
                                unwind_record_free(&b.rec);
                            }
    CHECKF(failures == 0, "%u of %u frame shapes failed verification", failures, total);
    CHECK(packed > 0 && full > 0);
    test_checks += (int)total;

    /* Frameless leaf: no unwind data. */
    FrameDesc leaf = {.frameless = true};
    build(&b, &leaf, 1);
    CHECK(b.rec.none);
    CHECK(verify(&b, err, sizeof err));

    /* A typical small non-leaf function must use packed data, matching what
     * MSVC emits: stp x29, x30, [sp, #-16]!; mov x29, sp. */
    FrameDesc small = {.locals = 0};
    build(&b, &small, 1);
    CHECK(b.rec.packed);
    CHECK(b.plan.nsteps == 2 && b.plan.steps[0].kind == FS_FPLR_PRE && b.plan.steps[0].off == 16);
    unwind_record_free(&b.rec);

    /* Microsoft's documented example 1 shape: x19 saved, 2064 bytes of locals
     * and fp/lr: str x19,[sp,#-16]!; sub sp,sp,#0x810; stp fp,lr,[sp];
     * mov fp,sp. The documented packed word for a 492-byte function is
     * 0x416101ED. */
    FrameDesc ex1 = {.n_gpr = 1, .locals = 0x810 - 16};
    frame_plan(&ex1, &b.plan);
    CHECK(b.plan.nsteps == 4);
    CHECK(b.plan.steps[0].kind == FS_STR_X_PRE && b.plan.steps[0].off == 16);
    CHECK(b.plan.steps[1].kind == FS_ALLOC && b.plan.steps[1].off == 0x810);
    CHECK(b.plan.steps[2].kind == FS_FPLR && b.plan.steps[2].off == 0);
    CHECK(b.plan.steps[3].kind == FS_SET_FP);
    {
        u32 epi = 492 - 4 * frame_epilogue_len(&b.plan);
        UnwindRecord r;
        unwind_build(&b.plan, 492, &epi, 1, &r);
        CHECKF(r.packed && r.packed_word == 0x416101EDu, "packed word %08x", r.packed_word);
    }

    /* Mutations must be rejected. */
    u32 mutations = 0, caught = 0;
    for (u32 ng = 0; ng <= 4; ng++)
        for (u32 nf = 0; nf <= 2; nf += 2)
            for (u32 li = 0; li < NC_ARRAY_LEN(locals_v); li += 3) {
                FrameDesc d = {.n_gpr = (u8)ng, .n_fpr = (u8)nf, .locals = locals_v[li]};
                build(&b, &d, 1);
                u32 plen = frame_prologue_len(&b.plan);
                for (u32 k = 0; k < plen; k++) {
                    u32 w = rd_u32(b.code + 4 * k);
                    u32 flip;
                    if ((w & 0x3A000000) == 0x28000000) flip = 1u << 15;      /* pair imm7 */
                    else if ((w & 0x3B000000) == 0x39000000) flip = 1u << 10; /* uimm12 */
                    else if ((w & 0x3B200000) == 0x38000000) flip = 1u << 15; /* imm9 */
                    else if ((w & 0x9F000000) == 0x91000000) flip = 1u << 14; /* add/sub imm */
                    else continue;
                    u32 m = w ^ flip;
                    for (int i = 0; i < 4; i++) b.code[4 * k + (u32)i] = (u8)(m >> (8 * i));
                    mutations++;
                    if (!verify(&b, err, sizeof err)) caught++;
                    for (int i = 0; i < 4; i++) b.code[4 * k + (u32)i] = (u8)(w >> (8 * i));
                }
                /* Swap the first two prologue instructions. */
                if (plen >= 2) {
                    u8 t[4];
                    memcpy(t, b.code, 4);
                    memcpy(b.code, b.code + 4, 4);
                    memcpy(b.code + 4, t, 4);
                    mutations++;
                    if (!verify(&b, err, sizeof err)) caught++;
                }
                unwind_record_free(&b.rec);
            }
    CHECKF(mutations > 50 && caught == mutations, "verifier caught %u of %u mutations", caught,
           mutations);

    /* Packed-word mutations: every change to RegI, RegF, H, CR or FrameSize
     * must be rejected. Changing RegI or RegF only drops or adds a register
     * restore; this is caught only because the sweep clobbers saved
     * callee-saved registers after the prologue. Without that, nothing would
     * ever change the register and the omission would go unnoticed. */
    u32 pk_mut = 0, pk_caught = 0;
    for (u32 ng = 0; ng <= 10; ng++)
        for (u32 nf = 0; nf <= 8; nf++) {
            FrameDesc d = {.n_gpr = (u8)ng, .n_fpr = (u8)nf, .locals = 32};
            build(&b, &d, 1);
            if (!b.rec.packed) { unwind_record_free(&b.rec); continue; }
            u32 orig = b.rec.packed_word;
            static const struct { u32 shift, width; } fields[] = {
                {13, 3}, {16, 4}, {20, 1}, {21, 2}, {23, 9}};
            for (u32 fi = 0; fi < NC_ARRAY_LEN(fields); fi++)
                for (u32 v = 0; v < (1u << fields[fi].width); v++) {
                    u32 mask = ((1u << fields[fi].width) - 1) << fields[fi].shift;
                    u32 m = (orig & ~mask) | v << fields[fi].shift;
                    if (m == orig) continue;
                    b.rec.packed_word = m;
                    pk_mut++;
                    if (!verify(&b, err, sizeof err)) pk_caught++;
                }
            b.rec.packed_word = orig;
            unwind_record_free(&b.rec);
        }
    CHECKF(pk_mut > 500 && pk_caught == pk_mut, "verifier caught %u of %u packed-word mutations",
           pk_caught, pk_mut);

    /* Record mutations: corrupt each unwind-code byte of a full record. */
    FrameDesc big = {.n_gpr = 4, .n_fpr = 2, .home_args = true, .locals = 9000, .outgoing = 32};
    build(&b, &big, 2);
    CHECK(!b.rec.packed && b.rec.xdata.len > 8);
    u32 rec_mut = 0, rec_caught = 0;
    u32 hdr = 4 + 4 * b.nepilogs;
    for (u32 i = hdr; i < b.rec.xdata.len; i++) {
        u8 orig = b.rec.xdata.data[i];
        b.rec.xdata.data[i] = orig ^ 0x01;
        rec_mut++;
        if (!verify(&b, err, sizeof err)) rec_caught++;
        b.rec.xdata.data[i] = orig;
    }
    /* Exactly four mutations are semantically equivalent and must survive:
     * the two padding bytes after the final end (never executed), and the
     * two bytes of the leading alloc_m. In a shape-B frame that allocation is
     * immediately followed (in unwind order) by set_fp, which restores sp
     * from x29, so its size never affects the unwound state - for our
     * verifier or for RtlVirtualUnwind. Everything else must be caught. */
    CHECKF(rec_caught == rec_mut - 4, "verifier caught %u of %u record mutations", rec_caught,
           rec_mut);
    unwind_record_free(&b.rec);
}
