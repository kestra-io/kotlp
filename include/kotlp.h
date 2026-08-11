/* kotlp - Portable OBServability wrapper
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
#ifndef KOTLP_H
#define KOTLP_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/resource.h>

#ifndef KOTLP_VERSION
#define KOTLP_VERSION "dev"
#endif
#define KOTLP_SCOPE_NAME "kotlp"

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
    const char *log_dir;      /* also write every record as bare OTLP NDJSON
                               * into this directory; NULL = console only     */
    long log_flush_interval_s;/* rotate the log dir file every N seconds;
                               * 0 = a single log.ndjson, no rotation         */
} kotlp_config;

/* --------------------------------------------------------------- cli args  */

/* Print the usage/help text to the given stream. */
void usage(FILE *f);
/* Parse argv into cfg, applying defaults. Returns 0 on success, -1 on a usage
 * error (a message is printed to stderr). Exits the process for --help and
 * --version. */
int parse_args(int argc, char **argv, kotlp_config *cfg);

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

uint64_t kotlp_now_unix_nano(void);
/* Milliseconds from an arbitrary fixed origin, for measuring elapsed time.
 * Distinct from kotlp_now_unix_nano(), which reads the wall clock and can jump
 * backwards; only differences of this are meaningful. */
uint64_t kotlp_now_mono_ms(void);
/* fill `out` (>= n*2+1 bytes) with `n` random bytes rendered as lowercase hex */
void kotlp_rand_hex(char *out, size_t n);
const char *kotlp_hostname(void);

/* Mark `fd` close-on-exec. Every descriptor kotlp opens for itself must be
 * marked before child_spawn(), or the wrapped command inherits it: it would
 * then hold a writable handle on our telemetry file and on the receiver's
 * listening socket, and each leaked fd also inflates the
 * process.open_file_descriptor.count we report for the tree.
 * One call site per fd, uniform across pipes, sockets and files - where the
 * descriptor is created atomically close-on-exec instead (accept4's
 * SOCK_CLOEXEC), prefer that, since it leaves no window in which a concurrent
 * fork could still inherit it. Returns 0 on success, -1 on failure. */
int kotlp_set_cloexec(int fd);

/* ----------------------------------------------------------------- otel io */

/* Thread-safe telemetry emit. fd is 1 (stdout) or 2 (stderr). Adds a newline.
 * When wrapping is enabled (the default, --format kjson) each record is framed
 * as ::{"otlp":<json>}:: ; with --format json the bare <json> is written.
 * Always tees to the --log-dir file sink (a no-op when it is disabled). With
 * --log-dir, the console side of this write is suppressed entirely (see
 * otel_emit_init): the file is the one full copy, and the console mirrors
 * -r/--raw instead. */
void otel_emit(int fd, const sb *s);
/* Thread-safe raw line emit (never wrapped, never teed to the file sink) -
 * used to pass child output through verbatim when log capture is disabled,
 * and for the console side of logs once --log-dir owns the structured copy.
 * Adds a newline. */
void otel_emit_raw(int fd, const char *data, size_t len);
/* Call once from main() before any worker thread starts. console_quiet
 * silences otel_emit()'s console side (used when --log-dir is set, so the
 * console shows plain output instead of duplicating the file's OTLP JSON). */
void otel_emit_init(bool wrap_otel, bool console_quiet);
/* write() the whole buffer, retrying short writes. */
void kotlp_full_write(int fd, const char *data, size_t len);

/* ---------------------------------------------------------------- file sink */

/* Mirror every record emitted through otel_emit() into <log_dir>/log.ndjson as
 * bare OTLP JSON (never framed), one record per line. With
 * cfg->log_flush_interval_s > 0 the file is rotated every N seconds into
 * log-1.ndjson, log-2.ndjson, ... The file becomes the one full copy of the
 * telemetry: the console instead switches to -r/--raw output (see
 * otel_emit_init's console_quiet). Call once from main() before any worker
 * thread starts; a no-op returning true when cfg->log_dir is NULL. Returns
 * false (after printing to stderr) when the directory or the first file
 * cannot be created. */
