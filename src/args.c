/* args.c - command-line parsing for koltp.
 *
 * Kept separate from main.c so the parser can be exercised directly by the unit
 * tests (the test runner links every object except main.o). */
#include "koltp.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void usage(FILE *f) {
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
        "  -r, --raw                disable everything (equivalent to\n"
        "                           --no-logs --no-metrics --no-traces)\n"
        "  -d, --debug              keep all telemetry (metrics, traces, the\n"
        "                           OTEL_* env) but print the child's logs raw,\n"
        "                           and dump the OTEL_* env to stderr\n"
        "  -P, --protocol PROTO     OTLP protocol the child exports with\n"
        "                           (default: json): 'json' or 'protobuf'\n"
        "                           (the receiver accepts both)\n"
        "  -f, --format FORMAT      output format (default: kjson):\n"
        "                             kjson - ::{\"oltp\":<json>}:: framed records\n"
        "                             json  - bare OTLP JSON (newline-delimited)\n"
        "  -V, --version            print version and exit\n"
        "  -h, --help               print this help and exit\n");
}

int parse_args(int argc, char **argv, koltp_config *cfg) {
    cfg->service_name = getenv("OTEL_SERVICE_NAME");
    cfg->interval_ms = 1000;
    cfg->otlp_port = 4318;
    cfg->enable_logs = true;
    cfg->enable_metrics = true;
    cfg->enable_traces = true;
    cfg->wrap_otel = true;
    cfg->debug = false;
    cfg->otlp_protocol = NULL;
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
        } else if (strcmp(a, "-r") == 0 || strcmp(a, "--raw") == 0) {
            /* shorthand for --no-logs --no-metrics --no-traces */
            cfg->enable_logs = false;
            cfg->enable_metrics = false;
            cfg->enable_traces = false;
        } else if (strcmp(a, "-d") == 0 || strcmp(a, "--debug") == 0) {
            /* keep the full telemetry env (receiver, metrics, OTEL_* vars)
             * but print the child's stdout/stderr raw for readability */
            cfg->debug = true;
        } else if (strcmp(a, "-P") == 0 || strcmp(a, "--protocol") == 0) {
            if (++i >= argc) goto missing;
            /* the embedded receiver decodes both; this picks what the child
             * emits. Accept the short and canonical OTLP spellings. */
            if (strcmp(argv[i], "json") == 0 ||
                strcmp(argv[i], "http/json") == 0) {
                cfg->otlp_protocol = "http/json";
            } else if (strcmp(argv[i], "protobuf") == 0 ||
                       strcmp(argv[i], "http/protobuf") == 0) {
                cfg->otlp_protocol = "http/protobuf";
            } else {
                fprintf(stderr,
                        "koltp: invalid protocol '%s' (expected 'json' or "
                        "'protobuf')\n",
                        argv[i]);
                return -1;
            }
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
