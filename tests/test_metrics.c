/* test_metrics.c - tests for the pure metrics helpers: /proc/stat parsing,
 * process-tree membership and the CPU-utilization rate. */
#include "kotlp.h"
#include "test.h"

#include <math.h>
#include <string.h>

#define CHECK_NEAR(a, b)                                                        \
    do {                                                                       \
        g_checks_run++;                                                        \
        if (fabs((double)(a) - (double)(b)) > 1e-9) {                          \
            g_checks_failed++;                                                 \
            fprintf(stderr, "  FAIL [%s] %s:%d: %s ~= %s (%.9g vs %.9g)\n",    \
                    g_current_test, __FILE__, __LINE__, #a, #b, (double)(a),   \
                    (double)(b));                                              \
        }                                                                      \
    } while (0)

static void test_parse_stat_basic(void) {
    /* fields after comm: state ppid ... utime(14) stime(15) ... threads(20) ...
     * vsize(23) rss(24). comm deliberately contains spaces and nested parens
     * to exercise the strrchr(')') split. */
    const char *line =
        "1234 (my (weird) proc) S 1000 1 1 0 -1 0 0 0 0 0 "
        "50 20 0 0 20 0 7 0 12345 123456 789 0 0";
    kotlp_proc_stat ps;
    memset(&ps, 0, sizeof(ps));
    CHECK(kotlp_parse_proc_stat(line, &ps));
    CHECK(ps.ppid == 1000);
    CHECK(ps.utime_ticks == 50);
    CHECK(ps.stime_ticks == 20);
    CHECK(ps.num_threads == 7);
    CHECK(ps.vsize_bytes == 123456);
    CHECK(ps.rss_pages == 789);
}

static void test_parse_stat_malformed(void) {
    kotlp_proc_stat ps;
    /* no ')' at all */
    CHECK(!kotlp_parse_proc_stat("totally bogus line", &ps));
    /* has ')' but far too few fields */
    CHECK(!kotlp_parse_proc_stat("1 (x) S 0 1 2", &ps));
}

static void test_mark_descendants(void) {
    /* tree:  100 -> 200 -> 300
     *        100 -> 400
     *        500 (ppid 1, unrelated) */
    pid_t pid[] = {100, 200, 300, 400, 500};
    pid_t ppid[] = {1, 100, 200, 100, 1};
    bool in[5];
    kotlp_mark_descendants(pid, ppid, 5, 100, in);
    CHECK(in[0] == true);  /* root             */
    CHECK(in[1] == true);  /* child 200        */
    CHECK(in[2] == true);  /* grandchild 300   */
    CHECK(in[3] == true);  /* child 400        */
    CHECK(in[4] == false); /* unrelated 500    */
}

static void test_mark_descendants_unordered(void) {
    /* descendants listed BEFORE their ancestors: the fixpoint must still find
     * them (300's parent 200 is not yet marked when 300 is first visited). */
    pid_t pid[] = {300, 200, 100, 500};
    pid_t ppid[] = {200, 100, 1, 999};
    bool in[4];
    kotlp_mark_descendants(pid, ppid, 4, 100, in);
    CHECK(in[0] == true);  /* 300 */
    CHECK(in[1] == true);  /* 200 */
    CHECK(in[2] == true);  /* 100 (root) */
    CHECK(in[3] == false); /* 500 */
}

static void test_mark_descendants_root_absent(void) {
    pid_t pid[] = {200, 300};
    pid_t ppid[] = {1, 1};
    bool in[2];
    kotlp_mark_descendants(pid, ppid, 2, 100, in);
    CHECK(in[0] == false);
    CHECK(in[1] == false);
}

static void test_cpu_utilization(void) {
    /* 2 cpu-seconds over 1 wall-second across 2 CPUs -> fully busy */
    CHECK_NEAR(kotlp_cpu_utilization(2.0, 1.0, 2), 1.0);
    /* 1 cpu-second over 1 wall-second across 4 CPUs -> 25% */
    CHECK_NEAR(kotlp_cpu_utilization(1.0, 1.0, 4), 0.25);
    /* idle */
    CHECK_NEAR(kotlp_cpu_utilization(0.0, 1.0, 4), 0.0);
    /* clamped to 1.0 even if the numbers say more */
    CHECK_NEAR(kotlp_cpu_utilization(10.0, 1.0, 2), 1.0);
    /* guards: non-positive wall / cpu count / negative delta -> 0 */
    CHECK_NEAR(kotlp_cpu_utilization(1.0, 0.0, 4), 0.0);
    CHECK_NEAR(kotlp_cpu_utilization(1.0, 1.0, 0), 0.0);
    CHECK_NEAR(kotlp_cpu_utilization(-1.0, 1.0, 2), 0.0);
}

void test_metrics(void) {
    test_parse_stat_basic();
    test_parse_stat_malformed();
    test_mark_descendants();
    test_mark_descendants_unordered();
    test_mark_descendants_root_absent();
    test_cpu_utilization();
}
