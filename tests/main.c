/* main.c - unit test runner. */
#include "test.h"

int g_checks_run = 0;
int g_checks_failed = 0;
const char *g_current_test = "(none)";

int main(void) {
    printf("koltp unit tests\n");
    RUN(test_json);
    RUN(test_util);
    RUN(test_otel);
    RUN(test_args);
    RUN(test_child);

    printf("\n%d checks, %d failed\n", g_checks_run, g_checks_failed);
    if (g_checks_failed == 0) {
        printf("OK\n");
        return 0;
    }
    printf("FAILED\n");
    return 1;
}