bool filesink_open(const kotlp_config *cfg);
/* Append one record. Called from otel_emit() with its console mutex already
 * held, so this never locks (and must not be called from anywhere else). */
void filesink_write(const char *json, size_t len);
/* Stop rotating, so the final metrics/root-span records land in the last file
 * and filesink_file_count() stays stable while they are built. */
void filesink_seal(void);
/* Number of files created so far (0 when the sink is disabled). */
int filesink_file_count(void);
void filesink_close(void);

/* Write the file name for rotation index `i` into `out`: i <= 0 yields
 * "log.ndjson", otherwise "log-<i>.ndjson". Pure; exposed for unit testing. */
void kotlp_log_file_name(char *out, size_t out_sz, int index);
/* mkdir -p: create `path` and any missing parents. Returns 0 on success, -1
 * with errno set otherwise. An already-existing directory is success. */
int kotlp_mkdir_p(const char *path);

/* Decode an OTLP/protobuf trace payload (ExportTraceServiceRequest, which is
 * wire-compatible with TracesData) into the equivalent OTLP/JSON, appended to
 * `out`. Used to normalise `http/protobuf` exports to the same NDJSON shape as
 * `http/json`. Malformed input yields a best-effort partial object. */
void otlp_traces_pb_to_json(sb *out, const uint8_t *data, size_t len);

/* Append an OTLP resource object: "resource":{...} (no leading/trailing comma) */
void otel_resource(sb *s, const kotlp_config *cfg, pid_t child_pid);
/* Append one OTLP key/value attribute object: {"key":..,"value":{..}} */
void otel_attr_str(sb *s, const char *key, const char *val);
void otel_attr_int(sb *s, const char *key, int64_t val);

/* --------------------------------------------------------------- features  */

/* logs: read both pipe fds until EOF, emitting one OTLP log record per line.
 * stdout-origin records go to fd 1, stderr-origin records go to fd 2.        */
void logs_pump(const kotlp_config *cfg, pid_t child_pid, int out_fd, int err_fd);

/* metrics helpers (pure; exposed for unit testing) -------------------------*/

/* Selected fields parsed from a /proc/<pid>/stat line (raw units). */
typedef struct {
    long ppid;                 /* field 4                                  */
    long long utime_ticks;     /* field 14, in clock ticks                 */
    long long stime_ticks;     /* field 15, in clock ticks                 */
    long long num_threads;     /* field 20                                 */
    long long starttime_ticks; /* field 22, ticks since boot               */
    long long vsize_bytes;     /* field 23, bytes                          */
    long long rss_pages;       /* field 24, in pages                       */
} kotlp_proc_stat;

/* Parse one /proc/<pid>/stat line. Handles a comm containing spaces/parens.
 * Returns false if the line is malformed/too short. */
bool kotlp_parse_proc_stat(const char *line, kotlp_proc_stat *out);

/* Mark which of the `n` processes are `root` or one of its descendants.
 * pid[i]/ppid[i] describe process i; in_tree[i] (length n) is filled in.
 * Parent links that loop - which a non-atomic /proc scan can produce - leave
 * the whole cycle outside the tree.
 * NOT REENTRANT: this keeps static scratch and is called from the metrics
 * sampler thread only. */
void kotlp_mark_descendants(const pid_t *pid, const pid_t *ppid, int n,
                            pid_t root, bool *in_tree);

/* CPU utilization in [0,1]: CPU-seconds consumed over the interval divided by
 * wall-seconds times the CPU count. Returns 0 for non-positive inputs. */
double kotlp_cpu_utilization(double cpu_delta_s, double wall_delta_s, int ncpu);

/* One member of the wrapped process tree, as seen in a single sample, carrying
 * that process's own cumulative counters. */
