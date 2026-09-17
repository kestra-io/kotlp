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
#include <stdlib.h>
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
static bool g_warned_open;       /* one stderr line per failure kind, not one per record */
static bool g_warned_write;
static bool g_last_write_ok = true; /* outcome of the most recent filesink_write() */
static bool g_had_failure;       /* sticky: a dropped record, or a sealed chunk that
                                  * failed fsync()/close() - see filesink_had_failure() */
static long long *g_sizes;       /* bytes written to each file, in creation order */
static int g_sizes_cap;

/* A FUSE-backed --log-dir (Cloud Run + gcsfuse, s3fs, ...) turns a network
 * error into a failing open()/write()/fsync(). One record-level retry with a
 * short pause absorbs the transient ones without stalling the child's output
 * for long: the pipe pump waits on this write. */
#define SINK_RETRIES 3
#define SINK_RETRY_PAUSE_US 20000

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

/* Open <g_dir>/<name for index>, truncating any existing file. Returns the fd
 * or -1; prints to stderr on failure unless `quiet`. Touches no sink state, so
 * a failed rotation leaves the current file exactly as it was. */
static int open_index(int index, bool quiet) {
    char name[32];
    kotlp_log_file_name(name, sizeof(name), index);

    sb path;
    sb_init(&path);
    sb_puts(&path, g_dir);
    size_t n = strlen(g_dir);
    if (n == 0 || g_dir[n - 1] != '/') sb_putc(&path, '/');
    sb_puts(&path, name);

    int fd = open(path.buf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0 && !quiet) {
        fprintf(stderr, "kotlp: cannot open log file '%s': %s\n", path.buf,
                strerror(errno));
    }
    sb_free(&path);
    return fd;
}

static void adopt(int fd) {
    g_fd = fd;
    g_file_start_ns = kotlp_now_unix_nano();
    g_count++;
    if (g_count > g_sizes_cap) {
        int new_cap = g_sizes_cap ? g_sizes_cap * 2 : 4;
        long long *grown = realloc(g_sizes, (size_t)new_cap * sizeof(*grown));
        if (grown) {
            g_sizes = grown;
            g_sizes_cap = new_cap;
        }
    }
    if (g_count <= g_sizes_cap) g_sizes[g_count - 1] = 0;
}

/* fsync() + close(), each checked. fsync is what makes a FUSE mount upload
 * the file, so an error here is the one chance to see a lost chunk; it is
 * retried a few times, then reported. Filesystems that do not support fsync
 * (EINVAL/ENOSYS/EROFS on pipes and some pseudo-fs) are not an error.
 * Returns false (and sets g_had_failure) once either step could not be
 * confirmed: whether this was a mid-run rotation or the final close, the file
 * being sealed here may be missing bytes on disk. */
static bool close_checked(int fd) {
    bool ok = true;
    for (int i = 0; i < SINK_RETRIES; i++) {
        if (fsync(fd) == 0 || errno == EINVAL || errno == ENOSYS || errno == EROFS) break;
        if (i == SINK_RETRIES - 1) {
            fprintf(stderr, "kotlp: fsync of log file failed: %s\n", strerror(errno));
            ok = false;
            break;
        }
        usleep(SINK_RETRY_PAUSE_US);
    }
    while (close(fd) != 0) {
        if (errno == EINTR) continue;
        fprintf(stderr, "kotlp: close of log file failed: %s\n", strerror(errno));
        ok = false;
        break;
    }
    if (!ok) g_had_failure = true;
    return ok;
}

/* --log-dir-probe: before touching the real log files, prove the directory is
 * both writable AND overwritable. On gcsfuse a runtime service account holding
 * only storage.objectCreator can create an object but never overwrite it, so
 * a plain open()/write()/close() of log-1.ndjson succeeds even though every
 * later rotation and the final close (both overwrite-shaped on that mount)
 * are doomed. Reproducing that shape - create, then reopen with O_TRUNC, then
 * read back - catches it at startup instead of after the child has already
 * run to completion. */
