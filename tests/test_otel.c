/* test_otel.c - tests for the OTLP/JSON attribute and resource builders. */
#include "koltp.h"
#include "test.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

static void test_attr_str(void) {
    sb s;
    sb_init(&s);
    otel_attr_str(&s, "service.name", "checkout");
    CHECK_STR_EQ(s.buf,
                 "{\"key\":\"service.name\",\"value\":{\"stringValue\":"
                 "\"checkout\"}}");
    sb_free(&s);

    /* values must be JSON-escaped */
    sb_init(&s);
    otel_attr_str(&s, "k", "a\"b");
    CHECK_CONTAINS(s.buf, "\"stringValue\":\"a\\\"b\"");
    sb_free(&s);
}

static void test_attr_int(void) {
    sb s;
    sb_init(&s);
    otel_attr_int(&s, "process.pid", 12345);
    /* OTLP/JSON encodes 64-bit ints as strings */
    CHECK_STR_EQ(s.buf,
                 "{\"key\":\"process.pid\",\"value\":{\"intValue\":"
                 "\"12345\"}}");
    sb_free(&s);

    sb_init(&s);
    otel_attr_int(&s, "n", -7);
    CHECK_CONTAINS(s.buf, "\"intValue\":\"-7\"");
    sb_free(&s);
}

static void test_resource(void) {
    char *argv[] = {"echo", "hi", NULL};
    koltp_config cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.service_name = "my-svc";
    cfg.argv = argv;
    cfg.argc = 2;

    sb s;
    sb_init(&s);
    otel_resource(&s, &cfg, 4242);

    CHECK_CONTAINS(s.buf, "\"resource\":{\"attributes\":[");
    CHECK_CONTAINS(s.buf, "\"service.name\"");
    CHECK_CONTAINS(s.buf, "\"my-svc\"");
    CHECK_CONTAINS(s.buf, "\"process.pid\"");
    CHECK_CONTAINS(s.buf, "\"4242\"");
    CHECK_CONTAINS(s.buf, "\"process.executable.name\"");
    CHECK_CONTAINS(s.buf, "\"echo\"");
    CHECK_CONTAINS(s.buf, "\"process.command_line\"");
    CHECK_CONTAINS(s.buf, "\"echo hi\"");
    CHECK_CONTAINS(s.buf, "\"telemetry.sdk.name\"");
    /* well-formed: balanced enough to end with the closing of attributes */
    CHECK_CONTAINS(s.buf, "]}");
    sb_free(&s);
}

/* Read one record back from a pipe into a NUL-terminated buffer. */
static void read_back(int rfd, char *buf, size_t cap) {
    ssize_t n = read(rfd, buf, cap - 1);
    if (n < 0) n = 0;
    buf[n] = '\0';
}

static void test_emit_framing(void) {
    int p[2];
    CHECK(pipe(p) == 0);
    char buf[128];

    sb s;
    sb_init(&s);
    sb_puts(&s, "{\"a\":1}");

    /* default: framed as ::{"oltp":<json>}:: */
    otel_emit_init(true, false);
    otel_emit(p[1], &s);
    read_back(p[0], buf, sizeof(buf));
    CHECK_STR_EQ(buf, "::{\"oltp\":{\"a\":1}}::\n");

    /* -f json: bare JSON, no framing */
    otel_emit_init(false, false);
    otel_emit(p[1], &s);
    read_back(p[0], buf, sizeof(buf));
    CHECK_STR_EQ(buf, "{\"a\":1}\n");

    /* passthrough is never framed, even when framing is enabled */
    otel_emit_init(true, false);
    otel_emit_raw(p[1], "plain text", 10);
    read_back(p[0], buf, sizeof(buf));
    CHECK_STR_EQ(buf, "plain text\n");

    sb_free(&s);
    close(p[0]);
    close(p[1]);
}

/* --log-dir: otel_emit's console side goes silent (the file gets the record
 * instead; logs_pump is what prints the raw line on the console). */
static void test_emit_console_quiet(void) {
    int p[2];
    CHECK(pipe(p) == 0);

    sb s;
    sb_init(&s);
    sb_puts(&s, "{\"a\":1}");

    otel_emit_init(true, true);
    CHECK(fcntl(p[0], F_SETFL, O_NONBLOCK) == 0);
    otel_emit(p[1], &s);
    char buf[16];
    ssize_t n = read(p[0], buf, sizeof(buf));
    CHECK(n < 0 && errno == EAGAIN);

    sb_free(&s);
    close(p[0]);
    close(p[1]);
    otel_emit_init(true, false); /* restore default for any test that follows */
}

void test_otel(void) {
    test_attr_str();
    test_attr_int();
    test_resource();
    test_emit_framing();
    test_emit_console_quiet();
}
