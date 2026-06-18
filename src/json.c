/* json.c - a tiny growable string builder with JSON string escaping.
 *
 * We only ever *write* JSON (NDJSON), so there is no parser here; the trace
 * receiver forwards the child's OTLP payload verbatim. */
#include "koltp.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sb_grow(sb *s, size_t extra) {
    if (s->len + extra + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap : 256;
    while (cap < s->len + extra + 1) cap *= 2;
    char *buf = realloc(s->buf, cap);
    if (!buf) {
        /* Out of memory in an observability sidecar should not take down the
         * wrapped workload silently; bail loudly instead. */
        fprintf(stderr, "koltp: out of memory\n");
        abort();
    }
    s->buf = buf;
    s->cap = cap;
}

void sb_init(sb *s) {
    s->buf = NULL;
    s->len = 0;
    s->cap = 0;
}

void sb_free(sb *s) {
    free(s->buf);
    sb_init(s);
}

void sb_reset(sb *s) { s->len = 0; }

void sb_putc(sb *s, char c) {
    sb_grow(s, 1);
    s->buf[s->len++] = c;
    s->buf[s->len] = '\0';
}

void sb_puts(sb *s, const char *str) {
    size_t n = strlen(str);
    sb_grow(s, n);
    memcpy(s->buf + s->len, str, n);
    s->len += n;
    s->buf[s->len] = '\0';
}

void sb_putf(sb *s, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) {
        va_end(ap2);
        return;
    }
    sb_grow(s, (size_t)n);
    vsnprintf(s->buf + s->len, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
    s->len += (size_t)n;
}

void sb_json_strn(sb *s, const char *str, size_t n) {
    static const char hex[] = "0123456789abcdef";
    sb_putc(s, '"');
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
        case '"': sb_puts(s, "\\\""); break;
        case '\\': sb_puts(s, "\\\\"); break;
        case '\b': sb_puts(s, "\\b"); break;
        case '\f': sb_puts(s, "\\f"); break;
        case '\n': sb_puts(s, "\\n"); break;
        case '\r': sb_puts(s, "\\r"); break;
        case '\t': sb_puts(s, "\\t"); break;
        default:
            if (c < 0x20) {
                char esc[7] = {'\\', 'u', '0', '0', hex[(c >> 4) & 0xf],
                               hex[c & 0xf], '\0'};
                sb_puts(s, esc);
            } else {
                sb_putc(s, (char)c);
            }
        }
    }
    sb_putc(s, '"');
}

void sb_json_str(sb *s, const char *str) {
    sb_json_strn(s, str, strlen(str));
}
