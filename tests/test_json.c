/* test_json.c - tests for the growable string builder and JSON escaping. */
#include "kotlp.h"
#include "test.h"

static void test_sb_basics(void) {
    sb s;
    sb_init(&s);
    CHECK(s.len == 0);

    sb_putc(&s, 'a');
    sb_puts(&s, "bc");
    CHECK_STR_EQ(s.buf, "abc");
    CHECK(s.len == 3);

    sb_putf(&s, "-%d-%s", 42, "x");
    CHECK_STR_EQ(s.buf, "abc-42-x");

    sb_reset(&s);
    CHECK(s.len == 0);
    sb_puts(&s, "fresh");
    CHECK_STR_EQ(s.buf, "fresh");

    sb_free(&s);
    CHECK(s.buf == NULL);
    CHECK(s.len == 0);
}

static void test_sb_growth(void) {
    sb s;
    sb_init(&s);
    for (int i = 0; i < 10000; i++) sb_putc(&s, 'z');
    CHECK(s.len == 10000);
    CHECK(s.buf[9999] == 'z');
    CHECK(s.buf[10000] == '\0'); /* always NUL-terminated */
    sb_free(&s);
}

static void test_json_escaping(void) {
    sb s;

    sb_init(&s);
    sb_json_str(&s, "plain");
    CHECK_STR_EQ(s.buf, "\"plain\"");
    sb_free(&s);

    sb_init(&s);
    sb_json_str(&s, "he\"llo");
    CHECK_STR_EQ(s.buf, "\"he\\\"llo\""); /* quote -> \" */
    sb_free(&s);

    sb_init(&s);
    sb_json_str(&s, "a\\b");
    CHECK_STR_EQ(s.buf, "\"a\\\\b\""); /* backslash -> \\ */
    sb_free(&s);

    sb_init(&s);
    sb_json_str(&s, "line1\nline2\ttab\r");
    CHECK_STR_EQ(s.buf, "\"line1\\nline2\\ttab\\r\"");
    sb_free(&s);

    /* control character below 0x20 must become \u00XX */
    sb_init(&s);
    char ctrl[2] = {0x01, '\0'};
    sb_json_str(&s, ctrl);
    CHECK_STR_EQ(s.buf, "\"\\u0001\"");
    sb_free(&s);
}

static void test_json_strn(void) {
    sb s;
    sb_init(&s);
    /* only the first 3 bytes should be emitted */
    sb_json_strn(&s, "abcdef", 3);
    CHECK_STR_EQ(s.buf, "\"abc\"");
    sb_free(&s);
}

void test_json(void) {
    test_sb_basics();
    test_sb_growth();
    test_json_escaping();
    test_json_strn();
}
