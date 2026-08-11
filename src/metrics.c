/* metrics.c - sample the wrapped process tree's resource usage and emit OTLP
 * metric records using OpenTelemetry process.* semantic conventions.
 *
 * Live per-sample data is read from /proc on Linux, aggregated across the child
 * AND all of its descendants (wrapped commands routinely fork: shells, make,
 * package managers, JVMs). On platforms without /proc (macOS, Windows, the BSDs
 * under Cosmopolitan) live sampling is skipped and an authoritative summary is
 * emitted from wait4()/getrusage() when the child exits (metrics_emit_final).
 *
 * Emitted metrics:
 *   process.cpu.time                     sum   s,  cpu.mode=user|system
 *   process.cpu.utilization              gauge 1,  cpu.mode=user|system
 *   process.memory.usage                 gauge By  (summed RSS)
 *   process.memory.virtual               gauge By  (summed vsize)
 *   process.disk.io                      sum   By  disk.io.direction=read|write
 *   process.thread.count                 gauge {thread}
 *   process.open_file_descriptor.count   gauge {count}
 *
 * The two sums are CUMULATIVE and monotonic, so they cover the whole tree over
 * the whole run - not just whoever is alive right now. /proc forgets a process
 * the instant it exits, so each sample's members are carried over to the next
 * one and whatever has disappeared is banked into a running total
 * (kotlp_retire_exited + kotlp_cumulative). Two accuracy caveats follow from
 * that: a member is frozen at its last sampled value, so CPU it burned between
 * the final sample and its exit is lost, and a child that both forked and
 * exited between two samples is never seen at all. The closing rusage record
 * recovers some of the latter, since it accounts for every reaped descendant.
 */
#include "kotlp.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* OS detection. Under cosmocc the binary is OS-agnostic (no __linux__ at
 * compile time), so the /proc layout and ru_maxrss units must be decided at
 * RUNTIME via IsLinux(); a native (non-APE) build falls back to the macro. */
#ifdef __COSMOPOLITAN__
#include <cosmo.h>
#define KOTLP_IS_LINUX() IsLinux()
#elif defined(__linux__)
#define KOTLP_IS_LINUX() 1
#else
#define KOTLP_IS_LINUX() 0
#endif

/* Upper bound on processes examined per sample. The tree is a subset of the
 * host's processes, so one bound serves both. */
enum { MAX_PROCS = 8192 };

struct metrics_sampler {
    pthread_t thread;
    const kotlp_config *cfg;
    pid_t pid;
    uint64_t start_ns;
    int ncpu;
    /* previous CPU reading, for the utilization rate */
    bool have_prev;
    double prev_cpu_user;
    double prev_cpu_sys;
    uint64_t prev_ns;
    /* Tree membership carried between samples so the cumulative counters can
     * survive a process exiting: `incoming` is the sample just taken, `members`
     * the one before it, and `retired_*` accumulates everything that has left
     * the tree. See kotlp_retire_exited. */
    kotlp_tree_member incoming[MAX_PROCS];
    int incoming_n;
    kotlp_tree_member members[MAX_PROCS];
    int member_n;
    double retired_cpu_user;
    double retired_cpu_sys;
    long long retired_read;
    long long retired_write;
    /* Last cumulative values published, so the final rusage record can be
     * reconciled against them and never read as a counter reset. Updated by
     * accumulate(), which sampler_main calls immediately before emit_sample();
     * do not introduce a path that accumulates without emitting. */
    bool have_samples; /* false when live sampling never produced a record */
    /* Whether any sample actually managed to read /proc/<pid>/io. It is not
     * implied by have_samples: a kernel built without CONFIG_TASK_IO_ACCOUNTING,
     * or a child that dropped privileges (EACCES on its own /proc/<pid>/io),
     * samples fine and never yields a single IO reading. Without this the final
     * record would splice a hard 0 in where the rusage figure belongs. */
    bool have_io_samples;
    double last_cpu_user;
    double last_cpu_sys;
    long long last_read;
    long long last_write;
    volatile sig_atomic_t stop;
    bool started;
};

