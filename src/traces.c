/* traces.c - embedded OTLP/HTTP trace receiver and wrapper root span.
 *
 * We bind a tiny HTTP server on 127.0.0.1:<port> and advertise it to the child
 * via OTEL_EXPORTER_OTLP_* env vars (see main.c). Spans the child's OTel SDK
 * exports over OTLP/HTTP are emitted to the console as NDJSON: http/json bodies
 * are forwarded verbatim, http/protobuf bodies are decoded to the same JSON
 * shape (see otlp_pb.c). We also synthesize one root span covering the whole
 * execution. */
#include "kotlp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <unistd.h>

struct trace_receiver {
    pthread_t thread;
    int listen_fd;
    int port; /* the port actually bound (after any fallback) */
    const kotlp_config *cfg;
    volatile sig_atomic_t stop;
    bool started;
};

#include <pthread.h>

static const char *HTTP_OK =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 21\r\n"
    "Connection: close\r\n\r\n"
    "{\"partialSuccess\":{}}";

/* Anything that is not an export attempt. The port is a well-known one (4318)
 * that anything on the host can reach, and answering every probe with a 200
 * made kotlp look like a general-purpose OTLP endpoint. */
static const char *HTTP_405 =
    "HTTP/1.1 405 Method Not Allowed\r\n"
    "Allow: POST\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n";

static const char *HTTP_400 =
    "HTTP/1.1 400 Bad Request\r\n"
    "Content-Length: 0\r\n"
    "Connection: close\r\n\r\n";

enum {
    /* One poll() wait. Short enough that the stop flag is noticed promptly. */
    RECV_POLL_MS = 100,
    /* How long a connection may go without delivering a byte. This is what
     * bounds a peer that connects and then stalls - which used to wedge the
     * receiver thread, and traces_stop() joins that thread, so kotlp would
     * never exit and never emit its root span with the child already reaped. */
    IDLE_TIMEOUT_MS = 5000,
    /* Absolute cap, so a peer that dribbles one byte per tick cannot hold the
     * connection open forever while always looking like it is progressing. */
    REQUEST_TIMEOUT_MS = 60000,
    /* Budget once shutdown has begun. Bounds how long traces_stop() can block
     * while still giving an in-flight final export from the child's SDK a
     * chance to land. */
    STOP_GRACE_MS = 500,
    /* Refuse to buffer an unbounded request body. */
    MAX_REQUEST_BYTES = 64 * 1024 * 1024,
};

/* Parse Content-Length from the request headers. `header_len` bounds the scan
 * so a "content-length:" sequence inside the body can never be picked up.
 * Returns -1 when absent, and -2 when present but unusable (negative, not a
 * number, or larger than we are willing to buffer).
 *
 * The upper bound is load-bearing, not defensive: the value feeds
 * `body_start + content_length`, and a header of LONG_MAX overflowed that
 * signed addition into a negative number. The "have we got the whole body yet"
 * guard then passed on a few bytes of payload and the forwarding loop walked
 * content_length bytes off the end of the heap. Any local process could kill
 * kotlp mid-run with one netcat line - measured: SIGBUS, no root span. */
static long parse_content_length(const char *headers, size_t header_len) {
    const char *p = headers;
    const char *limit = headers + header_len;
    while (p < limit) {
        if (strncasecmp(p, "content-length:", 15) == 0) {
            errno = 0;
            char *end = NULL;
            long v = strtol(p + 15, &end, 10);
            if (end == p + 15 || errno == ERANGE) return -2;
            if (v < 0 || v > MAX_REQUEST_BYTES) return -2;
            return v;
        }
        const char *nl = memchr(p, '\n', (size_t)(limit - p));
        if (!nl) break;
        p = nl + 1;
    }
    return -1;
}

/* Case-insensitive substring search within the first `len` bytes of `hay`. */
static bool ci_contains(const char *hay, size_t len, const char *needle) {
    size_t nl = strlen(needle);
    if (nl == 0) return true;
    if (len < nl) return false;
    for (size_t i = 0; i + nl <= len; i++) {
        if (strncasecmp(hay + i, needle, nl) == 0) return true;
    }
    return false;
}

/* True when the Content-Type header advertises protobuf rather than JSON.
 * `header_len` bounds the scan to the request headers (never the binary body).*/
