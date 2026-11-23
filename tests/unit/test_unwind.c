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

typedef struct Built
{
  FramePlan plan;
  u8 code[1024];
  u32 len;
  u32 epilogs[2];
  u32 nepilogs;
  UnwindRecord rec;
} Built;

static void Emit(Built* _b, const MirInst* _mi)
{
  u16 rt;
  u32 w = A64Encode(_mi, 0, &rt);
  for (int i = 0; i < 4; i++)
    _b->code[_b->len++] = (u8)(w >> (8 * i));
}

static void Build(Built* _b, const FrameDesc* _d, u32 _nepilogs)
{
  memset(_b, 0, sizeof *_b);
  FramePlanBuild(_d, &_b->plan);
  MirInst seq[FRAME_MAX_STEPS + 1];
  u32 n = FramePrologue(&_b->plan, seq, 0);
  for (u32 i = 0; i < n; i++)
    Emit(_b, &seq[i]);
  MirInst nop = {.op = A64OpNOP};
  Emit(_b, &nop);
  for (u32 e = 0; e < _nepilogs; e++)
  {
    if (e)
      Emit(_b, &nop);
    _b->epilogs[_b->nepilogs++] = _b->len;
    u32 m = FrameEpilogue(&_b->plan, seq);
    for (u32 i = 0; i < m; i++)
      Emit(_b, &seq[i]);
  }
  UnwindBuild(&_b->plan, _b->len, _b->epilogs, _b->nepilogs, &_b->rec);
}

static bool Verify(Built* _b, char* _err, size_t _n)
{
  return UnwindVerify(&_b->plan, _b->code, _b->len, _b->epilogs, _b->nepilogs, &_b->rec, _err, _n);
}

