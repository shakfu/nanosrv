# nanosrv

Python bindings for the **nanosrv** embedded HTTP/WebSocket server library, built with [nanobind](https://github.com/wjakob/nanobind) and [scikit-build-core](https://github.com/scikit-build/scikit-build-core).

nanosrv is a lightweight, single-file C++ server library (based on [Mongoose](https://github.com/cesanta/mongoose)) that provides HTTP and WebSocket support with both single-threaded and multi-threaded (sharded) event loops. The Python bindings wrap this as a native extension, giving Python code direct access to the C++ event loop with minimal overhead.

## Features

- **HTTP server** with typed request/response handlers
- **WebSocket** support (text, binary, ping/pong, upgrade)
- **Single-threaded** (`Manager`) and **multi-threaded** (`ShardedManager`) event loops
- **URL parsing**, **Base64** encode/decode, **URL** encode/decode
- **JSON** path-based extraction (string, number, integer, boolean)
- **Configurable logging** levels
- **Connection hardening**: idle timeout (`set_idle_timeout`), request-receive deadline
  (`set_request_timeout`), and request-body cap with 413 (`set_max_body_size`)
- GIL-releasing `poll()` and `run()` for responsive Python integration

## Server Implementations

The project provides six server implementations: five built on the nanosrv networking core, plus the upstream Mongoose 7.21 library (from which nanosrv was extracted) as a reference baseline. They differ in language, threading model, and abstraction level.

### Overview

| Implementation | Language | Threading | Event loop | Source |
|---|---|---|---|---|
| **mongoose 7.21** | C | Single-threaded | `mg_mgr_poll()` loop | `thirdparty/mongoose/main.c` |
| **mungo-server** | C | Single-threaded | `mg_mgr_poll()` loop | `projects/mungo/main.c` |
| **nanosrv-server** | C++ | Single-threaded | `Manager::poll()` loop | `projects/nanosrv-exe/main.cpp` |
| **nanosrv-sharded** | C++ | Multi-threaded (accept-and-hand-off) | 1 acceptor + N worker `Manager` loops | `projects/nanosrv-sharded/main_sharded.cpp` |
| **nanosrv Python Manager** | Python (nanobind) | Single-threaded | `Manager.poll()` from Python | `nanosrv.Manager` |
| **nanosrv Python ShardedManager** | Python (nanobind) | Multi-threaded | `ShardedManager.run()` from Python | `nanosrv.ShardedManager` |

### Architecture

**mongoose 7.21** is the upstream reference. Mongoose is a widely-used, battle-tested embedded networking library with HTTP, WebSocket, MQTT, and TLS support in a single `mongoose.c`/`mongoose.h` pair (~33K lines). It uses the same callback-based event loop as nanosrv. The server included here uses the identical API pattern as mungo-server, providing a baseline to verify that nanosrv's extraction from Mongoose introduces no performance regression.

**mungo-server** links against `mungo.c`/`mungo.h` -- a minimal subset extracted from Mongoose 7.21, stripped down to HTTP and WebSocket only (~5.5K lines vs ~33K). The API is identical: plain C function pointer callbacks receiving `(mg_connection*, int ev, void* ev_data)`. No abstraction layer -- you check the event code, cast `ev_data`, and call `mg_http_reply()`. The reduced code size means faster compilation and a smaller binary, at the cost of features removed during extraction (MQTT, TLS, multipart, SSI, etc.).

**nanosrv-server** wraps the C core in a C++ RAII layer. `Manager` owns the `mg_mgr` and provides `http_listen()` with typed `std::function<void(Connection&, HttpMessage&)>` callbacks -- no manual event code checks or void pointer casts. The cost is one virtual call through `std::function` per request. The C++ layer also adds `ConnectionRef`, typed enums (`Event`, `WsOpcode`, `LogLevel`), and convenience methods on `Connection` and `HttpMessage`.

**nanosrv-sharded** uses a single acceptor thread that listens for connections and distributes accepted socket FDs round-robin to N worker threads. Each worker runs its own independent `Manager` event loop. On accept, the FD is detached from the acceptor's kqueue/epoll (via `detach_fd()`), pushed to a per-worker lock-protected queue, and adopted by the worker using `wrapfd()` with the HTTP protocol handler installed. This avoids the macOS SO_REUSEPORT limitation (which does not load-balance across listeners) and provides true connection-level parallelism.

**nanosrv Python Manager** exposes the C++ `Manager` class to Python via nanobind. The GIL is released during `poll()` so the event loop can process I/O without blocking Python threads. When a request arrives, the C++ trampoline acquires the GIL, calls the Python handler, and releases it again. The overhead per request is one GIL acquire/release cycle plus the Python-to-C++ marshalling (~100us at the scale we measured).

**nanosrv Python ShardedManager** exposes the C++ `ShardedManager` to Python. Worker threads each run their own event loop (GIL released), acquiring the GIL only to execute the Python callback. Since CPython's GIL serializes all Python execution, multiple workers contend for the GIL -- the parallelism benefits I/O and C++-side work but not Python handler code. This makes the sharded Python variant slower than the single-threaded one for trivial Python handlers, but beneficial when the handler triggers significant C/C++ work (e.g., computation or I/O that releases the GIL).

### Feature Comparison

| Feature | mongoose 7.21 | mungo-server | nanosrv-server | nanosrv-sharded | nanosrv Python Manager | nanosrv Python ShardedManager |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| HTTP request/response | yes | yes | yes | yes | yes | yes |
| WebSocket | yes | yes | yes | yes | yes | yes |
| MQTT | yes | -- | -- | -- | -- | -- |
| TLS (OpenSSL/mbedTLS) | yes | -- | -- | -- | -- | -- |
| Multipart / SSI / OTA | yes | -- | -- | -- | -- | -- |
| Typed callbacks (no void*) | -- | -- | yes | yes | yes | yes |
| RAII resource management | -- | -- | yes | yes | yes | yes |
| Multi-threaded I/O | -- | -- | -- | yes | -- | yes |
| Signal handling | manual | manual | manual | manual | Python `signal` | Python `signal` + `threading.Event` |
| Dependencies | libc only | libc only | libc + libstdc++ | libc + libstdc++ | Python + nanobind | Python + nanobind |
| Library size (lines) | ~33K | ~5.5K | ~5.5K + C++ wrappers | ~5.5K + C++ wrappers | ~5.5K + bindings | ~5.5K + bindings |

## Quickstart

### Install and build

```bash
uv sync                # install dependencies + build extension
uv run pytest          # run tests
```

### Minimal HTTP server

```python
import nanosrv

mgr = nanosrv.Manager()
mgr.http_listen("http://0.0.0.0:8080", lambda conn, msg: (
    conn.http_reply(200, "Content-Type: text/plain\r\n",
                    f"Hello! You requested {msg.uri}")
))

while True:
    mgr.poll(1000)
```

### Sharded (multi-threaded) server

```python
import nanosrv
import threading

mgr = nanosrv.ShardedManager(0)  # 0 = use all CPU cores
mgr.http_listen("http://0.0.0.0:8080", lambda conn, msg: (
    conn.http_reply(200, "", "OK\n")
))

runner = threading.Thread(target=mgr.run, daemon=True)
runner.start()

# mgr.stop() to shut down
```

### WebSocket upgrade

```python
def handler(conn, event, data):
    if event == nanosrv.Event.HttpMessage:
        conn.ws_upgrade(data, "")
    elif event == nanosrv.Event.WsMessage:
        conn.ws_send_text(f"echo: {data.data}")

mgr = nanosrv.Manager()
mgr.http_listen_event("http://0.0.0.0:8080", handler)
```

### Utilities

```python
import nanosrv

# Base64
nanosrv.base64_encode("hello")   # "aGVsbG8="
nanosrv.base64_decode("aGVsbG8=") # "hello"

# URL encode/decode
nanosrv.url_encode("hello world") # "hello%20world"
nanosrv.url_decode("hello%20world") # "hello world"

# URL parsing
u = nanosrv.Url.parse("https://example.com:8443/path")
# u.host="example.com", u.port=8443, u.path="/path", u.is_ssl=True

# JSON path extraction
nanosrv.json.string('{"name": "nanosrv"}', "$.name")  # "nanosrv"
nanosrv.json.integer('{"n": 42}', "$.n")             # 42
nanosrv.json.number('{"x": 3.14}', "$.x")            # 3.14
nanosrv.json.boolean('{"ok": true}', "$.ok")          # True
```

## API Reference

### Classes

| Class | Description |
|---|---|
| `Manager` | Single-threaded event loop. Call `poll(timeout_ms)` in a loop. |
| `ShardedManager(n)` | Multi-threaded event loop. `n=0` uses hardware concurrency. Call `run()` to block, `stop()` to shut down. |
| `Connection` | Passed to handlers. Methods: `http_reply()`, `ws_send_text()`, `ws_send_binary()`, `ws_upgrade()`, `send_bytes()`, `close()`. |
| `ConnectionRef` | Non-owning handle returned by `http_listen()`. Methods: `http_reply()`, `send_bytes()`, `close()`. |
| `HttpMessage` | Read-only incoming HTTP message. Properties: `method`, `uri`, `query`, `body`, `status_code`. Methods: `header(name)`, `credentials()`. |
| `WsMessage` | Read-only WebSocket frame. Properties: `data`, `flags`, `opcode`. |
| `Url` | URL parse result. Static method: `Url.parse(url)`. Properties: `host`, `port`, `path`, `is_ssl`. |

### Enums

| Enum | Values |
|---|---|
| `Event` | `Error`, `Open`, `Poll`, `Resolve`, `Connect`, `Accept`, `TlsHandshake`, `Read`, `Write`, `Close`, `HttpHeaders`, `HttpMessage`, `WsOpen`, `WsMessage`, `WsControl`, `Wakeup`, `User` |
| `WsOpcode` | `Continue`, `Text`, `Binary`, `Close`, `Ping`, `Pong` |
| `LogLevel` | `None`, `Error`, `Info`, `Debug`, `Verbose` |

### Functions

| Function | Description |
|---|---|
| `base64_encode(s)` / `base64_decode(s)` | Base64 encode/decode |
| `url_encode(s)` / `url_decode(s)` | URL percent-encoding |
| `set_log_level(level)` / `get_log_level()` | Control log verbosity |
| `millis()` | Current time in milliseconds |
| `json.string(json, path)` | Extract string at JSON path |
| `json.number(json, path)` | Extract float at JSON path |
| `json.integer(json, path)` | Extract int at JSON path |
| `json.boolean(json, path)` | Extract bool at JSON path |

### Performance

Benchmarked with `wrk -t4 -c100 -d10s` on Apple Silicon (M2, 8 cores).

#### Trivial handler (no CPU work)

| Server | Req/sec | Avg Latency | p99 Latency | vs mongoose |
|---|---|---|---|---|
| mongoose 7.21 (C) | 205,744 | 466us | 779us | -- |
| mungo-server (C) | 208,152 | 461us | 860us | +1% |
| nanosrv-server (C++) | 200,851 | 486us | 817us | -2% |
| nanosrv-sharded (C++, 8 workers) | 183,784 | 683us | 8.5ms | -11% |
| nanosrv Python Manager (Python) | 161,698 | 610us | 1.24ms | -21% |
| nanosrv Python ShardedManager (Python, 8 workers) | 86,769 | 1.15ms | 2.67ms | -58% |

With a trivial handler, the single-threaded C servers dominate. Mongoose 7.21 and mungo-server are within noise of each other (~206K vs ~208K req/s), confirming that nanosrv's extraction from Mongoose introduces no performance regression despite removing ~27K lines of code. The C++ wrapper costs ~2% for `std::function` dispatch. The sharded server pays an accept-and-hand-off tax (mutex, queue, FD re-registration in kqueue) that exceeds the benefit when there is no CPU work to parallelize. The nanosrv Python Manager retains ~79% of native C throughput -- the remaining cost is GIL acquire/release per request. The nanosrv Python ShardedManager is slowest here because multiple worker threads contend for the GIL to run a trivial Python callback.

#### CPU-bound handler (busy spin)

The `--busy <us>` flag on the C++ servers adds a CPU spin loop to the handler, simulating real work (JSON serialization, computation, etc.):

| Busy (us) | Single req/s | Sharded req/s | Speedup |
|---|---|---|---|
| 0 | 200,315 | 186,308 | 0.93x |
| 10 | 66,181 | 145,511 | **2.2x** |
| 50 | 18,284 | 88,111 | **4.8x** |
| 100 | 9,542 | 51,804 | **5.4x** |
| 500 | 1,958 | 12,062 | **6.2x** |

With just 10us of handler work the sharded server already doubles throughput. At 500us (realistic for a database query or large JSON response) it delivers 6.2x speedup across 8 workers -- near-linear scaling. The crossover point is around 5-10us of handler CPU time.

#### Run benchmarks

```bash
make bench    # builds everything, runs all benchmarks, generates HTML report
```

This produces terminal output and an HTML report at `build/bench-report.html` with SVG charts and tables.

### When to Use Which

**mongoose 7.21** -- Use when you need the full Mongoose feature set: MQTT, TLS, multipart uploads, SSI, OTA updates, or any of the many protocols and utilities that Mongoose provides out of the box. It is battle-tested, widely deployed, and commercially supported. The performance is identical to mungo-server. Choose this over nanosrv when you need features that were stripped during extraction.

**mungo-server** -- Use when you only need HTTP and WebSocket and want the smallest possible footprint. At ~5.5K lines vs Mongoose's ~33K, it compiles faster, produces a smaller binary, and has less code surface to audit. The API is identical to Mongoose, so migrating between them is trivial. Best for embedded systems, microcontrollers, or any C project where you want a minimal HTTP server with no extras.

**nanosrv-server** -- The default choice for C++ projects. Typed callbacks, RAII, and `std::function` handlers make it safer and more ergonomic than the C API with negligible overhead (~2%). Use this for any single-threaded C++ server where the handler is fast (under ~5us) or where simplicity matters more than multi-core scaling.

**nanosrv-sharded** -- Use when your handler does real CPU work (>10us per request): computation, serialization, database query building, or any blocking operation. The accept-and-hand-off architecture distributes connections across workers for true parallelism. Not worth the overhead for trivial handlers -- the single-threaded server will be faster in that case.

**nanosrv Python Manager** -- The default choice for Python projects. You get 79% of native C throughput with a Pythonic API. The single-threaded event loop avoids GIL contention entirely. Use this for Python HTTP/WebSocket servers, prototyping, scripting, or any case where Python handler logic is the bottleneck (since the GIL already serializes it, multiple threads won't help).

**nanosrv Python ShardedManager** -- Use only when the Python handler triggers significant work that releases the GIL: calling into C extensions, doing I/O via libraries that release the GIL, or dispatching to native code. If your handler is pure Python, the single-threaded Manager will be faster. On Python 3.13+ with free-threading (PEP 703), the sharded variant would benefit from true parallel Python execution, but as of CPython 3.12 the GIL serializes all Python-level work.

**Decision flowchart:**

```
Is it a Python project?
  yes --> Is the handler pure Python?
            yes --> nanosrv Python Manager
            no  --> Does the handler release the GIL for heavy work?
                      yes --> nanosrv Python ShardedManager
                      no  --> nanosrv Python Manager
  no  --> Do you need MQTT, TLS, multipart, or other Mongoose features?
            yes --> mongoose 7.21
            no  --> Is it a C-only project or size-constrained?
                      yes --> mungo-server
                      no  --> Does the handler do >10us of CPU work per request?
                                yes --> nanosrv-sharded
                                no  --> nanosrv-server
```

## Build Targets

Use `make help` for the full list. Key targets:

```
make build       # rebuild extension after code changes
make test        # run pytest suite
make lint        # ruff check + fix
make format      # ruff format
make typecheck   # mypy type checking
make qa          # all of the above
make wheel       # build wheel distribution
make dist        # build wheel + sdist + twine check
make clean       # remove build artifacts
```

C++ server targets:

```
make server-build   # build nanosrv-server and nanosrv-sharded via CMake
make server-run     # build and run nanosrv-server
make server-test    # build and run C++ tests via ctest
make server-clean   # remove CMake build directory
make bench          # run wrk benchmarks (builds everything first)
```

## License

GPL3
