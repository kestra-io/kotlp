/* otlp_pb.c - decode an OTLP/protobuf trace payload into OTLP/JSON.
 *
 * The embedded receiver (traces.c) accepts OTLP over HTTP. With
 * `OTEL_EXPORTER_OTLP_PROTOCOL=http/json` the body is already JSON and is
 * forwarded verbatim; with `http/protobuf` the body is a binary
 * ExportTraceServiceRequest (wire-compatible with TracesData) which we decode
 * here into the exact same OTLP/JSON shape so downstream NDJSON consumers see a
 * single, consistent format.
 *
 * Only a minimal, dependency-free protobuf wire reader is implemented - just
 * enough for the fixed OTLP trace schema. Field numbers come from
 * opentelemetry/proto/{trace,common,resource}/v1.
 *
 * Conventions matched to the OTLP/JSON (ProtoJSON) encoding and to otel.c:
 *   - proto field names      -> lowerCamelCase JSON keys
 *   - trace/span id (bytes)  -> lowercase hex string
 *   - 64-bit ints / times    -> decimal string
 *   - enums (kind, status)   -> integer
 *   - AnyValue               -> {"stringValue":..} / {"intValue":"42"} / ...
 * Default (zero / empty) fields are omitted, exactly as the JSON exporters do.
 */
#include "koltp.h"

#include <string.h>

/* ----------------------------------------------------------- wire reader */

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} pbuf;

static bool pb_varint(pbuf *b, uint64_t *out) {
    uint64_t v = 0;
    int shift = 0;
    while (b->p < b->end) {
        uint8_t c = *b->p++;
        v |= (uint64_t)(c & 0x7f) << shift;
        if (!(c & 0x80)) {
            *out = v;
            return true;
        }
        shift += 7;
        if (shift >= 64) return false;
    }
    return false;
}

static bool pb_fixed64(pbuf *b, uint64_t *out) {
    if (b->end - b->p < 8) return false;
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v |= (uint64_t)(*b->p++) << (8 * i);
    *out = v;
    return true;
}

static bool pb_bytes(pbuf *b, const uint8_t **data, size_t *len) {
    uint64_t l;
    if (!pb_varint(b, &l)) return false;
    if ((uint64_t)(b->end - b->p) < l) return false;
    *data = b->p;
    *len = (size_t)l;
    b->p += l;
    return true;
}

static bool pb_tag(pbuf *b, uint32_t *field, uint32_t *wire) {
    uint64_t t;
    if (!pb_varint(b, &t)) return false;
    *field = (uint32_t)(t >> 3);
    *wire = (uint32_t)(t & 7);
    return true;
}

static bool pb_skip(pbuf *b, uint32_t wire) {
    uint64_t v;
    const uint8_t *d;
    size_t l;
    switch (wire) {
    case 0: return pb_varint(b, &v);          /* varint            */
    case 1: return pb_fixed64(b, &v);         /* 64-bit            */
    case 2: return pb_bytes(b, &d, &l);       /* length-delimited  */
    case 5:                                   /* 32-bit            */
        if (b->end - b->p < 4) return false;
        b->p += 4;
        return true;
    default: return false;
    }
}

/* Find the first length-delimited field `field` in [data,len). */
static bool find_ld(const uint8_t *data, size_t len, uint32_t field,
                    const uint8_t **od, size_t *ol) {
    pbuf b = {data, data + len};
    uint32_t f, w;
    while (pb_tag(&b, &f, &w)) {
        if (f == field && w == 2) return pb_bytes(&b, od, ol);
        if (!pb_skip(&b, w)) break;
    }
    return false;
}

static bool find_varint(const uint8_t *data, size_t len, uint32_t field,
                        uint64_t *ov) {
    pbuf b = {data, data + len};
    uint32_t f, w;
    while (pb_tag(&b, &f, &w)) {
        if (f == field && w == 0) return pb_varint(&b, ov);
        if (!pb_skip(&b, w)) break;
    }
    return false;
}

static bool find_fixed64(const uint8_t *data, size_t len, uint32_t field,
                         uint64_t *ov) {
    pbuf b = {data, data + len};
    uint32_t f, w;
    while (pb_tag(&b, &f, &w)) {
        if (f == field && w == 1) return pb_fixed64(&b, ov);
        if (!pb_skip(&b, w)) break;
    }
    return false;
}

/* ------------------------------------------------------------ json emit */

static void comma(sb *s, bool *first) {
    if (*first)
        *first = false;
    else
        sb_putc(s, ',');
}

static void key(sb *s, bool *first, const char *k) {
    comma(s, first);
    sb_putc(s, '"');
    sb_puts(s, k);
    sb_puts(s, "\":");
}

static void emit_hex(sb *s, const uint8_t *data, size_t len) {
    static const char hx[] = "0123456789abcdef";
    sb_putc(s, '"');
    for (size_t i = 0; i < len; i++) {
        sb_putc(s, hx[data[i] >> 4]);
        sb_putc(s, hx[data[i] & 0xf]);
    }
    sb_putc(s, '"');
}

