/* util.c - time, randomness and host identity helpers. */
#include "kotlp.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#if defined(__has_include)
#  if __has_include(<sys/random.h>)
#    include <sys/random.h>
#    define KOTLP_HAVE_GETRANDOM 1
#  endif
#endif

uint64_t kotlp_now_unix_nano(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_random(unsigned char *buf, size_t n) {
#ifdef KOTLP_HAVE_GETRANDOM
    size_t off = 0;
    while (off < n) {
        ssize_t r = getrandom(buf + off, n - off, 0);
        if (r > 0) {
            off += (size_t)r;
            continue;
        }
        break; /* fall through to the weak fallback below */
    }
    if (off == n) return;
#endif
    /* Fallback: not cryptographic, but trace/span ids only need uniqueness. */
    static uint64_t state;
    if (!state) state = kotlp_now_unix_nano() ^ ((uint64_t)getpid() << 32);
    for (size_t i = 0; i < n; i++) {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        buf[i] = (unsigned char)(state >> 33);
    }
}

void kotlp_rand_hex(char *out, size_t n) {
    static const char hex[] = "0123456789abcdef";
    unsigned char raw[32];
    if (n > sizeof(raw)) n = sizeof(raw);
    fill_random(raw, n);
    for (size_t i = 0; i < n; i++) {
        out[i * 2] = hex[(raw[i] >> 4) & 0xf];
        out[i * 2 + 1] = hex[raw[i] & 0xf];
    }
    out[n * 2] = '\0';
}

const char *kotlp_hostname(void) {
    static char name[256];
    if (name[0]) return name;
    if (gethostname(name, sizeof(name) - 1) != 0) {
        strcpy(name, "unknown");
    }
    name[sizeof(name) - 1] = '\0';
    return name;
}
