/* test_encode.c — differential test of the ARM64 encoder and printer.
 *
 * For every real instruction in arm64.def we generate a set of operand
 * combinations, encode them, print them, assemble the printed text with
 * llvm-mc and require byte-identical results. This checks the encoder and
 * the printer against an independent assembler in one pass. */
#include "test.h"

#include "../../src/core/vec.h"
#include "../../src/obj/coff/coff.h"
#include "../../src/target/arm64/a64.h"

#include <stdlib.h>

typedef VEC(MirInst) MirInstVec;

static const u8 REGS_A[] = {0, 1, 7, 17, 29, 30, 31};

static void Add(MirInstVec* _v, MirInst _mi)
{
  VEC_PUSH(_v, _mi);
}

static MirInst Mk(A64Op _op)
{
  return (MirInst){.op = _op};
}

static void GenRegs3(MirInstVec* _v, A64Op _op, MirInst _base)
{
  for (u32 i = 0; i < NC_ARRAY_LEN(REGS_A); i++)
  {
    MirInst m = _base;
    m.op = _op;
    m.rd = REGS_A[i];
    m.rn = REGS_A[(i + 2) % NC_ARRAY_LEN(REGS_A)];
    m.rm = REGS_A[(i + 4) % NC_ARRAY_LEN(REGS_A)];
    m.ra = REGS_A[(i + 5) % NC_ARRAY_LEN(REGS_A)];
    Add(_v, m);
  }
}