typedef struct {
    bool have_cpu;
    double cpu_user_seconds;
    double cpu_sys_seconds;
    bool have_cpu_util;
    double cpu_util_user;     /* ratio in [0,1]              */
    double cpu_util_sys;
    bool have_mem;
    long long rss_bytes;      /* summed resident set size    */
    bool have_vsize;
    long long vsize_bytes;    /* summed virtual memory size  */
    bool have_io;
    long long read_bytes;
    long long write_bytes;
    bool have_threads;
    long long threads;        /* summed thread count         */
    bool have_fds;
    long long open_fds;       /* summed open fd count        */
} sample;

/* --- pure helpers (platform-independent; exercised by the unit tests) ---- */

bool kotlp_parse_proc_stat(const char *line, kotlp_proc_stat *out) {
    /* comm (field 2) is wrapped in parens and may itself contain spaces and
     * parens, so split on the LAST ')': everything after it is space-separated
     * and starts at field 3 (state). */
    const char *rp = strrchr(line, ')');
    if (!rp) return false;

    char buf[4096];
    strncpy(buf, rp + 1, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';

    long long f[22];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(buf, " ", &save); tok && n < 22;
         tok = strtok_r(NULL, " ", &save)) {
        f[n++] = strtoll(tok, NULL, 10); /* f[0]=state -> 0, unused */
    }
    if (n < 22) return false;

    out->ppid = (long)f[1];          /* field 4  */
    out->utime_ticks = f[11];        /* field 14 */
    out->stime_ticks = f[12];        /* field 15 */
    out->num_threads = f[17];        /* field 20 */
    out->starttime_ticks = f[19];    /* field 22 */
    out->vsize_bytes = f[20];        /* field 23 */
    out->rss_pages = f[21];          /* field 24 */
    return true;
}

void kotlp_retire_exited(const kotlp_tree_member *prev, int prev_n,
                         const kotlp_tree_member *cur, int cur_n,
                         double *ret_cpu_user, double *ret_cpu_sys,
                         long long *ret_read, long long *ret_write) {
    for (int i = 0; i < prev_n; i++) {
        bool still_alive = false;
        for (int j = 0; j < cur_n; j++) {
            if (prev[i].pid == cur[j].pid &&
                prev[i].starttime_ticks == cur[j].starttime_ticks) {
                still_alive = true;
                break;
            }
        }
        if (still_alive) continue;
        /* Gone: its counters will never appear in a live sum again, so bank
         * the last values we saw. */
        *ret_cpu_user += prev[i].cpu_user;
        *ret_cpu_sys += prev[i].cpu_sys;
        *ret_read += prev[i].read_bytes;
        *ret_write += prev[i].write_bytes;
    }
}

kotlp_counters kotlp_cumulative(kotlp_counters live, kotlp_counters retired,
                                kotlp_counters floor) {
    kotlp_counters out;
    out.cpu_user = live.cpu_user + retired.cpu_user;
    out.cpu_sys = live.cpu_sys + retired.cpu_sys;
    out.read_bytes = live.read_bytes + retired.read_bytes;
    out.write_bytes = live.write_bytes + retired.write_bytes;
    if (out.cpu_user < floor.cpu_user) out.cpu_user = floor.cpu_user;
    if (out.cpu_sys < floor.cpu_sys) out.cpu_sys = floor.cpu_sys;
    if (out.read_bytes < floor.read_bytes) out.read_bytes = floor.read_bytes;
    if (out.write_bytes < floor.write_bytes) out.write_bytes = floor.write_bytes;
    return out;
}

kotlp_counters kotlp_final_counters(kotlp_counters rusage, kotlp_counters last,
                                    bool have_samples, bool have_io_samples) {
    kotlp_counters out = rusage;
    /* CPU is the same unit either way, and rusage additionally covers
     * descendants that were reaped before any sample saw them, so take
     * whichever is larger. */
    if (have_samples) {
        if (out.cpu_user < last.cpu_user) out.cpu_user = last.cpu_user;
        if (out.cpu_sys < last.cpu_sys) out.cpu_sys = last.cpu_sys;
    }
    /* IO is NOT the same unit: ru_inblock/ru_oublock count block-IO operations,
     * while the series so far is /proc's read_bytes/write_bytes. Continuing the
     * series is more honest than splicing in a number computed a different way,
     * in either direction - but only if there IS a series. When /proc IO was
     * never readable the sampled side is a flat 0, and publishing that would
     * replace a real (if differently-derived) figure with a made-up zero. */
    if (have_io_samples) {
        out.read_bytes = last.read_bytes;
        out.write_bytes = last.write_bytes;
    }
    return out;
}

