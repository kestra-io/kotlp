/* test_util.c - tests for time, random id generation and hostname helpers. */
#include "kotlp.h"
#include "test.h"

#include <string.h>

static int is_lower_hex(const char *s) {
    for (; *s; s++) {
        if (!((*s >= '0' && *s <= '9') || (*s >= 'a' && *s <= 'f'))) return 0;
    }
    return 1;
}

static void test_now_unix_nano(void) {
    uint64_t a = kotlp_now_unix_nano();
    uint64_t b = kotlp_now_unix_nano();
    /* well past the year 2001 in nanoseconds -> clock is plausibly real */
    CHECK(a > 1000000000000000000ull);
    CHECK(b >= a); /* non-decreasing */
}

/* The receiver measures its request deadlines with this, so it must never run
 * backwards - a single backwards step there is an instant spurious timeout. */
static void test_now_mono_ms(void) {
    uint64_t a = kotlp_now_mono_ms();
    uint64_t b = kotlp_now_mono_ms();
    CHECK(b >= a);
    CHECK(a > 0);
    /* elapsed time is measurable and sane over a short busy wait */
    uint64_t start = kotlp_now_mono_ms();
    volatile unsigned long spin = 0;
    while (kotlp_now_mono_ms() - start < 5) spin++;
    uint64_t elapsed = kotlp_now_mono_ms() - start;
    CHECK(elapsed >= 5);
    CHECK(elapsed < 5000); /* not a wildly wrong unit */
}

static void test_rand_hex(void) {
    char id8[17];
    char id16[33];

    kotlp_rand_hex(id8, 8);
    CHECK(strlen(id8) == 16); /* 8 bytes -> 16 hex chars */
    CHECK(is_lower_hex(id8));

    kotlp_rand_hex(id16, 16);
    CHECK(strlen(id16) == 32); /* 16 bytes -> 32 hex chars (trace id) */
    CHECK(is_lower_hex(id16));

    /* two draws should almost never collide */
    char a[33], b[33];
    kotlp_rand_hex(a, 16);
    kotlp_rand_hex(b, 16);
    CHECK(strcmp(a, b) != 0);
}

static void test_hostname(void) {
    const char *h = kotlp_hostname();
    CHECK(h != NULL);
    CHECK(strlen(h) > 0);
    /* cached: a second call returns the same pointer/content */
    CHECK_STR_EQ(kotlp_hostname(), h);
}

void test_util(void) {
    test_now_unix_nano();
    test_now_mono_ms();
    test_rand_hex();
    test_hostname();
}
