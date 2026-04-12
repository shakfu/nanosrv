# nanosrv-sharded

A multi-threaded HTTP server built on libnanosrv's `ShardedManager`. Distributes connections across N worker threads for parallel request handling.

## Architecture

A single acceptor thread listens for connections and distributes accepted socket FDs round-robin to N worker threads. Each worker runs its own independent `Manager` event loop. On accept, the FD is detached from the acceptor's kqueue/epoll, pushed to a per-worker lock-protected queue, and adopted by the worker with the HTTP handler installed.

This avoids the macOS `SO_REUSEPORT` limitation (which does not load-balance across listeners) and provides true connection-level parallelism.

## Usage

```bash
nanosrv-sharded [--port <1-65535>] [--threads <N>] [--busy <microseconds>]
```

**Options:**

| Flag | Default | Description |
|---|---|---|
| `--port` | 8000 | Listen port |
| `--threads` | 0 (all cores) | Number of worker threads |
| `--busy` | 0 | Microseconds of CPU spin per request (benchmarking) |

## Building

```bash
make server-build
```

Binary is at `build/cmake/nanosrv-sharded/nanosrv-sharded`.

## Performance

The sharded server pays an accept-and-hand-off overhead (mutex, queue, FD re-registration) that makes it slower than nanosrv-server for trivial handlers. The crossover point is around 5-10us of handler CPU time. Beyond that, scaling is near-linear:

| Handler work | Single req/s | Sharded req/s | Speedup |
|---|---|---|---|
| 0us | 200,315 | 186,308 | 0.93x |
| 10us | 66,181 | 145,511 | 2.2x |
| 100us | 9,542 | 51,804 | 5.4x |
| 500us | 1,958 | 12,062 | 6.2x |

Use nanosrv-server when the handler is fast (<5us). Use nanosrv-sharded when the handler does real CPU work.
