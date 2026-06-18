/* metrics.c - sample the wrapped process's resource usage and emit OTLP metric
 * records using OpenTelemetry process.* semantic conventions.
 *
 * Live per-sample data is read from /proc on Linux. On platforms without /proc
 * (macOS, Windows, the BSDs under Cosmopolitan) live sampling is skipped and an
 * authoritative summary is emitted from wait4()/getrusage() when the child
 * exits (see metrics_emit_final). */
#include "koltp.h"

#include <dirent.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct metrics_sampler {
    pthread_t thread;
    const koltp_config *cfg;
    pid_t pid;
    uint64_t start_ns;
    volatile sig_atomic_t stop;
    bool started;
};

typedef struct {
    bool have_cpu;
    double cpu_seconds;       /* utime+stime                 */
    bool have_mem;
    long long rss_bytes;      /* resident set size           */
    bool have_vsize;
    long long vsize_bytes;    /* virtual memory size         */
    bool have_io;
    long long read_bytes;
    long long write_bytes;
    bool have_fds;
    long long open_fds;
} sample;

#ifdef __linux__
static bool read_proc_stat(pid_t pid, sample *s) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return false;
    char line[4096];
    if (!fgets(line, sizeof(line), f)) {
        fclose(f);
        return false;
    }
    fclose(f);

    /* comm (field 2) is wrapped in parens and may contain spaces. */
    char *rp = strrchr(line, ')');
    if (!rp) return false;
    char *p = rp + 1;

    /* Tokenize the remainder; token[0] == field 3 (state). */
    long long fields[24];
    int n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok && n < 24;
         tok = strtok_r(NULL, " ", &save)) {
        fields[n++] = strtoll(tok, NULL, 10);
    }
    /* utime=field14 -> idx 11, stime=field15 -> idx 12,
     * vsize=field23 -> idx 20, rss=field24(pages) -> idx 21 */
    if (n <= 21) return false;
    long clk = sysconf(_SC_CLK_TCK);
    long pg = sysconf(_SC_PAGESIZE);
    if (clk <= 0) clk = 100;
    s->cpu_seconds = (double)(fields[11] + fields[12]) / (double)clk;
    s->have_cpu = true;
    s->vsize_bytes = fields[20];
    s->have_vsize = true;
    s->rss_bytes = fields[21] * (long long)pg;
    s->have_mem = true;
    return true;
}

static void read_proc_io(pid_t pid, sample *s) {
    char path[64];
    snprintf(path, sizeof(path), "/proc/%d/io", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        long long v;
        if (sscanf(line, "read_bytes: %lld", &v) == 1) {
            s->read_bytes = v;
            s->have_io = true;
        } else if (sscanf(line, "write_bytes: %lld", &v) == 1) {
            s->write_bytes = v;
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
    s->open_fds = count;
    s->have_fds = true;
}

static bool collect(pid_t pid, sample *s) {
    memset(s, 0, sizeof(*s));
    if (!read_proc_stat(pid, s)) return false;
    read_proc_io(pid, s);
    read_proc_fds(pid, s);
    return true;
}
#else
static bool collect(pid_t pid, sample *s) {
    (void)pid;
    memset(s, 0, sizeof(*s));
    return false; /* no live sampling without /proc */
}
#endif

/* --- OTLP metric emit helpers ------------------------------------------- */

static void metric_sum_double(sb *s, const char *name, const char *unit,
                              double val, uint64_t start_ns, uint64_t now_ns) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
               "\"dataPoints\":[{");
    sb_putf(s, "\"startTimeUnixNano\":\"%llu\",", (unsigned long long)start_ns);
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asDouble\":%.6f}]}}", val);
}

static void metric_sum_int_dir(sb *s, const char *name, const char *unit,
                               long long val, const char *direction,
                               uint64_t start_ns, uint64_t now_ns) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"sum\":{\"aggregationTemporality\":2,\"isMonotonic\":true,"
               "\"dataPoints\":[{");
    sb_putf(s, "\"startTimeUnixNano\":\"%llu\",", (unsigned long long)start_ns);
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asInt\":\"%lld\",", val);
    sb_puts(s, "\"attributes\":[");
    otel_attr_str(s, "disk.io.direction", direction);
    sb_puts(s, "]}]}}");
}

