#!/bin/sh
# Smoke test for kotlp: run a known command and assert the OTLP NDJSON output
# contains the expected log, metric and trace records.
set -eu

BIN="${1:-build/kotlp}"
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

# 7. default format (kjson) wraps each record as ::{"otlp":...}::
grep -q '::{"otlp":' "$OUT" || fail "default output is not ::{\"otlp\":...}:: framed"

# 8. -f json emits bare JSON (no framing), same content
JOUT="$TMP/json.ndjson"
"$BIN" -s smoke-json --no-metrics --no-traces -f json -- \
    sh -c 'echo plain-json' >"$JOUT" 2>/dev/null
grep -q 'resourceLogs' "$JOUT" || fail "-f json: missing resourceLogs"
grep -q '"plain-json"' "$JOUT" || fail "-f json: missing log body"
if grep -q '::{"otlp":' "$JOUT"; then fail "-f json output should not be framed"; fi

# 8b. -f kjson is explicitly framed; an invalid format is rejected
KOUT="$TMP/kjson.ndjson"
"$BIN" -s smoke-kjson --no-metrics --no-traces --format kjson -- \
    sh -c 'echo framed' >"$KOUT" 2>/dev/null
grep -q '::{"otlp":' "$KOUT" || fail "-f kjson should be framed"
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

# 11. --log-dir mirrors every record into DIR/log.ndjson as *bare* NDJSON, and
#     the console drops the OTLP syntax entirely (behaves like -r/--raw). The
#     directory is created if it is missing (including parents).
LOGDIR="$TMP/nested/logs"
LOUT="$TMP/logdir-console.ndjson"
"$BIN" -s smoke-logdir -i 200 --log-dir "$LOGDIR" -- \
    sh -c 'echo to-file' >"$LOUT" 2>/dev/null
NDJSON="$LOGDIR/log.ndjson"
[ -f "$NDJSON" ] || fail "--log-dir did not create $NDJSON"
grep -q '"to-file"' "$NDJSON"       || fail "--log-dir: missing log body"
grep -q 'resourceLogs' "$NDJSON"    || fail "--log-dir: missing resourceLogs"
grep -q 'resourceMetrics' "$NDJSON" || fail "--log-dir: missing resourceMetrics"
grep -q 'resourceSpans' "$NDJSON"   || fail "--log-dir: missing resourceSpans"
# the file is always bare, even though this run used the default -f kjson...
if grep -q '::{"otlp":' "$NDJSON"; then fail "--log-dir output must not be framed"; fi
# ...while the console shows the raw child line only, no OTLP JSON at all
if grep -q '::{"otlp":' "$LOUT"; then fail "--log-dir console must not be framed"; fi
if grep -q 'resourceLogs' "$LOUT"; then fail "--log-dir console must not carry OTLP JSON"; fi
grep -q '^to-file$' "$LOUT" || fail "--log-dir must still print the child's raw output"
# without --log-flush-interval there is no rotation and no file count reported
if grep -q 'kotlp.log.file.count' "$NDJSON"; then
    fail "kotlp.log.file.count must only appear with --log-flush-interval"
fi

# 12. --log-flush-interval rotates into log-N.ndjson and reports how many files
#     were produced on the root span (which lands in the last one).
ROTDIR="$TMP/rotated"
"$BIN" -s smoke-rotate -i 200 --log-dir "$ROTDIR" --log-flush-interval 1 -- \
    sh -c 'for i in 1 2 3; do echo tick; sleep 1; done' >/dev/null 2>&1
if [ -f "$ROTDIR/log.ndjson" ]; then fail "rotation must not write log.ndjson"; fi
ROT_FILES="$(ls "$ROTDIR"/log-*.ndjson 2>/dev/null | wc -l | tr -d ' ')"
[ "$ROT_FILES" -ge 2 ] || fail "expected >= 2 rotated files, got $ROT_FILES"
[ -f "$ROTDIR/log-1.ndjson" ] || fail "rotation must start at log-1.ndjson"
# the count is reported exactly once, in the highest-numbered file...
COUNT_HITS="$(grep -l 'kotlp.log.file.count' "$ROTDIR"/log-*.ndjson | wc -l | tr -d ' ')"
[ "$COUNT_HITS" -eq 1 ] || fail "kotlp.log.file.count in $COUNT_HITS files, want 1"
LAST="$ROTDIR/log-$ROT_FILES.ndjson"
grep -q 'kotlp.log.file.count' "$LAST" || \
    fail "kotlp.log.file.count should be in the last file ($LAST)"
