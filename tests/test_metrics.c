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
     * starttime(22) vsize(23) rss(24). comm deliberately contains spaces and
     * nested parens to exercise the strrchr(')') split. */
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
    CHECK(ps.starttime_ticks == 12345);
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

/* Shorthand for a tree member: pid, starttime, cpu_user, cpu_sys, read, write */
static kotlp_tree_member tm(pid_t pid, long long start, double cu, double cs,
                            long long rd, long long wr) {
    kotlp_tree_member m;
    m.pid = pid;
    m.starttime_ticks = start;
    m.cpu_user = cu;
    m.cpu_sys = cs;
    m.read_bytes = rd;
    m.write_bytes = wr;
    return m;
}

static void test_retire_nothing_exited(void) {
    /* the same two processes in both samples: nothing is banked */
    kotlp_tree_member prev[] = {tm(100, 5, 1.0, 0.5, 10, 20),
                                tm(200, 6, 2.0, 1.0, 30, 40)};
    kotlp_tree_member cur[] = {tm(100, 5, 1.5, 0.7, 15, 25),
                               tm(200, 6, 2.5, 1.2, 35, 45)};
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(prev, 2, cur, 2, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 0.0);
    CHECK_NEAR(cs, 0.0);
    CHECK(rd == 0);
    CHECK(wr == 0);
}

static void test_retire_one_exited(void) {
    /* 200 is gone: its last-known counters must be carried forward, or the
     * live-only sum would drop them and the cumulative series would decrease */
    kotlp_tree_member prev[] = {tm(100, 5, 1.0, 0.5, 10, 20),
                                tm(200, 6, 2.0, 1.0, 30, 40)};
    kotlp_tree_member cur[] = {tm(100, 5, 1.5, 0.7, 15, 25)};
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(prev, 2, cur, 1, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 2.0);
    CHECK_NEAR(cs, 1.0);
    CHECK(rd == 30);
    CHECK(wr == 40);
}

static void test_retire_accumulates(void) {
    /* the totals are running, not per-call: a second round adds to the first */
    kotlp_tree_member prev[] = {tm(100, 5, 1.0, 0.5, 10, 20)};
    double cu = 2.0, cs = 1.0;
    long long rd = 30, wr = 40;
    kotlp_retire_exited(prev, 1, NULL, 0, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 3.0);
    CHECK_NEAR(cs, 1.5);
    CHECK(rd == 40);
    CHECK(wr == 60);
}

static void test_retire_recycled_pid(void) {
    /* Same pid, different starttime: the OS reused the number for a brand new
     * process whose counters restart near zero. The original must be retired,
     * otherwise its contribution vanishes and the total goes backwards. */
    kotlp_tree_member prev[] = {tm(100, 5, 9.0, 4.0, 900, 400)};
    kotlp_tree_member cur[] = {tm(100, 77, 0.01, 0.0, 1, 0)};
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(prev, 1, cur, 1, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 9.0);
    CHECK_NEAR(cs, 4.0);
    CHECK(rd == 900);
    CHECK(wr == 400);
}

static void test_retire_first_sample(void) {
    /* no previous sample to reconcile against */
    kotlp_tree_member cur[] = {tm(100, 5, 1.0, 0.5, 10, 20)};
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(NULL, 0, cur, 1, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 0.0);
    CHECK(rd == 0);
}

static void test_retire_whole_tree_exited(void) {
    /* the child and every descendant finished between two samples */
    kotlp_tree_member prev[] = {tm(100, 5, 1.0, 0.5, 10, 20),
                                tm(200, 6, 2.0, 1.0, 30, 40),
                                tm(300, 7, 3.0, 1.5, 50, 60)};
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(prev, 3, NULL, 0, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, 6.0);
    CHECK_NEAR(cs, 3.0);
    CHECK(rd == 90);
    CHECK(wr == 120);
}

static kotlp_counters ctr(double cu, double cs, long long rd, long long wr) {
    kotlp_counters c;
    c.cpu_user = cu;
    c.cpu_sys = cs;
    c.read_bytes = rd;
    c.write_bytes = wr;
    return c;
}

static void test_cumulative_adds_retired(void) {
    kotlp_counters out = kotlp_cumulative(ctr(1.0, 0.5, 10, 20),
                                          ctr(2.0, 1.0, 30, 40),
                                          ctr(0, 0, 0, 0));
    CHECK_NEAR(out.cpu_user, 3.0);
    CHECK_NEAR(out.cpu_sys, 1.5);
    CHECK(out.read_bytes == 40);
    CHECK(out.write_bytes == 60);
}

