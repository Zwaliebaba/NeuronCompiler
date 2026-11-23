/* test_module.c — function finalisation: constant materialisation and
 * branch relaxation. */
#include "test.h"

#include "../../src/target/arm64/a64_module.h"

/* Evaluates a constant-materialisation sequence. */
static u64 eval_seq(const MFunc *f, bool w) {
    u64 v = 0;
    for (u32 i = 0; i < f->insts.len; i++) {
        const MInst *mi = &f->insts.data[i];
        u64 chunk = (u64)mi->imm << mi->amount;
        switch (mi->op) {
        case A64_MOVZX: case A64_MOVZW: v = chunk; break;
        case A64_MOVNX: case A64_MOVNW: v = ~chunk; break;
        case A64_MOVKX: case A64_MOVKW: v = (v & ~((u64)0xFFFF << mi->amount)) | chunk; break;
        case A64_ORRXri: case A64_ORRWri: v = (u64)mi->imm; break;
        default: return 0xBADBADBADull;
        }
    }
    return w ? (v & 0xFFFFFFFFu) : v;
}

static void test_mov_imm(void) {
    static const u64 vals[] = {0, 1, 0xFFFF, 0x10000, 0xFFFFFFFF, 0xFFFFFFF5, 0x12345678,
                               0xFFFFFFFFFFFFFFFFull, 0xFFFFFFFFFFFF1234ull, 0x5555555555555555ull,
                               0x123456789ABCDEF0ull, 0x0000FFFF00000000ull, 0x8000000000000000ull,
                               0xFFFF0000FFFF0000ull, 0x00FF00FF00FF00FFull};
    for (u32 i = 0; i < NC_ARRAY_LEN(vals); i++)
        for (u32 w = 0; w <= 1; w++) {
            MFunc f;
            mf_init(&f, "t", true, (FrameDesc){.frameless = true});
            mf_mov_imm(&f, 3, vals[i], w);
            u64 want = w ? (vals[i] & 0xFFFFFFFFu) : vals[i];
            CHECKF(eval_seq(&f, w) == want, "mov_imm %llx (w=%u)", (unsigned long long)vals[i], w);
            CHECKF(f.insts.len >= 1 && f.insts.len <= (w ? 2u : 4u), "mov_imm %llx: %u insts",
                   (unsigned long long)vals[i], f.insts.len);
            for (u32 k = 0; k < f.insts.len; k++) {
                u16 rt;
                (void)a64_encode(&f.insts.data[k], 0, &rt); /* must be encodable */
            }
            mf_free(&f);
        }
    /* Cheapest forms. */
    MFunc f;
    mf_init(&f, "t", true, (FrameDesc){.frameless = true});
    mf_mov_imm(&f, 0, 0xFFFFFFFFFFFF1234ull, false);
    CHECK(f.insts.len == 1 && f.insts.data[0].op == A64_MOVNX);
    vec_clear(&f.insts);
    mf_mov_imm(&f, 0, 0x5555555555555555ull, false);
    CHECK(f.insts.len == 1 && f.insts.data[0].op == A64_ORRXri);
    mf_free(&f);
}

static void test_relaxation(void) {
    Arena a;
    arena_init(&a);
    A64Module m;
    a64m_init(&m, &a, "t.obj", "t.c", false);

    /* tbz reaches +-32 KiB, b.eq +-1 MiB: put the targets beyond both. */
    MFunc f;
    mf_init(&f, "relax", false, (FrameDesc){.frameless = true});
    u32 near_end = mf_new_label(&f), far_end = mf_new_label(&f);
    mf_emit(&f, (MInst){.op = A64_TBZ, .rd = 0, .imm = 3, .tkind = TK_LABEL, .target = near_end});
    mf_emit(&f, (MInst){.op = A64_Bcc, .shift = CC_EQ, .tkind = TK_LABEL, .target = far_end});
    for (u32 i = 0; i < 9000; i++) mf_emit(&f, (MInst){.op = A64_NOP});
    mf_label(&f, near_end);
    for (u32 i = 0; i < 270000; i++) mf_emit(&f, (MInst){.op = A64_NOP});
    mf_label(&f, far_end);
    mf_emit(&f, (MInst){.op = A64_P_EPILOGUE});
    a64m_emit_function(&m, &f);

    /* Expected: tbnz w0, #3, +8 ; b near ; b.ne +8 ; b far ; nops ... */
    CoffSection *text = nullptr;
    for (u32 s = 1; s <= m.obj.sections.len; s++)
        if (strcmp(coff_section(&m.obj, s)->name, ".text$mn") == 0) text = coff_section(&m.obj, s);
    CHECK(text != nullptr);
    if (text) {
        const u8 *c = text->data.data;
        u32 w0 = rd_u32(c), w1 = rd_u32(c + 4), w2 = rd_u32(c + 8), w3 = rd_u32(c + 12);
        CHECKF((w0 & 0xFF000000) == 0x37000000 && ((w0 >> 5) & 0x3FFF) == 2, "tbnz +8: %08x", w0);
        CHECKF((w1 & 0xFC000000) == 0x14000000, "b: %08x", w1);
        CHECKF((w1 & 0x3FFFFFF) * 4 == 4 * (9000 + 3), "b near offset: %08x", w1);
        CHECKF((w2 & 0xFF00001F) == 0x54000001 && ((w2 >> 5) & 0x7FFFF) == 2, "b.ne +8: %08x", w2);
        CHECKF((w3 & 0xFC000000) == 0x14000000 && (w3 & 0x3FFFFFF) == 9000 + 270000 + 1,
               "b far: %08x", w3);
        CHECK(text->data.len == 4 * (4 + 9000 + 270000 + 1));
    }
    mf_free(&f);
    a64m_free(&m);
    arena_release(&a);
}

void test_module(void) {
    test_mov_imm();
    test_relaxation();
}
