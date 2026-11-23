/* test_encode.c — differential test of the ARM64 encoder and printer.
 *
 * For every real instruction in arm64.def we generate a set of operand
 * combinations, encode them, print them, assemble the printed text with
 * llvm-mc and require byte-identical results. This checks the encoder and
 * the printer against an independent assembler in one pass. */
#include "test.h"

#include "../../src/core/vec.h"
#include "../../src/target/arm64/a64.h"

#include <stdlib.h>

typedef VEC(MInst) MInstVec;

static const u8 regs_a[] = {0, 1, 7, 17, 29, 30, 31};

static void add(MInstVec *v, MInst mi) { vec_push(v, mi); }

static MInst mk(A64Op op) { return (MInst){.op = op}; }

static void gen_regs3(MInstVec *v, A64Op op, MInst base) {
    for (u32 i = 0; i < NC_ARRAY_LEN(regs_a); i++) {
        MInst m = base;
        m.op = op;
        m.rd = regs_a[i];
        m.rn = regs_a[(i + 2) % NC_ARRAY_LEN(regs_a)];
        m.rm = regs_a[(i + 4) % NC_ARRAY_LEN(regs_a)];
        m.ra = regs_a[(i + 5) % NC_ARRAY_LEN(regs_a)];
        add(v, m);
    }
}

