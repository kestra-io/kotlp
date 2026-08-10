/* logs.c - capture the child's stdout/stderr and re-emit each line as an
 * OTLP log record (OpenTelemetry semantic conventions: log.iostream).
 *
 * stdout-origin lines are written to our stdout (fd 1), stderr-origin lines to
 * our stderr (fd 2), so the two streams stay distinguishable downstream. */
#include "kotlp.h"

#include <errno.h>
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
    bool pending_cr; /* saw a CR whose LF may land in the next read    */
} stream_state;

static void emit_log_line(const kotlp_config *cfg, pid_t child_pid,
                          stream_state *st, const char *data, size_t n) {
    sb out;
    sb_init(&out);
    uint64_t now = kotlp_now_unix_nano();
    sb_puts(&out, "{\"resourceLogs\":[{");
    otel_resource(&out, cfg, child_pid);
    sb_puts(&out, ",\"scopeLogs\":[{\"scope\":{\"name\":\"" KOTLP_SCOPE_NAME
                  "\",\"version\":\"" KOTLP_VERSION "\"},\"logRecords\":[{");
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

/* Emit one captured line. Pass the raw line through untouched when log capture
 * is disabled (--no-logs) or in --debug mode (telemetry stays on, but logs are
 * shown verbatim for readability); otherwise emit it as an OTLP log record.
 *
 * With --log-dir, the file already gets the full OTLP record (otel_emit tees
 * to it and otel_emit_init silenced its console side), so the console would
 * otherwise go silent for logs. Print the raw line there too, same as -r/--raw,
 * so the console still shows the wrapped command's actual output. */
static void emit_or_passthrough(const kotlp_config *cfg, pid_t child_pid,
                                stream_state *st) {
    if (cfg->enable_logs && !cfg->debug) {
        emit_log_line(cfg, child_pid, st, st->line.buf ? st->line.buf : "",
                      st->line.len);
        if (cfg->log_dir) {
            otel_emit_raw(st->dst_fd, st->line.buf ? st->line.buf : "",
                          st->line.len);
        }
    } else {
        /* passthrough: the child's own bytes, never OTel-wrapped (line endings
         * are still normalised to LF, since otel_emit_raw appends one) */
        otel_emit_raw(st->dst_fd, st->line.buf ? st->line.buf : "",
                      st->line.len);
    }
    sb_reset(&st->line);
}

/* Split the freshly read bytes on newlines, emitting one record per line and
 * buffering any trailing partial line for the next read.
 *
 * A CR is only a line-ending artifact when an LF follows it, so just that pairing
 * is swallowed and every other CR is kept as data (progress bars and the like
 * use bare CRs, and dropping them silently corrupts the body). The pair can
 * straddle a read boundary, hence the CR carried over on the stream. */
static void consume(const kotlp_config *cfg, pid_t child_pid, stream_state *st,
                    const char *data, size_t n) {
    for (size_t i = 0; i < n; i++) {
        char c = data[i];
        if (st->pending_cr) {
            st->pending_cr = false;
            if (c == '\n') {
                emit_or_passthrough(cfg, child_pid, st); /* CRLF terminator */
                continue;
            }
            sb_putc(&st->line, '\r'); /* bare CR: part of the payload */
        }
        if (c == '\r') {
            st->pending_cr = true;
        } else if (c == '\n') {
            emit_or_passthrough(cfg, child_pid, st);
        } else {
            sb_putc(&st->line, c);
        }
    }
}

/* True when the buffered line holds nothing but carriage returns. Such a
 * remainder is a tool clearing its progress line on the way out, so it carries
 * no message and is dropped rather than emitted as a record of its own. */
static bool line_is_only_crs(const sb *line) {
    for (size_t i = 0; i < line->len; i++) {
        if (line->buf[i] != '\r') return false;
    }
    return true;
}

static void flush_remainder(const kotlp_config *cfg, pid_t child_pid,
                            stream_state *st) {
    /* A CR at the very end of the stream never found its LF, so it is data. */
    if (st->pending_cr) {
        sb_putc(&st->line, '\r');
        st->pending_cr = false;
    }
    if (st->line.len > 0 && !line_is_only_crs(&st->line)) {
        emit_or_passthrough(cfg, child_pid, st);
    } else {
        sb_reset(&st->line);
    }
}

void logs_pump(const kotlp_config *cfg, pid_t child_pid, int out_fd, int err_fd) {
    stream_state streams[2] = {
        {.src_fd = out_fd, .dst_fd = STDOUT_FILENO, .stream_name = "stdout",
         .severity_number = SEV_INFO, .severity_text = "INFO"},
        {.src_fd = err_fd, .dst_fd = STDERR_FILENO, .stream_name = "stderr",
         .severity_number = SEV_ERROR, .severity_text = "ERROR"},
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
        /* A forwarded SIGINT/SIGTERM interrupts poll(), and the handlers are
         * installed without SA_RESTART. Bailing out here would stop draining
         * the child's pipes mid-run (golden rule 4) and strand the read ends. */
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }

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
