#include "test.h"

#include "../../src/core/arena.h"
#include "../../src/core/buf.h"
#include "../../src/core/crc32.h"
#include "../../src/core/vec.h"

void TestCore(void)
{
  Arena a;
  ArenaInit(&a);
  ArenaMark m0 = ArenaGetMark(&a);
  for (u32 i = 0; i < 10000; i++)
  {
    u64* p = ArenaAlloc(&a, sizeof(u64) * (i % 7 + 1), 8);
    CHECK(((uintptr_t)p & 7) == 0);
    *p = i;
  }
  void* big = ArenaAlloc(&a, 1 << 20, 64);
  CHECK(((uintptr_t)big & 63) == 0);
  memset(big, 0xAB, 1 << 20);
  char* s = ArenaPrintf(&a, "%s-%d", "x", 42);
  CHECK(strcmp(s, "x-42") == 0);
  ArenaReset(&a, m0);
  CHECK(a.head == nullptr);
  ArenaRelease(&a);

  VEC(u32) v = {};
  for (u32 i = 0; i < 1000; i++)
    VEC_PUSH(&v, i * 3);
  CHECK(v.len == 1000 && v.data[999] == 2997);
  VEC_FREE(&v);

  Buf b = {};
  BufU8(&b, 1);
  BufU16(&b, 0x0302);
  BufU32(&b, 0x07060504);
  BufU64(&b, 0x0F0E0D0C0B0A0908ull);
  CHECK(b.len == 15);
  for (u32 i = 0; i < 15; i++)
    CHECK(b.data[i] == i + 1);
  BufAlign(&b, 16, 0);
  CHECK(b.len == 16);
  BufPutU32(&b, 0, 0xDEADBEEF);
  CHECK(BufGetU32(&b, 0) == 0xDEADBEEF);
  BufFree(&b);

  /* Known CRC-32 check value: crc32("123456789") = 0xCBF43926. */
  CHECK((Crc32Jam("123456789", 9) ^ 0xFFFFFFFFu) == 0xCBF43926u);
}
