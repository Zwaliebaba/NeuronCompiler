/* test.h — minimal unit-test harness (no external framework, NC-Boot C). */
#ifndef NC_TEST_H
#define NC_TEST_H

#include "../../src/core/core.h"

#include <stdio.h>

extern int g_testFailures;
extern int g_testChecks;

#define CHECK(cond)                                                             \
  do                                                                            \
  {                                                                             \
    g_testChecks++;                                                             \
    if (!(cond))                                                                \
    {                                                                           \
      g_testFailures++;                                                         \
      fprintf(stderr, "%s(%d): check failed: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                           \
  } while (0)

#define CHECKF(cond, ...)                                                       \
  do                                                                            \
  {                                                                             \
    g_testChecks++;                                                             \
    if (!(cond))                                                                \
    {                                                                           \
      g_testFailures++;                                                         \
      fprintf(stderr, "%s(%d): check failed: %s: ", __FILE__, __LINE__, #cond); \
      fprintf(stderr, __VA_ARGS__);                                             \
      fputc('\n', stderr);                                                      \
    }                                                                           \
  } while (0)

/* Test entry points, one per file. */
void TestCore(void);
void TestLogicalImm(void);
void TestEncodeDifferential(void);
void TestEncodeSymbolic(void);
void TestUnwind(void);
void TestCoff(void);
void TestModule(void);

/* Returns true if an external tool is on PATH (tests that need llvm-mc etc.
 * are skipped, loudly, when it is not). */
bool TestHaveTool(const char* _name);

/* Directory for test scratch files: $NCC_TEST_TMP, else $TMPDIR/$TEMP, else ".". */
const char* TestTmpdir(void);

#endif
