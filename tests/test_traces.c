/* Unit tests for the trace receiver's pure request-framing helper.
 *
 * Everything else in traces.c needs a socket and a live child, so it lives in
 * the smoke test. kotlp_request_state is here because the boundary it guards -
 * a body declared right at the 64 MiB cap - is otherwise only reachable by
 * actually sending 64 MiB, and whether the old code dropped it depended on
 * where a read boundary landed. */
#include "kotlp.h"
#include "test.h"

/* A plausible request head: "POST /v1/traces HTTP/1.1\r\n...\r\n\r\n". Only the
 * two offsets matter here. */
enum { HDR_END = 120, BODY_START = HDR_END + 4 };

static void test_request_headers_incomplete(void) {
    /* no terminator yet, and nowhere near the allowance: keep reading */
    CHECK(kotlp_request_state(8192, -1, 0, -1) == KOTLP_REQ_NEED_MORE);
}

static void test_request_headers_too_large(void) {
    /* a peer that never terminates its headers must be rejected, not buffered
     * forever */
    CHECK(kotlp_request_state(KOTLP_MAX_HEADER_BYTES, -1, 0, -1) ==
          KOTLP_REQ_NEED_MORE);
    CHECK(kotlp_request_state(KOTLP_MAX_HEADER_BYTES + 1, -1, 0, -1) ==
          KOTLP_REQ_HEADERS_TOO_LARGE);
    /* And once the terminator IS found, the verdict is the same. Judging only
     * the pre-terminator case would let an oversized head through whenever the
     * read that carried the terminator also carried it past the allowance -
     * measured: 70000 bytes of headers answered 200 OK, 200000 answered 400. */
    long over = KOTLP_MAX_HEADER_BYTES + 1;
    CHECK(kotlp_request_state((size_t)over + 6, over, over + 4, 2) ==
          KOTLP_REQ_HEADERS_TOO_LARGE);
    CHECK(kotlp_request_state(KOTLP_MAX_HEADER_BYTES + 6, KOTLP_MAX_HEADER_BYTES,
                              KOTLP_MAX_HEADER_BYTES + 4,
                              2) == KOTLP_REQ_COMPLETE);
}

static void test_request_body_progress(void) {
    CHECK(kotlp_request_state(BODY_START + 10, HDR_END, BODY_START, 100) ==
          KOTLP_REQ_NEED_MORE);
    CHECK(kotlp_request_state(BODY_START + 99, HDR_END, BODY_START, 100) ==
          KOTLP_REQ_NEED_MORE);
    CHECK(kotlp_request_state(BODY_START + 100, HDR_END, BODY_START, 100) ==
          KOTLP_REQ_COMPLETE);
    /* a peer that pipelined more than it declared is still complete */
    CHECK(kotlp_request_state(BODY_START + 400, HDR_END, BODY_START, 100) ==
          KOTLP_REQ_COMPLETE);
}

static void test_request_empty_body(void) {
    /* `Content-Length: 0` is a complete request the moment the headers land -
     * an empty export, not something to wait for */
    CHECK(kotlp_request_state(BODY_START, HDR_END, BODY_START, 0) ==
          KOTLP_REQ_COMPLETE);
}

static void test_request_no_content_length(void) {
    /* nothing delimits a body (a chunked encoding, say): stop rather than wait
     * out the idle timeout on every such request */
    CHECK(kotlp_request_state(BODY_START, HDR_END, BODY_START, -1) ==
          KOTLP_REQ_COMPLETE);
}

static void test_request_body_too_large(void) {
    /* parse_content_length refuses this first, but the helper is what makes
     * "keep reading" terminate, so it must not rely on that having happened */
    CHECK(kotlp_request_state(BODY_START, HDR_END, BODY_START,
                              (long)KOTLP_MAX_REQUEST_BYTES + 1) ==
          KOTLP_REQ_BODY_TOO_LARGE);
    CHECK(kotlp_request_state(BODY_START, HDR_END, BODY_START,
                              KOTLP_MAX_REQUEST_BYTES) == KOTLP_REQ_NEED_MORE);
}

/* The regression this suite exists for. The accepted Content-Length bounds a
 * BODY (<= KOTLP_MAX_REQUEST_BYTES), so a body declared at exactly the cap must
 * be read to completion however long the headers were. The old read loop capped
 * headers+body against the same number, so this request validated fine and was
 * then discarded partway through - while the peer still got a 200 OK, so the
 * SDK never retried. */
static void test_request_max_body_with_headers(void) {
    const long cl = KOTLP_MAX_REQUEST_BYTES;
    size_t buffered = (size_t)BODY_START + (size_t)cl - 1;
    CHECK(kotlp_request_state(buffered, HDR_END, BODY_START, cl) ==
          KOTLP_REQ_NEED_MORE);
    CHECK(kotlp_request_state(buffered + 1, HDR_END, BODY_START, cl) ==
          KOTLP_REQ_COMPLETE);
    /* and with headers filling the whole allowance, still complete */
    long big_end = KOTLP_MAX_HEADER_BYTES - 4;
    long big_start = big_end + 4;
    CHECK(kotlp_request_state((size_t)big_start + (size_t)cl, big_end, big_start,
                              cl) == KOTLP_REQ_COMPLETE);
}

void test_traces(void) {
    test_request_headers_incomplete();
    test_request_headers_too_large();
    test_request_body_progress();
    test_request_empty_body();
    test_request_no_content_length();
    test_request_body_too_large();
    test_request_max_body_with_headers();
}
