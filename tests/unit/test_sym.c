#include "test.h"

#include "../../src/core/hash.h"
#include "../../src/core/sym.h"

#include <stdio.h>

void TestSym(void)
{
  Arena arena;
  ArenaInit(&arena);
  SymTable* t = SymTableCreate(&arena);
  CHECK(SymCount(t) == 0);

  Sym abc = SymInternCstr(t, "abc");
  CHECK(!SymIsNone(abc));
  CHECK(SymEq(abc, SymInternCstr(t, "abc")));
  CHECK(SymEq(abc, SymIntern(t, "abcdef", 3))); /* a prefix of a longer buffer */
  CHECK(!SymEq(abc, SymInternCstr(t, "abd")));
  CHECK(!SymEq(abc, SymInternCstr(t, "ab")));
  CHECK(strcmp(SymText(t, abc), "abc") == 0 && SymLength(t, abc) == 3);

  /* The empty string is an ordinary symbol, distinct from SYM_NONE. */
  Sym empty = SymIntern(t, "", 0);
  CHECK(!SymIsNone(empty) && SymLength(t, empty) == 0 && SymText(t, empty)[0] == 0);

  /* Embedded NUL bytes are part of the string. */
  Sym withNul = SymIntern(t, "a\0b", 3);
  CHECK(!SymEq(withNul, SymIntern(t, "a\0c", 3)));
  CHECK(SymLength(t, withNul) == 3 && memcmp(SymText(t, withNul), "a\0b", 4) == 0);

  /* A precomputed hash finds the same symbol. */
  CHECK(SymEq(abc, SymInternHashed(t, "abc", 3, HashBytes("abc", 3))));

  /* Many symbols: the table grows and every handle stays valid. */
  u32 before = SymCount(t);
  static Sym g_many[50000];
  char name[32];
  for (u32 i = 0; i < NC_ARRAY_LEN(g_many); i++)
  {
    snprintf(name, sizeof name, "id_%u", i);
    g_many[i] = SymInternCstr(t, name);
  }
  CHECK(SymCount(t) == before + NC_ARRAY_LEN(g_many));
  for (u32 i = 0; i < NC_ARRAY_LEN(g_many); i++)
  {
    snprintf(name, sizeof name, "id_%u", i);
    CHECKF(SymEq(g_many[i], SymInternCstr(t, name)) && strcmp(SymText(t, g_many[i]), name) == 0, "symbol %s", name);
  }
  CHECK(SymCount(t) == before + NC_ARRAY_LEN(g_many));
  /* Handles are dense and in interning order (R15: nothing hash-ordered). */
  CHECK(g_many[1].index == g_many[0].index + 1);

  SymTableDestroy(t);
  ArenaRelease(&arena);
}