# ...and it matches the number of files actually on disk
REPORTED="$(grep -o 'kotlp\.log\.file\.count","value":{"intValue":"[0-9]*' "$LAST" \
    | sed 's/.*"//')"
[ "$REPORTED" = "$ROT_FILES" ] || \
    fail "kotlp.log.file.count is $REPORTED but $ROT_FILES files exist"

# 13. CR handling: a CR is stripped only as part of a CRLF line ending. A bare CR
#     is payload and must survive into the body (escaped as \r by the JSON
#     encoder). Input is "a\r\n" then "b\rc\n" -> bodies "a" and "b\rc".
CROUT="$TMP/cr.ndjson"
"$BIN" -s smoke-cr --no-metrics --no-traces -f json -- \
    sh -c 'printf "a\r\nb\rc\n"' >"$CROUT" 2>/dev/null
grep -q '"stringValue":"a"' "$CROUT" || fail "CRLF line should yield a clean body"
if grep -q '"stringValue":"a\\r"' "$CROUT"; then
    fail "the CR of a CRLF pair must not reach the body"
fi
grep -q '"stringValue":"b\\rc"' "$CROUT" || fail "a bare CR must survive as data"

# 13b. the CRLF pair may straddle a read() boundary - the CR is written, then the
#      LF only arrives in a later read. The pending CR must still pair up.
SPLITOUT="$TMP/cr-split.ndjson"
"$BIN" -s smoke-cr-split --no-metrics --no-traces -f json -- \
    sh -c 'printf "x\r"; sleep 1; printf "\ny\n"' >"$SPLITOUT" 2>/dev/null
grep -q '"stringValue":"x"' "$SPLITOUT" || \
    fail "a CRLF split across reads should still yield a clean body"
if grep -q '"stringValue":"x\\r"' "$SPLITOUT"; then
    fail "a CRLF split across reads must not leave the CR in the body"
fi
grep -q '"stringValue":"y"' "$SPLITOUT" || fail "line after a split CRLF is missing"

# 13c. end of stream: a CR closing an otherwise non-empty line is data and is
#      flushed with it, but a CR-only remainder emits no record at all.
EOFOUT="$TMP/cr-eof.ndjson"
"$BIN" -s smoke-cr-eof --no-metrics --no-traces -f json -- \
    sh -c 'printf "z\r"' >"$EOFOUT" 2>/dev/null
grep -q '"stringValue":"z\\r"' "$EOFOUT" || \
    fail "a trailing CR on a non-empty line must be flushed as data"
BAREOUT="$TMP/cr-bare.ndjson"
"$BIN" -s smoke-cr-bare --no-metrics --no-traces -f json -- \
    sh -c 'printf "w\n\r"' >"$BAREOUT" 2>/dev/null
grep -q '"stringValue":"w"' "$BAREOUT" || fail "line before a trailing bare CR is missing"
if grep -q '"stringValue":"\\r"' "$BAREOUT"; then
    fail "a CR-only remainder must not emit a record of its own"
fi
# a remainder of SEVERAL CRs is just as empty of message as a single one
MULTIOUT="$TMP/cr-multi.ndjson"
"$BIN" -s smoke-cr-multi --no-metrics --no-traces -f json -- \
    sh -c 'printf "v\n\r\r\r"' >"$MULTIOUT" 2>/dev/null
grep -q '"stringValue":"v"' "$MULTIOUT" || fail "line before trailing bare CRs is missing"
if grep -q '"stringValue":"\\r\\r' "$MULTIOUT"; then
    fail "a remainder of several CRs must not emit a record either"
fi

# 14. fd hygiene: everything kotlp opens for itself is close-on-exec, so the
#     wrapped command inherits nothing but stdin/stdout/stderr. Run with BOTH a
#     log dir and a trace receiver - the two descriptors that used to leak - and
#     have the child report its OWN fd table rather than reading the tree-summed
#     process.open_file_descriptor.count: the metric depends on how many
#     processes `sh -c` forks (some shells tail-exec a single command), which
#     would let the leak slip through on a one-process tree.
#     Expect exactly 0, 1, 2 plus `ls`'s own directory fd (3); the leaky version
#     also showed the log file and the listening socket.
if [ -d /proc/self/fd ]; then
    FDDIR="$TMP/fdlogs"
    "$BIN" -s smoke-fds --no-metrics -p 4320 --log-dir "$FDDIR" -- \
        sh -c 'ls -1 /proc/self/fd' >/dev/null 2>&1
    FDFILE="$FDDIR/log.ndjson"
    [ -f "$FDFILE" ] || fail "fd check: --log-dir produced no $FDFILE"
    grep -q 'resourceSpans' "$FDFILE" || \
        fail "fd check: no root span, so the trace receiver never bound a socket to leak"
    FDS="$(grep -o '"stringValue":"[0-9]*"' "$FDFILE" | sed 's/[^0-9]//g' | sort -n | tr '\n' ' ')"
    [ "$FDS" = "0 1 2 3 " ] || \
        fail "child inherits kotlp descriptors: its fd table is [$FDS], want [0 1 2 3 ]"
else
    echo "smoke_test: no /proc on this platform, skipping the fd inheritance check"
fi

# 15. process.cpu.time is declared CUMULATIVE + isMonotonic, so it must never
#     decrease. The wrapped command keeps forking children that burn CPU and
#     exit, which is exactly what used to make the "currently alive" sum drop.
#     Every dip a collector sees here is read as a counter reset.
#     Live sampling needs /proc, so on a platform without it there is only the
#     single closing rusage record and no series to check.
if [ ! -d /proc/self ]; then
    echo "smoke_test: no /proc on this platform, skipping the metrics series checks"
else
MONODIR="$TMP/mono"
"$BIN" -s smoke-mono -i 150 --no-traces --log-dir "$MONODIR" -- \
    sh -c 'for i in 1 2 3 4 5 6; do
             (i=0; while [ $i -lt 300000 ]; do i=$((i+1)); done)
           done' >/dev/null 2>&1
MONOFILE="$MONODIR/log.ndjson"
[ -f "$MONOFILE" ] || fail "monotonicity check: no $MONOFILE"
grep -q '"isMonotonic":true' "$MONOFILE" || fail "process.cpu.time lost its isMonotonic flag"
# Pull the cpu.mode=user datapoint out of each process.cpu.time sum, in order.
# Split each record on the metric boundary and keep only the process.cpu.time
# objects: process.cpu.utilization also carries a cpu.mode=user asDouble, and
# matching on that alone would interleave two unrelated series.
awk '{
    n = split($0, p, "\\{\"name\":")
    for (i = 2; i <= n; i++)
        if (p[i] ~ /^"process\.cpu\.time"/ && p[i] ~ /"stringValue":"user"/ &&
            match(p[i], /"asDouble":[0-9.]+/))
            print substr(p[i], RSTART + 11, RLENGTH - 11)
}' "$MONOFILE" > "$TMP/cpu-series"
SAMPLES="$(wc -l < "$TMP/cpu-series" | tr -d ' ')"
# /proc exists, so live sampling must have produced a real series; too few
# samples means something regressed, not that the platform cannot do it.
[ "$SAMPLES" -ge 3 ] || \
    fail "expected >= 3 process.cpu.time samples on a /proc host, got $SAMPLES"