static void metric_gauge_int(sb *s, const char *name, const char *unit,
                             long long val, uint64_t now_ns) {
    sb_puts(s, "{\"name\":");
    sb_json_str(s, name);
    sb_puts(s, ",\"unit\":");
    sb_json_str(s, unit);
    sb_puts(s, ",\"gauge\":{\"dataPoints\":[{");
    sb_putf(s, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now_ns);
    sb_putf(s, "\"asInt\":\"%lld\"}]}}", val);
}

static void emit_sample(const koltp_config *cfg, pid_t pid, uint64_t start_ns,
                        const sample *s) {
    sb out;
    sb_init(&out);
    uint64_t now = koltp_now_unix_nano();
    sb_puts(&out, "{\"resourceMetrics\":[{");
    otel_resource(&out, cfg, pid);
    sb_puts(&out, ",\"scopeMetrics\":[{\"scope\":{\"name\":\"" KOLTP_SCOPE_NAME
                  "\",\"version\":\"" KOLTP_VERSION "\"},\"metrics\":[");
    bool first = true;
#define SEP() do { if (!first) sb_putc(&out, ','); first = false; } while (0)
    if (s->have_cpu) {
        SEP();
        metric_sum_double(&out, "process.cpu.time", "s", s->cpu_seconds,
                          start_ns, now);
    }
    if (s->have_mem) {
        SEP();
        metric_gauge_int(&out, "process.memory.usage", "By", s->rss_bytes, now);
    }
    if (s->have_vsize) {
        SEP();
        metric_gauge_int(&out, "process.memory.virtual", "By", s->vsize_bytes,
                         now);
    }
    if (s->have_io) {
        SEP();
        metric_sum_int_dir(&out, "process.disk.io", "By", s->read_bytes, "read",
                           start_ns, now);
        SEP();
        metric_sum_int_dir(&out, "process.disk.io", "By", s->write_bytes,
                           "write", start_ns, now);
    }
    if (s->have_fds) {
        SEP();
        metric_gauge_int(&out, "process.open_file_descriptor.count", "{count}",
                         s->open_fds, now);
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
            emit_sample(m->cfg, m->pid, m->start_ns, &s);
        }
        sleep_ms_interruptible(m, m->cfg->interval_ms);
    }
    return NULL;
}

metrics_sampler *metrics_start(const koltp_config *cfg, pid_t child_pid) {
    static metrics_sampler m;
    m.cfg = cfg;
    m.pid = child_pid;
    m.start_ns = koltp_now_unix_nano();
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

void metrics_emit_final(const koltp_config *cfg, pid_t child_pid,
                        const struct rusage *ru) {
    sample s;
    memset(&s, 0, sizeof(s));
    double user = (double)ru->ru_utime.tv_sec + ru->ru_utime.tv_usec / 1e6;
    double sys = (double)ru->ru_stime.tv_sec + ru->ru_stime.tv_usec / 1e6;
    s.have_cpu = true;
    s.cpu_seconds = user + sys;
    s.have_mem = true;
    /* ru_maxrss is KiB on Linux, bytes on macOS/BSD. Normalize to bytes on the
     * platforms where we know the unit; otherwise report the raw value. */
#ifdef __linux__
    s.rss_bytes = (long long)ru->ru_maxrss * 1024;
#else
    s.rss_bytes = (long long)ru->ru_maxrss;
#endif
    s.vsize_bytes = 0;
    s.have_io = true;
    s.read_bytes = (long long)ru->ru_inblock * 512;
    s.write_bytes = (long long)ru->ru_oublock * 512;
    uint64_t start = koltp_now_unix_nano();
    emit_sample(cfg, child_pid, start, &s);
}