static bool body_is_protobuf(const char *headers, size_t header_len) {
    const char *p = headers;
    const char *limit = headers + header_len;
    while (p < limit) {
        if (strncasecmp(p, "content-type:", 13) == 0) {
            const char *v = p + 13;
            const char *nl = memchr(v, '\n', (size_t)(limit - v));
            size_t vl = nl ? (size_t)(nl - v) : (size_t)(limit - v);
            return ci_contains(v, vl, "protobuf");
        }
        const char *nl = memchr(p, '\n', (size_t)(limit - p));
        if (!nl) break;
        p = nl + 1;
    }
    return false; /* default to JSON (OTLP/HTTP default is also JSON-friendly) */
}

static void handle_conn(trace_receiver *t, int fd) {
    sb req;
    sb_init(&req);
    char buf[8192];
    long header_end = -1;
    long content_length = -1;
    long body_start = 0;
    uint64_t began = kotlp_now_mono_ms();
    uint64_t last_progress = began;
    uint64_t stop_seen = 0;
    bool timed_out = false;
    bool capped = false;
    bool bad_method = false;
    bool bad_length = false;

    /* Read until we have the full headers plus the declared body, or until a
     * deadline passes. Every wait is bounded: the receiver is a single thread
     * that traces_stop() joins, so a peer that connects and then stalls must
     * not be able to hold it.
     *
     * The budget is an IDLE timeout rather than a total one, under a generous
     * absolute cap. A total budget would truncate a legitimate slow transfer -
     * notably any client sending `Expect: 100-continue`, which waits about a
     * second for a response we never send before starting the body. */
    for (;;) {
        uint64_t now = kotlp_now_mono_ms();
        if (t->stop && stop_seen == 0) stop_seen = now;
        if (now - last_progress >= IDLE_TIMEOUT_MS ||
            now - began >= REQUEST_TIMEOUT_MS ||
            /* Measured from when shutdown was first observed, not from when the
             * connection opened, so a request already older than the grace
             * still gets its full grace to finish. */
            (stop_seen != 0 && now - stop_seen >= STOP_GRACE_MS)) {
            timed_out = true;
            break;
        }

        struct pollfd pfd = {fd, POLLIN, 0};
        int r = poll(&pfd, 1, RECV_POLL_MS);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) continue; /* nothing yet - re-check the deadlines */
        if (pfd.revents & POLLNVAL) break;
        if (!(pfd.revents & (POLLIN | POLLHUP | POLLERR))) continue;

        ssize_t got = read(fd, buf, sizeof(buf));
        if (got < 0) {
            if (errno == EINTR) continue;
            /* The listener is non-blocking and macOS/BSD hand that down to the
             * accepted socket, so a readable-then-empty socket reports EAGAIN
             * there where Linux would simply block. Not an error. */
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            break;
        }
        if (got == 0) break; /* peer closed */
        for (ssize_t i = 0; i < got; i++) sb_putc(&req, buf[i]);
        last_progress = kotlp_now_mono_ms();

        if (header_end < 0) {
            char *he = strstr(req.buf, "\r\n\r\n");
            if (he) {
                header_end = (long)(he - req.buf);
                body_start = header_end + 4;
                content_length = parse_content_length(req.buf, (size_t)header_end);
                if (content_length == -2) {
                    bad_length = true;
                    break;
                }
                /* Only exports are served. The path is deliberately not
                 * checked: a caller may point OTEL_EXPORTER_OTLP_TRACES_ENDPOINT
                 * at any path, and rejecting one would break a legitimate SDK. */
                if (strncmp(req.buf, "POST ", 5) != 0) {
                    bad_method = true;
                    break;
                }
            }
        }
        if (header_end >= 0) {
            long have_body = (long)req.len - body_start;
            if (content_length < 0 || have_body >= content_length) break;
        }
        if (req.len > (size_t)MAX_REQUEST_BYTES) {
            capped = true;
            break;
        }
    }

    if (bad_method || bad_length) {
        const char *reply = bad_method ? HTTP_405 : HTTP_400;
        if (bad_length)
            fprintf(stderr, "kotlp: trace receiver rejected a request with an "
                            "unusable Content-Length\n");
        ssize_t wr = write(fd, reply, strlen(reply));
        (void)wr;
        sb_free(&req);
        return;
    }

    if (header_end >= 0 && content_length > 0 &&
        (long)req.len >= body_start + content_length) {
        const char *body = req.buf + body_start;
        sb out;
        sb_init(&out);
        if (body_is_protobuf(req.buf, (size_t)header_end)) {
            /* http/protobuf: decode the binary OTLP payload into the same
             * OTLP/JSON shape the http/json path forwards. */
            otlp_traces_pb_to_json(&out, (const uint8_t *)body,
                                   (size_t)content_length);
        } else {
            /* http/json: forward the payload verbatim as one NDJSON record,
             * collapsing any pretty-print newlines so it stays single-line. */
            for (long i = 0; i < content_length; i++) {
                char c = body[i];
                if (c == '\n' || c == '\r') continue;
                sb_putc(&out, c);
            }
        }
        if (out.len > 0) otel_emit(STDOUT_FILENO, &out);
        sb_free(&out);
    } else if (header_end >= 0 && content_length != 0) {
        /* Headers arrived but nothing was forwarded: either the promised body
         * never fully turned up, or there was no Content-Length to delimit one
         * (a chunked encoding, which is not decoded here). Dropping it while
         * answering 200 OK made a lost export indistinguishable from a
         * delivered one.
         *
         * `Content-Length: 0` is deliberately not in here. An empty POST - a
         * bare `curl -X POST`, an SDK flushing an empty batch, a liveness probe
         * - is a successful export of nothing, not a dropped payload, and
         * logging one wrote a spurious error line to kotlp's stderr. */
        long have_body = (long)req.len - body_start;
        if (have_body < 0) have_body = 0;
        if (content_length > 0) {
            fprintf(stderr,
                    "kotlp: trace receiver dropped a truncated payload (%ld of "
                    "%ld bytes%s)\n",
                    have_body, content_length,
                    timed_out ? ", timed out"
                              : capped ? ", over the size cap" : "");
        } else { /* content_length < 0: the header was absent altogether */
            fprintf(stderr,
                    "kotlp: trace receiver dropped a %ld-byte payload with no "
                    "usable Content-Length\n",
                    have_body);
        }
    }

    /* Still 200 even for a dropped payload: a non-2xx makes the SDK retry, and
     * a body we could not read once - too slow, or too large - is one we would
     * fail to read again. Best-effort though: on the truncated paths we leave
     * bytes unread, so the close() below is an RST rather than a FIN and the
     * peer may never see this response at all. */
    ssize_t wr = write(fd, HTTP_OK, strlen(HTTP_OK));
    (void)wr;
    sb_free(&req);
}

