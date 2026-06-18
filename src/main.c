/* main.c - koltp entry point: parse args, spawn the wrapped command, wire up the
 * three observability features and proxy the child's exit status. */
#include "koltp.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t g_child_pid = 0;

static void forward_signal(int sig) {
    if (g_child_pid > 0) kill(g_child_pid, sig);
}

/* Print every OTEL_* environment variable the child will inherit, so it is
 * clear which telemetry configuration koltp is sending. */
static void debug_dump_env(void) {
    for (char **e = environ; e && *e; e++) {
        if (strncmp(*e, "OTEL_", 5) == 0) fprintf(stderr, "koltp: env %s\n", *e);
    }
}

int main(int argc, char **argv) {
    koltp_config cfg;
    if (parse_args(argc, argv, &cfg) != 0) return 2;

    otel_emit_init(cfg.wrap_otel);
    setenv("OTEL_SERVICE_NAME", cfg.service_name, 1);

    /* Start the embedded trace receiver and advertise it to the child. */
    trace_receiver *traces = NULL;
    if (cfg.enable_traces) {
        traces = traces_start(&cfg);
        if (traces) {
            char endpoint[64];
            snprintf(endpoint, sizeof(endpoint), "http://127.0.0.1:%d",
                     traces_port(traces));
            setenv("OTEL_EXPORTER_OTLP_ENDPOINT", endpoint, 1);
            /* The receiver decodes both http/json and http/protobuf. With
             * --protocol the choice is explicit and wins; otherwise default to
             * http/json but leave any pre-set OTEL_* protocol untouched. */
            const char *proto = cfg.otlp_protocol ? cfg.otlp_protocol
                                                  : "http/json";
            int proto_force = cfg.otlp_protocol ? 1 : 0;
            setenv("OTEL_EXPORTER_OTLP_PROTOCOL", proto, proto_force);
            setenv("OTEL_EXPORTER_OTLP_TRACES_PROTOCOL", proto, proto_force);
            /* Force uncompressed bodies: the receiver does not decompress. */
            setenv("OTEL_EXPORTER_OTLP_COMPRESSION", "none", 1);
            setenv("OTEL_TRACES_EXPORTER", "otlp", 1);
        }
    }

    if (cfg.debug) debug_dump_env();

    char trace_id[33];
    char span_id[17];
    koltp_rand_hex(trace_id, 16);
    koltp_rand_hex(span_id, 8);

    uint64_t start_ns = koltp_now_unix_nano();

    int out_fd = -1, err_fd = -1;
    pid_t pid = child_spawn(&cfg, &out_fd, &err_fd);
    if (pid < 0) {
        if (traces) traces_stop(traces);
        return 1;
    }
    g_child_pid = pid;

    /* Forward common termination signals to the wrapped process. */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = forward_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    metrics_sampler *metrics = NULL;
    if (cfg.enable_metrics) metrics = metrics_start(&cfg, pid);

    /* Drain stdout/stderr until both close (the child has finished writing). */
    logs_pump(&cfg, pid, out_fd, err_fd);

    int status = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof(ru));
    while (wait4(pid, &status, 0, &ru) < 0 && errno == EINTR) {
    }
    g_child_pid = 0;

    if (metrics) metrics_stop(metrics);
    if (traces) traces_stop(traces);

    int exit_code = child_exit_code(status);
    int term_signal = WIFSIGNALED(status) ? WTERMSIG(status) : 0;

    uint64_t end_ns = koltp_now_unix_nano();

    if (cfg.enable_metrics) metrics_emit_final(&cfg, pid, &ru);
    if (cfg.enable_traces)
        traces_emit_root_span(&cfg, pid, trace_id, span_id, start_ns, end_ns,
                              exit_code, term_signal);

    return exit_code;
}