static void emit_base64(sb *s, const uint8_t *data, size_t len) {
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sb_putc(s, '"');
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8) |
                     data[i + 2];
        sb_putc(s, b64[(n >> 18) & 63]);
        sb_putc(s, b64[(n >> 12) & 63]);
        sb_putc(s, b64[(n >> 6) & 63]);
        sb_putc(s, b64[n & 63]);
    }
    size_t rem = len - i;
    if (rem == 1) {
        uint32_t n = (uint32_t)data[i] << 16;
        sb_putc(s, b64[(n >> 18) & 63]);
        sb_putc(s, b64[(n >> 12) & 63]);
        sb_putc(s, '=');
        sb_putc(s, '=');
    } else if (rem == 2) {
        uint32_t n = ((uint32_t)data[i] << 16) | ((uint32_t)data[i + 1] << 8);
        sb_putc(s, b64[(n >> 18) & 63]);
        sb_putc(s, b64[(n >> 12) & 63]);
        sb_putc(s, b64[(n >> 6) & 63]);
        sb_putc(s, '=');
    }
    sb_putc(s, '"');
}

/* Field helpers: each emits "key":value only when the field is present. */

static void emit_str_field(sb *s, bool *first, const char *k,
                           const uint8_t *data, size_t len, uint32_t field) {
    const uint8_t *d;
    size_t l;
    if (!find_ld(data, len, field, &d, &l)) return;
    key(s, first, k);
    sb_json_strn(s, (const char *)d, l);
}

static void emit_hex_field(sb *s, bool *first, const char *k,
                           const uint8_t *data, size_t len, uint32_t field) {
    const uint8_t *d;
    size_t l;
    if (!find_ld(data, len, field, &d, &l)) return;
    key(s, first, k);
    emit_hex(s, d, l);
}

/* 64-bit (fixed64) timestamp -> decimal string. */
static void emit_u64str_field(sb *s, bool *first, const char *k,
                              const uint8_t *data, size_t len, uint32_t field) {
    uint64_t v;
    if (!find_fixed64(data, len, field, &v)) return;
    key(s, first, k);
    sb_putf(s, "\"%llu\"", (unsigned long long)v);
}

/* enum / small uint -> bare JSON integer. */
static void emit_int_field(sb *s, bool *first, const char *k,
                           const uint8_t *data, size_t len, uint32_t field) {
    uint64_t v;
    if (!find_varint(data, len, field, &v)) return;
    key(s, first, k);
    sb_putf(s, "%llu", (unsigned long long)v);
}

/* Forward declarations for the recursive AnyValue / message emitters. */
static void emit_anyvalue(sb *s, const uint8_t *data, size_t len);
static void emit_keyvalue(sb *s, const uint8_t *data, size_t len);

/* Emit "key":[...] for a repeated length-delimited (sub-message) field, using
 * `emit` per element. Omitted entirely when there are no elements. */
static void emit_repeated(sb *s, bool *first, const char *k,
                          const uint8_t *data, size_t len, uint32_t field,
                          void (*emit)(sb *, const uint8_t *, size_t)) {
    /* presence check first so empty (and truncated) arrays stay omitted */
    pbuf scan = {data, data + len};
    uint32_t f, w;
    bool any = false;
    while (pb_tag(&scan, &f, &w)) {
        if (f == field && w == 2) {
            const uint8_t *d;
            size_t l;
            if (pb_bytes(&scan, &d, &l)) any = true; /* element is well-formed */
            break;
        }
        if (!pb_skip(&scan, w)) break;
    }
    if (!any) return;

    key(s, first, k);
    sb_putc(s, '[');
    pbuf b = {data, data + len};
    bool firste = true;
    while (pb_tag(&b, &f, &w)) {
        if (f == field && w == 2) {
            const uint8_t *d;
            size_t l;
            if (!pb_bytes(&b, &d, &l)) break;
            if (firste)
                firste = false;
            else
                sb_putc(s, ',');
            emit(s, d, l);
        } else if (!pb_skip(&b, w)) {
            break;
        }
    }
    sb_putc(s, ']');
}

/* Emit "key":{...} for a single sub-message field. */
static void emit_submsg(sb *s, bool *first, const char *k, const uint8_t *data,
                        size_t len, uint32_t field,
                        void (*emit)(sb *, const uint8_t *, size_t)) {
    const uint8_t *d;
    size_t l;
    if (!find_ld(data, len, field, &d, &l)) return;
    key(s, first, k);
    emit(s, d, l);
}

static void emit_arrayvalue(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_repeated(s, &first, "values", data, len, 1, emit_anyvalue);
    sb_putc(s, '}');
}

static void emit_kvlist(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_repeated(s, &first, "values", data, len, 1, emit_keyvalue);
    sb_putc(s, '}');
}

/* AnyValue is a oneof; a set field is always serialized (even at its default),
 * so exactly one of these is present. */