/* Fixpoint marking: O(n^2) per pass, repeated until nothing changes. Retained
 * only for process tables larger than the scratch below can index. */
static void mark_descendants_fixpoint(const pid_t *pid, const pid_t *ppid, int n,
                                      pid_t root, bool *in_tree) {
    for (int i = 0; i < n; i++) in_tree[i] = (pid[i] == root);
    bool changed = true;
    while (changed) {
        changed = false;
        for (int i = 0; i < n; i++) {
            if (in_tree[i]) continue;
            for (int j = 0; j < n; j++) {
                if (in_tree[j] && pid[j] == ppid[i]) {
                    in_tree[i] = true;
                    changed = true;
                    break;
                }
            }
        }
    }
}

/* Scratch for the linear path. Static, like the /proc scan buffers in
 * collect(): this runs on the sampler thread only. */
enum { MARK_MAP_CAP = 2 * MAX_PROCS };
/* The insertion probe below scans for an empty slot without a cap, so it
 * terminates only because at most MAX_PROCS keys go into MARK_MAP_CAP slots.
 * Both of these are load-bearing, not decorative. */
_Static_assert(MARK_MAP_CAP >= 2 * MAX_PROCS, "load factor must stay <= 0.5");
_Static_assert((MARK_MAP_CAP & (MARK_MAP_CAP - 1)) == 0,
               "the probe mask needs a power of two");

/* 0 = empty, otherwise the process table index plus one. Storing index+1 rather
 * than the pid means there is no reserved key, so a pid of 0 is an ordinary
 * entry instead of erasing its own slot. */
static int s_map_idx[MARK_MAP_CAP];
static signed char s_state[MAX_PROCS]; /* 0 unknown, 1 in, 2 out, 3 on the path */
static int s_stack[MAX_PROCS];

static unsigned map_slot(pid_t p) {
    /* Fibonacci hashing: the useful entropy of the product is in its high bits,
     * so fold those down before masking - masking alone would just permute the
     * low bits of the pid. */
    unsigned h = (unsigned)p * 2654435761u;
    return (h ^ (h >> 16)) & (MARK_MAP_CAP - 1);
}

/* Index of `p` in the process table, or -1. */
static int map_lookup(const pid_t *pid, pid_t p) {
    unsigned h = map_slot(p);
    for (unsigned probes = 0; probes < MARK_MAP_CAP; probes++) {
        int v = s_map_idx[h];
        if (v == 0) return -1;
        if (pid[v - 1] == p) return v - 1;
        h = (h + 1) & (MARK_MAP_CAP - 1);
    }
    return -1;
}

void kotlp_mark_descendants(const pid_t *pid, const pid_t *ppid, int n,
                            pid_t root, bool *in_tree) {
    if (n <= 0) return;
    if (n > MAX_PROCS) {
        mark_descendants_fixpoint(pid, ppid, n, root, in_tree);
        return;
    }

    /* Index the table once, then answer each "is this a descendant of root?"
     * by walking parent links upward until the answer is known. Every node
     * visited on the way is memoised with that answer, so each is resolved once
     * and the whole pass is linear rather than quadratic-per-fixpoint-pass. */
    memset(s_map_idx, 0, sizeof(s_map_idx));
    for (int i = 0; i < n; i++) {
        unsigned h = map_slot(pid[i]);
        while (s_map_idx[h] != 0 && pid[s_map_idx[h] - 1] != pid[i])
            h = (h + 1) & (MARK_MAP_CAP - 1);
        /* First occurrence wins. A repeated pid cannot come out of readdir on
         * /proc, and this deliberately differs from the fixpoint fallback,
         * which effectively lets any duplicate that is in the tree win. */
        if (s_map_idx[h] == 0) s_map_idx[h] = i + 1;
    }

    memset(s_state, 0, sizeof(s_state));
    for (int i = 0; i < n; i++) {
        if (s_state[i] != 0) continue;
        int depth = 0;
        int cur = i;
        signed char verdict;
        for (;;) {
            if (s_state[cur] == 1 || s_state[cur] == 2) {
                verdict = s_state[cur]; /* already resolved */
                break;
            }
            if (s_state[cur] == 3) {
                /* Back onto the path we are currently walking. A /proc scan is
                 * not an atomic snapshot, so parent links can appear to loop;
                 * treat the whole cycle as outside the tree. */
                verdict = 2;
                break;
            }
            if (pid[cur] == root) {
                s_state[cur] = 1;
                verdict = 1;
                break;
            }
            s_state[cur] = 3;
            s_stack[depth++] = cur;
            int parent = map_lookup(pid, ppid[cur]);
            if (parent < 0) {
                verdict = 2; /* parent is not in the table: not our tree */
                break;
            }
            cur = parent;
        }
        while (depth > 0) s_state[s_stack[--depth]] = verdict;
    }

    for (int i = 0; i < n; i++) in_tree[i] = (s_state[i] == 1);
}

