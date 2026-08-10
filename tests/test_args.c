/* test_args.c - tests for command-line parsing, in particular the feature
 * toggles and the -r/--raw shorthand. */
#include "kotlp.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* parse_args reads $OTEL_SERVICE_NAME; clear it so the env doesn't leak into
 * the service.name expectations below. */
static void clear_env(void) { unsetenv("OTEL_SERVICE_NAME"); }

static void test_defaults(void) {
    clear_env();
    char *argv[] = {"kotlp", "echo", "hi", NULL};
    kotlp_config cfg;
    CHECK(parse_args(3, argv, &cfg) == 0);
    /* every feature is on by default */
    CHECK(cfg.enable_logs == true);
    CHECK(cfg.enable_metrics == true);
    CHECK(cfg.enable_traces == true);
    CHECK(cfg.interval_ms == 1000);
    CHECK(cfg.otlp_port == 4318);
    CHECK(cfg.wrap_otel == true);
    CHECK(cfg.argc == 2);
    CHECK_STR_EQ(cfg.argv[0], "echo");
    /* service name derives from the command's base name */
    CHECK_STR_EQ(cfg.service_name, "echo");
}

static void test_individual_disable_flags(void) {
    clear_env();
    char *argv[] = {"kotlp", "--no-logs", "--no-metrics", "--no-traces",
                    "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(5, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
}

/* -r and --raw must disable all three features, exactly as the long-form
 * combination above does. */
static void test_raw_long(void) {
    clear_env();
    char *argv[] = {"kotlp", "--raw", "echo", "hi", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
    /* unrelated settings keep their defaults */
    CHECK(cfg.wrap_otel == true);
    CHECK(cfg.argc == 2);
    CHECK_STR_EQ(cfg.argv[0], "echo");
}

static void test_raw_short(void) {
    clear_env();
    char *argv[] = {"kotlp", "-r", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(3, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
}

/* --raw composes with other options regardless of position. */
static void test_raw_with_other_options(void) {
    clear_env();
    char *argv[] = {"kotlp", "-s", "svc", "-r", "--", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(6, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
    CHECK_STR_EQ(cfg.service_name, "svc");
}

/* -d/--debug shows logs raw but keeps every telemetry feature (and the env)
 * enabled - it is NOT the same as --raw / --no-*. */
static void test_debug_long(void) {
    clear_env();
    char *argv[] = {"kotlp", "--debug", "echo", "hi", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == 0);
    CHECK(cfg.debug == true);
    /* telemetry stays on so the OTEL_* env is still sent to the child */
    CHECK(cfg.enable_logs == true);
    CHECK(cfg.enable_metrics == true);
    CHECK(cfg.enable_traces == true);
}

static void test_debug_short(void) {
    clear_env();
    char *argv[] = {"kotlp", "-d", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(3, argv, &cfg) == 0);
    CHECK(cfg.debug == true);
    CHECK(cfg.enable_traces == true);
}

static void test_debug_default_off(void) {
    clear_env();
    char *argv[] = {"kotlp", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == 0);
    CHECK(cfg.debug == false);
}

/* --protocol selects what the child exports; default leaves it unset so the
 * OTEL_* env can still override it. */
static void test_protocol_default_unset(void) {
    clear_env();
    char *argv[] = {"kotlp", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == 0);
    CHECK(cfg.otlp_protocol == NULL);
}

static void test_protocol_json(void) {
    clear_env();
    char *argv[] = {"kotlp", "--protocol", "json", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == 0);
    CHECK(cfg.otlp_protocol != NULL);
    CHECK_STR_EQ(cfg.otlp_protocol, "http/json");
}

static void test_protocol_protobuf_aliases(void) {
    clear_env();
    /* short alias */
    char *argv1[] = {"kotlp", "-P", "protobuf", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv1, &cfg) == 0);
    CHECK_STR_EQ(cfg.otlp_protocol, "http/protobuf");
    /* canonical spelling */
    char *argv2[] = {"kotlp", "--protocol", "http/protobuf", "echo", NULL};
    CHECK(parse_args(4, argv2, &cfg) == 0);
    CHECK_STR_EQ(cfg.otlp_protocol, "http/protobuf");
}

static void test_protocol_invalid(void) {
    clear_env();
    char *argv[] = {"kotlp", "--protocol", "grpc", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == -1);
}

static void test_protocol_missing_arg(void) {
    clear_env();
    char *argv[] = {"kotlp", "--protocol", NULL};
    kotlp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == -1);
}

static void test_no_command_is_error(void) {
    clear_env();
    char *argv[] = {"kotlp", "-r", NULL};
    kotlp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == -1);
}

/* --log-dir / --log-flush-interval: the file sink is off unless asked for. */
static void test_log_dir_default_off(void) {
    clear_env();
    char *argv[] = {"kotlp", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == 0);
    CHECK(cfg.log_dir == NULL);
    CHECK(cfg.log_flush_interval_s == 0);
}

static void test_log_dir(void) {
    clear_env();
    char *argv[] = {"kotlp", "--log-dir", "/tmp/kotlp-logs", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == 0);
    CHECK(cfg.log_dir != NULL);
    CHECK_STR_EQ(cfg.log_dir, "/tmp/kotlp-logs");
    /* no rotation unless --log-flush-interval is given */
    CHECK(cfg.log_flush_interval_s == 0);
}

static void test_log_flush_interval(void) {
    clear_env();
    char *argv[] = {"kotlp", "--log-dir", "/tmp/kotlp-logs",
                    "--log-flush-interval", "5", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(6, argv, &cfg) == 0);
    CHECK_STR_EQ(cfg.log_dir, "/tmp/kotlp-logs");
    CHECK(cfg.log_flush_interval_s == 5);
}

/* the two options may be given in either order */
static void test_log_flush_interval_before_dir(void) {
    clear_env();
    char *argv[] = {"kotlp", "--log-flush-interval", "2", "--log-dir",
                    "/tmp/kotlp-logs", "--", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(7, argv, &cfg) == 0);
    CHECK(cfg.log_flush_interval_s == 2);
    CHECK_STR_EQ(cfg.log_dir, "/tmp/kotlp-logs");
}

/* rotating without a destination directory is a usage error */
static void test_log_flush_interval_requires_dir(void) {
    clear_env();
    char *argv[] = {"kotlp", "--log-flush-interval", "5", "echo", NULL};
    kotlp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == -1);
}

static void test_log_flush_interval_invalid(void) {
    clear_env();
    kotlp_config cfg;
    char *zero[] = {"kotlp", "--log-dir", "/tmp/x", "--log-flush-interval", "0",
                    "echo", NULL};
    CHECK(parse_args(6, zero, &cfg) == -1);
    char *neg[] = {"kotlp", "--log-dir", "/tmp/x", "--log-flush-interval", "-3",
                   "echo", NULL};
    CHECK(parse_args(6, neg, &cfg) == -1);
    char *nan[] = {"kotlp", "--log-dir", "/tmp/x", "--log-flush-interval",
                   "soon", "echo", NULL};
    CHECK(parse_args(6, nan, &cfg) == -1);
}

static void test_log_options_missing_arg(void) {
    clear_env();
    kotlp_config cfg;
    char *dir[] = {"kotlp", "--log-dir", NULL};
    CHECK(parse_args(2, dir, &cfg) == -1);
    char *interval[] = {"kotlp", "--log-dir", "/tmp/x", "--log-flush-interval",
                        NULL};
    CHECK(parse_args(4, interval, &cfg) == -1);
}

void test_args(void) {
    test_defaults();
    test_individual_disable_flags();
    test_raw_long();
    test_raw_short();
    test_raw_with_other_options();
    test_debug_long();
    test_debug_short();
    test_debug_default_off();
    test_protocol_default_unset();
    test_protocol_json();
    test_protocol_protobuf_aliases();
    test_protocol_invalid();
    test_protocol_missing_arg();
    test_no_command_is_error();
    test_log_dir_default_off();
    test_log_dir();
    test_log_flush_interval();
    test_log_flush_interval_before_dir();
    test_log_flush_interval_requires_dir();
    test_log_flush_interval_invalid();
    test_log_options_missing_arg();
}