static void gen(MInstVec *v, A64Op op) {
    const A64InsnInfo *ii = &a64_insn_info[op];
    bool x64 = (ii->bits >> 31) != 0;
    u32 width = x64 ? 64 : 32;
    MInst m = mk(op);
    switch (ii->form) {
    case F_PSEUDO: break;
    case F_ADDSUB_IMM:
    case F_CMP_IMM: {
        static const i64 imms[] = {0, 1, 4095, 4096, 0x123000, 0xFFF000};
        for (u32 i = 0; i < NC_ARRAY_LEN(imms); i++) {
            m.rd = regs_a[i % 7]; m.rn = regs_a[(i + 3) % 7]; m.imm = imms[i];
            add(v, m);
        }
        break;
    }
    case F_MOV_SP:
        m.rd = 31; m.rn = 0; add(v, m);
        m.rd = 29; m.rn = 31; add(v, m);
        m.rd = 31; m.rn = 29; add(v, m);
        break;
    case F_LOGIC_IMM:
    case F_TST_IMM: {
        static const u64 vals[] = {1, 0xFF, 0xFFFF0000FFFF0000ull, 0x5555555555555555ull,
                                   0x7FFFFFFFFFFFFFFEull, 0x00000000F0F0F0F0ull, 0x7FFFFFFF,
                                   0x8000000000000000ull, 0x3C, 0xFFFFFFFFFFFFFF00ull};
        for (u32 i = 0; i < NC_ARRAY_LEN(vals); i++) {
            u32 n, r, s;
            if (!a64_encode_logical_imm(vals[i], width, &n, &r, &s)) continue;
            m.rd = regs_a[i % 7];
            if (ii->kd == K_X && m.rd == 31) m.rd = 3; /* ands xzr is tst, printed differently */
            m.rn = regs_a[(i + 1) % 6];
            m.imm = (i64)vals[i];
            add(v, m);
        }
        break;
    }
    case F_MOVW: {
        static const i64 imms[] = {0, 1, 0xFFFF, 0x1234};
        for (u32 s = 0; s < width; s += 16)
            for (u32 i = 0; i < NC_ARRAY_LEN(imms); i++) {
                m.rd = regs_a[(i + s / 16) % 7]; m.imm = imms[i]; m.amount = (u8)s;
                add(v, m);
            }
        break;
    }
    case F_ADR: {
        static const i64 adr[] = {0, 4, -4, 1048572, -1048576, 12345};
        static const i64 adrp[] = {0, 4096, -4096, 0x7FFFF000, -0x80000000LL};
        const i64 *t = op == A64_ADR ? adr : adrp;
        u32 n = op == A64_ADR ? NC_ARRAY_LEN(adr) : NC_ARRAY_LEN(adrp);
        for (u32 i = 0; i < n; i++) {
            m.rd = regs_a[i % 7]; m.tkind = TK_OFFSET; m.imm = t[i];
            add(v, m);
        }
        break;
    }
    case F_BITFIELD: {
        u32 hi = width - 1;
        u32 pairs[][2] = {{0, 0}, {3, 7}, {hi, hi - 1}, {1, hi}};
        for (u32 i = 0; i < NC_ARRAY_LEN(pairs); i++) {
            m.rd = regs_a[i]; m.rn = regs_a[i + 2]; m.imm = pairs[i][0];
            m.amount = (u8)pairs[i][1];
            add(v, m);
        }
        break;
    }
    case F_LSL_IMM:
    case F_LSR_IMM: {
        u32 amts[] = {1, 5, width - 1};
        for (u32 i = 0; i < 3; i++) {
            m.rd = regs_a[i]; m.rn = regs_a[i + 3]; m.amount = (u8)amts[i];
            add(v, m);
        }
        break;
    }
    case F_EXTEND:
    case F_DP1:
    case F_FP1:
    case F_DP3:
    case F_DP3_NOA:
    case F_DP2:
    case F_FP2:
    case F_FCMP: gen_regs3(v, op, m); break;
    case F_LOGIC_REG:
    case F_ADDSUB_REG:
    case F_CMP_REG:
    case F_MOV_REG:
    case F_NEG: {
        bool arith = ii->form == F_ADDSUB_REG || ii->form == F_NEG ||
                     (ii->form == F_CMP_REG && op != A64_TSTXrs && op != A64_TSTWrs);
        bool is_mov = op == A64_MOVXr || op == A64_MOVWr;
        for (u32 sh = 0; sh < 4; sh++) {
            if (arith && sh == SH_ROR) continue;
            u32 amts[] = {0, 3, width - 1};
            for (u32 a = 0; a < 3; a++) {
                if (is_mov && (amts[a] || sh != SH_LSL)) continue;
                m.rd = regs_a[(sh + a) % 7]; m.rn = regs_a[(sh + a + 2) % 7];
                m.rm = regs_a[(sh + a + 4) % 7];
                if (ii->form == F_LOGIC_REG && op == A64_ANDSXrs && m.rd == 31) m.rd = 2;
                if (ii->form == F_LOGIC_REG && op == A64_ANDSWrs && m.rd == 31) m.rd = 2;
                if ((op == A64_SUBSXrs || op == A64_SUBSWrs || op == A64_ADDSXrs ||
                     op == A64_ADDSWrs) && m.rd == 31) m.rd = 2;
                if ((op == A64_SUBXrs || op == A64_SUBWrs) && m.rn == 31) m.rn = 4; /* neg alias */
                if ((op == A64_ORRXrs || op == A64_ORRWrs) && m.rn == 31) m.rn = 4; /* mov alias */
                if ((op == A64_ORNXrs) && m.rn == 31) m.rn = 4;                    /* mvn alias */
                m.shift = (u8)sh; m.amount = (u8)amts[a];
                add(v, m);
            }
        }
        break;
    }
    case F_ADDSUB_EXT: {
        static const A64Extend exs[] = {EX_UXTW, EX_SXTW, EX_UXTX, EX_SXTX, EX_UXTB, EX_SXTH};
        static const u8 rds[] = {31, 0, 31, 5, 7, 17};
        static const u8 rns[] = {31, 31, 1, 6, 31, 2};
        for (u32 i = 0; i < NC_ARRAY_LEN(exs); i++)
            for (u8 amt = 0; amt <= 4; amt += 2) {
                m.rd = rds[i]; m.rn = rns[i]; m.rm = (u8)(i + 9); m.shift = exs[i];
                m.amount = amt;
                add(v, m);
            }
        break;
    }
    case F_CSEL:
    case F_CSET: {
        static const A64Cond cs[] = {CC_EQ, CC_NE, CC_LT, CC_GT, CC_HS, CC_LS, CC_VS};
        for (u32 i = 0; i < NC_ARRAY_LEN(cs); i++) {
            m.rd = regs_a[i]; m.rn = regs_a[(i + 1) % 7]; m.rm = regs_a[(i + 2) % 7];
            if (ii->form == F_CSEL && (op == A64_CSINCX || op == A64_CSINVX || op == A64_CSNEGX ||
                                       op == A64_CSINCW) && m.rn == m.rm) m.rm = 4;
            if (ii->form == F_CSEL && m.rn == 31 && m.rm == 31) m.rm = 4;
            m.shift = cs[i];
            add(v, m);
        }
        break;
    }
    case F_LDST_UIMM: {
        i64 s = (i64)1 << ii->sz;
        i64 offs[] = {0, s, 4095 * s, 17 * s};
        for (u32 i = 0; i < 4; i++) {
            m.rd = regs_a[i + 1]; m.rn = i == 0 ? 31 : regs_a[i + 2]; m.imm = offs[i];
            add(v, m);
        }
        break;
    }
    case F_LDST_PRE:
    case F_LDST_POST:
    case F_LDST_UNSCALED: {
        static const i64 offs[] = {-256, -16, 0, 8, 255};
        for (u32 i = 0; i < NC_ARRAY_LEN(offs); i++) {
            m.rd = (u8)(i + 1); m.rn = i == 1 ? 31 : (u8)(i + 10); m.imm = offs[i];
            add(v, m);
        }
        break;
    }
    case F_LDST_REG: {
        static const A64Extend exs[] = {EX_UXTW, EX_SXTW, EX_UXTX, EX_SXTX};
        for (u32 i = 0; i < 4; i++)
            for (u8 s = 0; s <= 1; s++) {
                m.rd = (u8)(i + 1); m.rn = i == 2 ? 31 : (u8)(i + 10); m.rm = (u8)(i + 20);
                m.shift = exs[i]; m.amount = s;
                add(v, m);
            }
        break;
    }
    case F_LDST_PAIR:
    case F_LDST_PAIR_PRE:
    case F_LDST_PAIR_POST: {
        i64 s = (i64)1 << ii->sz;
        i64 ks[] = {-64, -2, 0, 1, 63};
        for (u32 i = 0; i < NC_ARRAY_LEN(ks); i++) {
            m.rd = (u8)(19 + i); m.ra = (u8)(i == 2 ? 30 : 1 + i); m.rn = i < 2 ? 31 : (u8)(9 + i);
            m.imm = ks[i] * s;
            add(v, m);
        }
        break;
    }
    case F_LDR_LIT:
    case F_B:
    case F_BCOND:
    case F_CB: {
        i64 lim = a64_branch_range(op);
        i64 offs[] = {0, 4, -4, lim - 4, -lim, 4096};
        for (u32 i = 0; i < NC_ARRAY_LEN(offs); i++) {
            m.tkind = TK_OFFSET; m.imm = offs[i]; m.rd = regs_a[i];
            m.shift = (u8)(i * 3 % 14);
            add(v, m);
        }
        break;
    }
    case F_TB: {
        static const i64 bits[] = {0, 5, 31, 32, 63};
        static const i64 offs[] = {0, 32764, -32768, 8, -4};
        for (u32 i = 0; i < 5; i++) {
            m.rd = regs_a[i]; m.imm = bits[i]; m.tkind = TK_OFFSET;
            m.target = 0;
            MInst t = m;
            t.imm = bits[i];
            /* pcrel travels separately; stash it in .target for the driver below */
            t.target = (u32)(offs[i] & 0xFFFFFFFF);
            add(v, t);
        }
        break;
    }
    case F_BR:
    case F_RET:
        for (u32 i = 0; i < 6; i++) { m.rn = regs_a[i]; add(v, m); }
        break;
    case F_SYS: add(v, m); break;
    case F_BRK:
        m.imm = 0; add(v, m); m.imm = 0xF000; add(v, m); m.imm = 0xFFFF; add(v, m);
        break;
    case F_FMOV_IMM: {
        static const u8 imms[] = {0x70, 0x00, 0x80, 0x1F, 0xFF, 0x60};
        for (u32 i = 0; i < NC_ARRAY_LEN(imms); i++) { m.rd = (u8)i; m.imm = imms[i]; add(v, m); }
        break;
    }
    }
}