double kotlp_cpu_utilization(double cpu_delta_s, double wall_delta_s, int ncpu) {
    if (wall_delta_s <= 0.0 || ncpu <= 0 || cpu_delta_s <= 0.0) return 0.0;
    double u = cpu_delta_s / (wall_delta_s * (double)ncpu);
    if (u > 1.0) u = 1.0;
    return u;
}

/* --- /proc collection (Linux runtime only) ------------------------------ */

/* Read one process's cumulative IO counters. Adds them to the running sample
 * and also reports them separately, so the per-process value can be carried
 * across samples (see kotlp_retire_exited). */
static void read_proc_io(pid_t pid, sample *s, long long *out_read,
                         long long *out_write) {
    *out_read = 0;
    *out_write = 0;
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/io", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long v;
        if (sscanf(line, "read_bytes: %lld", &v) == 1) {
            *out_read = v;
            s->read_bytes += v;
            s->have_io = true;
        } else if (sscanf(line, "write_bytes: %lld", &v) == 1) {
            *out_write = v;
            s->write_bytes += v;
            s->have_io = true;
        }
    }
    fclose(f);
}

static void read_proc_fds(pid_t pid, sample *s) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/fd", (int)pid);
    DIR *d = opendir(path);
    if (!d) return;
    long long count = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        count++;
    }
    closedir(d);
    s->open_fds += count;
    s->have_fds = true;
}

/* Per-process figures parsed from /proc/<pid>/stat in one scan. */
typedef struct {
    double cpu_user;
    double cpu_sys;
    long long vsize;
    long long rss;
    long long threads;
    long long starttime;
} proc_metrics;

/* Sample the tree rooted at `root`. Fills `s` with the summed live figures and,
 * when `members` is non-NULL, writes each live member's own cumulative counters
 * into it (up to MAX_PROCS entries) and sets *member_n.
 *
 * On cost: the per-process /proc/<pid>/io and /proc/<pid>/fd reads below run
 * only for processes already known to be in the tree, so they scale with the
 * wrapped workload rather than with the host. What does scale with the host is
 * the stat() + parse of every /proc/<pid>/stat in the loop above, and that is
 * unavoidable - building the tree needs every process's ppid. Marking the tree
 * out of that table used to be the other host-scaled cost and no longer is
 * (see kotlp_mark_descendants). */
