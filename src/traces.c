/* traces.c - embedded OTLP/HTTP trace receiver and wrapper root span.
 *
 * We bind a tiny HTTP server on 127.0.0.1:<port> and advertise it to the child
 * via OTEL_EXPORTER_OTLP_* env vars (see main.c). Spans the child's OTel SDK
 * exports over OTLP/HTTP are emitted to the console as NDJSON: http/json bodies
 * are forwarded verbatim, http/protobuf bodies are decoded to the same JSON
 * shape (see otlp_pb.c). We also synthesize one root span covering the whole
 * execution. */
#include "koltp.h"

#include <arpa/inet.h>
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
    const koltp_config *cfg;
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

static long parse_content_length(const char *headers) {
    const char *p = headers;
    while (*p) {
        if (strncasecmp(p, "content-length:", 15) == 0) {
            return strtol(p + 15, NULL, 10);
        }
        const char *nl = strchr(p, '\n');
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
    (void)t;
    sb req;
    sb_init(&req);
    char buf[8192];
    long header_end = -1;
    long content_length = -1;
    long body_start = 0;

    /* Read until we have the full headers plus the declared body. */
    for (;;) {
        ssize_t got = read(fd, buf, sizeof(buf));
        if (got <= 0) break;
        for (ssize_t i = 0; i < got; i++) sb_putc(&req, buf[i]);

        if (header_end < 0) {
            char *he = strstr(req.buf, "\r\n\r\n");
            if (he) {
                header_end = (long)(he - req.buf);
                body_start = header_end + 4;
                content_length = parse_content_length(req.buf);
            }
        }
        if (header_end >= 0) {
            long have_body = (long)req.len - body_start;
            if (content_length < 0 || have_body >= content_length) break;
        }
        if (req.len > 64u * 1024 * 1024) break; /* safety cap */
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
    }

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
        int fd = accept(t->listen_fd, NULL, NULL);
        if (fd < 0) continue;
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

trace_receiver *traces_start(const koltp_config *cfg) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        fprintf(stderr, "koltp: trace receiver socket() failed\n");
        return NULL;
    }
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    /* Try the requested port; if it is unavailable (already in use, privileged,
     * ...), fall back to an OS-assigned ephemeral port so a busy 4318 never
     * disables tracing. The actual port is advertised to the child later. */
    if (bind_loopback(fd, cfg->otlp_port) != 0) {
        if (cfg->otlp_port == 0 || bind_loopback(fd, 0) != 0) {
            fprintf(stderr, "koltp: trace receiver cannot bind a loopback port\n");
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

void traces_emit_root_span(const koltp_config *cfg, pid_t child_pid,
                           const char *trace_id_hex, const char *span_id_hex,
                           uint64_t start_ns, uint64_t end_ns, int exit_code,
                           int term_signal) {
    sb out;
    sb_init(&out);
    sb_puts(&out, "{\"resourceSpans\":[{");
    otel_resource(&out, cfg, child_pid);
    sb_puts(&out, ",\"scopeSpans\":[{\"scope\":{\"name\":\"" KOLTP_SCOPE_NAME
                  "\",\"version\":\"" KOLTP_VERSION "\"},\"spans\":[{");
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