static void *receiver_main(void *arg) {
    trace_receiver *t = arg;
    while (!t->stop) {
        struct pollfd pfd = {t->listen_fd, POLLIN, 0};
        int r = poll(&pfd, 1, 200);
        if (r <= 0) continue;
        if (!(pfd.revents & POLLIN)) continue;
        /* accept4 rather than accept: this thread runs concurrently with the
         * fork in child_spawn(), so setting FD_CLOEXEC as a second step would
         * leave a window in which the child inherits the connection. */
        int fd = accept4(t->listen_fd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) continue; /* including EAGAIN: the listener is non-blocking */
        /* The response is ~120 bytes and always fits in an empty send buffer,
         * so this write cannot block today. Bound it anyway, so the property is
         * enforced by the socket rather than by that arithmetic staying true. */
        struct timeval snd = {IDLE_TIMEOUT_MS / 1000, 0};
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &snd, sizeof(snd));
        handle_conn(t, fd);
        close(fd);
    }
    return NULL;
}

/* Bind `fd` to 127.0.0.1:`port` (port 0 asks the OS for an ephemeral port). */
static int bind_loopback(int fd, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);
    return bind(fd, (struct sockaddr *)&addr, sizeof(addr));
}

/* Return the port a bound socket is actually listening on, or -1 on error. */
static int bound_port(int fd) {
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);
    if (getsockname(fd, (struct sockaddr *)&addr, &len) != 0) return -1;
    return (int)ntohs(addr.sin_port);
}

