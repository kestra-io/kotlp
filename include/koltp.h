/* koltp - Portable OBServability wrapper
 *
 * A single Actually Portable Executable (APE) that wraps an arbitrary command
 * line and emits OpenTelemetry-formatted JSON for:
 *   - logs    : the child's stdout/stderr captured as OTLP log records
 *   - traces  : an embedded OTLP/HTTP receiver re-emits spans sent by the child,
 *               plus a root span describing the wrapped execution
 *   - metrics : periodic sampling of the child's CPU / memory / IO / fd usage
 *
 * Everything is written here so the project compiles as a single binary with
 * cosmocc and runs unmodified on Linux, macOS, Windows and the BSDs across
 * amd64 and arm64.
 */
#ifndef KOLTP_H
#define KOLTP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/resource.h>

#define KOLTP_VERSION "0.1.0"
#define KOLTP_SCOPE_NAME "koltp"

/* ------------------------------------------------------------------ config */

typedef struct {
    const char *service_name; /* OTEL service.name resource attribute        */
    char **argv;              /* NULL-terminated child command + args         */
    int argc;                 /* number of entries in argv                    */
    long interval_ms;         /* metrics sampling interval                    */
    int otlp_port;            /* port for the embedded OTLP/HTTP receiver     */
    bool enable_logs;         /* capture stdout/stderr as OTLP logs           */
    bool enable_metrics;      /* sample child resource usage                  */
    bool enable_traces;       /* run the embedded OTLP/HTTP trace receiver    */
    bool wrap_otel;           /* true=kjson framing (default), false=bare json */
    bool debug;               /* keep all telemetry env, but show logs raw    */
    const char *otlp_protocol;/* child OTLP protocol override: "http/json" or
                               * "http/protobuf"; NULL = default (http/json,
                               * still overridable via the OTEL_* env)        */
} koltp_config;

/* --------------------------------------------------------------- cli args  */

/* Print the usage/help text to the given stream. */
void usage(FILE *f);
/* Parse argv into cfg, applying defaults. Returns 0 on success, -1 on a usage
 * error (a message is printed to stderr). Exits the process for --help and
 * --version. */
int parse_args(int argc, char **argv, koltp_config *cfg);

/* ------------------------------------------------------ dynamic string buf */

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
} sb;

void sb_init(sb *s);
void sb_free(sb *s);
void sb_reset(sb *s);
void sb_putc(sb *s, char c);
void sb_puts(sb *s, const char *str);
void sb_putf(sb *s, const char *fmt, ...);
/* append a JSON string literal (with surrounding quotes), escaping as needed */
void sb_json_str(sb *s, const char *str);
void sb_json_strn(sb *s, const char *str, size_t n);

/* --------------------------------------------------------------- util/time */

uint64_t koltp_now_unix_nano(void);
/* fill `out` (>= n*2+1 bytes) with `n` random bytes rendered as lowercase hex */
void koltp_rand_hex(char *out, size_t n);
const char *koltp_hostname(void);

/* ----------------------------------------------------------------- otel io */

/* Thread-safe telemetry emit. fd is 1 (stdout) or 2 (stderr). Adds a newline.
 * When wrapping is enabled (the default, --format kjson) each record is framed
 * as ::{"oltp":<json>}:: ; with --format json the bare <json> is written. */
void otel_emit(int fd, const sb *s);
/* Thread-safe raw line emit (never wrapped) - used to pass child output
 * through verbatim when log capture is disabled. Adds a newline. */
void otel_emit_raw(int fd, const char *data, size_t len);
/* Call once from main() before any worker thread starts. */
void otel_emit_init(bool wrap_otel);

/* Decode an OTLP/protobuf trace payload (ExportTraceServiceRequest, which is
 * wire-compatible with TracesData) into the equivalent OTLP/JSON, appended to
 * `out`. Used to normalise `http/protobuf` exports to the same NDJSON shape as
 * `http/json`. Malformed input yields a best-effort partial object. */
void otlp_traces_pb_to_json(sb *out, const uint8_t *data, size_t len);

/* Append an OTLP resource object: "resource":{...} (no leading/trailing comma) */
void otel_resource(sb *s, const koltp_config *cfg, pid_t child_pid);
/* Append one OTLP key/value attribute object: {"key":..,"value":{..}} */
void otel_attr_str(sb *s, const char *key, const char *val);
void otel_attr_int(sb *s, const char *key, int64_t val);

/* --------------------------------------------------------------- features  */

/* logs: read both pipe fds until EOF, emitting one OTLP log record per line.
 * stdout-origin records go to fd 1, stderr-origin records go to fd 2.        */
void logs_pump(const koltp_config *cfg, pid_t child_pid, int out_fd, int err_fd);

/* metrics: background sampler. Started/stopped around the child lifetime.    */
typedef struct metrics_sampler metrics_sampler;
metrics_sampler *metrics_start(const koltp_config *cfg, pid_t child_pid);
void metrics_stop(metrics_sampler *m);
/* Emit a final, authoritative usage record from wait4() rusage data.         */
void metrics_emit_final(const koltp_config *cfg, pid_t child_pid,
                        const struct rusage *ru);

/* traces: embedded OTLP/HTTP receiver. Returns NULL if disabled/failed.
 * Binds cfg->otlp_port on loopback, falling back to an OS-assigned ephemeral
 * port when the requested one is unavailable.                                 */
typedef struct trace_receiver trace_receiver;
trace_receiver *traces_start(const koltp_config *cfg);
/* The port actually bound (may differ from cfg->otlp_port after fallback).    */
int traces_port(const trace_receiver *t);
void traces_stop(trace_receiver *t);
/* Emit the wrapper's own root span for the whole execution.                  */
void traces_emit_root_span(const koltp_config *cfg, pid_t child_pid,
                           const char *trace_id_hex, const char *span_id_hex,
                           uint64_t start_ns, uint64_t end_ns, int exit_code,
                           int term_signal);

/* --------------------------------------------------------------- process   */

/* Spawn cfg->argv with stdout/stderr redirected to fresh pipes.
 * On success returns the child pid and fills *out_fd / *err_fd with the read
 * ends of the pipes. Returns -1 on failure.                                  */
pid_t child_spawn(const koltp_config *cfg, int *out_fd, int *err_fd);

/* Map a wait4()/waitpid() status into the exit code the wrapper should return:
 * the child's own exit status, or 128 + signal number when it was killed by a
 * signal (the convention used by POSIX shells). */
int child_exit_code(int status);

#endif /* KOLTP_H */
