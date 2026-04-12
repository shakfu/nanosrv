# nanosrv-server

A single-threaded HTTP server built on libnanosrv. This is the default server executable for the nanosrv project.

## Overview

nanosrv-server runs a single `Manager` event loop, dispatching HTTP requests through a typed `std::function` callback. It serves as both a practical server and a benchmark reference for single-threaded performance.

The `--busy` flag adds a CPU spin loop to the handler, simulating real work (JSON serialization, database queries, etc.) for benchmarking purposes.

## Usage

```bash
nanosrv-server [--port <1-65535>] [--busy <microseconds>]
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `--port` | 8000 | Listen port |
| `--busy` | 0 | Microseconds of CPU spin per request (benchmarking) |

## Building

```bash
make server-build
```

Binary is at `build/cmake/nanosrv-exe/nanosrv-server`.

## Performance

With a trivial handler on Apple Silicon (M2, 8 cores), nanosrv-server achieves ~200K req/s with wrk (`-t4 -c100 -d10s`), within 2% of the raw C baseline. The cost is one `std::function` virtual call per request.

For CPU-bound handlers (>10us per request), see nanosrv-sharded which distributes work across multiple cores.
