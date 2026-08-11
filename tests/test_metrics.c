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
    /* descendants listed BEFORE their ancestors, which is what readdir on /proc
     * actually hands back: 300 is reached before anything is known about 200. */
    pid_t pid[] = {300, 200, 100, 500};
    pid_t ppid[] = {200, 100, 1, 999};
    bool in[4];
    kotlp_mark_descendants(pid, ppid, 4, 100, in);
    CHECK(in[0] == true);  /* 300 */
    CHECK(in[1] == true);  /* 200 */
    CHECK(in[2] == true);  /* 100 (root) */
    CHECK(in[3] == false); /* 500 */
}

/* Number of entries in `in` that disagree with "index lies in [lo,hi]".
 * Returns a count rather than asserting per element, so a large table stays one
 * CHECK and a failure reports how far off it was. */
static int count_wrong(const bool *in, int n, int lo, int hi) {
    int wrong = 0;
    for (int i = 0; i < n; i++) {
        bool want = (i >= lo && i <= hi);
        if (in[i] != want) wrong++;
    }
    return wrong;
}

static void test_mark_descendants_deep_chain(void) {
    /* A chain far deeper than a process tree ever gets, walked in the worst
     * order (each entry's parent appears later in the table). */
    enum { N = 500 };
    static pid_t pid[N];
    static pid_t ppid[N];
    static bool in[N];
    for (int i = 0; i < N; i++) {
        pid[i] = (pid_t)(1000 + i);
        ppid[i] = (pid_t)(1000 + i + 1); /* parent is the NEXT entry */
    }
    ppid[N - 1] = 1;              /* the last entry is the top of the chain */
    kotlp_mark_descendants(pid, ppid, N, pid[N - 1], in);
    CHECK(count_wrong(in, N, 0, N - 1) == 0);
    /* Rooted halfway down: only that entry and the ones below it are in. Every
     * index is verified, not a few spots - this is where an off-by-one in the
     * walk's unwind would show up. */
    kotlp_mark_descendants(pid, ppid, N, pid[250], in);
    CHECK(count_wrong(in, N, 0, 250) == 0);
}

/* The two sizes that straddle the internal switch between the linear walk and
 * the fixpoint fallback. Kept shallow (one root, everything else a direct
 * child) because the fallback is quadratic and a deep chain at this size would
 * take minutes. */
static void test_mark_descendants_large(void) {
    enum { BIG = 9000 }; /* comfortably past the internal MAX_PROCS of 8192 */
    static pid_t pid[BIG];
    static pid_t ppid[BIG];
    static bool in[BIG];
    for (int i = 0; i < BIG; i++) {
        pid[i] = (pid_t)(1000 + i);
        ppid[i] = 1000; /* every entry is a direct child of pid[0] */
    }
    ppid[0] = 1;

    /* just under the threshold: the linear path */
    kotlp_mark_descendants(pid, ppid, 8192, pid[0], in);
    CHECK(count_wrong(in, 8192, 0, 8191) == 0);

    /* over it: the fixpoint fallback, which must agree */
    kotlp_mark_descendants(pid, ppid, BIG, pid[0], in);
    CHECK(count_wrong(in, BIG, 0, BIG - 1) == 0);

    /* and with the root absent, both paths mark nothing */
    kotlp_mark_descendants(pid, ppid, BIG, (pid_t)7, in);
    CHECK(count_wrong(in, BIG, 1, 0) == 0); /* empty range: nothing in tree */
}

/* pid 0 is not a real process, but the exported contract does not exclude it
 * and an implementation that reserves 0 as an empty-slot marker would silently
 * drop it and everything below it. */
static void test_mark_descendants_pid_zero(void) {
    pid_t pid[] = {0, 100, 200};
    pid_t ppid[] = {1, 0, 100};
    bool in[3];
    kotlp_mark_descendants(pid, ppid, 3, 0, in);
    CHECK(in[0] == true); /* the root itself */
    CHECK(in[1] == true); /* child of pid 0 */
    CHECK(in[2] == true); /* grandchild */
}

static void test_mark_descendants_self_parent(void) {
    /* /proc is not an atomic snapshot, so a malformed link must not loop. */
    pid_t pid[] = {100, 200, 300};
    pid_t ppid[] = {1, 200, 100}; /* 200 is its own parent */
    bool in[3];
    kotlp_mark_descendants(pid, ppid, 3, 100, in);
    CHECK(in[0] == true);  /* the root */
    CHECK(in[1] == false); /* self-parented, never reaches the root */
    CHECK(in[2] == true);  /* an ordinary child */
}

