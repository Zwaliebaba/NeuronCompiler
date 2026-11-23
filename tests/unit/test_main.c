#include "test.h"

#include <stdlib.h>
#include <string.h>

int test_failures;
int test_checks;

bool test_have_tool(const char *name) {
    char cmd[256];
#if defined(_WIN32)
    snprintf(cmd, sizeof cmd, "where %s >nul 2>nul", name);
#else
    snprintf(cmd, sizeof cmd, "command -v %s >/dev/null 2>&1", name);
#endif
    return system(cmd) == 0;
}

const char *test_tmpdir(void) {
    const char *vars[] = {"NCC_TEST_TMP", "TMPDIR", "TEMP", "TMP"};
    for (size_t i = 0; i < NC_ARRAY_LEN(vars); i++) {
        const char *d = getenv(vars[i]);
        if (d && *d) return d;
    }
    return ".";
}

typedef struct TestCase {
    const char *name;
    void (*fn)(void);
} TestCase;

static const TestCase tests[] = {
    {"core", test_core},
    {"logical_imm", test_logical_imm},
    {"encode", test_encode_differential},
    {"unwind", test_unwind},
    {"coff", test_coff},
    {"module", test_module},
};

int main(int argc, char **argv) {
    for (size_t i = 0; i < NC_ARRAY_LEN(tests); i++) {
        if (argc > 1) {
            bool wanted = false;
            for (int a = 1; a < argc; a++) wanted |= strcmp(argv[a], tests[i].name) == 0;
            if (!wanted) continue;
        }
        int before = test_failures, checks = test_checks;
        tests[i].fn();
        printf("%-12s %s (%d checks)\n", tests[i].name,
               test_failures == before ? "ok" : "FAILED", test_checks - checks);
    }
    if (test_failures) {
        printf("%d of %d checks failed\n", test_failures, test_checks);
        return 1;
    }
    printf("all %d checks passed\n", test_checks);
    return 0;
}