static void test_cumulative_floor_holds(void) {
    /* live+retired came out below what we already published: the floor wins */
    kotlp_counters out = kotlp_cumulative(ctr(0.1, 0.1, 1, 1),
                                          ctr(0, 0, 0, 0),
                                          ctr(5.0, 4.0, 500, 400));
    CHECK_NEAR(out.cpu_user, 5.0);
    CHECK_NEAR(out.cpu_sys, 4.0);
    CHECK(out.read_bytes == 500);
    CHECK(out.write_bytes == 400);
}

static void test_cumulative_floor_does_not_cap(void) {
    /* the floor is a lower bound only - real growth must pass through */
    kotlp_counters out = kotlp_cumulative(ctr(9.0, 8.0, 900, 800),
                                          ctr(0, 0, 0, 0),
                                          ctr(5.0, 4.0, 500, 400));
    CHECK_NEAR(out.cpu_user, 9.0);
    CHECK(out.read_bytes == 900);
}

/* The property the whole commit exists for: feed a scripted sequence of samples
 * through the same pipeline the sampler uses and assert the published series
 * never decreases - including the cases the smoke test cannot stage on demand,
 * like a live process whose /proc/<pid>/io stops being readable. */
static void test_published_series_never_decreases(void) {
    /* Each row is one sample: the live tree, then whether /proc/<pid>/io could
     * be read for it at all. Sample 3 loses a CPU-heavy member (it exited),
     * sample 4 cannot read io for the survivor, sample 5 recycles pid 200. */
    kotlp_tree_member s1[] = {tm(100, 5, 1.0, 0.5, 100, 50),
                              tm(200, 6, 2.0, 1.0, 200, 100)};
    kotlp_tree_member s2[] = {tm(100, 5, 1.5, 0.7, 150, 70),
                              tm(200, 6, 3.0, 1.5, 300, 150)};
    kotlp_tree_member s3[] = {tm(100, 5, 2.0, 0.9, 200, 90)};
    kotlp_tree_member s4[] = {tm(100, 5, 2.2, 1.0, 0, 0)}; /* io unreadable */
    kotlp_tree_member s5[] = {tm(100, 5, 2.4, 1.1, 260, 120),
                              tm(200, 99, 0.01, 0.0, 1, 0)}; /* pid recycled */
    struct {
        const kotlp_tree_member *m;
        int n;
    } samples[] = {{s1, 2}, {s2, 2}, {s3, 1}, {s4, 1}, {s5, 2}};

    kotlp_counters retired = ctr(0, 0, 0, 0);
    kotlp_counters last = ctr(0, 0, 0, 0);
    const kotlp_tree_member *prev = NULL;
    int prev_n = 0;

    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        kotlp_retire_exited(prev, prev_n, samples[i].m, samples[i].n,
                            &retired.cpu_user, &retired.cpu_sys,
                            &retired.read_bytes, &retired.write_bytes);
        kotlp_counters live = ctr(0, 0, 0, 0);
        for (int j = 0; j < samples[i].n; j++) {
            live.cpu_user += samples[i].m[j].cpu_user;
            live.cpu_sys += samples[i].m[j].cpu_sys;
            live.read_bytes += samples[i].m[j].read_bytes;
            live.write_bytes += samples[i].m[j].write_bytes;
        }
        kotlp_counters out = kotlp_cumulative(live, retired, last);
        CHECK(out.cpu_user >= last.cpu_user);
        CHECK(out.cpu_sys >= last.cpu_sys);
        CHECK(out.read_bytes >= last.read_bytes);
        CHECK(out.write_bytes >= last.write_bytes);
        last = out;
        prev = samples[i].m;
        prev_n = samples[i].n;
    }
    /* the recycled pid 200 must not have erased the original's 3.0 cpu-seconds */
    CHECK(last.cpu_user >= 3.0);
}

void test_metrics(void) {
    test_parse_stat_basic();
    test_parse_stat_malformed();
    test_mark_descendants();
    test_mark_descendants_unordered();
    test_mark_descendants_root_absent();
    test_cpu_utilization();
    test_retire_nothing_exited();
    test_retire_one_exited();
    test_retire_accumulates();
    test_retire_recycled_pid();
    test_retire_first_sample();
    test_retire_whole_tree_exited();
    test_cumulative_adds_retired();
    test_cumulative_floor_holds();
    test_cumulative_floor_does_not_cap();
    test_published_series_never_decreases();
}
