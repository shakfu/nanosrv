# nanosrv-server

A single-threaded HTTP server built on libnanosrv. This is the default server executable for the nanosrv project.

## Overview

nanosrv-server runs a single `Manager` event loop, dispatching HTTP requests through a typed `std::function` callback. It serves as both a practical server and a benchmark reference for single-threaded performance.

The `--busy` flag adds a CPU spin loop to the handler, simulating real work (JSON serialization, database queries, etc.) for benchmarking purposes.

## Usage

```bash
nanosrv-server [--port <1-65535>] [--busy <microseconds>]
               [--idle-timeout <ms>] [--request-timeout <ms>] [--max-body <bytes>]
               [--max-connections <N>] [--max-send-buffer <bytes>]
               [--drain-timeout <ms>]
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `--port` | 8000 | Listen port |
| `--busy` | 0 | Microseconds of CPU spin per request (benchmarking) |
| `--idle-timeout` | 0 (disabled) | Close idle connections after N ms |
| `--request-timeout` | 0 (disabled) | Close connections that buffer an incomplete request past N ms |
| `--max-body` | 0 (disabled) | Reject request bodies larger than N bytes with HTTP 413 |
| `--max-connections` | 0 (disabled) | Cap simultaneously accepted connections; excess are closed immediately |
| `--max-send-buffer` | 0 (disabled) | Close a connection whose unsent outbound backlog exceeds N bytes (drops a slow reader) |
| `--drain-timeout` | 5000 | On SIGINT/SIGTERM, stop accepting and finish in-flight requests for up to N ms before forcing close (0 = stop immediately) |

## Building

```bash
make server-build
```

Binary is at `build/cmake/nanosrv-exe/nanosrv-server`.

## Performance

With a trivial handler on Apple Silicon (M2, 8 cores), nanosrv-server achieves ~200K req/s with wrk (`-t4 -c100 -d10s`), within 2% of the raw C baseline. The cost is one `std::function` virtual call per request.

For CPU-bound handlers (>10us per request), see nanosrv-sharded which distributes work across multiple cores.