static void Gen(MirInstVec* _v, A64Op _op)
{
  const A64InsnInfo* ii = &A64_INSN_INFO[_op];
  bool x64 = (ii->bits >> 31) != 0;
  u32 width = x64 ? 64 : 32;
  MirInst m = Mk(_op);
  switch (ii->form)
  {
  case A64FormPseudo:
    break;
  case A64FormAddsubImm:
  case A64FormCmpImm:
  {
    static const i64 IMMS[] = {0, 1, 4095, 4096, 0x123000, 0xFFF000};
    for (u32 i = 0; i < NC_ARRAY_LEN(IMMS); i++)
    {
      m.rd = REGS_A[i % 7];
      m.rn = REGS_A[(i + 3) % 7];
      m.imm = IMMS[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormMovSp:
    m.rd = 31;
    m.rn = 0;
    Add(_v, m);
    m.rd = 29;
    m.rn = 31;
    Add(_v, m);
    m.rd = 31;
    m.rn = 29;
    Add(_v, m);
    break;
  case A64FormLogicImm:
  case A64FormTstImm:
  {
    static const u64 VALS[] = {1,
                               0xFF,
                               0xFFFF0000FFFF0000ull,
                               0x5555555555555555ull,
                               0x7FFFFFFFFFFFFFFEull,
                               0x00000000F0F0F0F0ull,
                               0x7FFFFFFF,
                               0x8000000000000000ull,
                               0x3C,
                               0xFFFFFFFFFFFFFF00ull};
    for (u32 i = 0; i < NC_ARRAY_LEN(VALS); i++)
    {
      u32 n, r, s;
      if (!A64EncodeLogicalImm(VALS[i], width, &n, &r, &s))
        continue;
      m.rd = REGS_A[i % 7];
      if (ii->kd == A64KindX && m.rd == 31)
        m.rd = 3; /* ands xzr is tst, printed differently */
      m.rn = REGS_A[(i + 1) % 6];
      m.imm = (i64)VALS[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormMovw:
  {
    static const i64 IMMS[] = {0, 1, 0xFFFF, 0x1234};
    for (u32 s = 0; s < width; s += 16)
      for (u32 i = 0; i < NC_ARRAY_LEN(IMMS); i++)
      {
        m.rd = REGS_A[(i + s / 16) % 7];
        m.imm = IMMS[i];
        m.amount = (u8)s;
        Add(_v, m);
      }
    break;
  }
  case A64FormAdr:
  {
    static const i64 ADR[] = {0, 4, -4, 1048572, -1048576, 12345};
    static const i64 ADRP[] = {0, 4096, -4096, 0x7FFFF000, -0x80000000LL};
    const i64* t = _op == A64OpADR ? ADR : ADRP;
    u32 n = _op == A64OpADR ? NC_ARRAY_LEN(ADR) : NC_ARRAY_LEN(ADRP);
    for (u32 i = 0; i < n; i++)
    {
      m.rd = REGS_A[i % 7];
      m.tkind = A64TargetKindOffset;
      m.imm = t[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormBitfield:
  {
    u32 hi = width - 1;
    u32 pairs[][2] = {{0, 0}, {3, 7}, {hi, hi - 1}, {1, hi}};
    for (u32 i = 0; i < NC_ARRAY_LEN(pairs); i++)
    {
      m.rd = REGS_A[i];
      m.rn = REGS_A[i + 2];
      m.imm = pairs[i][0];
      m.amount = (u8)pairs[i][1];
      Add(_v, m);
    }
    break;
  }
  case A64FormLslImm:
  case A64FormLsrImm:
  {
    u32 amts[] = {1, 5, width - 1};
    for (u32 i = 0; i < 3; i++)
    {
      m.rd = REGS_A[i];
      m.rn = REGS_A[i + 3];
      m.amount = (u8)amts[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormExtend:
  case A64FormDp1:
  case A64FormFp1:
  case A64FormDp3:
  case A64FormDp3Noa:
  case A64FormDp2:
  case A64FormFp2:
  case A64FormFcmp:
    GenRegs3(_v, _op, m);
    break;
  case A64FormLogicReg:
  case A64FormAddsubReg:
  case A64FormCmpReg:
  case A64FormMovReg:
  case A64FormNeg:
  {
    bool arith =
      ii->form == A64FormAddsubReg || ii->form == A64FormNeg || (ii->form == A64FormCmpReg && _op != A64OpTSTXrs && _op != A64OpTSTWrs);
    bool isMov = _op == A64OpMOVXr || _op == A64OpMOVWr;
    for (u32 sh = 0; sh < 4; sh++)
    {
      if (arith && sh == A64ShiftRor)
        continue;
      u32 amts[] = {0, 3, width - 1};
      for (u32 a = 0; a < 3; a++)
      {
        if (isMov && (amts[a] || sh != A64ShiftLsl))
          continue;
        m.rd = REGS_A[(sh + a) % 7];
        m.rn = REGS_A[(sh + a + 2) % 7];
        m.rm = REGS_A[(sh + a + 4) % 7];
        if (ii->form == A64FormLogicReg && _op == A64OpANDSXrs && m.rd == 31)
          m.rd = 2;
        if (ii->form == A64FormLogicReg && _op == A64OpANDSWrs && m.rd == 31)
          m.rd = 2;
        if ((_op == A64OpSUBSXrs || _op == A64OpSUBSWrs || _op == A64OpADDSXrs || _op == A64OpADDSWrs) && m.rd == 31)
          m.rd = 2;
        if ((_op == A64OpSUBXrs || _op == A64OpSUBWrs) && m.rn == 31)
          m.rn = 4; /* neg alias */
        if ((_op == A64OpORRXrs || _op == A64OpORRWrs) && m.rn == 31)
          m.rn = 4; /* mov alias */
        if ((_op == A64OpORNXrs) && m.rn == 31)
          m.rn = 4; /* mvn alias */
        m.shift = (u8)sh;
        m.amount = (u8)amts[a];
        Add(_v, m);
      }
    }
    break;
  }
  case A64FormAddsubExt:
  {
    static const A64Extend EXS[] = {A64ExtendUxtw, A64ExtendSxtw, A64ExtendUxtx, A64ExtendSxtx, A64ExtendUxtb, A64ExtendSxth};
    static const u8 RDS[] = {31, 0, 31, 5, 7, 17};
    static const u8 RNS[] = {31, 31, 1, 6, 31, 2};
    for (u32 i = 0; i < NC_ARRAY_LEN(EXS); i++)
      for (u8 amt = 0; amt <= 4; amt += 2)
      {
        m.rd = RDS[i];
        m.rn = RNS[i];
        m.rm = (u8)(i + 9);
        m.shift = EXS[i];
        m.amount = amt;
        Add(_v, m);
      }
    break;
  }
  case A64FormCsel:
  case A64FormCset:
  {
    static const A64Cond CS[] = {A64CondEq, A64CondNe, A64CondLt, A64CondGt, A64CondHs, A64CondLs, A64CondVs};
    for (u32 i = 0; i < NC_ARRAY_LEN(CS); i++)
    {
      m.rd = REGS_A[i];
      m.rn = REGS_A[(i + 1) % 7];
      m.rm = REGS_A[(i + 2) % 7];
      if (ii->form == A64FormCsel && (_op == A64OpCSINCX || _op == A64OpCSINVX || _op == A64OpCSNEGX || _op == A64OpCSINCW) && m.rn == m.rm)
        m.rm = 4;
      if (ii->form == A64FormCsel && m.rn == 31 && m.rm == 31)
        m.rm = 4;
      m.shift = CS[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormLdstUimm:
  {
    i64 s = (i64)1 << ii->accessSizeLog2;
    i64 offs[] = {0, s, 4095 * s, 17 * s};
    for (u32 i = 0; i < 4; i++)
    {
      m.rd = REGS_A[i + 1];
      m.rn = i == 0 ? 31 : REGS_A[i + 2];
      m.imm = offs[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormLdstPre:
  case A64FormLdstPost:
  case A64FormLdstUnscaled:
  {
    static const i64 OFFS[] = {-256, -16, 0, 8, 255};
    for (u32 i = 0; i < NC_ARRAY_LEN(OFFS); i++)
    {
      m.rd = (u8)(i + 1);
      m.rn = i == 1 ? 31 : (u8)(i + 10);
      m.imm = OFFS[i];
      Add(_v, m);
    }
    break;
  }
  case A64FormLdstReg:
  {
    static const A64Extend EXS[] = {A64ExtendUxtw, A64ExtendSxtw, A64ExtendUxtx, A64ExtendSxtx};
    for (u32 i = 0; i < 4; i++)
      for (u8 s = 0; s <= 1; s++)
      {
        m.rd = (u8)(i + 1);
        m.rn = i == 2 ? 31 : (u8)(i + 10);
        m.rm = (u8)(i + 20);
        m.shift = EXS[i];
        m.amount = s;
        Add(_v, m);
      }
    break;
  }
  case A64FormLdstPair:
  case A64FormLdstPairPre:
  case A64FormLdstPairPost:
  {
    i64 s = (i64)1 << ii->accessSizeLog2;
    i64 ks[] = {-64, -2, 0, 1, 63};
    for (u32 i = 0; i < NC_ARRAY_LEN(ks); i++)
    {
      m.rd = (u8)(19 + i);
      m.ra = (u8)(i == 2 ? 30 : 1 + i);
      m.rn = i < 2 ? 31 : (u8)(9 + i);
      m.imm = ks[i] * s;
      Add(_v, m);
    }
    break;
  }
  case A64FormLdrLit:
  case A64FormB:
  case A64FormBcond:
  case A64FormCb:
  {
    i64 lim = A64BranchRange(_op);
    i64 offs[] = {0, 4, -4, lim - 4, -lim, 4096};
    for (u32 i = 0; i < NC_ARRAY_LEN(offs); i++)
    {
      m.tkind = A64TargetKindOffset;
      m.imm = offs[i];
      m.rd = REGS_A[i];
      m.shift = (u8)(i * 3 % 14);
      Add(_v, m);
    }
    break;
  }
  case A64FormTb:
  {
    static const i64 BITS[] = {0, 5, 31, 32, 63};
    static const i64 OFFS[] = {0, 32764, -32768, 8, -4};
    for (u32 i = 0; i < 5; i++)
    {
      m.rd = REGS_A[i];
      m.imm = BITS[i];
      m.tkind = A64TargetKindOffset;
      m.target = 0;
      MirInst t = m;
      t.imm = BITS[i];
      /* pcrel travels separately; stash it in .target for the driver below */
      t.target = (u32)(OFFS[i] & 0xFFFFFFFF);
      Add(_v, t);
    }
    break;
  }
  case A64FormBr:
  case A64FormRet:
    for (u32 i = 0; i < 6; i++)
    {
      m.rn = REGS_A[i];
      Add(_v, m);
    }
    break;
  case A64FormSys:
    Add(_v, m);
    break;
  case A64FormBrk:
    m.imm = 0;
    Add(_v, m);
    m.imm = 0xF000;
    Add(_v, m);
    m.imm = 0xFFFF;
    Add(_v, m);
    break;
  case A64FormFmovImm:
  {
    static const u8 IMMS[] = {0x70, 0x00, 0x80, 0x1F, 0xFF, 0x60};
    for (u32 i = 0; i < NC_ARRAY_LEN(IMMS); i++)
    {
      m.rd = (u8)i;
      m.imm = IMMS[i];
      Add(_v, m);
    }
    break;
  }
  }
}

/* For A64TargetKindOffset targets the PC-relative offset is in .imm, except for
 * tbz/tbnz where .imm holds the bit number and .target the offset. */
static i64 PcrelOf(const MirInst* _m)
{
  if (_m->tkind != A64TargetKindOffset)
    return 0;
  if (A64_INSN_INFO[_m->op].form == A64FormTb)
    return (i64)(i32)_m->target;
  return _m->imm;
}

void TestEncodeDifferential(void)
{
  if (!TestHaveTool("llvm-mc"))
  {
    fprintf(stderr, "encode: SKIPPED (llvm-mc not found)\n");
    return;
  }
  MirInstVec cases = {};
  u32 perOp[A64OpRealCount] = {};
  for (u32 op = 0; op < A64OpRealCount; op++)
  {
    u32 before = cases.len;
    Gen(&cases, (A64Op)op);
    perOp[op] = cases.len - before;
  }
  for (u32 op = 0; op < A64OpRealCount; op++)
    CHECKF(perOp[op] > 0, "no encoding test cases for %s", A64_OP_NAMES[op]);

  const char* dir = TestTmpdir();
  char asmPath[512], outPath[512], cmd[1400];
  snprintf(asmPath, sizeof asmPath, "%s/ncc-test-encode.s", dir);
  snprintf(outPath, sizeof outPath, "%s/ncc-test-encode.out", dir);

  FILE* f = fopen(asmPath, "w");
  CHECK(f != nullptr);
  if (!f)
    return;
  u32* words = NcXrealloc(nullptr, sizeof(u32) * cases.len);
  char text[256];
  for (u32 i = 0; i < cases.len; i++)
  {
    u16 rt;
    i64 pc = PcrelOf(&cases.data[i]);
    words[i] = A64Encode(&cases.data[i], pc, &rt);
    A64Format(text, sizeof text, &cases.data[i], pc, nullptr, nullptr);
    fprintf(f, "\t%s\n", text);
  }
  fclose(f);

  snprintf(cmd, sizeof cmd, "llvm-mc -triple=aarch64-pc-windows-msvc -mattr=+v8.3a -show-encoding %s > %s 2>&1", asmPath, outPath);
  int rc = system(cmd);
  CHECKF(rc == 0, "llvm-mc failed, see %s", outPath);

  FILE* in = fopen(outPath, "r");
  CHECK(in != nullptr);
  if (!in)
    return;
  char line[512];
  u32 idx = 0, mismatches = 0;
  while (fgets(line, sizeof line, in))
  {
    char* e = strstr(line, "encoding: [");
    if (!e)
      continue;
    unsigned b[4];
    if (sscanf(e, "encoding: [0x%x,0x%x,0x%x,0x%x]", &b[0], &b[1], &b[2], &b[3]) != 4)
    {
      CHECKF(false, "unparsable llvm-mc line: %s", line);
      continue;
    }
    u32 ref = b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24;
    if (idx < cases.len && ref != words[idx])
    {
      A64Format(text, sizeof text, &cases.data[idx], PcrelOf(&cases.data[idx]), nullptr, nullptr);
      if (mismatches++ < 40)
        fprintf(stderr, "encode mismatch: %-40s ncc=%08x llvm-mc=%08x (%s)\n", text, words[idx], ref, A64_OP_NAMES[cases.data[idx].op]);
    }
    idx++;
  }
  fclose(in);
  CHECKF(idx == cases.len, "llvm-mc produced %u encodings for %u instructions", idx, cases.len);
  CHECKF(mismatches == 0, "%u encoding mismatches", mismatches);
  g_testChecks += (int)cases.len;
  NcXfree(words);
  VEC_FREE(&cases);
}

static int CmpU64(const void* _a, const void* _b)
{
  u64 x = *(const u64*)_a, y = *(const u64*)_b;
  return x < y ? -1 : x > y;
}

void TestLogicalImm(void)
{
  /* Enumerate every architecturally valid (N, immr, imms) triple, decode it,
     * and require that the encoder accepts the value and reproduces it. The
     * number of distinct values is a known constant: 5334 for 64-bit and
     * 1302 for 32-bit registers. */
  for (u32 width = 32; width <= 64; width += 32)
  {
    u64* vals = NcXrealloc(nullptr, sizeof(u64) * 8192);
    u32 nv = 0;
    for (u32 n = 0; n <= (width == 64 ? 1u : 0u); n++)
      for (u32 imms = 0; imms < 64; imms++)
      {
        u32 combined = (n << 6) | (~imms & 0x3F);
        if (combined == 0)
          continue;
        u32 size = 1u << (31 - NC_CLZ32_NONZERO(combined));
        if (size > width || (imms & (size - 1)) == size - 1)
          continue;
        for (u32 immr = 0; immr < size; immr++)
        {
          u64 v = A64DecodeLogicalImm(n, immr, imms, width);
          vals[nv++] = v;
          u32 n2, r2, s2;
          bool ok = A64EncodeLogicalImm(v, width, &n2, &r2, &s2);
          CHECKF(ok, "w%u value %llx not encodable", width, (unsigned long long)v);
          if (ok)
            CHECKF(A64DecodeLogicalImm(n2, r2, s2, width) == v, "w%u value %llx round-trip", width, (unsigned long long)v);
        }
      }
    qsort(vals, nv, sizeof *vals, CmpU64);
    u32 distinct = 0;
    for (u32 i = 0; i < nv; i++)
      distinct += (i == 0 || vals[i] != vals[i - 1]);
    CHECKF(distinct == (width == 64 ? 5334u : 1302u), "w%u: %u distinct values", width, distinct);
    NcXfree(vals);
  }
  u32 n, r, s;
  CHECK(!A64EncodeLogicalImm(0, 64, &n, &r, &s));
  CHECK(!A64EncodeLogicalImm(~(u64)0, 64, &n, &r, &s));
  CHECK(!A64EncodeLogicalImm(0x1234, 64, &n, &r, &s));
  CHECK(!A64EncodeLogicalImm(0x100000000ull, 32, &n, &r, &s));
  CHECK(A64EncodeLogicalImm(0xFF, 32, &n, &r, &s) && n == 0 && r == 0 && s == 7);
}

/* ---- symbol-operand forms ------------------------------------------------
 * The differential test above only covers instructions without symbols,
 * because llvm-mc -show-encoding prints fixup bits as placeholders. This one
 * assembles the symbolic forms into a COFF object with llvm-mc and compares
 * the instruction words and relocation types with ours. It exists because
 * the first Windows CI run found that `add ..., :secrel_hi12:sym` lacked the
 * lsl #12 bit: link.exe rejected it (LNK2040), lld-link did not. */

static const char* const RELOC_NAMES[] = {
  [IMAGE_REL_ARM64_PAGEBASE_REL21] = "IMAGE_REL_ARM64_PAGEBASE_REL21",
  [IMAGE_REL_ARM64_REL21] = "IMAGE_REL_ARM64_REL21",
  [IMAGE_REL_ARM64_PAGEOFFSET_12A] = "IMAGE_REL_ARM64_PAGEOFFSET_12A",
  [IMAGE_REL_ARM64_PAGEOFFSET_12L] = "IMAGE_REL_ARM64_PAGEOFFSET_12L",
  [IMAGE_REL_ARM64_SECREL_LOW12A] = "IMAGE_REL_ARM64_SECREL_LOW12A",
  [IMAGE_REL_ARM64_SECREL_HIGH12A] = "IMAGE_REL_ARM64_SECREL_HIGH12A",
  [IMAGE_REL_ARM64_SECREL_LOW12L] = "IMAGE_REL_ARM64_SECREL_LOW12L",
  [IMAGE_REL_ARM64_BRANCH26] = "IMAGE_REL_ARM64_BRANCH26",
  [IMAGE_REL_ARM64_BRANCH19] = "IMAGE_REL_ARM64_BRANCH19",
  [IMAGE_REL_ARM64_BRANCH14] = "IMAGE_REL_ARM64_BRANCH14",
};

static const char* SymCb(void* _ctx, u32 _sym)
{
  (void)_ctx;
  (void)_sym;
  return "ext_sym";
}

void TestEncodeSymbolic(void)
{
  if (!TestHaveTool("llvm-mc") || !TestHaveTool("llvm-objdump"))
  {
    fprintf(stderr, "encode_symbolic: SKIPPED (llvm-mc or llvm-objdump not found)\n");
    return;
  }
  const MirInst cases[] = {
    {.op = A64OpADRP, .rd = 8, .tkind = A64TargetKindSym},
    {.op = A64OpADR, .rd = 3, .tkind = A64TargetKindSym},
    {.op = A64OpADDXri, .rd = 1, .rn = 1, .tkind = A64TargetKindSym},
    {.op = A64OpADDXri, .rd = 8, .rn = 8, .tkind = A64TargetKindSym, .smod = A64SymModSecrelHi12},
    {.op = A64OpADDXri, .rd = 8, .rn = 8, .tkind = A64TargetKindSym, .smod = A64SymModSecrelLo12},
    {.op = A64OpLDRXui, .rd = 16, .rn = 16, .tkind = A64TargetKindSym},
    {.op = A64OpLDRWui, .rd = 9, .rn = 9, .tkind = A64TargetKindSym},
    {.op = A64OpSTRXui, .rd = 2, .rn = 5, .tkind = A64TargetKindSym},
    {.op = A64OpLDRBBui, .rd = 2, .rn = 5, .tkind = A64TargetKindSym},
    {.op = A64OpLDRWui, .rd = 9, .rn = 8, .tkind = A64TargetKindSym, .smod = A64SymModSecrelLo12},
    {.op = A64OpSTRWui, .rd = 0, .rn = 8, .tkind = A64TargetKindSym, .smod = A64SymModSecrelLo12},
    {.op = A64OpBL, .tkind = A64TargetKindSym},
    {.op = A64OpB, .tkind = A64TargetKindSym},
    {.op = A64OpBcc, .shift = A64CondNe, .tkind = A64TargetKindSym},
    {.op = A64OpCBZX, .rd = 4, .tkind = A64TargetKindSym},
    {.op = A64OpTBNZ, .rd = 4, .imm = 7, .tkind = A64TargetKindSym},
  };
  u32 n = NC_ARRAY_LEN(cases);
  const char* dir = TestTmpdir();
  char sPath[512], oPath[512], dPath[512], cmd[4096];
  snprintf(sPath, sizeof sPath, "%s/ncc-test-sym.s", dir);
  snprintf(oPath, sizeof oPath, "%s/ncc-test-sym.obj", dir);
  snprintf(dPath, sizeof dPath, "%s/ncc-test-sym.dis", dir);
  FILE* f = fopen(sPath, "w");
  CHECK(f != nullptr);
  if (!f)
    return;
  u32 words[32];
  u16 rtypes[32];
  char text[256];
  for (u32 i = 0; i < n; i++)
  {
    words[i] = A64Encode(&cases[i], 0, &rtypes[i]);
    A64Format(text, sizeof text, &cases[i], 0, SymCb, nullptr);
    fprintf(f, "\t%s\n", text);
  }
  fclose(f);
  snprintf(cmd, sizeof cmd,
           "llvm-mc -triple=aarch64-pc-windows-msvc -filetype=obj %s -o %s && "
           "llvm-objdump -dr %s > %s",
           sPath, oPath, oPath, dPath);
  CHECKF(system(cmd) == 0, "llvm-mc/llvm-objdump failed on %s", sPath);

  FILE* in = fopen(dPath, "r");
  CHECK(in != nullptr);
  if (!in)
    return;
  char line[512];
  u32 wi = 0, ri = 0;
  while (fgets(line, sizeof line, in))
  {
    unsigned off, w;
    char rel[96];
    if (sscanf(line, " %x: %8x", &off, &w) == 2 && strstr(line, "\t") && off / 4 == wi && wi < n)
    {
      CHECKF(w == words[wi], "symbolic %s: ncc=%08x llvm-mc=%08x", A64_OP_NAMES[cases[wi].op], words[wi], w);
      wi++;
    }
    else if (sscanf(line, " %x: %95s", &off, rel) == 2 && strncmp(rel, "IMAGE_REL_", 10) == 0)
    {
      u32 k = off / 4;
      CHECKF(k < n && rtypes[k] < NC_ARRAY_LEN(RELOC_NAMES) && RELOC_NAMES[rtypes[k]] && strcmp(RELOC_NAMES[rtypes[k]], rel) == 0,
             "symbolic %s: relocation ncc=%s llvm-mc=%s", k < n ? A64_OP_NAMES[cases[k].op] : "?",
             k < n && rtypes[k] < NC_ARRAY_LEN(RELOC_NAMES) && RELOC_NAMES[rtypes[k]] ? RELOC_NAMES[rtypes[k]] : "?", rel);
      ri++;
    }
  }
  fclose(in);
  CHECKF(wi == n, "llvm-objdump showed %u of %u instructions", wi, n);
  CHECKF(ri == n, "llvm-objdump showed %u of %u relocations", ri, n);
}