/* For TK_OFFSET targets the PC-relative offset is in .imm, except for
 * tbz/tbnz where .imm holds the bit number and .target the offset. */
static i64 pcrel_of(const MInst *m) {
    if (m->tkind != TK_OFFSET) return 0;
    if (a64_insn_info[m->op].form == F_TB) return (i64)(i32)m->target;
    return m->imm;
}

void test_encode_differential(void) {
    if (!test_have_tool("llvm-mc")) {
        fprintf(stderr, "encode: SKIPPED (llvm-mc not found)\n");
        return;
    }
    MInstVec cases = {};
    u32 per_op[A64_NUM_REAL] = {};
    for (u32 op = 0; op < A64_NUM_REAL; op++) {
        u32 before = cases.len;
        gen(&cases, (A64Op)op);
        per_op[op] = cases.len - before;
    }
    for (u32 op = 0; op < A64_NUM_REAL; op++)
        CHECKF(per_op[op] > 0, "no encoding test cases for %s", a64_op_names[op]);

    const char *dir = test_tmpdir();
    char asm_path[512], out_path[512], cmd[1400];
    snprintf(asm_path, sizeof asm_path, "%s/ncc-test-encode.s", dir);
    snprintf(out_path, sizeof out_path, "%s/ncc-test-encode.out", dir);

    FILE *f = fopen(asm_path, "w");
    CHECK(f != nullptr);
    if (!f) return;
    u32 *words = nc_xrealloc(nullptr, sizeof(u32) * cases.len);
    char text[256];
    for (u32 i = 0; i < cases.len; i++) {
        u16 rt;
        i64 pc = pcrel_of(&cases.data[i]);
        words[i] = a64_encode(&cases.data[i], pc, &rt);
        a64_format(text, sizeof text, &cases.data[i], pc, nullptr, nullptr);
        fprintf(f, "\t%s\n", text);
    }
    fclose(f);

    snprintf(cmd, sizeof cmd,
             "llvm-mc -triple=aarch64-pc-windows-msvc -mattr=+v8.3a -show-encoding %s > %s 2>&1",
             asm_path, out_path);
    int rc = system(cmd);
    CHECKF(rc == 0, "llvm-mc failed, see %s", out_path);

    FILE *in = fopen(out_path, "r");
    CHECK(in != nullptr);
    if (!in) return;
    char line[512];
    u32 idx = 0, mismatches = 0;
    while (fgets(line, sizeof line, in)) {
        char *e = strstr(line, "encoding: [");
        if (!e) continue;
        unsigned b[4];
        if (sscanf(e, "encoding: [0x%x,0x%x,0x%x,0x%x]", &b[0], &b[1], &b[2], &b[3]) != 4) {
            CHECKF(false, "unparsable llvm-mc line: %s", line);
            continue;
        }
        u32 ref = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
        if (idx < cases.len && ref != words[idx]) {
            a64_format(text, sizeof text, &cases.data[idx], pcrel_of(&cases.data[idx]), nullptr,
                       nullptr);
            if (mismatches++ < 40)
                fprintf(stderr, "encode mismatch: %-40s ncc=%08x llvm-mc=%08x (%s)\n", text,
                        words[idx], ref, a64_op_names[cases.data[idx].op]);
        }
        idx++;
    }
    fclose(in);
    CHECKF(idx == cases.len, "llvm-mc produced %u encodings for %u instructions", idx, cases.len);
    CHECKF(mismatches == 0, "%u encoding mismatches", mismatches);
    test_checks += (int)cases.len;
    nc_xfree(words);
    vec_free(&cases);
}

