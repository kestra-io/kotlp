/* test_otlp_pb.c - tests for the OTLP/protobuf -> OTLP/JSON trace decoder.
 *
 * We hand-encode protobuf payloads with a tiny builder (mirroring how an OTel
 * SDK would serialize on the wire), run them through otlp_traces_pb_to_json,
 * and assert on the resulting JSON. */
#include "koltp.h"
#include "test.h"

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------ tiny protobuf encoder */

typedef struct {
    uint8_t b[8192];
    size_t n;
} pb;

static void pb_init(pb *p) { p->n = 0; }
static void pb_raw(pb *p, const uint8_t *d, size_t l) {
    memcpy(p->b + p->n, d, l);
    p->n += l;
}
static void pb_varint(pb *p, uint64_t v) {
    do {
        uint8_t c = v & 0x7f;
        v >>= 7;
        if (v) c |= 0x80;
        p->b[p->n++] = c;
    } while (v);
}
static void pb_tag(pb *p, uint32_t field, uint32_t wire) {
    pb_varint(p, ((uint64_t)field << 3) | wire);
}
/* length-delimited field (string/bytes/sub-message) */
static void pb_ld(pb *p, uint32_t field, const uint8_t *d, size_t l) {
    pb_tag(p, field, 2);
    pb_varint(p, l);
    pb_raw(p, d, l);
}
static void pb_str(pb *p, uint32_t field, const char *s) {
    pb_ld(p, field, (const uint8_t *)s, strlen(s));
}
static void pb_vfield(pb *p, uint32_t field, uint64_t v) {
    pb_tag(p, field, 0);
    pb_varint(p, v);
}
static void pb_fixed64(pb *p, uint32_t field, uint64_t v) {
    pb_tag(p, field, 1);
    for (int i = 0; i < 8; i++) p->b[p->n++] = (uint8_t)(v >> (8 * i));
}
static void pb_sub(pb *p, uint32_t field, const pb *child) {
    pb_ld(p, field, child->b, child->n);
}

/* ----------------------------------------------------------------- tests */

static void decode(const pb *p, sb *out) {
    sb_init(out);
    otlp_traces_pb_to_json(out, p->b, p->n);
}

/* A full TracesData with one span carrying a string and an int attribute. */
static void test_full_span(void) {
    /* AnyValue { string_value = "checkout" } */
    pb sval;
    pb_init(&sval);
    pb_str(&sval, 1, "checkout");
    /* KeyValue { key = "svc", value = sval } */
    pb attr1;
    pb_init(&attr1);
    pb_str(&attr1, 1, "svc");
    pb_sub(&attr1, 2, &sval);

    /* AnyValue { int_value = 42 } */
    pb ival;
    pb_init(&ival);
    pb_vfield(&ival, 3, 42);
    /* KeyValue { key = "http.status", value = ival } */
    pb attr2;
    pb_init(&attr2);
    pb_str(&attr2, 1, "http.status");
    pb_sub(&attr2, 2, &ival);

    /* Status { code = 2 (ERROR), message = "boom" } */
    pb status;
    pb_init(&status);
    pb_str(&status, 2, "boom");
    pb_vfield(&status, 3, 2);

    /* Span */
    pb span;
    pb_init(&span);
    uint8_t tid[16];
    uint8_t sid[8];
    for (int i = 0; i < 16; i++) tid[i] = (uint8_t)(i + 1); /* 0102..10 */
    for (int i = 0; i < 8; i++) sid[i] = (uint8_t)(0xa0 + i);
    pb_ld(&span, 1, tid, 16);             /* trace_id */
    pb_ld(&span, 2, sid, 8);              /* span_id  */
    pb_str(&span, 5, "GET /");            /* name     */
    pb_vfield(&span, 6, 2);               /* kind = SERVER */
    pb_fixed64(&span, 7, 1000000000ull);  /* start    */
    pb_fixed64(&span, 8, 2000000000ull);  /* end      */
    pb_sub(&span, 9, &attr1);             /* attribute */
    pb_sub(&span, 9, &attr2);             /* attribute */
    pb_sub(&span, 15, &status);           /* status   */

    /* ScopeSpans { scope{name=app}, spans=[span] } */
    pb scope;
    pb_init(&scope);
    pb_str(&scope, 1, "app");
    pb scope_spans;
    pb_init(&scope_spans);
    pb_sub(&scope_spans, 1, &scope);
    pb_sub(&scope_spans, 2, &span);

    /* ResourceSpans { resource{attr service.name}, scope_spans } */
    pb rattr_val;
    pb_init(&rattr_val);
    pb_str(&rattr_val, 1, "my-svc");
    pb rattr;
    pb_init(&rattr);
    pb_str(&rattr, 1, "service.name");
    pb_sub(&rattr, 2, &rattr_val);
    pb resource;
    pb_init(&resource);
    pb_sub(&resource, 1, &rattr);
    pb resource_spans;
    pb_init(&resource_spans);
    pb_sub(&resource_spans, 1, &resource);
    pb_sub(&resource_spans, 2, &scope_spans);

    /* TracesData { resource_spans } */
    pb root;
    pb_init(&root);
    pb_sub(&root, 1, &resource_spans);

    sb out;
    decode(&root, &out);

    /* structure */
    CHECK_CONTAINS(out.buf, "\"resourceSpans\":[{");
    CHECK_CONTAINS(out.buf, "\"scopeSpans\":[{");
    CHECK_CONTAINS(out.buf, "\"spans\":[{");
    /* resource + scope */
    CHECK_CONTAINS(out.buf, "\"service.name\"");
    CHECK_CONTAINS(out.buf, "\"my-svc\"");
    CHECK_CONTAINS(out.buf, "\"name\":\"app\"");
    /* span ids as lowercase hex */
    CHECK_CONTAINS(out.buf, "\"traceId\":\"0102030405060708090a0b0c0d0e0f10\"");
    CHECK_CONTAINS(out.buf, "\"spanId\":\"a0a1a2a3a4a5a6a7\"");
    CHECK_CONTAINS(out.buf, "\"name\":\"GET /\"");
    /* enum kind as integer */
    CHECK_CONTAINS(out.buf, "\"kind\":2");
    /* 64-bit times as decimal strings */
    CHECK_CONTAINS(out.buf, "\"startTimeUnixNano\":\"1000000000\"");
    CHECK_CONTAINS(out.buf, "\"endTimeUnixNano\":\"2000000000\"");
    /* attributes: string + int (int encoded as string per OTLP/JSON) */
    CHECK_CONTAINS(out.buf, "\"key\":\"svc\"");
    CHECK_CONTAINS(out.buf, "\"stringValue\":\"checkout\"");
    CHECK_CONTAINS(out.buf, "\"key\":\"http.status\"");
    CHECK_CONTAINS(out.buf, "\"intValue\":\"42\"");
    /* status */
    CHECK_CONTAINS(out.buf, "\"status\":{");
    CHECK_CONTAINS(out.buf, "\"message\":\"boom\"");
    CHECK_CONTAINS(out.buf, "\"code\":2");
    /* output is single-line (NDJSON-safe) */
    CHECK(strchr(out.buf, '\n') == NULL);
    sb_free(&out);
}

