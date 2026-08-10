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
    out->vsize_bytes = f[20];        /* field 23 */
    out->rss_pages = f[21];          /* field 24 */
    return true;
}

void kotlp_mark_descendants(const pid_t *pid, const pid_t *ppid, int n,
                            pid_t root, bool *in_tree) {
    for (int i = 0; i < n; i++) in_tree[i] = (pid[i] == root);
    /* fixpoint: a process joins the tree once its parent is in it. Real process
     * trees are shallow, so this converges in a handful of passes. */
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

double kotlp_cpu_utilization(double cpu_delta_s, double wall_delta_s, int ncpu) {
    if (wall_delta_s <= 0.0 || ncpu <= 0 || cpu_delta_s <= 0.0) return 0.0;
    double u = cpu_delta_s / (wall_delta_s * (double)ncpu);
    if (u > 1.0) u = 1.0;
    return u;
}

/* --- /proc collection (Linux runtime only) ------------------------------ */

static void read_proc_io(pid_t pid, sample *s) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/io", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long v;
        if (sscanf(line, "read_bytes: %lld", &v) == 1) {
            s->read_bytes += v;
            s->have_io = true;
        } else if (sscanf(line, "write_bytes: %lld", &v) == 1) {
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
} proc_metrics;

static bool collect(pid_t root, sample *s) {
    memset(s, 0, sizeof(*s));
    if (!KOTLP_IS_LINUX()) return false; /* /proc semantics are Linux-specific */

    DIR *d = opendir("/proc");
    if (!d) return false;

    enum { MAX = 8192 };
    static pid_t pids[MAX];
    static pid_t ppids[MAX];
    static proc_metrics pm[MAX];
    static bool in_tree[MAX];

    long clk = sysconf(_SC_CLK_TCK);
    long pg = sysconf(_SC_PAGESIZE);
    if (clk <= 0) clk = 100;
    if (pg <= 0) pg = 4096;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX) {
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
        n++;
    }
    closedir(d);
    if (n == 0) return false;

    kotlp_mark_descendants(pids, ppids, n, root, in_tree);

    bool any = false;
    for (int i = 0; i < n; i++) {
        if (!in_tree[i]) continue;
        any = true;
        s->cpu_user_seconds += pm[i].cpu_user;
        s->cpu_sys_seconds += pm[i].cpu_sys;
        s->rss_bytes += pm[i].rss;
        s->vsize_bytes += pm[i].vsize;
        s->threads += pm[i].threads;
        read_proc_io(pids[i], s);
        read_proc_fds(pids[i], s);
    }
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

static void *sampler_main(void *arg) {
    metrics_sampler *m = arg;
    while (!m->stop) {
        sample s;
        if (collect(m->pid, &s)) {
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

metrics_sampler *metrics_start(const kotlp_config *cfg, pid_t child_pid) {
    static metrics_sampler m;
    m.cfg = cfg;
    m.pid = child_pid;
    m.start_ns = kotlp_now_unix_nano();
    m.ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
    if (m.ncpu < 1) m.ncpu = 1;
    m.have_prev = false;
    m.prev_cpu_user = 0;
    m.prev_cpu_sys = 0;
    m.prev_ns = 0;
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
    uint64_t start = kotlp_now_unix_nano();
    emit_sample(cfg, child_pid, start, &s);
}