static bool collect(pid_t root, sample *s, kotlp_tree_member *members,
                    int *member_n) {
    memset(s, 0, sizeof(*s));
    if (member_n) *member_n = 0;
    if (!KOTLP_IS_LINUX()) return false; /* /proc semantics are Linux-specific */

    DIR *d = opendir("/proc");
    if (!d) return false;

    static pid_t pids[MAX_PROCS];
    static pid_t ppids[MAX_PROCS];
    static proc_metrics pm[MAX_PROCS];
    static bool in_tree[MAX_PROCS];

    long clk = sysconf(_SC_CLK_TCK);
    long pg = sysconf(_SC_PAGESIZE);
    if (clk <= 0) clk = 100;
    if (pg <= 0) pg = 4096;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_PROCS) {
        char *end;
        long pid = strtol(e->d_name, &end, 10);
        if (*end != '\0' || pid <= 0) continue; /* not a pid directory */

        char path[64];
        snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        char line[4096];
        char *got = fgets(line, sizeof(line), f);
        fclose(f);
        kotlp_proc_stat ps;
        if (!got || !kotlp_parse_proc_stat(line, &ps)) continue;

        pids[n] = (pid_t)pid;
        ppids[n] = (pid_t)ps.ppid;
        pm[n].cpu_user = (double)ps.utime_ticks / (double)clk;
        pm[n].cpu_sys = (double)ps.stime_ticks / (double)clk;
        pm[n].vsize = ps.vsize_bytes;
        pm[n].rss = ps.rss_pages * (long long)pg;
        pm[n].threads = ps.num_threads;
        pm[n].starttime = ps.starttime_ticks;
        n++;
    }
    closedir(d);
    if (n == 0) return false;

    kotlp_mark_descendants(pids, ppids, n, root, in_tree);

    bool any = false;
    int mn = 0;
    for (int i = 0; i < n; i++) {
        if (!in_tree[i]) continue;
        any = true;
        s->cpu_user_seconds += pm[i].cpu_user;
        s->cpu_sys_seconds += pm[i].cpu_sys;
        s->rss_bytes += pm[i].rss;
        s->vsize_bytes += pm[i].vsize;
        s->threads += pm[i].threads;
        long long rd = 0, wr = 0;
        read_proc_io(pids[i], s, &rd, &wr);
        read_proc_fds(pids[i], s);
        if (members && mn < MAX_PROCS) {
            members[mn].pid = pids[i];
            members[mn].starttime_ticks = pm[i].starttime;
            members[mn].cpu_user = pm[i].cpu_user;
            members[mn].cpu_sys = pm[i].cpu_sys;
            members[mn].read_bytes = rd;
            members[mn].write_bytes = wr;
            mn++;
        }
    }
    if (member_n) *member_n = mn;
    if (!any) return false;
    s->have_cpu = true;
    s->have_mem = true;
    s->have_vsize = true;
    s->have_threads = true;
    return true;
}

/* --- OTLP metric emit helpers ------------------------------------------- */

static void datapoint_attr(sb *s, const char *attr_key, const char *attr_val) {
    if (!attr_key) return;
    sb_puts(s, ",\"attributes\":[");
    otel_attr_str(s, attr_key, attr_val);
    sb_putc(s, ']');
}

static void metric_sum_double(sb *s, const char *name, const char *unit,
                              double val, uint64_t start_ns, uint64_t now_ns,
                              const char *attr_key, const char *attr_val) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
               "\"dataPoints\":[{");
    sb_putf(s, "\"startTimeUnixNano\":\"%llu\",", (unsigned long long)start_ns);
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asDouble\":%.6f", val);
    datapoint_attr(s, attr_key, attr_val);
    sb_puts(s, "}]}}");
}

static void metric_sum_int(sb *s, const char *name, const char *unit,
                           long long val, uint64_t start_ns, uint64_t now_ns,
                           const char *attr_key, const char *attr_val) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
               "\"dataPoints\":[{");
    sb_putf(s, "\"startTimeUnixNano\":\"%llu\",", (unsigned long long)start_ns);
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asInt\":\"%lld\"", val);
    datapoint_attr(s, attr_key, attr_val);
    sb_puts(s, "}]}}");
}

static void metric_gauge_int(sb *s, const char *name, const char *unit,
                             long long val, uint64_t now_ns,
                             const char *attr_key, const char *attr_val) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"gauge\":{\"dataPoints\":[{");
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asInt\":\"%lld\"", val);
    datapoint_attr(s, attr_key, attr_val);
    sb_puts(s, "}]}}");
}

static void metric_gauge_double(sb *s, const char *name, const char *unit,
                                double val, uint64_t now_ns,
                                const char *attr_key, const char *attr_val) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"gauge\":{\"dataPoints\":[{");
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asDouble\":%.6f", val);
    datapoint_attr(s, attr_key, attr_val);
    sb_puts(s, "}]}}");
}