typedef struct {
    pid_t pid;
    long long starttime_ticks; /* /proc/<pid>/stat field 22: with the pid, a
                                * stable identity. A recycled pid restarts its
                                * counters near zero, and without this we would
                                * mistake that for the same process going
                                * backwards.                                  */
    double cpu_user;           /* seconds, cumulative for this process        */
    double cpu_sys;
    long long read_bytes;      /* bytes, cumulative for this process          */
    long long write_bytes;
} kotlp_tree_member;

/* Fold the members of `prev` that are absent from `cur` - i.e. the processes
 * that exited between the two samples - into the running retired totals.
 *
 * process.cpu.time and process.disk.io are reported as CUMULATIVE and
 * monotonic, but /proc only knows about processes that are still alive, so a
 * plain sum over the live tree drops a process's whole contribution the moment
 * it exits. Carrying the last-known value of everything that has left keeps the
 * emitted total non-decreasing while still tracking the live tree. Membership
 * is matched on pid AND starttime, so a recycled pid retires the old entry
 * rather than being mistaken for it.
 *
 * Retirement is one-way: a member banked here is never un-banked. If a process
 * is retired by mistake - the host-wide scan hit its process cap, or a
 * mid-tree /proc/<pid>/stat read failed and briefly orphaned a subtree - and
 * then reappears, its contribution is counted twice. That inflates the counter,
 * which is the safe direction to be wrong in: the series stays monotonic.
 *
 * Pure in its results, but NOT REENTRANT: `cur` is indexed through static
 * scratch (like kotlp_mark_descendants) so the membership test is linear rather
 * than O(prev_n x cur_n). Called from the metrics sampler thread only. */
void kotlp_retire_exited(const kotlp_tree_member *prev, int prev_n,
                         const kotlp_tree_member *cur, int cur_n,
                         double *ret_cpu_user, double *ret_cpu_sys,
                         long long *ret_read, long long *ret_write);

/* The four counters kotlp publishes as CUMULATIVE monotonic sums. */
typedef struct {
    double cpu_user;
    double cpu_sys;
    long long read_bytes;
    long long write_bytes;
} kotlp_counters;

/* Combine one sample's live sums with everything already retired, then hold the
 * result at or above `floor` - the values last published.
 *
 * The floor is a backstop, not the mechanism. Retirement alone already makes
 * the series non-decreasing: per-process /proc counters are monotonic, so
 * surviving members never shrink, newcomers only add, and members that leave
 * are carried at their last-seen value. The floor exists so that "monotonic"
 * holds structurally even in a case we did not anticipate.
 *
 * Pure; exposed for unit testing. */
kotlp_counters kotlp_cumulative(kotlp_counters live, kotlp_counters retired,
                                kotlp_counters floor);

/* Reconcile the closing rusage summary (`rusage`) with the last cumulative
 * values the live sampler published (`last`).
 *
 * The two flags are NOT the same question. `have_samples` says live sampling
 * produced records at all; `have_io_samples` says at least one of them actually
 * read /proc/<pid>/io. A kernel built without CONFIG_TASK_IO_ACCOUNTING, or a
 * child that drops privileges, samples happily and never yields a single IO
 * reading - and continuing that empty series would publish a hard 0 where
 * rusage has a real (if differently-derived) figure.
 *
 * The flag is deliberately coarse: "some member, in some sample, was readable".
 * A tree that is only PARTLY readable therefore publishes the readable subset
 * rather than rusage's whole-tree figure. That follows the same unit argument -
 * a partial byte count is still a byte count, where rusage counts block-IO
 * operations - and it keeps the final record consistent with the series it
 * continues. `have_io_samples` without `have_samples` cannot occur: one
 * accumulate() call sets both.
 *
 * Pure; exposed for unit testing. */
kotlp_counters kotlp_final_counters(kotlp_counters rusage, kotlp_counters last,
                                    bool have_samples, bool have_io_samples);

/* metrics: background sampler. Started/stopped around the child lifetime.
 * `start_ns` is the moment the run began, and becomes the startTimeUnixNano of
 * every cumulative datapoint - live and final - so they all describe the same
 * collection window. Pass main()'s single value, not a fresh clock reading. */
