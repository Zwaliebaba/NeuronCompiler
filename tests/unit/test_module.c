/* test_module.c — function finalisation: constant materialisation and
 * branch relaxation. */
#include "test.h"

#include "../../src/target/arm64/a64_module.h"

/* Evaluates a constant-materialisation sequence. */
static u64 EvalSeq(const MirFunc* _f, bool _w)
{
  u64 v = 0;
  for (u32 i = 0; i < _f->insts.len; i++)
  {
    const MirInst* mi = &_f->insts.data[i];
    u64 chunk = (u64)mi->imm << mi->amount;
    switch (mi->op)
    {
    case A64OpMOVZX:
    case A64OpMOVZW:
      v = chunk;
      break;
    case A64OpMOVNX:
    case A64OpMOVNW:
      v = ~chunk;
      break;
    case A64OpMOVKX:
    case A64OpMOVKW:
      v = (v & ~((u64)0xFFFF << mi->amount)) | chunk;
      break;
    case A64OpORRXri:
    case A64OpORRWri:
      v = (u64)mi->imm;
      break;
    default:
      return 0xBADBADBADull;
    }
  }
  return _w ? (v & 0xFFFFFFFFu) : v;
}

static void TestMovImm(void)
{
  static const u64 VALS[] = {0,
                             1,
                             0xFFFF,
                             0x10000,
                             0xFFFFFFFF,
                             0xFFFFFFF5,
                             0x12345678,
                             0xFFFFFFFFFFFFFFFFull,
                             0xFFFFFFFFFFFF1234ull,
                             0x5555555555555555ull,
                             0x123456789ABCDEF0ull,
                             0x0000FFFF00000000ull,
                             0x8000000000000000ull,
                             0xFFFF0000FFFF0000ull,
                             0x00FF00FF00FF00FFull};
  for (u32 i = 0; i < NC_ARRAY_LEN(VALS); i++)
    for (u32 w = 0; w <= 1; w++)
    {
      MirFunc f;
      MirFuncInit(&f, "t", true, (FrameDesc){.frameless = true});
      MirFuncMovImm(&f, 3, VALS[i], w);
      u64 want = w ? (VALS[i] & 0xFFFFFFFFu) : VALS[i];
      CHECKF(EvalSeq(&f, w) == want, "mov_imm %llx (w=%u)", (unsigned long long)VALS[i], w);
      CHECKF(f.insts.len >= 1 && f.insts.len <= (w ? 2u : 4u), "mov_imm %llx: %u insts", (unsigned long long)VALS[i], f.insts.len);
      for (u32 k = 0; k < f.insts.len; k++)
      {
        u16 rt;
        (void)A64Encode(&f.insts.data[k], 0, &rt); /* must be encodable */
      }
      MirFuncFree(&f);
    }
  /* Cheapest forms. */
  MirFunc f;
  MirFuncInit(&f, "t", true, (FrameDesc){.frameless = true});
  MirFuncMovImm(&f, 0, 0xFFFFFFFFFFFF1234ull, false);
  CHECK(f.insts.len == 1 && f.insts.data[0].op == A64OpMOVNX);
  VEC_CLEAR(&f.insts);
  MirFuncMovImm(&f, 0, 0x5555555555555555ull, false);
  CHECK(f.insts.len == 1 && f.insts.data[0].op == A64OpORRXri);
  MirFuncFree(&f);
}

static void TestRelaxation(void)
{
  Arena a;
  ArenaInit(&a);
  A64Module m;
  A64ModuleInit(&m, &a, "t.obj", "t.c", false);

  /* tbz reaches +-32 KiB, b.eq +-1 MiB: put the targets beyond both. */
  MirFunc f;
  MirFuncInit(&f, "relax", false, (FrameDesc){.frameless = true});
  u32 nearEnd = MirFuncNewLabel(&f), farEnd = MirFuncNewLabel(&f);
  MirFuncEmit(&f, (MirInst){.op = A64OpTBZ, .rd = 0, .imm = 3, .tkind = A64TargetKindLabel, .target = nearEnd});
  MirFuncEmit(&f, (MirInst){.op = A64OpBcc, .shift = A64CondEq, .tkind = A64TargetKindLabel, .target = farEnd});
  for (u32 i = 0; i < 9000; i++)
    MirFuncEmit(&f, (MirInst){.op = A64OpNOP});
  MirFuncLabel(&f, nearEnd);
  for (u32 i = 0; i < 270000; i++)
    MirFuncEmit(&f, (MirInst){.op = A64OpNOP});
  MirFuncLabel(&f, farEnd);
  MirFuncEmit(&f, (MirInst){.op = A64OpPseudoEpilogue});
  A64ModuleEmitFunction(&m, &f);

  /* Expected: tbnz w0, #3, +8 ; b near ; b.ne +8 ; b far ; nops ... */
  CoffSection* text = nullptr;
  for (u32 s = 1; s <= m.obj.sections.len; s++)
    if (strcmp(CoffSectionGet(&m.obj, s)->name, ".text$mn") == 0)
      text = CoffSectionGet(&m.obj, s);
  CHECK(text != nullptr);
  if (text)
  {
    const u8* c = text->data.data;
    u32 w0 = BufReadU32(c), w1 = BufReadU32(c + 4), w2 = BufReadU32(c + 8), w3 = BufReadU32(c + 12);
    CHECKF((w0 & 0xFF000000) == 0x37000000 && ((w0 >> 5) & 0x3FFF) == 2, "tbnz +8: %08x", w0);
    CHECKF((w1 & 0xFC000000) == 0x14000000, "b: %08x", w1);
    CHECKF((w1 & 0x3FFFFFF) * 4 == 4 * (9000 + 3), "b near offset: %08x", w1);
    CHECKF((w2 & 0xFF00001F) == 0x54000001 && ((w2 >> 5) & 0x7FFFF) == 2, "b.ne +8: %08x", w2);
    CHECKF((w3 & 0xFC000000) == 0x14000000 && (w3 & 0x3FFFFFF) == 9000 + 270000 + 1, "b far: %08x", w3);
    CHECK(text->data.len == 4 * (4 + 9000 + 270000 + 1));
  }
  MirFuncFree(&f);
  A64ModuleFree(&m);
  ArenaRelease(&a);
}

void TestModule(void)
{
  TestMovImm();
  TestRelaxation();
}
