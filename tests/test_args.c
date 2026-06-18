/* test_args.c - tests for command-line parsing, in particular the feature
 * toggles and the -r/--raw shorthand. */
#include "koltp.h"
#include "test.h"

#include <stdlib.h>
#include <string.h>

/* parse_args reads $OTEL_SERVICE_NAME; clear it so the env doesn't leak into
 * the service.name expectations below. */
static void clear_env(void) { unsetenv("OTEL_SERVICE_NAME"); }

static void test_defaults(void) {
    clear_env();
    char *argv[] = {"koltp", "echo", "hi", NULL};
    koltp_config cfg;
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
    char *argv[] = {"koltp", "--no-logs", "--no-metrics", "--no-traces",
                    "echo", NULL};
    koltp_config cfg;
    CHECK(parse_args(5, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
}

/* -r and --raw must disable all three features, exactly as the long-form
 * combination above does. */
static void test_raw_long(void) {
    clear_env();
    char *argv[] = {"koltp", "--raw", "echo", "hi", NULL};
    koltp_config cfg;
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
    char *argv[] = {"koltp", "-r", "echo", NULL};
    koltp_config cfg;
    CHECK(parse_args(3, argv, &cfg) == 0);
    CHECK(cfg.enable_logs == false);
    CHECK(cfg.enable_metrics == false);
    CHECK(cfg.enable_traces == false);
}

/* --raw composes with other options regardless of position. */
static void test_raw_with_other_options(void) {
    clear_env();
    char *argv[] = {"koltp", "-s", "svc", "-r", "--", "echo", NULL};
    koltp_config cfg;
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
    char *argv[] = {"koltp", "--debug", "echo", "hi", NULL};
    koltp_config cfg;
    CHECK(parse_args(4, argv, &cfg) == 0);
    CHECK(cfg.debug == true);
    /* telemetry stays on so the OTEL_* env is still sent to the child */
    CHECK(cfg.enable_logs == true);
    CHECK(cfg.enable_metrics == true);
    CHECK(cfg.enable_traces == true);
}

static void test_debug_short(void) {
    clear_env();
    char *argv[] = {"koltp", "-d", "echo", NULL};
    koltp_config cfg;
    CHECK(parse_args(3, argv, &cfg) == 0);
    CHECK(cfg.debug == true);
    CHECK(cfg.enable_traces == true);
}

static void test_debug_default_off(void) {
    clear_env();
    char *argv[] = {"koltp", "echo", NULL};
    koltp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == 0);
    CHECK(cfg.debug == false);
}

static void test_no_command_is_error(void) {
    clear_env();
    char *argv[] = {"koltp", "-r", NULL};
    koltp_config cfg;
    CHECK(parse_args(2, argv, &cfg) == -1);
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
    test_no_command_is_error();
}
