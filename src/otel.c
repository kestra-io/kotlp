/* otel.c - shared OpenTelemetry/OTLP JSON building blocks and console sink. */
#include "koltp.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* All telemetry lines (logs, metrics, traces) share the two console streams,
 * and several threads emit concurrently. Serialize whole lines so records are
 * never interleaved at the byte level. */
static pthread_mutex_t g_out_mu;
static bool g_wrap_otel = true;
static bool g_console_quiet = false;

/* Called once from main() before any worker thread is started. */
void otel_emit_init(bool wrap_otel, bool console_quiet) {
    g_wrap_otel = wrap_otel;
    g_console_quiet = console_quiet;
    pthread_mutex_init(&g_out_mu, NULL);
}

void koltp_full_write(int fd, const char *data, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, data + off, len - off);
        if (w <= 0) break;
        off += (size_t)w;
    }
}

void otel_emit(int fd, const sb *s) {
    static const char prefix[] = "::{\"oltp\":";
    static const char suffix[] = "}::";
    pthread_mutex_lock(&g_out_mu);
    /* With --log-dir the file is the one full copy of the record; the console
     * drops the OTLP syntax entirely (logs_pump prints the raw line itself). */
    if (!g_console_quiet) {
        if (g_wrap_otel) koltp_full_write(fd, prefix, sizeof(prefix) - 1);
        koltp_full_write(fd, s->buf ? s->buf : "", s->len);
        if (g_wrap_otel) koltp_full_write(fd, suffix, sizeof(suffix) - 1);
        koltp_full_write(fd, "\n", 1);
    }
    /* Tee to --log-dir, if enabled: always the bare record, never framed. Runs
     * under the same mutex so file lines cannot interleave either. */
    filesink_write(s->buf ? s->buf : "", s->len);
    pthread_mutex_unlock(&g_out_mu);
}

void otel_emit_raw(int fd, const char *data, size_t len) {
    /* Raw child bytes are not OTLP JSON, so they never go to the file sink. */
    pthread_mutex_lock(&g_out_mu);
    koltp_full_write(fd, data, len);
    koltp_full_write(fd, "\n", 1);
    pthread_mutex_unlock(&g_out_mu);
}

void otel_attr_str(sb *s, const char *key, const char *val) {
    sb_puts(s, "{\"key\":");
    sb_json_str(s, key);
    sb_puts(s, ",\"value\":{\"stringValue\":");
    sb_json_str(s, val);
    sb_puts(s, "}}");
}

void otel_attr_int(sb *s, const char *key, int64_t val) {
    sb_puts(s, "{\"key\":");
    sb_json_str(s, key);
    sb_puts(s, ",\"value\":{\"intValue\":\"");
    sb_putf(s, "%lld", (long long)val);
    sb_puts(s, "\"}}");
}

void otel_resource(sb *s, const koltp_config *cfg, pid_t child_pid) {
    sb_puts(s, "\"resource\":{\"attributes\":[");
    otel_attr_str(s, "service.name", cfg->service_name);
    sb_putc(s, ',');
    otel_attr_str(s, "telemetry.sdk.name", "koltp");
    sb_putc(s, ',');
    otel_attr_str(s, "telemetry.sdk.language", "c");
    sb_putc(s, ',');
    otel_attr_str(s, "telemetry.sdk.version", KOLTP_VERSION);
    sb_putc(s, ',');
    otel_attr_str(s, "host.name", koltp_hostname());
    sb_putc(s, ',');
    otel_attr_int(s, "process.pid", (int64_t)child_pid);
    if (cfg->argc > 0) {
        sb_putc(s, ',');
        otel_attr_str(s, "process.executable.name", cfg->argv[0]);
        sb_putc(s, ',');
        /* process.command_line: join argv with spaces (best effort) */
        sb command;
        sb_init(&command);
        for (int i = 0; i < cfg->argc; i++) {
            if (i) sb_putc(&command, ' ');
            sb_puts(&command, cfg->argv[i]);
        }
        otel_attr_str(s, "process.command_line", command.buf ? command.buf : "");
        sb_free(&command);
    }
    sb_puts(s, "]}");
}
