#!/bin/sh
# Smoke test for koltp: run a known command and assert the OTLP NDJSON output
# contains the expected log, metric and trace records.
set -eu

BIN="${1:-build/koltp}"
if [ ! -x "$BIN" ]; then
    echo "smoke_test: binary not found or not executable: $BIN" >&2
    exit 1
fi

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

OUT="$TMP/out.ndjson"
ERR="$TMP/err.ndjson"

# Wrap a command that writes to both stdout and stderr and burns a little CPU.
set +e
"$BIN" -s smoke-test -i 200 -- \
    sh -c 'echo "hello stdout"; echo "oops stderr" >&2; i=0; while [ $i -lt 100000 ]; do i=$((i+1)); done; exit 7' \
    >"$OUT" 2>"$ERR"
CODE=$?
set -e

fail() { echo "smoke_test: FAIL - $1" >&2; echo "--- stdout ---"; cat "$OUT"; echo "--- stderr ---"; cat "$ERR"; exit 1; }

# 1. exit code is proxied
[ "$CODE" -eq 7 ] || fail "expected exit code 7, got $CODE"

# 2. stdout log record present on our stdout, tagged as stdout iostream
grep -q '"hello stdout"' "$OUT" || fail "missing stdout log body"
grep -q '"stdout"' "$OUT"      || fail "missing log.iostream=stdout"
grep -q 'resourceLogs' "$OUT"  || fail "missing resourceLogs envelope"

# 3. stderr log record present on our stderr
grep -q '"oops stderr"' "$ERR" || fail "missing stderr log body"
grep -q '"stderr"' "$ERR"      || fail "missing log.iostream=stderr"

# 4. metrics emitted (process.cpu.time at minimum)
grep -q 'resourceMetrics' "$OUT"   || fail "missing resourceMetrics"
grep -q 'process.cpu.time' "$OUT"  || fail "missing process.cpu.time metric"

# 5. root span emitted with the proxied exit code
grep -q 'resourceSpans' "$OUT"     || fail "missing resourceSpans"
grep -q 'process.exit.code' "$OUT" || fail "missing process.exit.code attribute"

# 6. service.name resource attribute set from -s
grep -q 'smoke-test' "$OUT" || fail "missing service.name resource attribute"

# 7. default format (kjson) wraps each record as ::{"oltp":...}::
grep -q '::{"oltp":' "$OUT" || fail "default output is not ::{\"oltp\":...}:: framed"

# 8. -f json emits bare JSON (no framing), same content
JOUT="$TMP/json.ndjson"
"$BIN" -s smoke-json --no-metrics --no-traces -f json -- \
    sh -c 'echo plain-json' >"$JOUT" 2>/dev/null
grep -q 'resourceLogs' "$JOUT" || fail "-f json: missing resourceLogs"
grep -q '"plain-json"' "$JOUT" || fail "-f json: missing log body"
if grep -q '::{"oltp":' "$JOUT"; then fail "-f json output should not be framed"; fi

# 8b. -f kjson is explicitly framed; an invalid format is rejected
KOUT="$TMP/kjson.ndjson"
"$BIN" -s smoke-kjson --no-metrics --no-traces --format kjson -- \
    sh -c 'echo framed' >"$KOUT" 2>/dev/null
grep -q '::{"oltp":' "$KOUT" || fail "-f kjson should be framed"
if "$BIN" -f bogus -- true >/dev/null 2>&1; then fail "invalid -f value should error"; fi

# 9. a final metrics record is emitted even when the sampling interval never
#    elapses (instant-exit child, huge interval -> only the rusage summary).
FOUT="$TMP/final.ndjson"
"$BIN" -s smoke-final -i 600000 --no-logs --no-traces -f json -- \
    true >"$FOUT" 2>/dev/null
FINAL_COUNT="$(grep -c 'resourceMetrics' "$FOUT" || true)"
[ "$FINAL_COUNT" -ge 1 ] || fail "no final metrics record on instant exit (got $FINAL_COUNT)"
grep -q 'process.cpu.time' "$FOUT" || fail "final metrics record missing process.cpu.time"

# 10. port safety: when the requested OTLP port is busy, the receiver must fall
#     back to a free port and tracing must still work (root span emitted).
HOLD_PORT=4319
"$BIN" -s port-holder -p "$HOLD_PORT" --no-logs --no-metrics -- \
    sh -c 'sleep 3' >/dev/null 2>&1 &
HOLD_PID=$!
sleep 1 # let the holder bind the port before we collide with it
POUT="$TMP/port.ndjson"
"$BIN" -s port-fallback -p "$HOLD_PORT" --no-metrics -f json -- \
    sh -c 'echo ok' >"$POUT" 2>/dev/null
kill "$HOLD_PID" 2>/dev/null || true
wait "$HOLD_PID" 2>/dev/null || true
grep -q 'resourceSpans' "$POUT" || \
    fail "trace receiver did not fall back when port $HOLD_PORT was busy"

echo "smoke_test: PASS"
