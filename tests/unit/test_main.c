#include "test.h"

#include <stdlib.h>
#include <string.h>

int g_testFailures;
int g_testChecks;

bool TestHaveTool(const char* _name)
{
  char cmd[256];
#if defined(_WIN32)
  snprintf(cmd, sizeof cmd, "where %s >nul 2>nul", _name);
#else
  snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", _name);
#endif
  return system(cmd) == 0;
}

const char* TestTmpdir(void)
{
  const char* vars[] = {"NCC_TEST_TMP", "TMPDIR", "TEMP", "TMP"};
  for (size_t i = 0; i < NC_ARRAY_LEN(vars); i++)
  {
    const char* d = getenv(vars[i]);
    if (d && *d)
      return d;
  }
  return ".";
}

typedef struct TestCase
{
  const char* name;
  void (*fn)(void);
} TestCase;

static const TestCase TESTS[] = {
  {"core", TestCore},
  {"logical_imm", TestLogicalImm},
  {"encode", TestEncodeDifferential},
  {"encode_sym", TestEncodeSymbolic},
  {"unwind", TestUnwind},
  {"coff", TestCoff},
  {"module", TestModule},
  {"hmap", TestHmap},
  {"sym", TestSym},
  {"source", TestSource},
  {"diag", TestDiag},
};

int main(int argc, char** argv)
{
  for (size_t i = 0; i < NC_ARRAY_LEN(TESTS); i++)
  {
    if (argc > 1)
    {
      bool wanted = false;
      for (int a = 1; a < argc; a++)
        wanted |= strcmp(argv[a], TESTS[i].name) == 0;
      if (!wanted)
        continue;
    }
    int before = g_testFailures, checks = g_testChecks;
    TESTS[i].fn();
    printf("%-12s %s (%d checks)\n", TESTS[i].name, g_testFailures == before ? "ok" : "FAILED", g_testChecks - checks);
  }
  if (g_testFailures)
  {
    printf("%d of %d checks failed\n", g_testFailures, g_testChecks);
    return 1;
  }
  printf("all %d checks passed\n", g_testChecks);
  return 0;
}