static void emit_sample(const kotlp_config *cfg, pid_t pid, uint64_t start_ns,
                        const sample *s) {
    sb out;
    sb_init(&out);
    uint64_t now = kotlp_now_unix_nano();
    sb_puts(&out, "{\"resourceMetrics\":[{");
    otel_resource(&out, cfg, pid);
    sb_puts(&out, ",\"scopeMetrics\":[{\"scope\":{\"name\":\"" KOTLP_SCOPE_NAME
                  "\",\"version\":\"" KOTLP_VERSION "\"},\"metrics\":[");
    bool first = true;
#define SEP() do { if (!first) sb_putc(&out, ','); first = false; } while (0)
    if (s->have_cpu) {
        SEP();
        metric_sum_double(&out, "process.cpu.time", "s", s->cpu_user_seconds,
                          start_ns, now, "cpu.mode", "user");
        SEP();
        metric_sum_double(&out, "process.cpu.time", "s", s->cpu_sys_seconds,
                          start_ns, now, "cpu.mode", "system");
    }
    if (s->have_cpu_util) {
        SEP();
        metric_gauge_double(&out, "process.cpu.utilization", "1",
                            s->cpu_util_user, now, "cpu.mode", "user");
        SEP();
        metric_gauge_double(&out, "process.cpu.utilization", "1",
                            s->cpu_util_sys, now, "cpu.mode", "system");
    }
    if (s->have_mem) {
        SEP();
        metric_gauge_int(&out, "process.memory.usage", "By", s->rss_bytes, now,
                         NULL, NULL);
    }
    if (s->have_vsize) {
        SEP();
        metric_gauge_int(&out, "process.memory.virtual", "By", s->vsize_bytes,
                         now, NULL, NULL);
    }
    if (s->have_io) {
        SEP();
        metric_sum_int(&out, "process.disk.io", "By", s->read_bytes, start_ns,
                       now, "disk.io.direction", "read");
        SEP();
        metric_sum_int(&out, "process.disk.io", "By", s->write_bytes, start_ns,
                       now, "disk.io.direction", "write");
    }
    if (s->have_threads) {
        SEP();
        metric_gauge_int(&out, "process.thread.count", "{thread}", s->threads,
                         now, NULL, NULL);
    }
    if (s->have_fds) {
        SEP();
        metric_gauge_int(&out, "process.open_file_descriptor.count", "{count}",
                         s->open_fds, now, NULL, NULL);
    }
#undef SEP
    sb_puts(&out, "]}]}]}");
    otel_emit(STDOUT_FILENO, &out);
    sb_free(&out);
}

static void sleep_ms_interruptible(metrics_sampler *m, long ms) {
    long elapsed = 0;
    const long chunk = 100;
    while (elapsed < ms && !m->stop) {
        long this = ms - elapsed < chunk ? ms - elapsed : chunk;
        struct timespec ts = {this / 1000, (this % 1000) * 1000000L};
        nanosleep(&ts, NULL);
        elapsed += this;
    }
}

/* Turn the live-only sums in `s` into the cumulative totals we publish, and
 * record them as the floor for the next sample. See kotlp_cumulative. */
static void accumulate(metrics_sampler *m, sample *s) {
    kotlp_retire_exited(m->members, m->member_n, m->incoming, m->incoming_n,
                        &m->retired_cpu_user, &m->retired_cpu_sys,
                        &m->retired_read, &m->retired_write);
    /* the sample just taken becomes the baseline for the next one */
    memcpy(m->members, m->incoming,
           sizeof(kotlp_tree_member) * (size_t)m->incoming_n);
    m->member_n = m->incoming_n;

    kotlp_counters live = {s->cpu_user_seconds, s->cpu_sys_seconds,
                           s->read_bytes, s->write_bytes};
    kotlp_counters retired = {m->retired_cpu_user, m->retired_cpu_sys,
                              m->retired_read, m->retired_write};
    kotlp_counters floor = {m->last_cpu_user, m->last_cpu_sys, m->last_read,
                            m->last_write};
    kotlp_counters out = kotlp_cumulative(live, retired, floor);

    /* Record whether this sample saw a real /proc IO reading before the live
     * flag is overwritten below. metrics_emit_final needs to tell "the series
     * is genuinely 0 bytes" from "there was never a series". */
    if (s->have_io) m->have_io_samples = true;

    s->cpu_user_seconds = out.cpu_user;
    s->cpu_sys_seconds = out.cpu_sys;
    s->read_bytes = out.read_bytes;
    s->write_bytes = out.write_bytes;
    /* Decide this from the published total, not from the live read: exited
     * members still contributed IO, and a live process whose /proc/<pid>/io
     * stops being readable must not punch a hole in the series. */
    if (out.read_bytes > 0 || out.write_bytes > 0) s->have_io = true;

    m->last_cpu_user = out.cpu_user;
    m->last_cpu_sys = out.cpu_sys;
    m->last_read = out.read_bytes;
    m->last_write = out.write_bytes;
    m->have_samples = true;
}

