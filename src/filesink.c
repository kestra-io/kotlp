/* filesink.c - optional file sink for the telemetry stream (--log-dir).
 *
 * Every record that goes through otel_emit() is also appended here as *bare*
 * OTLP JSON, one record per line: the file is machine-facing NDJSON, so it never
 * carries the ::{"otlp":...}:: console framing regardless of --format. Raw child
 * passthrough (otel_emit_raw, used by --no-logs / --debug) is not JSON and is
 * therefore never written here.
 *
 * With --log-flush-interval the file is rotated into log-1.ndjson,
 * log-2.ndjson, ... Rotation is lazy: the roll happens on the first record
 * written after the interval elapsed, and the index advances by one, so a quiet
 * period never leaves an empty file behind and the file count reported on the
 * root span always matches what is on disk.
 *
 * There is a single sink per process (like the metrics sampler and the trace
 * receiver), so the state is file-static. filesink_write() runs under
 * otel_emit()'s mutex and takes no lock of its own; filesink_open()/_seal()/
 * _close() are only called from main() outside the threads' lifetime. */
#include "kotlp.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_fd = -1;
static const char *g_dir;        /* borrowed from argv, like the other cfg strings */
static long g_interval_s;
static uint64_t g_file_start_ns; /* when the current file was opened */
static int g_index;              /* rotation index of the current file */
static int g_count;              /* files created so far */
static bool g_sealed;

void kotlp_log_file_name(char *out, size_t out_sz, int index) {
    if (!out || out_sz == 0) return;
    if (index <= 0) {
        snprintf(out, out_sz, "log.ndjson");
    } else {
        snprintf(out, out_sz, "log-%d.ndjson", index);
    }
}

int kotlp_mkdir_p(const char *path) {
    if (!path || !path[0]) {
        errno = EINVAL;
        return -1;
    }
    /* Create every component in turn, tolerating the ones that already exist.
     * Uses a growable buffer so there is no path length limit, and only '/' as
     * the separator (Cosmopolitan accepts it on Windows too). */
    sb p;
    sb_init(&p);
    int rc = 0;
    for (const char *c = path;; c++) {
        if (*c == '/' || *c == '\0') {
            /* skip "", "/" and a trailing separator: nothing to create */
            if (p.len > 0 && !(p.len == 1 && p.buf[0] == '/')) {
                if (mkdir(p.buf, 0755) != 0 && errno != EEXIST) {
                    rc = -1;
                    break;
                }
            }
        }
        if (*c == '\0') break;
        sb_putc(&p, *c);
    }
    sb_free(&p);
    return rc;
}

/* Open <g_dir>/<name for g_index>, replacing any current file. */
static bool open_current(void) {
    char name[32];
    kotlp_log_file_name(name, sizeof(name), g_index);

    sb path;
    sb_init(&path);
    sb_puts(&path, g_dir);
    size_t n = strlen(g_dir);
    if (n == 0 || g_dir[n - 1] != '/') sb_putc(&path, '/');
    sb_puts(&path, name);

    int fd = open(path.buf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "kotlp: cannot open log file '%s': %s\n", path.buf,
                strerror(errno));
        sb_free(&path);
        return false;
    }
    sb_free(&path);

    g_fd = fd;
    g_file_start_ns = kotlp_now_unix_nano();
    g_count++;
    return true;
}

bool filesink_open(const kotlp_config *cfg) {
    if (!cfg->log_dir) return true; /* sink disabled */

    g_dir = cfg->log_dir;
    g_interval_s = cfg->log_flush_interval_s;
    g_index = g_interval_s > 0 ? 1 : 0; /* log-1.ndjson vs log.ndjson */
    g_count = 0;
    g_sealed = false;

    if (kotlp_mkdir_p(g_dir) != 0) {
        fprintf(stderr, "kotlp: cannot create log dir '%s': %s\n", g_dir,
                strerror(errno));
        return false;
    }
    return open_current();
}

void filesink_write(const char *json, size_t len) {
    if (g_fd < 0) return;

    if (!g_sealed && g_interval_s > 0) {
        uint64_t now = kotlp_now_unix_nano();
        uint64_t elapsed = now > g_file_start_ns ? now - g_file_start_ns : 0;
        if (elapsed >= (uint64_t)g_interval_s * 1000000000ull) {
            close(g_fd);
            g_fd = -1;
            g_index++;
            /* On failure the sink goes quiet rather than killing the run: the
             * child is already going and its console telemetry is unaffected. */
            if (!open_current()) return;
        }
    }

    kotlp_full_write(g_fd, json, len);
    kotlp_full_write(g_fd, "\n", 1);
}

void filesink_seal(void) { g_sealed = true; }

int filesink_file_count(void) { return g_count; }

void filesink_close(void) {
    if (g_fd < 0) return;
    close(g_fd);
    g_fd = -1;
}