static void test_mark_descendants_cycle(void) {
    /* 200 -> 300 -> 200 is a closed loop with no path to the root */
    pid_t pid[] = {100, 200, 300, 400};
    pid_t ppid[] = {1, 300, 200, 200}; /* 400 hangs off the cycle */
    bool in[4];
    kotlp_mark_descendants(pid, ppid, 4, 100, in);
    CHECK(in[0] == true);  /* the root */
    CHECK(in[1] == false); /* in the cycle */
    CHECK(in[2] == false); /* in the cycle */
    CHECK(in[3] == false); /* below the cycle, so also unreachable */
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

/* Membership at a scale the sampler really reaches (`make -j64` under a short
 * --interval), checked against the pairwise scan the map replaced. Duplicated
 * identities, recycled pids and pid 0 are all in the table, since those are what
 * an open-addressed map gets wrong: a bad hash or a mishandled probe chain would
 * report a live member as exited and bank its counters twice. */
static void test_retire_large_tree(void) {
    enum { N = 4096 };
    static kotlp_tree_member prev[N];
    static kotlp_tree_member cur[N];
    int cur_n = 0;
    double expect_cu = 0;
    for (int i = 0; i < N; i++) {
        /* pid 0 exercises the "no reserved key" property of the slot values. */
        pid_t pid = (pid_t)(i == 7 ? 0 : i * 3 + 1);
        prev[i] = tm(pid, 100 + i, (double)i, 0.5, i * 10, i * 20);
        if (i % 4 == 0) { /* every fourth member exits before the next sample */
            expect_cu += (double)i;
            continue;
        }
        cur[cur_n++] = (i % 8 == 1)
                           /* same pid, new starttime: the number was recycled,
                            * so the old member is gone and must be retired */
                           ? tm(pid, 900000 + i, 0.01, 0.0, 1, 1)
                           : tm(pid, 100 + i, (double)i + 1.0, 0.6, i * 11,
                                i * 21);
        if (i % 8 == 1) expect_cu += (double)i;
    }
    /* a duplicate identity in `cur` must not change any answer */
    cur[cur_n] = cur[0];
    cur_n++;

    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    kotlp_retire_exited(prev, N, cur, cur_n, &cu, &cs, &rd, &wr);
    CHECK_NEAR(cu, expect_cu);

    /* and all four totals match what the O(prev_n x cur_n) scan would bank */
    double ref_cu = 0, ref_cs = 0;
    long long ref_rd = 0, ref_wr = 0;
    for (int i = 0; i < N; i++) {
        bool alive = false;
        for (int j = 0; j < cur_n && !alive; j++)
            alive = prev[i].pid == cur[j].pid &&
                    prev[i].starttime_ticks == cur[j].starttime_ticks;
        if (alive) continue;
        ref_cu += prev[i].cpu_user;
        ref_cs += prev[i].cpu_sys;
        ref_rd += prev[i].read_bytes;
        ref_wr += prev[i].write_bytes;
    }
    CHECK_NEAR(cu, ref_cu);
    CHECK_NEAR(cs, ref_cs);
    CHECK(rd == ref_rd);
    CHECK(wr == ref_wr);
}

/* Time `rounds` retirements of an n-member tree in which nothing exited, so
 * every lookup has to probe and hit. */
static uint64_t time_retirements(int n, int rounds) {
    enum { MAXN = 4096 };
    static kotlp_tree_member prev[MAXN];
    static kotlp_tree_member cur[MAXN];
    for (int i = 0; i < n; i++) {
        prev[i] = tm((pid_t)(i + 1), 100 + i, 1.0, 0.5, 10, 20);
        cur[i] = prev[i];
    }
    double cu = 0, cs = 0;
    long long rd = 0, wr = 0;
    uint64_t t0 = kotlp_now_mono_ms();
    for (int r = 0; r < rounds; r++)
        kotlp_retire_exited(prev, n, cur, n, &cu, &cs, &rd, &wr);
    uint64_t elapsed = kotlp_now_mono_ms() - t0;
    CHECK_NEAR(cu, 0.0); /* nothing exited, so nothing was banked */
    return elapsed;
}

/* The cost shape, not a benchmark. Retirement used to be O(prev_n x cur_n) per
 * sample - the same quadratic item 4 removed from tree marking, just keyed on
 * tree size instead of host size, so a wide fan-out (`kotlp -i 100 -- make
 * -j64`) put millions of comparisons in every interval.
 *
 * What is asserted is the RATIO between two tree sizes, not a wall-clock budget:
 * a budget would encode how fast this machine is and could flake on a throttled
 * CI runner. Measured here at 8x the members: 4.6x linear, 56x pairwise. */
static void test_retire_is_not_quadratic(void) {
    enum { SMALL = 512, BIG = 4096, ROUNDS = 2000, MAX_RATIO = 20 };
    uint64_t t_small = time_retirements(SMALL, ROUNDS);
    uint64_t t_big = time_retirements(BIG, ROUNDS);
    /* Floor the divisor: the clock is millisecond-resolution, and a fast
     * machine can finish the small case in near-zero time. */
    if (t_small < 4) t_small = 4;
    CHECK(t_big <= t_small * MAX_RATIO);
    if (t_big > t_small * MAX_RATIO)
        fprintf(stderr,
                "    (%d retirements: %d members %llu ms, %d members %llu ms)\n",
                (int)ROUNDS, (int)SMALL, (unsigned long long)t_small, (int)BIG,
                (unsigned long long)t_big);
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

static void test_final_counters_no_samples(void) {
    /* no live sampling at all (macOS/Windows/BSD): the rusage record stands */
    kotlp_counters out = kotlp_final_counters(ctr(1.5, 0.5, 1024000, 512000),
                                              ctr(0, 0, 0, 0), false, false);
    CHECK_NEAR(out.cpu_user, 1.5);
    CHECK(out.read_bytes == 1024000);
    CHECK(out.write_bytes == 512000);
}

static void test_final_counters_continue_io_series(void) {
    /* /proc IO was readable, so the final record continues that series rather
     * than splicing in rusage's block-IO counts, which are a different unit */
    kotlp_counters out = kotlp_final_counters(ctr(1.5, 0.5, 1024000, 512000),
                                              ctr(2.0, 0.9, 4096, 8192), true,
                                              true);
    CHECK_NEAR(out.cpu_user, 2.0); /* the sampled CPU was higher */
    CHECK_NEAR(out.cpu_sys, 0.9);
    CHECK(out.read_bytes == 4096);
    CHECK(out.write_bytes == 8192);
}

static void test_final_counters_io_never_sampled(void) {
    /* Samples ran, but /proc/<pid>/io was never readable - no
     * CONFIG_TASK_IO_ACCOUNTING, or the child dropped privileges. The sampled
     * IO side is a flat 0, so publishing it would replace rusage's real figure
     * with a made-up zero. CPU still reconciles: that flag is a separate one. */
    kotlp_counters out = kotlp_final_counters(ctr(1.5, 0.5, 1024000, 512000),
                                              ctr(2.0, 0.9, 0, 0), true, false);
    CHECK_NEAR(out.cpu_user, 2.0);
    CHECK(out.read_bytes == 1024000);
    CHECK(out.write_bytes == 512000);
}

static void test_final_counters_rusage_wins_on_cpu(void) {
    /* rusage covers descendants reaped between two samples, so it can exceed
     * the sampled total - and then it is the more complete figure */
    kotlp_counters out = kotlp_final_counters(ctr(9.0, 4.0, 0, 0),
                                              ctr(2.0, 0.9, 100, 200), true,
                                              true);
    CHECK_NEAR(out.cpu_user, 9.0);
    CHECK_NEAR(out.cpu_sys, 4.0);
    CHECK(out.read_bytes == 100); /* the IO series still continues */
}

void test_metrics(void) {
    test_parse_stat_basic();
    test_parse_stat_malformed();
    test_mark_descendants();
    test_mark_descendants_unordered();
    test_mark_descendants_deep_chain();
    test_mark_descendants_large();
    test_mark_descendants_pid_zero();
    test_mark_descendants_self_parent();
    test_mark_descendants_cycle();
    test_mark_descendants_root_absent();
    test_cpu_utilization();
    test_retire_nothing_exited();
    test_retire_one_exited();
    test_retire_accumulates();
    test_retire_recycled_pid();
    test_retire_first_sample();
    test_retire_whole_tree_exited();
    test_retire_large_tree();
    test_retire_is_not_quadratic();
    test_final_counters_no_samples();
    test_final_counters_continue_io_series();
    test_final_counters_io_never_sampled();
    test_final_counters_rusage_wins_on_cpu();
    test_cumulative_adds_retired();
    test_cumulative_floor_holds();
    test_cumulative_floor_does_not_cap();
    test_published_series_never_decreases();
}