DROPS="$(awk 'NR>1 && $1 < prev { n++ } { prev=$1 } END { print n+0 }' "$TMP/cpu-series")"
[ "$DROPS" -eq 0 ] || \
    fail "process.cpu.time decreased $DROPS time(s) across $SAMPLES samples: $(tr '\n' ' ' < "$TMP/cpu-series")"

# 16. every cumulative datapoint shares one startTimeUnixNano - the start of the
#     run - so the final rusage record describes the whole execution instead of
#     the zero-length window it used to claim by stamping "now" as its start.
STARTS="$(grep -o '"startTimeUnixNano":"[0-9]*"' "$MONOFILE" | sort -u | wc -l | tr -d ' ')"
[ "$STARTS" -eq 1 ] || fail "expected one startTimeUnixNano across all sums, got $STARTS"
LASTREC="$(grep 'resourceMetrics' "$MONOFILE" | tail -1)"
W_START="$(echo "$LASTREC" | grep -o '"startTimeUnixNano":"[0-9]*"' | head -1 | sed 's/[^0-9]//g')"
W_END="$(echo "$LASTREC" | grep -o '"timeUnixNano":"[0-9]*"' | head -1 | sed 's/[^0-9]//g')"
[ -n "$W_START" ] && [ -n "$W_END" ] || fail "final metrics record has no collection window"
[ "$W_END" -gt "$W_START" ] || \
    fail "final metrics window is not positive (start=$W_START end=$W_END)"
fi

echo "smoke_test: PASS"
