/* main.c - koltp entry point: parse args, spawn the wrapped command, wire up the
 * three observability features and proxy the child's exit status. */
#include "koltp.h"

#include <errno.h>
#include <libgen.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static volatile sig_atomic_t g_child_pid = 0;

static void forward_signal(int sig) {
    if (g_child_pid > 0) kill(g_child_pid, sig);
}

static void usage(FILE *f) {
    fprintf(f,
        "koltp " KOLTP_VERSION " - portable OpenTelemetry process wrapper\n"
        "\n"
        "Usage:\n"
        "  koltp [options] -- <command> [args...]\n"
        "  koltp [options] <command> [args...]\n"
        "\n"
        "Wraps <command>, emitting OpenTelemetry JSON (NDJSON) to the console:\n"
        "  - logs   : the child's stdout/stderr as OTLP log records\n"
        "  - metrics: periodic CPU / memory / IO / fd usage of the child\n"
        "  - traces : embedded OTLP/HTTP receiver + a root execution span\n"
        "\n"
        "Options:\n"
        "  -s, --service-name NAME  service.name resource attribute\n"
        "                           (default: $OTEL_SERVICE_NAME or command name)\n"
        "  -i, --interval MS        metrics sampling interval (default: 1000)\n"
        "  -p, --otlp-port PORT     embedded OTLP/HTTP port (default: 4318;\n"
        "                           falls back to a free port if it is busy.\n"
        "                           Use 0 to always pick a free port)\n"
        "      --no-logs            disable log capture (pass output through)\n"
        "      --no-metrics         disable resource sampling\n"
        "      --no-traces          disable the embedded trace receiver\n"
        "  -f, --format FORMAT      output format (default: kjson):\n"
        "                             kjson - ::{\"oltp\":<json>}:: framed records\n"
        "                             json  - bare OTLP JSON (newline-delimited)\n"
        "  -V, --version            print version and exit\n"
        "  -h, --help               print this help and exit\n");
}

static int parse_args(int argc, char **argv, koltp_config *cfg) {
    cfg->service_name = getenv("OTEL_SERVICE_NAME");
    cfg->interval_ms = 1000;
    cfg->otlp_port = 4318;
    cfg->enable_logs = true;
    cfg->enable_metrics = true;
    cfg->enable_traces = true;
    cfg->wrap_otel = true;
    cfg->argv = NULL;
    cfg->argc = 0;

    int i = 1;
    for (; i < argc; i++) {
        const char *a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break; /* start of the command */
        if (strcmp(a, "--") == 0) {
            i++;
            break;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(stdout);
            exit(0);
        } else if (strcmp(a, "-V") == 0 || strcmp(a, "--version") == 0) {
            printf("koltp %s\n", KOLTP_VERSION);
            exit(0);
        } else if (strcmp(a, "-s") == 0 || strcmp(a, "--service-name") == 0) {
            if (++i >= argc) goto missing;
            cfg->service_name = argv[i];
        } else if (strcmp(a, "-i") == 0 || strcmp(a, "--interval") == 0) {
            if (++i >= argc) goto missing;
            cfg->interval_ms = strtol(argv[i], NULL, 10);
            if (cfg->interval_ms < 1) cfg->interval_ms = 1;
        } else if (strcmp(a, "-p") == 0 || strcmp(a, "--otlp-port") == 0) {
            if (++i >= argc) goto missing;
            cfg->otlp_port = (int)strtol(argv[i], NULL, 10);
        } else if (strcmp(a, "--no-logs") == 0) {
            cfg->enable_logs = false;
        } else if (strcmp(a, "--no-metrics") == 0) {
            cfg->enable_metrics = false;
        } else if (strcmp(a, "--no-traces") == 0) {
            cfg->enable_traces = false;
        } else if (strcmp(a, "-f") == 0 || strcmp(a, "--format") == 0) {
            if (++i >= argc) goto missing;
            if (strcmp(argv[i], "kjson") == 0) {
                cfg->wrap_otel = true;
            } else if (strcmp(argv[i], "json") == 0) {
                cfg->wrap_otel = false;
            } else {
                fprintf(stderr,
                        "koltp: invalid format '%s' (expected 'kjson' or 'json')\n",
                        argv[i]);
                return -1;
            }
        } else {
            fprintf(stderr, "koltp: unknown option '%s'\n", a);
            usage(stderr);
            return -1;
        }
        continue;
    missing:
        fprintf(stderr, "koltp: option '%s' requires an argument\n", a);
        return -1;
    }

    if (i >= argc) {
        fprintf(stderr, "koltp: no command given\n\n");
        usage(stderr);
        return -1;
    }
    cfg->argv = &argv[i];
    cfg->argc = argc - i;

    if (!cfg->service_name || !cfg->service_name[0]) {
        /* derive from the command's base name */
        static char namebuf[256];
        strncpy(namebuf, cfg->argv[0], sizeof(namebuf) - 1);
        namebuf[sizeof(namebuf) - 1] = '\0';
        cfg->service_name = basename(namebuf);
    }
    return 0;
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
            setenv("OTEL_EXPORTER_OTLP_PROTOCOL", "http/json", 1);
            setenv("OTEL_EXPORTER_OTLP_TRACES_PROTOCOL", "http/json", 1);
            setenv("OTEL_EXPORTER_OTLP_COMPRESSION", "none", 1);
            setenv("OTEL_TRACES_EXPORTER", "otlp", 1);
        }
    }

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

    int exit_code = 0;
    int term_signal = 0;
    if (WIFEXITED(status)) {
        exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        term_signal = WTERMSIG(status);
        exit_code = 128 + term_signal;
    }

    uint64_t end_ns = koltp_now_unix_nano();

    if (cfg.enable_metrics) metrics_emit_final(&cfg, pid, &ru);
    if (cfg.enable_traces)
        traces_emit_root_span(&cfg, pid, trace_id, span_id, start_ns, end_ns,
                              WIFEXITED(status) ? WEXITSTATUS(status) : exit_code,
                              term_signal);

    return exit_code;
}