typedef struct metrics_sampler metrics_sampler;
metrics_sampler *metrics_start(const kotlp_config *cfg, pid_t child_pid,
                               uint64_t start_ns);
void metrics_stop(metrics_sampler *m);
/* Emit a final, authoritative usage record from wait4() rusage data.
 * `m` is the (already stopped) sampler, or NULL when live sampling never ran;
 * its last emitted totals floor the cumulative counters so the final record
 * cannot itself be a decrease - rusage is a different accounting (reaped
 * descendants only, and block IO rather than bytes) and can read lower than
 * the /proc-derived samples it follows. */
void metrics_emit_final(const kotlp_config *cfg, pid_t child_pid,
                        const metrics_sampler *m, uint64_t start_ns,
                        const struct rusage *ru);

/* traces: embedded OTLP/HTTP receiver. Returns NULL if disabled/failed.
 * Binds cfg->otlp_port on loopback, falling back to an OS-assigned ephemeral
 * port when the requested one is unavailable.                                 */
typedef struct trace_receiver trace_receiver;

/* What the receiver is willing to buffer for one request.
 *
 * The header allowance is deliberately separate from the body cap. A single cap
 * over headers+body measures something different from what Content-Length
 * validation accepts (a body): a declared length within body_start bytes of the
 * cap passed validation and was then discarded mid-read, while the peer still
 * got its 200 OK, so the SDK never retried. Whether it fired depended on where
 * a read boundary landed - an intermittent silent drop rather than a clean
 * rejection. Keep the two bounds measuring the two things. */
enum {
    KOTLP_MAX_HEADER_BYTES = 64 * 1024,
    KOTLP_MAX_REQUEST_BYTES = 64 * 1024 * 1024,
};

/* How much of a request has arrived, given `buffered` bytes read so far.
 * `header_end` is < 0 until the "\r\n\r\n" terminator is seen; `content_length`
 * is what the Content-Length header said (< 0 when absent).
 *
 * This is the only thing bounding how much the receiver buffers, so it checks
 * both limits itself rather than trusting the caller to have validated the
 * declared length first.
 *
 * Pure; exposed for unit testing - the boundary it guards is otherwise only
 * reachable with an actual 64 MiB request. */
typedef enum {
    KOTLP_REQ_NEED_MORE = 0,    /* keep reading                               */
    KOTLP_REQ_COMPLETE,         /* headers and the declared body are all here  */
    KOTLP_REQ_HEADERS_TOO_LARGE,/* no terminator within the header allowance   */
    KOTLP_REQ_BODY_TOO_LARGE    /* declared body past the cap we will buffer   */
} kotlp_req_state;
kotlp_req_state kotlp_request_state(size_t buffered, long header_end,
                                    long body_start, long content_length);

trace_receiver *traces_start(const kotlp_config *cfg);
/* The port actually bound (may differ from cfg->otlp_port after fallback).    */
int traces_port(const trace_receiver *t);
void traces_stop(trace_receiver *t);
/* Emit the wrapper's own root span for the whole execution.                  */
void traces_emit_root_span(const kotlp_config *cfg, pid_t child_pid,
                           const char *trace_id_hex, const char *span_id_hex,
                           uint64_t start_ns, uint64_t end_ns, int exit_code,
                           int term_signal);

/* --------------------------------------------------------------- process   */

/* Spawn cfg->argv with stdout/stderr redirected to fresh pipes.
 * On success returns the child pid and fills *out_fd / *err_fd with the read
 * ends of the pipes. Returns -1 on failure.                                  */
pid_t child_spawn(const kotlp_config *cfg, int *out_fd, int *err_fd);

/* Map a wait4()/waitpid() status into the exit code the wrapper should return:
 * the child's own exit status, or 128 + signal number when it was killed by a
 * signal (the convention used by POSIX shells). */
int child_exit_code(int status);

#endif /* KOTLP_H */