static bool probe_dir(const char *dir) {
    sb path;
    sb_init(&path);
    sb_puts(&path, dir);
    size_t n = strlen(dir);
    if (n == 0 || dir[n - 1] != '/') sb_putc(&path, '/');
    sb_puts(&path, ".kotlp-probe");

    const char *first = "kotlp-probe-1\n";
    const char *second = "kotlp-probe-2\n";
    bool ok = false;

    int fd = open(path.buf, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr, "kotlp: --log-dir-probe: cannot create '%s': %s\n",
                path.buf, strerror(errno));
        goto done;
    }
    if (!kotlp_full_write(fd, first, strlen(first))) {
        fprintf(stderr, "kotlp: --log-dir-probe: write to '%s' failed: %s\n",
                path.buf, strerror(errno));
        close(fd);
        goto done;
    }
    if (!close_checked(fd)) {
        fprintf(stderr,
                "kotlp: --log-dir-probe: '%s' could not be closed/synced\n",
                path.buf);
        goto done;
    }

    /* Reopen with O_TRUNC: this is the overwrite a rotation or the final
     * close performs, and the step objectCreator-only credentials fail. */
    fd = open(path.buf, O_WRONLY | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        fprintf(stderr,
                "kotlp: --log-dir-probe: cannot reopen/overwrite '%s': %s\n",
                path.buf, strerror(errno));
        goto done;
    }
    if (!kotlp_full_write(fd, second, strlen(second))) {
        fprintf(stderr,
                "kotlp: --log-dir-probe: overwrite of '%s' failed: %s\n",
                path.buf, strerror(errno));
        close(fd);
        goto done;
    }
    if (!close_checked(fd)) {
        fprintf(stderr,
                "kotlp: --log-dir-probe: '%s' could not be closed/synced after "
                "the overwrite\n",
                path.buf);
        goto done;
    }

    fd = open(path.buf, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "kotlp: --log-dir-probe: cannot reopen '%s' for "
                        "read-back: %s\n",
                path.buf, strerror(errno));
        goto done;
    }
    char buf[32] = {0};
    ssize_t r = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (r < 0 || strcmp(buf, second) != 0) {
        fprintf(stderr,
                "kotlp: --log-dir-probe: read-back of '%s' did not match what "
                "was written - the mount is not reliably overwritable\n",
                path.buf);
        goto done;
    }

    ok = true;
done:
    unlink(path.buf);
    /* close_checked() above may have set g_had_failure; harmless, since a
     * failed probe makes filesink_open() return false and main() exits
     * immediately, before filesink_had_failure() would ever be consulted. */
    sb_free(&path);
    return ok;
}

bool filesink_open(const kotlp_config *cfg) {
    if (!cfg->log_dir) return true; /* sink disabled */

    g_dir = cfg->log_dir;
    g_interval_s = cfg->log_flush_interval_s;
    g_index = g_interval_s > 0 ? 1 : 0; /* log-1.ndjson vs log.ndjson */
    g_count = 0;
    g_sealed = false;

    g_warned_open = false;
    g_warned_write = false;
    g_last_write_ok = true;
    g_had_failure = false;
    free(g_sizes);
    g_sizes = NULL;
    g_sizes_cap = 0;

    if (kotlp_mkdir_p(g_dir) != 0) {
        fprintf(stderr, "kotlp: cannot create log dir '%s': %s\n", g_dir,
                strerror(errno));
        return false;
    }
    if (cfg->log_dir_probe && !probe_dir(g_dir)) return false;
    int fd = open_index(g_index, false);
    if (fd < 0) return false;
    adopt(fd);
    return true;
}

/* One record, retried on a failing write(). Returns false once the retries
 * are exhausted; the record is then dropped, with one stderr line for the
 * whole run rather than one per record. */
static bool write_record(const char *json, size_t len) {
    for (int i = 0; i < SINK_RETRIES; i++) {
        if (kotlp_full_write(g_fd, json, len) && kotlp_full_write(g_fd, "\n", 1)) {
            if (g_count - 1 < g_sizes_cap) g_sizes[g_count - 1] += (long long)(len + 1);
            return true;
        }
        if (i < SINK_RETRIES - 1) usleep(SINK_RETRY_PAUSE_US);
    }
    g_had_failure = true;
    if (!g_warned_write) {
        g_warned_write = true;
        fprintf(stderr, "kotlp: write to log file failed, records may be lost: %s\n",
                strerror(errno));
    }
    return false;
}

bool filesink_write(const char *json, size_t len) {
    if (g_fd < 0) return true; /* sink disabled: nothing to drop */

    if (!g_sealed && g_interval_s > 0) {
        uint64_t now = kotlp_now_unix_nano();
        uint64_t elapsed = now > g_file_start_ns ? now - g_file_start_ns : 0;
        if (elapsed >= (uint64_t)g_interval_s * 1000000000ull) {
            /* Open the next file before closing the current one. If the open
             * fails (a FUSE mount hiccup, a full or read-only directory) the
             * current file keeps receiving records and the rotation is retried
             * on the next write; the index only advances on success, so the
             * on-disk sequence stays gap-free and the root span's file count
             * stays true. Previously the sink went permanently quiet here and
             * every later record, terminal span included, was lost. */
            int next = open_index(g_index + 1, g_warned_open);
            if (next >= 0) {
                close_checked(g_fd);
                g_index++;
                adopt(next);
            } else if (!g_warned_open) {
                g_warned_open = true;
                fprintf(stderr, "kotlp: log rotation failed, continuing in the current file\n");
            }
        }
    }

    g_last_write_ok = write_record(json, len);
    return g_last_write_ok;
}

void filesink_seal(void) { g_sealed = true; }

int filesink_file_count(void) { return g_count; }

void filesink_file_sizes(sb *out) {
    for (int i = 0; i < g_count && i < g_sizes_cap; i++) {
        if (i) sb_putc(out, ',');
        sb_putf(out, "%lld", g_sizes[i]);
    }
}

bool filesink_last_write_ok(void) { return g_last_write_ok; }

bool filesink_had_failure(void) { return g_had_failure; }

void filesink_close(void) {
    if (g_fd < 0) return;
    close_checked(g_fd);
    g_fd = -1;
}
