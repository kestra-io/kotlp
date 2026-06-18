# AGENTS.md

Guidance for AI coding agents (and humans) working in this repository.

## Project in one paragraph

`koltp` is a single C program compiled with **Cosmopolitan Libc** (`cosmocc`)
into one **Actually Portable Executable** that runs on Linux, macOS, Windows
and the BSDs across amd64/arm64. It wraps an arbitrary command and emits
**OpenTelemetry OTLP/JSON** to the console for three features: **logs**
(stdout/stderr capture), **traces** (embedded OTLP/HTTP receiver + a root
span) and **metrics** (process resource sampling). Output is NDJSON.

## Golden rules

1. **Portability first.** Every line must compile and behave correctly under
   `cosmocc` on all target OSes. Prefer POSIX APIs that Cosmopolitan
   polyfills (`fork`, `execvp`, `pipe`, `poll`, `pthread`, BSD sockets,
   `wait4`, `getrusage`, `clock_gettime`, `getrandom`). Do **not** assume a
   specific OS at runtime.
2. **Guard OS-specific code.** Anything that reads `/proc` or otherwise depends
   on the host OS must sit behind `#ifdef __linux__` (or equivalent) with a
   portable fallback. See `src/metrics.c` for the pattern.
3. **No external dependencies.** The binary must stay self-contained — no
   linking against system libraries beyond what `cosmocc` provides. No vendored
   third-party C either, unless it is also APE-clean.
4. **Never block the wrapped process.** The pipes from the child must always be
   drained (`logs_pump` runs even when `--no-logs` is set, passing bytes
   through). Always proxy the child's real exit code.
5. **Thread-safe console writes.** All telemetry is emitted via `otel_emit()`,
   which serializes whole NDJSON lines under a mutex. Never `printf` telemetry
   directly — records would interleave between the logs/metrics/traces threads.
6. **Output framing.** By default `otel_emit()` frames each record as
   `::{"oltp":<json>}::` (`-f kjson`); `-f json` (`cfg.wrap_otel == false`) emits the bare
   record. Raw child passthrough (when `--no-logs` is set) goes through
   `otel_emit_raw()` and is **never** framed. Keep that distinction intact.

## Build & test

```sh
make            # build build/koltp (auto-downloads cosmocc on first run)
make test-unit  # build + run the C unit tests (build/test-unit)
make test       # build + scripts/smoke_test.sh
make check      # unit tests + smoke test
make run        # quick manual demo
```

There are two layers of tests:

1. **Unit tests** in `tests/` (`test_json.c`, `test_util.c`, `test_otel.c`)
   using the tiny harness in `tests/test.h`. They link against the project
   objects (minus `main.o`) and exercise the pure helpers: the `sb` string
   builder + JSON escaping, time/random-id/hostname, and the OTLP
   attribute/resource builders. **Add a suite (and register it in
   `tests/main.c` + `test.h`) when you add a pure unit; add CHECKs when you
   change escaping or OTLP field formatting.** Suites that need a child
   process, sockets or `/proc` belong in the smoke test instead.
2. The **smoke test** (`scripts/smoke_test.sh`) wraps a known command and greps
   the NDJSON for the expected logs/metrics/traces records and the proxied exit
   code. **Add an assertion to it for any new end-to-end feature.**

CI (`.github/workflows/ci.yml`) builds with `cosmocc`, runs the unit tests and
the smoke test on Linux, then re-runs the *same* `test-unit` APE and the smoke
test on macOS (Intel + arm64). Releases (`.github/workflows/release.yml`) gate
on both test layers before building the APE on a `v*` tag and attaching the
single binary + SHA-256 to a GitHub Release.

## Code map

| File | Responsibility |
|------|----------------|
| `include/koltp.h` | All shared declarations and the `koltp_config` struct |
| `src/main.c`     | Arg parsing, orchestration, signal forwarding, exit proxy |
| `src/child.c`    | `fork`/`execvp` with stdout/stderr piped back |
| `src/logs.c`     | Per-line stdout/stderr → OTLP `LogRecord` (or passthrough) |
| `src/metrics.c`  | `/proc` (Linux) + `rusage` sampling → OTLP metrics |
| `src/traces.c`   | Embedded OTLP/HTTP receiver + synthetic root span |
| `src/otel.c`     | OTLP/JSON resource/attribute builders + thread-safe sink |
| `src/json.c`     | Growable string buffer (`sb`) + JSON escaping |
| `src/util.c`     | Monotonic-ish time, random trace/span ids, hostname |

## Conventions

- **C11**, compiled with `-Wall -Wextra`; keep it warning-clean.
- Build OTLP JSON with the `sb` string builder and the `otel_*`/`sb_json_*`
  helpers — do not hand-format JSON strings (escaping bugs).
- 64-bit integers in OTLP/JSON are **strings** (e.g. `timeUnixNano`,
  `asInt`, `intValue`). Follow the existing helpers.
- Keep semantic-convention names accurate: `process.cpu.time`,
  `process.memory.usage`, `process.disk.io` (+ `disk.io.direction`),
  `process.open_file_descriptor.count`, `log.iostream`, `process.exit.code`.
- Bump `KOLTP_VERSION` in `include/koltp.h` when cutting a release tag.

## Things to be careful about

- **Cosmopolitan quirks:** a few libc corners differ from glibc. If something
  behaves oddly, check the [Cosmopolitan docs](https://github.com/jart/cosmopolitan)
  before assuming a bug here.
- **The trace receiver does not decode gzip or protobuf** — only OTLP/HTTP+JSON.
  The child env is set to `http/json` + `compression=none` to match.
- **Live metrics are Linux-only** (via `/proc`); other OSes get the `rusage`
  summary at exit. Preserve that fallback when editing `src/metrics.c`.
- The metrics sampler and trace receiver are background `pthread`s tied to the
  child lifetime — always `*_stop()` them before emitting the final records.