static void *sampler_main(void *arg) {
    metrics_sampler *m = arg;
    while (!m->stop) {
        sample s;
        if (collect(m->pid, &s, m->incoming, &m->incoming_n)) {
            accumulate(m, &s);
            uint64_t now = kotlp_now_unix_nano();
            if (m->have_prev) {
                double wall = (double)(now - m->prev_ns) / 1e9;
                s.cpu_util_user = kotlp_cpu_utilization(
                    s.cpu_user_seconds - m->prev_cpu_user, wall, m->ncpu);
                s.cpu_util_sys = kotlp_cpu_utilization(
                    s.cpu_sys_seconds - m->prev_cpu_sys, wall, m->ncpu);
                s.have_cpu_util = true;
            }
            m->prev_cpu_user = s.cpu_user_seconds;
            m->prev_cpu_sys = s.cpu_sys_seconds;
            m->prev_ns = now;
            m->have_prev = true;
            emit_sample(m->cfg, m->pid, m->start_ns, &s);
        }
        sleep_ms_interruptible(m, m->cfg->interval_ms);
    }
    return NULL;
}

metrics_sampler *metrics_start(const kotlp_config *cfg, pid_t child_pid,
                               uint64_t start_ns) {
    static metrics_sampler m;
    m.cfg = cfg;
    m.pid = child_pid;
    m.start_ns = start_ns;
    m.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (m.ncpu < 1) m.ncpu = 1;
    m.have_prev = false;
    m.prev_cpu_user = 0;
    m.prev_cpu_sys = 0;
    m.prev_ns = 0;
    m.incoming_n = 0;
    m.member_n = 0;
    m.retired_cpu_user = 0;
    m.retired_cpu_sys = 0;
    m.retired_read = 0;
    m.retired_write = 0;
    m.have_samples = false;
    m.have_io_samples = false;
    m.last_cpu_user = 0;
    m.last_cpu_sys = 0;
    m.last_read = 0;
    m.last_write = 0;
    m.stop = 0;
    m.started = false;
    if (pthread_create(&m.thread, NULL, sampler_main, &m) != 0) return NULL;
    m.started = true;
    return &m;
}

void metrics_stop(metrics_sampler *m) {
    if (!m || !m->started) return;
    m->stop = 1;
    pthread_join(m->thread, NULL);
    m->started = false;
}

void metrics_emit_final(const kotlp_config *cfg, pid_t child_pid,
                        const metrics_sampler *m, uint64_t start_ns,
                        const struct rusage *ru) {
    sample s;
    memset(&s, 0, sizeof(s));
    s.have_cpu = true;
    s.cpu_user_seconds =
        (double)ru->ru_utime.tv_sec + ru->ru_utime.tv_usec / 1e6;
    s.cpu_sys_seconds =
        (double)ru->ru_stime.tv_sec + ru->ru_stime.tv_usec / 1e6;
    s.have_mem = true;
    /* ru_maxrss is KiB on Linux, bytes on macOS/BSD. Decide at runtime so the
     * single APE binary reports bytes correctly on whichever OS it runs. */
    s.rss_bytes = (long long)ru->ru_maxrss * (KOTLP_IS_LINUX() ? 1024 : 1);
    s.have_io = true;
    s.read_bytes = (long long)ru->ru_inblock * 512;
    s.write_bytes = (long long)ru->ru_oublock * 512;

    if (m) {
        kotlp_counters rusage = {s.cpu_user_seconds, s.cpu_sys_seconds,
                                 s.read_bytes, s.write_bytes};
        kotlp_counters last = {m->last_cpu_user, m->last_cpu_sys, m->last_read,
                               m->last_write};
        kotlp_counters out = kotlp_final_counters(rusage, last, m->have_samples,
                                                  m->have_io_samples);
        s.cpu_user_seconds = out.cpu_user;
        s.cpu_sys_seconds = out.cpu_sys;
        s.read_bytes = out.read_bytes;
        s.write_bytes = out.write_bytes;
    }
    emit_sample(cfg, child_pid, start_ns, &s);
}