/* AnyValue variants: bool, double, bytes, array, kvlist. */
static void test_value_kinds(void) {
    pb attrs; /* a KeyValueList: repeated KeyValue at field 1 */
    pb_init(&attrs);

    /* bool true */
    {
        pb v;
        pb_init(&v);
        pb_vfield(&v, 2, 1);
        pb kv;
        pb_init(&kv);
        pb_str(&kv, 1, "ok");
        pb_sub(&kv, 2, &v);
        pb_sub(&attrs, 1, &kv);
    }
    /* bytes -> base64 ("Man" -> "TWFu") */
    {
        pb v;
        pb_init(&v);
        pb_ld(&v, 7, (const uint8_t *)"Man", 3);
        pb kv;
        pb_init(&kv);
        pb_str(&kv, 1, "raw");
        pb_sub(&kv, 2, &v);
        pb_sub(&attrs, 1, &kv);
    }
    /* array of two ints */
    {
        pb e1, e2;
        pb_init(&e1);
        pb_vfield(&e1, 3, 1);
        pb_init(&e2);
        pb_vfield(&e2, 3, 2);
        pb arr; /* ArrayValue: repeated AnyValue at field 1 */
        pb_init(&arr);
        pb_sub(&arr, 1, &e1);
        pb_sub(&arr, 1, &e2);
        pb v;
        pb_init(&v);
        pb_sub(&v, 5, &arr); /* array_value */
        pb kv;
        pb_init(&kv);
        pb_str(&kv, 1, "list");
        pb_sub(&kv, 2, &v);
        pb_sub(&attrs, 1, &kv);
    }

    sb out;
    sb_init(&out);
    otlp_traces_pb_to_json(&out, attrs.b, 0); /* len 0 -> empty object */
    CHECK_STR_EQ(out.buf, "{}");
    sb_free(&out);

    /* Decode the attrs as a KeyValueList via emit path: wrap into a resource so
     * the public entry point exercises the recursion. */
    pb resource;
    pb_init(&resource);
    /* resource.attributes = the three KeyValues we built */
    pb_raw(&resource, attrs.b, attrs.n); /* already KeyValue fields at field 1 */
    pb rs;
    pb_init(&rs);
    pb_sub(&rs, 1, &resource);
    pb root;
    pb_init(&root);
    pb_sub(&root, 1, &rs);

    sb out2;
    decode(&root, &out2);
    CHECK_CONTAINS(out2.buf, "\"key\":\"ok\"");
    CHECK_CONTAINS(out2.buf, "\"boolValue\":true");
    CHECK_CONTAINS(out2.buf, "\"key\":\"raw\"");
    CHECK_CONTAINS(out2.buf, "\"bytesValue\":\"TWFu\"");
    CHECK_CONTAINS(out2.buf, "\"key\":\"list\"");
    CHECK_CONTAINS(out2.buf, "\"arrayValue\":{\"values\":[");
    CHECK_CONTAINS(out2.buf, "\"intValue\":\"1\"");
    CHECK_CONTAINS(out2.buf, "\"intValue\":\"2\"");
    sb_free(&out2);
}

/* Empty / malformed input must not crash and must stay valid-ish JSON. */
static void test_empty_and_truncated(void) {
    sb out;
    sb_init(&out);
    otlp_traces_pb_to_json(&out, (const uint8_t *)"", 0);
    CHECK_STR_EQ(out.buf, "{}");
    sb_free(&out);

    /* a tag claiming a length-delimited field longer than the buffer */
    uint8_t bad[] = {0x0a, 0x7f}; /* field 1, wire 2, len 127, no data */
    sb_init(&out);
    otlp_traces_pb_to_json(&out, bad, sizeof(bad));
    CHECK_STR_EQ(out.buf, "{}"); /* truncated element is skipped */
    sb_free(&out);
}

void test_otlp_pb(void) {
    test_full_span();
    test_value_kinds();
    test_empty_and_truncated();
}