trace_receiver *traces_start(const kotlp_config *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "kotlp: trace receiver socket() failed\n");
        return NULL;
    }
    /* The child must not inherit the listening socket: it would be able to
     * accept() our OTLP connections, and it would keep the port bound after we
     * close it. */
    kotlp_set_cloexec(fd);
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Try the requested port; if it is unavailable (already in use, privileged,
     * ...), fall back to an OS-assigned ephemeral port so a busy 4318 never
     * disables tracing. The actual port is advertised to the child later. */
    if (bind_loopback(fd, cfg->otlp_port) != 0) {
        if (cfg->otlp_port == 0 || bind_loopback(fd, 0) != 0) {
            fprintf(stderr, "kotlp: trace receiver cannot bind a loopback port\n");
            close(fd);
            return NULL;
        }
    }
    int port = bound_port(fd);
    if (port < 0) {
        close(fd);
        return NULL;
    }
    if (listen(fd, 16) != 0) {
        close(fd);
        return NULL;
    }
    /* poll() reporting POLLIN does not guarantee accept4() will not block: if
     * the queued connection is reset in between, a blocking listener waits for
     * the next one. That is the same "thread pinned indefinitely" failure the
     * read loop is guarding against, so take the listener out of blocking mode
     * too. macOS and the BSDs pass O_NONBLOCK down to the accepted socket,
     * which is why the read loop treats EAGAIN as "nothing yet". */
    int fl = fcntl(fd, F_GETFL, 0);
    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);

    static trace_receiver t;
    t.listen_fd = fd;
    t.port = port;
    t.cfg = cfg;
    t.stop = 0;
    t.started = false;
    if (pthread_create(&t.thread, NULL, receiver_main, &t) != 0) {
        close(fd);
        return NULL;
    }
    t.started = true;
    return &t;
}

int traces_port(const trace_receiver *t) { return t ? t->port : -1; }

void traces_stop(trace_receiver *t) {
    if (!t || !t->started) return;
    t->stop = 1;
    pthread_join(t->thread, NULL);
    close(t->listen_fd);
    t->started = false;
}

void traces_emit_root_span(const kotlp_config *cfg, pid_t child_pid,
                           const char *trace_id_hex, const char *span_id_hex,
                           uint64_t start_ns, uint64_t end_ns, int exit_code,
                           int term_signal) {
    sb out;
    sb_init(&out);
    sb_puts(&out, "{\"resourceSpans\":[{");
    otel_resource(&out, cfg, child_pid);
    sb_puts(&out, ",\"scopeSpans\":[{\"scope\":{\"name\":\"" KOTLP_SCOPE_NAME
                  "\",\"version\":\"" KOTLP_VERSION "\"},\"spans\":[{");
    sb_puts(&out, "\"traceId\":");
    sb_json_str(&out, trace_id_hex);
    sb_puts(&out, ",\"spanId\":");
    sb_json_str(&out, span_id_hex);
    sb_puts(&out, ",\"name\":");
    {
        sb name;
        sb_init(&name);
        sb_puts(&name, "exec ");
        sb_puts(&name, cfg->argc > 0 ? cfg->argv[0] : "(none)");
        sb_json_str(&out, name.buf);
        sb_free(&name);
    }
    sb_puts(&out, ",\"kind\":1"); /* SPAN_KIND_INTERNAL */
    sb_putf(&out, ",\"startTimeUnixNano\":\"%llu\"",
            (unsigned long long)start_ns);
    sb_putf(&out, ",\"endTimeUnixNano\":\"%llu\"", (unsigned long long)end_ns);
    sb_puts(&out, ",\"attributes\":[");
    otel_attr_int(&out, "process.exit.code", exit_code);
    if (term_signal > 0) {
        sb_putc(&out, ',');
        otel_attr_int(&out, "process.exit.signal", term_signal);
    }
    /* How many log-N.ndjson files --log-flush-interval produced. The sink is
     * sealed before this span is built, so the count is final and this record
     * lands in the last of those files. */
    if (cfg->log_dir && cfg->log_flush_interval_s > 0) {
        sb_putc(&out, ',');
        otel_attr_int(&out, "kotlp.log.file.count", filesink_file_count());
    }
    sb_puts(&out, "],\"status\":{");
    if (exit_code == 0 && term_signal == 0) {
        sb_puts(&out, "\"code\":1"); /* STATUS_CODE_OK */
    } else {
        sb_puts(&out, "\"code\":2,\"message\":"); /* STATUS_CODE_ERROR */
        char msg[64];
        if (term_signal > 0)
            snprintf(msg, sizeof(msg), "terminated by signal %d", term_signal);
        else
            snprintf(msg, sizeof(msg), "exited with code %d", exit_code);
        sb_json_str(&out, msg);
    }
    sb_puts(&out, "}}]}]}]}");
    otel_emit(STDOUT_FILENO, &out);
    sb_free(&out);
}
