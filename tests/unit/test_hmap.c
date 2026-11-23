#include "test.h"

#include "../../src/core/hash.h"

/* A well-distributed map, checked against a dense reference array. */
#define HMAP_NAME U32Map
#define HMAP_K u32
#define HMAP_V u32
#define HMAP_HASH(k) HashMix64(k)
#define HMAP_EQ(a, b) ((a) == (b))
#include "../../src/core/hmap.inc"

/* Every key collides: one probe chain, so Robin Hood displacement and
 * backward-shift deletion are exercised on every operation. */
#define HMAP_NAME CollideMap
#define HMAP_K u32
#define HMAP_V u32
#define HMAP_HASH(k) ((void)(k), 0ull)
#define HMAP_EQ(a, b) ((a) == (b))
#include "../../src/core/hmap.inc"

enum
{
  REF_KEYS = 4096,
  NO_VALUE = 0xFFFFFFFFu
};

static u64 g_rng = 0x9E3779B97F4A7C15ull; /* deterministic: the test must not depend on the run */

static u32 NextRandom(void)
{
  g_rng ^= g_rng << 13;
  g_rng ^= g_rng >> 7;
  g_rng ^= g_rng << 17;
  return (u32)(g_rng >> 16);
}

static void CheckAgainstReference(const U32Map* _map, const u32* _reference)
{
  u32 present = 0;
  for (u32 k = 0; k < REF_KEYS; k++)
  {
    const u32* v = U32MapGet(_map, k);
    if (_reference[k] == NO_VALUE)
      CHECKF(v == nullptr, "key %u should be absent", k);
    else
    {
      present++;
      CHECKF(v && *v == _reference[k], "key %u: expected %u", k, _reference[k]);
    }
  }
  CHECK(_map->count == present);
}

void TestHmap(void)
{
  static u32 g_reference[REF_KEYS];
  for (u32 k = 0; k < REF_KEYS; k++)
    g_reference[k] = NO_VALUE;

  U32Map map = {};
  CHECK(U32MapGet(&map, 7) == nullptr);
  CHECK(!U32MapRemove(&map, 7));

  for (u32 step = 0; step < 200000; step++)
  {
    u32 k = NextRandom() % REF_KEYS;
    switch (NextRandom() % 3)
    {
    case 0:
    case 1:
    {
      u32 value = NextRandom();
      u32* stored = U32MapPut(&map, k, value);
      CHECK(stored && *stored == value);
      g_reference[k] = value;
      break;
    }
    default:
      CHECK(U32MapRemove(&map, k) == (g_reference[k] != NO_VALUE));
      g_reference[k] = NO_VALUE;
      break;
    }
    if (step % 20000 == 0)
      CheckAgainstReference(&map, g_reference);
  }
  CheckAgainstReference(&map, g_reference);

  /* Empty it completely, then reuse it. */
  for (u32 k = 0; k < REF_KEYS; k++)
  {
    U32MapRemove(&map, k);
    g_reference[k] = NO_VALUE;
  }
  CheckAgainstReference(&map, g_reference);
  *U32MapPut(&map, 5, 50) += 1;
  CHECK(*U32MapGet(&map, 5) == 51);
  U32MapFree(&map);
  CHECK(map.capacity == 0 && map.count == 0 && U32MapGet(&map, 5) == nullptr);

  CollideMap c = {};
  for (u32 k = 0; k < 13; k++)
    CollideMapPut(&c, k, k * 10);
  for (u32 k = 0; k < 13; k += 2)
    CHECK(CollideMapRemove(&c, k));
  for (u32 k = 0; k < 13; k++)
  {
    const u32* v = CollideMapGet(&c, k);
    CHECKF(k % 2 ? v && *v == k * 10 : v == nullptr, "colliding key %u", k);
  }
  CHECK(c.count == 6);
  CollideMapFree(&c);
}
