/* test.h — minimal unit-test harness (no external framework, NC-Boot C). */
#ifndef NC_TEST_H
#define NC_TEST_H

#include "../../src/core/core.h"

#include <stdio.h>

extern int test_failures;
extern int test_checks;

#define CHECK(cond)                                                              \
    do {                                                                         \
        test_checks++;                                                           \
        if (!(cond)) {                                                           \
            test_failures++;                                                     \
            fprintf(stderr, "%s(%d): check failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                        \
    } while (0)

#define CHECKF(cond, ...)                                                        \
    do {                                                                         \
        test_checks++;                                                           \
        if (!(cond)) {                                                           \
            test_failures++;                                                     \
            fprintf(stderr, "%s(%d): check failed: %s: ", __FILE__, __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                                        \
            fputc('\n', stderr);                                                 \
        }                                                                        \
    } while (0)

/* Test entry points, one per file. */
void test_core(void);
void test_logical_imm(void);
void test_encode_differential(void);
void test_unwind(void);
void test_coff(void);
void test_module(void);

/* Returns true if an external tool is on PATH (tests that need llvm-mc etc.
 * are skipped, loudly, when it is not). */
bool test_have_tool(const char *name);

/* Directory for test scratch files: $NCC_TEST_TMP, else $TMPDIR/$TEMP, else ".". */
const char *test_tmpdir(void);

#endif
