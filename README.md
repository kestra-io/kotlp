# koltp — OpenTelemetry process wrapper

`koltp` is a tiny command-line tool that runs **any** command and emits
[OpenTelemetry](https://opentelemetry.io/) JSON to the console — no agent,
no daemon, no language SDK required.

It ships as a single [**Actually Portable Executable**](https://justine.lol/ape.html)
(APE) built with [Cosmopolitan Libc](https://github.com/jart/cosmopolitan):
**one binary file** runs natively on **Linux, macOS, Windows, FreeBSD, OpenBSD
and NetBSD**, on both **amd64 and arm64**.

```sh
koltp -- ./my-program --its --own --flags
```

## What it does

`koltp` spawns the wrapped process and adds three layers of observability, all
emitted as newline-delimited JSON (NDJSON) in the OTLP/JSON wire format and
following OpenTelemetry [Semantic Conventions](https://opentelemetry.io/docs/specs/semconv/):

| Feature  | What you get | Where it goes |
|----------|--------------|---------------|
| **Logs** | Each line of the child's stdout/stderr becomes an OTLP `LogRecord` with `log.iostream` set to `stdout`/`stderr`. | stdout-origin records → **our stdout**; stderr-origin records → **our stderr** |
| **Traces** | An embedded **OTLP/HTTP receiver** captures spans the child exports, and a synthetic **root span** describes the whole execution (duration, exit code, signal). | stdout |
| **Metrics** | Periodic samples of the child **process tree's CPU time/utilization, resident/virtual memory, disk IO, thread count and open file descriptors** as OTLP metrics (`process.*`). | stdout |

Because everything is OTLP/JSON, you can pipe `koltp` output straight into an
OpenTelemetry Collector, `jq`, or any log shipper (use `-f json` for bare,
unframed records; see [Output framing](#output-framing)). Add
[`--log-dir`](#writing-telemetry-to-files---log-dir) to archive the same records
to disk as well.

## Quick start

```sh
# Build the APE (downloads the Cosmopolitan toolchain on first run)
make

# Run something under observation
build/koltp -- sh -c 'echo working; sleep 1; echo failed >&2; exit 3'

# Pretty-print just the logs with jq (-f json emits bare, unframed JSON)
build/koltp -f json -- ./my-program 2>/dev/null | jq 'select(.resourceLogs)'
```

Example log record (one line, pretty-printed here for readability):

```json
{
  "resourceLogs": [{
    "resource": { "attributes": [
      { "key": "service.name", "value": { "stringValue": "my-program" } },
      { "key": "process.pid",  "value": { "intValue": "48213" } }
    ]},
    "scopeLogs": [{
      "scope": { "name": "koltp", "version": "0.1.0" },
      "logRecords": [{
        "timeUnixNano": "1718700000000000000",
        "severityNumber": 9, "severityText": "INFO",
        "body": { "stringValue": "working" },
        "attributes": [{ "key": "log.iostream", "value": { "stringValue": "stdout" } }]
      }]
    }]
  }]
}
```

## Usage

```
koltp [options] -- <command> [args...]
koltp [options] <command> [args...]

Options:
  -s, --service-name NAME  service.name resource attribute
                           (default: $OTEL_SERVICE_NAME or the command name)
  -i, --interval MS        metrics sampling interval (default: 1000)
  -p, --otlp-port PORT     embedded OTLP/HTTP port (default: 4318; falls back
                           to a free port if busy; 0 = always pick a free port)
      --no-logs            disable log capture (output passes through verbatim)
      --no-metrics         disable resource sampling
      --no-traces          disable the embedded trace receiver
  -r, --raw                disable everything (equivalent to
                           --no-logs --no-metrics --no-traces)
  -d, --debug              keep all telemetry (metrics, traces, the OTEL_*
                           env) but print the child's logs raw, and dump
                           the OTEL_* env to stderr
  -P, --protocol PROTO     OTLP protocol the child exports with (default:
                           json): 'json' or 'protobuf' (receiver accepts both)
  -f, --format FORMAT      output format (default: kjson):
                             kjson - ::{"oltp":<json>}:: framed records
                             json  - bare OTLP JSON (newline-delimited)
      --log-dir DIR        also write every record as bare OTLP NDJSON to
                           DIR/log.ndjson (created if needed; the console
                           output is unchanged)
      --log-flush-interval SECONDS
                           rotate the log dir file every SECONDS into
                           log-1.ndjson, log-2.ndjson, ... and report
                           koltp.log.file.count on the root span
                           (requires --log-dir)
  -V, --version            print version and exit
  -h, --help               print help and exit
```

### Output framing (`-f` / `--format`)

By default (`-f kjson`) every telemetry record is emitted on its own line,
**framed** so it is easy to pick out of a mixed console stream:

```
::{"oltp":<the OTLP/JSON record>}::
```

Pass `-f json` to emit the bare OTLP/JSON record (newline-delimited) with no
framing — handy for piping straight into `jq` or an OpenTelemetry Collector:

```sh
koltp -f json -- ./my-program 2>/dev/null | jq 'select(.resourceLogs)'
```

The `--no-logs` passthrough output (raw child bytes) is never framed in either
format.

### Writing telemetry to files (`--log-dir`)

`--log-dir DIR` mirrors every telemetry record into `DIR/log.ndjson`. This is
**additive**: the console output is exactly what it would have been without the
option, so you can keep piping stdout *and* archive a file.

```sh
koltp --log-dir ./telemetry -- ./my-program
jq . ./telemetry/log.ndjson
```

The directory (and any missing parents) is created for you. File lines are
**always bare OTLP JSON**, one record per line, regardless of `-f/--format` —
the console keeps its framing, the `.ndjson` file stays directly consumable by
`jq` and OpenTelemetry Collectors.

Add `--log-flush-interval SECONDS` to roll the file, so a long run produces a
sequence you can ship as each file closes:

```sh
koltp --log-dir ./telemetry --log-flush-interval 60 -- ./my-long-job
# ./telemetry/log-1.ndjson  ./telemetry/log-2.ndjson  ./telemetry/log-3.ndjson ...
```

Rotation is **lazy**: the file rolls on the first record written after the
interval has elapsed, and the index advances by one — so an idle stretch never
leaves an empty file behind. The final root span records how many files were
produced, and lands in the last of them:

```json
{ "key": "koltp.log.file.count", "value": { "intValue": "3" } }
```

That attribute is only present when `--log-flush-interval` is used;
`--log-flush-interval` requires `--log-dir`.

Because the file holds OTLP records only, the raw child bytes emitted by
`--no-logs` and `-d/--debug` are *not* written to it — in those modes the file
contains metrics and traces.

`koltp` proxies the child's exit code (and reports `128 + signal` if the child
was killed by a signal). `SIGINT`/`SIGTERM`/`SIGHUP` are forwarded to the child.

## Capturing traces from your app

When traces are enabled (default), `koltp` starts an OTLP/HTTP receiver on
`127.0.0.1:4318` (or, if that port is busy, an automatically chosen free port)
and exports these environment variables to the child — pointing at the port it
actually bound — so most OpenTelemetry SDKs auto-configure themselves:

```
OTEL_EXPORTER_OTLP_ENDPOINT=http://127.0.0.1:4318
OTEL_EXPORTER_OTLP_PROTOCOL=http/json        # only if not already set
OTEL_EXPORTER_OTLP_TRACES_PROTOCOL=http/json # only if not already set
OTEL_EXPORTER_OTLP_COMPRESSION=none
OTEL_TRACES_EXPORTER=otlp
OTEL_SERVICE_NAME=<service name>
```

The receiver accepts both OTLP/HTTP encodings on the same port:

- **`http/json`** — forwarded to the console verbatim.
- **`http/protobuf`** — decoded into the equivalent OTLP/JSON, so downstream
  consumers always see one consistent NDJSON format.

`koltp` defaults the child to `http/json`. Switch it with `--protocol`:

```
koltp --protocol protobuf -- ./your-app
```

Without `--protocol`, koltp leaves `OTEL_EXPORTER_OTLP_PROTOCOL` /
`OTEL_EXPORTER_OTLP_TRACES_PROTOCOL` untouched if you set them yourself, so the
env still works too:

```
OTEL_EXPORTER_OTLP_PROTOCOL=http/protobuf koltp -- ./your-app
```

(An explicit `--protocol` takes precedence over those env vars.)

Either way the spans are emitted alongside `koltp`'s own root span. (The receiver
expects **uncompressed** bodies — gzip payloads are not decoded — and speaks
OTLP over HTTP only, not gRPC.)

## Platform notes

| Capability | Linux | macOS / Windows / BSD |
|------------|:-----:|:---------------------:|
| Log capture (stdout/stderr) | ✅ | ✅ |
| Embedded trace receiver     | ✅ | ✅ |
| Root execution span         | ✅ | ✅ |
| **Live** metric sampling    | ✅ (`/proc`) | ⚠️ summary only |
| Final usage summary (`rusage`) | ✅ | ✅ |

Live per-interval metrics are read from `/proc` and are therefore Linux-only
(the single APE binary detects the host OS at runtime). Each sample aggregates
the child **and all of its descendants**, and emits:

| Metric | Type | Unit | Attributes |
|---|---|---|---|
| `process.cpu.time` | sum | `s` | `cpu.mode`=user\|system |
| `process.cpu.utilization` | gauge | `1` | `cpu.mode`=user\|system |
| `process.memory.usage` | gauge | `By` | — (summed RSS) |
| `process.memory.virtual` | gauge | `By` | — (summed vsize) |
| `process.disk.io` | sum | `By` | `disk.io.direction`=read\|write |
| `process.thread.count` | gauge | `{thread}` | — |
| `process.open_file_descriptor.count` | gauge | `{count}` | — |

`process.cpu.utilization` is the CPU-seconds consumed over the interval divided
by the elapsed wall time and the CPU count, so it ranges 0–1 across the machine.

On the other platforms `koltp` still emits an authoritative usage summary from
`wait4()`/`getrusage()` when the child exits (CPU time, peak RSS, block IO).

## Building

`make` uses `cosmocc` if it is on your `PATH`; otherwise it downloads the
Cosmopolitan toolchain into `build/cosmocc/` automatically.

```sh
make            # build build/koltp
make test-unit  # build + run the C unit tests (build/test-unit)
make test       # build + run the end-to-end smoke test
make check      # unit tests + smoke test
make run        # build + run a demo command
make clean      # remove objects and the binary
make distclean  # also remove the downloaded toolchain
make install    # install to /usr/local/bin (honors DESTDIR)
```

### Tests

- **Unit tests** (`tests/`) cover the pure building blocks — the string
  builder + JSON escaping (`json.c`), time/random-id/hostname helpers
  (`util.c`), and the OTLP attribute/resource builders (`otel.c`). They use a
  tiny dependency-free harness (`tests/test.h`) and compile into their own APE,
  `build/test-unit`.
- The **smoke test** (`scripts/smoke_test.sh`) runs the real binary end-to-end
  and asserts the emitted OTLP logs, metrics, traces and the proxied exit code.

CI runs both on Linux, and re-runs the very same `test-unit` APE and the
smoke test on macOS (Intel **and** arm64) to prove the binary is portable.

Requirements for the auto-download path: `curl` and `unzip`. The produced
`build/koltp` is the APE — copy that one file to any supported OS/arch and run it.

## How it works

```
            ┌────────────────────────── koltp ──────────────────────────┐
            │                                                           │
  argv ───▶ │  fork/exec child  ──stdout/stderr pipes──▶ logs_pump ─────┼─▶ OTLP logs
            │        │                                                   │
            │        ├──▶ metrics sampler (/proc or rusage) ────────────┼─▶ OTLP metrics
            │        │                                                   │
   child ◀──┼── env: OTEL_EXPORTER_OTLP_ENDPOINT=127.0.0.1:4318          │
   spans ───┼──▶ embedded OTLP/HTTP receiver ───────────────────────────┼─▶ OTLP traces
            │     + synthetic root span (duration, exit code)            │
            └───────────────────────────────────────────────────────────┘
```

Source layout:

```
include/koltp.h   shared declarations
src/main.c       orchestration, signal/exit proxying
src/args.c       command-line parsing
src/child.c      fork/exec with piped stdout/stderr; exit-code mapping
src/logs.c       stdout/stderr -> OTLP log records
src/metrics.c    /proc + rusage sampling -> OTLP metrics
src/traces.c     embedded OTLP/HTTP receiver + root span
src/otlp_pb.c    OTLP/protobuf trace payload -> OTLP/JSON decoder
src/otel.c       OTLP/JSON building blocks + thread-safe console sink
src/filesink.c   optional --log-dir NDJSON file sink + rotation
src/json.c       growable string buffer with JSON escaping
src/util.c       time, random ids, hostname
tests/           unit tests (test.h harness + test_*.c suites)
scripts/         smoke_test.sh end-to-end integration test
```

See [AGENTS.md](AGENTS.md) for contributor and AI-agent guidance.

## License

[MIT](LICENSE) © 2026 Ludovic DEHON
