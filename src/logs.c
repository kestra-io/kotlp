/* logs.c - capture the child's stdout/stderr and re-emit each line as an
 * OTLP log record (OpenTelemetry semantic conventions: log.iostream).
 *
 * stdout-origin lines are written to our stdout (fd 1), stderr-origin lines to
 * our stderr (fd 2), so the two streams stay distinguishable downstream. */
#include "koltp.h"

#include <poll.h>
#include <string.h>
#include <unistd.h>

/* OTel severity numbers: INFO=9, ERROR=17 (see logs data model). */
#define SEV_INFO 9
#define SEV_ERROR 17

typedef struct {
    int src_fd;      /* pipe read end                                */
    int dst_fd;      /* console stream to mirror the OTLP record onto */
    const char *stream_name;
    int severity_number;
    const char *severity_text;
    sb line;         /* accumulates a partial line across reads       */
    bool eof;
} stream_state;

static void emit_log_line(const koltp_config *cfg, pid_t child_pid,
                          stream_state *st, const char *data, size_t n) {
    sb out;
    sb_init(&out);
    uint64_t now = koltp_now_unix_nano();
    sb_puts(&out, "{\"resourceLogs\":[{");
    otel_resource(&out, cfg, child_pid);
    sb_puts(&out, ",\"scopeLogs\":[{\"scope\":{\"name\":\"" KOLTP_SCOPE_NAME
                  "\",\"version\":\"" KOLTP_VERSION "\"},\"logRecords\":[{");
    sb_putf(&out, "\"timeUnixNano\":\"%llu\",", (unsigned long long)now);
    sb_putf(&out, "\"observedTimeUnixNano\":\"%llu\",", (unsigned long long)now);
    sb_putf(&out, "\"severityNumber\":%d,", st->severity_number);
    sb_puts(&out, "\"severityText\":");
    sb_json_str(&out, st->severity_text);
    sb_puts(&out, ",\"body\":{\"stringValue\":");
    sb_json_strn(&out, data, n);
    sb_puts(&out, "},\"attributes\":[");
    otel_attr_str(&out, "log.iostream", st->stream_name);
    sb_puts(&out, "]}]}]}]}");
    otel_emit(st->dst_fd, &out);
    sb_free(&out);
}

/* Emit one captured line, or - when log capture is disabled - pass the raw
 * line through to the corresponding console stream untouched. */
static void emit_or_passthrough(const koltp_config *cfg, pid_t child_pid,
                                stream_state *st) {
    if (cfg->enable_logs) {
        emit_log_line(cfg, child_pid, st, st->line.buf ? st->line.buf : "",
                      st->line.len);
    } else {
        /* passthrough: emit the child's bytes verbatim, never OTel-wrapped */
        otel_emit_raw(st->dst_fd, st->line.buf ? st->line.buf : "",
                      st->line.len);
    }
    sb_reset(&st->line);
}

/* Split the freshly read bytes on newlines, emitting one record per line and
 * buffering any trailing partial line for the next read. */
static void consume(const koltp_config *cfg, pid_t child_pid, stream_state *st,
                    const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            emit_or_passthrough(cfg, child_pid, st);
        } else if (data[i] != '\r') {
            sb_putc(&st->line, data[i]);
        }
    }
}

static void flush_remainder(const koltp_config *cfg, pid_t child_pid,
                            stream_state *st) {
    if (st->line.len > 0) {
        emit_or_passthrough(cfg, child_pid, st);
    }
}

void logs_pump(const koltp_config *cfg, pid_t child_pid, int out_fd, int err_fd) {
    stream_state streams[2] = {
        {out_fd, STDOUT_FILENO, "stdout", SEV_INFO, "INFO", {0}, false},
        {err_fd, STDERR_FILENO, "stderr", SEV_ERROR, "ERROR", {0}, false},
    };
    sb_init(&streams[0].line);
    sb_init(&streams[1].line);

    char buf[8192];
    while (!streams[0].eof || !streams[1].eof) {
        struct pollfd pfds[2];
        int nf = 0;
        int idx[2];
        for (int i = 0; i < 2; i++) {
            if (!streams[i].eof) {
                pfds[nf].fd = streams[i].src_fd;
                pfds[nf].events = POLLIN;
                pfds[nf].revents = 0;
                idx[nf] = i;
                nf++;
            }
        }
        if (nf == 0) break;

        int r = poll(pfds, nf, -1);
        if (r < 0) break;

        for (int k = 0; k < nf; k++) {
            if (!(pfds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            stream_state *st = &streams[idx[k]];
            ssize_t got = read(st->src_fd, buf, sizeof(buf));
            if (got > 0) {
                consume(cfg, child_pid, st, buf, (size_t)got);
            } else {
                st->eof = true;
                flush_remainder(cfg, child_pid, st);
                close(st->src_fd);
            }
        }
    }

    sb_free(&streams[0].line);
    sb_free(&streams[1].line);
}
