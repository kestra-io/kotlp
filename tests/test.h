/* test.h - a tiny, dependency-free unit test harness.
 *
 * Kept deliberately minimal so the tests stay APE-clean (no external test
 * framework). Each CHECK* macro records a result; the runner in main.c reports
 * the totals and the process exit code reflects success/failure. */
#ifndef KOLTP_TEST_H
#define KOLTP_TEST_H

#include <stdio.h>
#include <string.h>

extern int g_checks_run;
extern int g_checks_failed;
extern const char *g_current_test;

#define CHECK(cond)                                                            \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (!(cond)) {                                                         \
            g_checks_failed++;                                                 \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s\n", g_current_test,         \
                    __FILE__, __LINE__, #cond);                                \
        }                                                                      \
    } while (0)

#define CHECK_STR_EQ(actual, expected)                                         \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (strcmp((actual), (expected)) != 0) {                              \
            g_checks_failed++;                                                 \
            fprintf(stderr,                                                    \
                    "  FAIL [%s] %s:%d:\n    expected: %s\n    actual  : %s\n", \
                    g_current_test, __FILE__, __LINE__, (expected), (actual)); \
        }                                                                      \
    } while (0)

#define CHECK_CONTAINS(haystack, needle)                                       \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (strstr((haystack), (needle)) == NULL) {                           \
            g_checks_failed++;                                                 \
            fprintf(stderr,                                                    \
                    "  FAIL [%s] %s:%d:\n    %s\n    does not contain: %s\n",  \
                    g_current_test, __FILE__, __LINE__, (haystack), (needle)); \
        }                                                                      \
    } while (0)

#define RUN(test_fn)                                                           \
    do {                                                                       \
        g_current_test = #test_fn;                                            \
        printf("- %s\n", #test_fn);                                            \
        test_fn();                                                             \
    } while (0)

/* test suites */
void test_json(void);
void test_util(void);
void test_otel(void);
void test_args(void);
void test_child(void);

#endif /* KOLTP_TEST_H */