static int cmp_u64(const void *a, const void *b) {
    u64 x = *(const u64 *)a, y = *(const u64 *)b;
    return x < y ? -1 : x > y;
}

void test_logical_imm(void) {
    /* Enumerate every architecturally valid (N, immr, imms) triple, decode it,
     * and require that the encoder accepts the value and reproduces it. The
     * number of distinct values is a known constant: 5334 for 64-bit and
     * 1302 for 32-bit registers. */
    for (u32 width = 32; width <= 64; width += 32) {
        u64 *vals = nc_xrealloc(nullptr, sizeof(u64) * 8192);
        u32 nv = 0;
        for (u32 n = 0; n <= (width == 64 ? 1u : 0u); n++)
            for (u32 imms = 0; imms < 64; imms++) {
                u32 combined = (n << 6) | (~imms & 0x3F);
                if (combined == 0) continue;
                u32 size = 1u << (31 - (u32)__builtin_clz(combined));
                if (size > width || (imms & (size - 1)) == size - 1) continue;
                for (u32 immr = 0; immr < size; immr++) {
                    u64 v = a64_decode_logical_imm(n, immr, imms, width);
                    vals[nv++] = v;
                    u32 n2, r2, s2;
                    bool ok = a64_encode_logical_imm(v, width, &n2, &r2, &s2);
                    CHECKF(ok, "w%u value %llx not encodable", width, (unsigned long long)v);
                    if (ok)
                        CHECKF(a64_decode_logical_imm(n2, r2, s2, width) == v,
                               "w%u value %llx round-trip", width, (unsigned long long)v);
                }
            }
        qsort(vals, nv, sizeof *vals, cmp_u64);
        u32 distinct = 0;
        for (u32 i = 0; i < nv; i++) distinct += (i == 0 || vals[i] != vals[i - 1]);
        CHECKF(distinct == (width == 64 ? 5334u : 1302u), "w%u: %u distinct values", width,
               distinct);
        nc_xfree(vals);
    }
    u32 n, r, s;
    CHECK(!a64_encode_logical_imm(0, 64, &n, &r, &s));
    CHECK(!a64_encode_logical_imm(~(u64)0, 64, &n, &r, &s));
    CHECK(!a64_encode_logical_imm(0x1234, 64, &n, &r, &s));
    CHECK(!a64_encode_logical_imm(0x100000000ull, 32, &n, &r, &s));
    CHECK(a64_encode_logical_imm(0xFF, 32, &n, &r, &s) && n == 0 && r == 0 && s == 7);
}