static void emit_anyvalue(sb *s, const uint8_t *data, size_t len) {
    const uint8_t *d;
    size_t l;
    uint64_t v;
    sb_putc(s, '{');
    if (find_ld(data, len, 1, &d, &l)) { /* string_value */
        sb_puts(s, "\"stringValue\":");
        sb_json_strn(s, (const char *)d, l);
    } else if (find_varint(data, len, 2, &v)) { /* bool_value */
        sb_puts(s, v ? "\"boolValue\":true" : "\"boolValue\":false");
    } else if (find_varint(data, len, 3, &v)) { /* int_value (int64) */
        sb_putf(s, "\"intValue\":\"%lld\"", (long long)(int64_t)v);
    } else if (find_fixed64(data, len, 4, &v)) { /* double_value */
        double dv;
        memcpy(&dv, &v, sizeof(dv));
        sb_puts(s, "\"doubleValue\":");
        sb_putf(s, "%.17g", dv);
    } else if (find_ld(data, len, 5, &d, &l)) { /* array_value */
        sb_puts(s, "\"arrayValue\":");
        emit_arrayvalue(s, d, l);
    } else if (find_ld(data, len, 6, &d, &l)) { /* kvlist_value */
        sb_puts(s, "\"kvlistValue\":");
        emit_kvlist(s, d, l);
    } else if (find_ld(data, len, 7, &d, &l)) { /* bytes_value */
        sb_puts(s, "\"bytesValue\":");
        emit_base64(s, d, l);
    }
    sb_putc(s, '}');
}

static void emit_keyvalue(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_str_field(s, &first, "key", data, len, 1);
    emit_submsg(s, &first, "value", data, len, 2, emit_anyvalue);
    sb_putc(s, '}');
}

static void emit_status(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_str_field(s, &first, "message", data, len, 2);
    emit_int_field(s, &first, "code", data, len, 3);
    sb_putc(s, '}');
}

static void emit_event(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_u64str_field(s, &first, "timeUnixNano", data, len, 1);
    emit_str_field(s, &first, "name", data, len, 2);
    emit_repeated(s, &first, "attributes", data, len, 3, emit_keyvalue);
    emit_int_field(s, &first, "droppedAttributesCount", data, len, 4);
    sb_putc(s, '}');
}

static void emit_link(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_hex_field(s, &first, "traceId", data, len, 1);
    emit_hex_field(s, &first, "spanId", data, len, 2);
    emit_str_field(s, &first, "traceState", data, len, 3);
    emit_repeated(s, &first, "attributes", data, len, 4, emit_keyvalue);
    emit_int_field(s, &first, "droppedAttributesCount", data, len, 5);
    sb_putc(s, '}');
}

static void emit_span(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_hex_field(s, &first, "traceId", data, len, 1);
    emit_hex_field(s, &first, "spanId", data, len, 2);
    emit_str_field(s, &first, "traceState", data, len, 3);
    emit_hex_field(s, &first, "parentSpanId", data, len, 4);
    emit_str_field(s, &first, "name", data, len, 5);
    emit_int_field(s, &first, "kind", data, len, 6);
    emit_u64str_field(s, &first, "startTimeUnixNano", data, len, 7);
    emit_u64str_field(s, &first, "endTimeUnixNano", data, len, 8);
    emit_repeated(s, &first, "attributes", data, len, 9, emit_keyvalue);
    emit_int_field(s, &first, "droppedAttributesCount", data, len, 10);
    emit_repeated(s, &first, "events", data, len, 11, emit_event);
    emit_int_field(s, &first, "droppedEventsCount", data, len, 12);
    emit_repeated(s, &first, "links", data, len, 13, emit_link);
    emit_int_field(s, &first, "droppedLinksCount", data, len, 14);
    emit_submsg(s, &first, "status", data, len, 15, emit_status);
    sb_putc(s, '}');
}

static void emit_scope(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_str_field(s, &first, "name", data, len, 1);
    emit_str_field(s, &first, "version", data, len, 2);
    emit_repeated(s, &first, "attributes", data, len, 3, emit_keyvalue);
    emit_int_field(s, &first, "droppedAttributesCount", data, len, 4);
    sb_putc(s, '}');
}

static void emit_scope_spans(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_submsg(s, &first, "scope", data, len, 1, emit_scope);
    emit_repeated(s, &first, "spans", data, len, 2, emit_span);
    emit_str_field(s, &first, "schemaUrl", data, len, 3);
    sb_putc(s, '}');
}

static void emit_resource(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_repeated(s, &first, "attributes", data, len, 1, emit_keyvalue);
    emit_int_field(s, &first, "droppedAttributesCount", data, len, 2);
    sb_putc(s, '}');
}

static void emit_resource_spans(sb *s, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(s, '{');
    emit_submsg(s, &first, "resource", data, len, 1, emit_resource);
    emit_repeated(s, &first, "scopeSpans", data, len, 2, emit_scope_spans);
    emit_str_field(s, &first, "schemaUrl", data, len, 3);
    sb_putc(s, '}');
}

void otlp_traces_pb_to_json(sb *out, const uint8_t *data, size_t len) {
    bool first = true;
    sb_putc(out, '{');
    /* ExportTraceServiceRequest.resource_spans == TracesData.resource_spans */
    emit_repeated(out, &first, "resourceSpans", data, len, 1,
                  emit_resource_spans);
    sb_putc(out, '}');
}