void TestUnwind(void)
{
  static const u32 LOCALS_V[] = {0, 8, 48, 480, 496, 500, 3000, 4064, 4080, 5000, 70000, 2000000};
  static const u32 OUT_V[] = {0, 16, 64, 600};
  char err[256];
  static Built g_b;
  u32 total = 0, packed = 0, full = 0, failures = 0;

  for (u32 ng = 0; ng <= 10; ng++)
    for (u32 nf = 0; nf <= 8; nf++)
      for (u32 home = 0; home <= 1; home++)
        for (u32 dyn = 0; dyn <= 1; dyn++)
          for (u32 li = 0; li < NC_ARRAY_LEN(LOCALS_V); li++)
            for (u32 oi = 0; oi < NC_ARRAY_LEN(OUT_V); oi++)
              for (u32 ne = 1; ne <= 2; ne++)
              {
                FrameDesc d = {
                  .nGpr = (u8)ng, .nFpr = (u8)nf, .homeArgs = home, .dynamic = dyn, .locals = LOCALS_V[li], .outgoing = OUT_V[oi]};
                Build(&g_b, &d, ne);
                total++;
                if (g_b.rec.packed)
                  packed++;
                else
                  full++;
                if (!Verify(&g_b, err, sizeof err))
                {
                  if (failures++ < 10)
                    fprintf(stderr,
                            "unwind: gpr=%u fpr=%u home=%u dyn=%u locals=%u "
                            "out=%u epilogs=%u: %s\n",
                            ng, nf, home, dyn, LOCALS_V[li], OUT_V[oi], ne, err);
                }
                UnwindRecordFree(&g_b.rec);
              }
  CHECKF(failures == 0, "%u of %u frame shapes failed verification", failures, total);
  CHECK(packed > 0 && full > 0);
  g_testChecks += (int)total;

  /* Frameless leaf: no unwind data. */
  FrameDesc leaf = {.frameless = true};
  Build(&g_b, &leaf, 1);
  CHECK(g_b.rec.none);
  CHECK(Verify(&g_b, err, sizeof err));

  /* A typical small non-leaf function must use packed data, matching what
     * MSVC emits: stp x29, x30, [sp, #-16]!; mov x29, sp. */
  FrameDesc small = {.locals = 0};
  Build(&g_b, &small, 1);
  CHECK(g_b.rec.packed);
  CHECK(g_b.plan.nsteps == 2 && g_b.plan.steps[0].kind == FrameStepKindFplrPre && g_b.plan.steps[0].off == 16);
  UnwindRecordFree(&g_b.rec);

  /* Microsoft's documented example 1 shape: x19 saved, 2064 bytes of locals
     * and fp/lr: str x19,[sp,#-16]!; sub sp,sp,#0x810; stp fp,lr,[sp];
     * mov fp,sp. The documented packed word for a 492-byte function is
     * 0x416101ED. */
  FrameDesc ex1 = {.nGpr = 1, .locals = 0x810 - 16};
  FramePlanBuild(&ex1, &g_b.plan);
  CHECK(g_b.plan.nsteps == 4);
  CHECK(g_b.plan.steps[0].kind == FrameStepKindStrXPre && g_b.plan.steps[0].off == 16);
  CHECK(g_b.plan.steps[1].kind == FrameStepKindAlloc && g_b.plan.steps[1].off == 0x810);
  CHECK(g_b.plan.steps[2].kind == FrameStepKindFplr && g_b.plan.steps[2].off == 0);
  CHECK(g_b.plan.steps[3].kind == FrameStepKindSetFp);
  {
    u32 epi = 492 - 4 * FrameEpilogueLen(&g_b.plan);
    UnwindRecord r;
    UnwindBuild(&g_b.plan, 492, &epi, 1, &r);
    CHECKF(r.packed && r.packedWord == 0x416101EDu, "packed word %08x", r.packedWord);
  }

  /* Mutations must be rejected. */
  u32 mutations = 0, caught = 0;
  for (u32 ng = 0; ng <= 4; ng++)
    for (u32 nf = 0; nf <= 2; nf += 2)
      for (u32 li = 0; li < NC_ARRAY_LEN(LOCALS_V); li += 3)
      {
        FrameDesc d = {.nGpr = (u8)ng, .nFpr = (u8)nf, .locals = LOCALS_V[li]};
        Build(&g_b, &d, 1);
        u32 plen = FramePrologueLen(&g_b.plan);
        for (u32 k = 0; k < plen; k++)
        {
          u32 w = BufReadU32(g_b.code + (size_t)4 * k);
          u32 flip;
          if ((w & 0x3A000000) == 0x28000000 || /* pair imm7 */
              (w & 0x3B200000) == 0x38000000)   /* imm9 */
            flip = 1u << 15;
          else if ((w & 0x3B000000) == 0x39000000)
            flip = 1u << 10; /* uimm12 */
          else if ((w & 0x9F000000) == 0x91000000)
            flip = 1u << 14; /* add/sub imm */
          else
            continue;
          u32 m = w ^ flip;
          for (int i = 0; i < 4; i++)
            g_b.code[4 * k + (u32)i] = (u8)(m >> (8 * i));
          mutations++;
          if (!Verify(&g_b, err, sizeof err))
            caught++;
          for (int i = 0; i < 4; i++)
            g_b.code[4 * k + (u32)i] = (u8)(w >> (8 * i));
        }
        /* Swap the first two prologue instructions. */
        if (plen >= 2)
        {
          u8 t[4];
          memcpy(t, g_b.code, 4);
          memcpy(g_b.code, g_b.code + 4, 4);
          memcpy(g_b.code + 4, t, 4);
          mutations++;
          if (!Verify(&g_b, err, sizeof err))
            caught++;
        }
        UnwindRecordFree(&g_b.rec);
      }
  CHECKF(mutations > 50 && caught == mutations, "verifier caught %u of %u mutations", caught, mutations);

  /* Packed-word mutations: every change to RegI, RegF, H, CR or FrameSize
     * must be rejected. Changing RegI or RegF only drops or adds a register
     * restore; this is caught only because the RunSweep clobbers saved
     * callee-saved registers after the prologue. Without that, nothing would
     * ever change the register and the omission would go unnoticed. */
  u32 pkMut = 0, pkCaught = 0;
  for (u32 ng = 0; ng <= 10; ng++)
    for (u32 nf = 0; nf <= 8; nf++)
    {
      FrameDesc d = {.nGpr = (u8)ng, .nFpr = (u8)nf, .locals = 32};
      Build(&g_b, &d, 1);
      if (!g_b.rec.packed)
      {
        UnwindRecordFree(&g_b.rec);
        continue;
      }
      u32 orig = g_b.rec.packedWord;
      static const struct
      {
        u32 shift, width;
      } FIELDS[] = {{13, 3}, {16, 4}, {20, 1}, {21, 2}, {23, 9}};
      for (u32 fi = 0; fi < NC_ARRAY_LEN(FIELDS); fi++)
        for (u32 v = 0; v < (1u << FIELDS[fi].width); v++)
        {
          u32 mask = ((1u << FIELDS[fi].width) - 1) << FIELDS[fi].shift;
          u32 m = (orig & ~mask) | v << FIELDS[fi].shift;
          if (m == orig)
            continue;
          g_b.rec.packedWord = m;
          pkMut++;
          if (!Verify(&g_b, err, sizeof err))
            pkCaught++;
        }
      g_b.rec.packedWord = orig;
      UnwindRecordFree(&g_b.rec);
    }
  CHECKF(pkMut > 500 && pkCaught == pkMut, "verifier caught %u of %u packed-word mutations", pkCaught, pkMut);

  /* Record mutations: corrupt each unwind-code byte of a full record. */
  FrameDesc big = {.nGpr = 4, .nFpr = 2, .homeArgs = true, .locals = 9000, .outgoing = 32};
  Build(&g_b, &big, 2);
  CHECK(!g_b.rec.packed && g_b.rec.xdata.len > 8);
  u32 recMut = 0, recCaught = 0;
  u32 hdr = 4 + 4 * g_b.nepilogs;
  for (u32 i = hdr; i < g_b.rec.xdata.len; i++)
  {
    u8 orig = g_b.rec.xdata.data[i];
    g_b.rec.xdata.data[i] = orig ^ 0x01;
    recMut++;
    if (!Verify(&g_b, err, sizeof err))
      recCaught++;
    g_b.rec.xdata.data[i] = orig;
  }
  /* Exactly two mutations are semantically equivalent and must survive:
     * the two bytes of the leading alloc_m. In a shape-B frame that
     * allocation is immediately followed (in unwind order) by set_fp, which
     * restores sp from x29, so its size never affects the unwound state - for
     * our verifier or for RtlVirtualUnwind. The two padding bytes after the
     * final end (0xE4) are never executed, but a flipped bit turns them into
     * end_c (0xE5), which the verifier rejects outright because ncc never
     * emits function fragments. Everything else must be caught. */
  CHECKF(recCaught == recMut - 2, "verifier caught %u of %u record mutations", recCaught, recMut);
  UnwindRecordFree(&g_b.rec);
}
