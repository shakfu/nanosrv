# nanosrv

Python bindings for the **nanosrv** embedded HTTP/WebSocket server library, built with [nanobind](https://github.com/wjakob/nanobind) and [scikit-build-core](https://github.com/scikit-build/scikit-build-core).

nanosrv is a lightweight, single-file C++ server library (based on [Mongoose](https://github.com/cesanta/mongoose)) that provides HTTP and WebSocket support with both single-threaded and multi-threaded (sharded) event loops. The Python bindings wrap this as a native extension, giving Python code direct access to the C++ event loop with minimal overhead.

## Features

- **HTTP server** with typed request/response handlers
- **WebSocket** support (text, binary, ping/pong, upgrade) on both event loops
- **Binary-safe payloads**: request bodies and WebSocket frames arrive as `bytes`;
  every send accepts `bytes`, `bytearray`, `memoryview` or `str`
- **Streamed responses**: chunked writes and Server-Sent Events from Python
  (`start_chunked`, `write_chunk`, `start_sse`, `sse_send`), with
  `send_queue_len` for backpressure
- **Single-threaded** (`Manager`) and **multi-threaded** (`ShardedManager`) event loops
- **URL parsing**, **Base64** encode/decode, **URL** encode/decode
- **JSON** path-based extraction (string, number, integer, boolean)
- **Configurable logging** levels
- **Connection hardening**: idle timeout (`set_idle_timeout`), request-receive deadline
  (`set_request_timeout`), client connect timeout (`set_connect_timeout`), request-body
  cap with 413 (`set_max_body_size`), max-connection cap (`set_max_connections`), and
  send-buffer watermark (`set_max_send_buffer`)
- **Observability**: cumulative counters and a live connection gauge (`Manager.metrics`,
  `ShardedManager.metrics`)
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

**nanosrv Python ShardedManager** exposes the C++ `ShardedManager` to Python. Worker threads each run their own event loop (GIL released), acquiring the GIL only to execute the Python callback. Each worker thread registers with CPython once, when it starts, rather than once per request. On a GIL interpreter the parallelism benefits the C++-side work (event loop, parsing, socket I/O) but not the Python handler itself, which the GIL still serializes: measured against a single-threaded `Manager` on Linux, 1.7x for a trivial handler and 0.96x for one doing ~100us of pure Python. On a free-threaded interpreter the Python handler parallelises too -- 3.4x and 4.3x for those same two cases. See [Free-threaded CPython](#free-threaded-cpython-313t-and-later).

### Feature Comparison

| Feature | mongoose 7.21 | mungo-server | nanosrv-server | nanosrv-sharded | nanosrv Python Manager | nanosrv Python ShardedManager |
|---|:---:|:---:|:---:|:---:|:---:|:---:|
| HTTP request/response | yes | yes | yes | yes | yes | yes |
| WebSocket | yes | yes | yes | yes | yes | yes |
| MQTT | yes | -- | -- | -- | -- | -- |
| TLS (mbedTLS) | yes | -- | opt-in | opt-in | -- | -- |
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
        # data.data is bytes (binary frames included); data.text decodes UTF-8
        conn.ws_send_text(f"echo: {data.text}")

mgr = nanosrv.Manager()
mgr.http_listen_event("http://0.0.0.0:8080", handler)
```

`ShardedManager.http_listen_event()` takes the same handler, so WebSocket works
on the multi-threaded loop too. The handler runs on the worker thread that owns
the connection, so it must be thread-safe.

### Streaming a response

```python
def handler(conn, msg):
    conn.start_chunked(200, "Content-Type: text/plain\r\n")
    for piece in produce():
        if conn.send_queue_len > 1 << 20:   # reader cannot keep up
            break
        conn.write_chunk(piece)
    conn.write_chunk(b"")                   # terminating chunk
```

For Server-Sent Events, `start_sse()` sends the event-stream headers and
`sse_send(data, event=..., id=...)` emits one correctly framed event.

### Acting on a connection later

The `Connection` handed to a handler is valid only during that call. To push to
it afterwards, keep `conn.id` and use `wakeup()` from any thread -- the handler
is called again with `Event.Wakeup` and your payload as `bytes`:

```python
mgr.wakeup(conn_id, b"payload")   # Manager and ShardedManager both
```

### Utilities

```python
import nanosrv

# Base64
nanosrv.base64_encode("hello")   # "aGVsbG8="
nanosrv.base64_decode("aGVsbG8=") # b"hello"  (decoding yields bytes)

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
| `Connection` | Passed to handlers. Methods: `http_reply()`, `start_chunked()`, `write_chunk()`, `start_sse()`, `sse_send()`, `ws_send_text()`, `ws_send_binary()`, `ws_upgrade()`, `send_bytes()`, `drain()`, `close()`. Property: `send_queue_len`. |
| `ConnectionRef` | Non-owning handle returned by `http_listen()`. Methods: `http_reply()`, `send_bytes()`, `close()`. |
| `HttpMessage` | Read-only incoming HTTP message. Properties: `method`, `uri`, `query`, `body` (bytes), `text` (body decoded UTF-8), `status_code`. Methods: `header(name)`, `credentials()`. |
| `WsMessage` | Read-only WebSocket frame. Properties: `data` (bytes), `text` (data decoded UTF-8), `flags`, `opcode`. |
| `Metrics` | Snapshot of a manager's counters. Properties: `accepted`, `closed`, `errors`, `bytes_read`, `bytes_written`, `active`. Read via `Manager.metrics` / `ShardedManager.metrics`. |
| `Url` | URL parse result. Static method: `Url.parse(url)`. Properties: `host`, `port`, `path`, `is_ssl`. |

#### Payload types: bytes vs str

Wire payloads cross as **bytes**; protocol metadata crosses as **str**.

| Value | Type | Notes |
|---|---|---|
| `HttpMessage.body`, `WsMessage.data` | `bytes` | Never raises, whatever the payload |
| `HttpMessage.text`, `WsMessage.text` | `str` | Strict UTF-8; raises `UnicodeDecodeError` on binary input, which is the caller's explicit choice |
| `method`, `uri`, `query`, `header()`, `credentials()` | `str` | Decoded with `surrogateescape`, so a malformed request cannot raise inside a handler. Lossless: `s.encode("utf-8", "surrogateescape")` recovers the raw bytes |
| Any send argument | `bytes` \| `bytearray` \| `memoryview` \| `str` | A `str` is encoded UTF-8 |

This is a change from v0.2.0 and earlier, where every payload was `str`: a
binary body or WebSocket frame raised `UnicodeDecodeError` inside the handler,
and no send path accepted `bytes`. Handlers written against that behaviour need
`msg.text` wherever they used `msg.body` as text.

Note that `close()` is immediate and discards whatever is still buffered for
sending; after `send_bytes()` or a streamed response, use `drain()` to close
once the output has been flushed.

#### Callback object lifetime

The `Connection`, `HttpMessage`, and `WsMessage` objects handed to a handler are
**borrowed views valid only for the duration of that call**. They point into
buffers owned by the event loop, which reuses or frees them as soon as the
handler returns. Do not stash one and touch it later (from another thread, a
timer, or a subsequent event) -- read what you need during the call, and copy
out any bytes you must keep (e.g. `bytes(msg.body)` / `std::string(hm.body...)`).
The Python bindings enforce this: a stored `Connection`/`HttpMessage` raises if
used after the callback returns (see `tests/test_nanosrv.py`,
`TestCallbackObjectLifetime`). To act on a connection later, keep `conn.id` and
call `wakeup(id, data)` on the `Manager` or `ShardedManager` from any thread;
the handler is invoked again with `Event.Wakeup` and the payload as `bytes`. The long-lived exception is `ConnectionRef`, the
non-owning handle returned by `http_listen()`, which stays valid until its
listener closes.

### Enums

| Enum | Values |
|---|---|
| `Event` | `Error`, `Open`, `Poll`, `Resolve`, `Connect`, `Accept`, `TlsHandshake`, `Read`, `Write`, `Close`, `HttpHeaders`, `HttpMessage`, `WsOpen`, `WsMessage`, `WsControl`, `Wakeup`, `User` |
| `WsOpcode` | `Continue`, `Text`, `Binary`, `Close`, `Ping`, `Pong` |
| `LogLevel` | `Off`, `Error`, `Info`, `Debug`, `Verbose` (the zero level was called `None` before; `LogLevel.None` is a `SyntaxError` in Python, so it is now `Off`, with `getattr(LogLevel, "None")` kept as an alias) |

### Functions

| Function | Description |
|---|---|
| `base64_encode(s)` / `base64_decode(s)` | Base64 encode/decode. Encoding accepts bytes or str and returns str; decoding returns `bytes` |
| `url_encode(s)` / `url_decode(s)` | URL percent-encoding. `url_decode_bytes(s)` returns the decoded bytes verbatim |
| `set_log_level(level)` / `get_log_level()` | Control log verbosity |
| `millis()` | Current time in milliseconds |
| `tls_available()` | Whether the build has a TLS backend (`False` in the default build; `True` when built with the mbedTLS backend -- see Build Targets) |
| `json.string(json, path)` | Extract string at JSON path |
| `json.number(json, path)` | Extract float at JSON path |
| `json.integer(json, path)` | Extract int at JSON path |
| `json.boolean(json, path)` | Extract bool at JSON path |

### Limitations & security notes

- **TLS is opt-in (C++ build).** The default build links a no-op TLS stub, so
  `https://` and `wss://` are not supported: `tls_available()` returns `False`,
  and calling `http_listen` / `http_listen_event` with a TLS URL raises
  `RuntimeError` immediately rather than failing later at the handshake. An
  mbedTLS backend can be enabled when building the C++ library/server with
  `-DNANOSRV_TLS=mbed` (see Build Targets), after which `tls_available()` is
  `True` and TLS listeners/clients work. The Python wheel currently ships with
  the stub (no TLS); terminate TLS in front of nanosrv (e.g. a reverse proxy)
  when using the bindings.
- **IP ACLs are not enforced automatically.** The library provides
  `check_ip_acl()` (C++), which now matches both IPv4 and IPv6 with bitwise
  prefix comparison -- a previous version returned early for IPv6, so a
  restrictive ACL silently failed open for IPv6 peers. It is a building block:
  no listener applies an ACL on its own yet, so wire it into your handler if you
  need address filtering.
- **Not yet hardened for hostile networks.** See the connection-hardening knobs
  (`set_idle_timeout`, `set_request_timeout`, `set_connect_timeout`,
  `set_max_body_size`, `set_max_connections`, `set_max_send_buffer`) and the
  graceful-drain support, but treat exposure to untrusted clients as
  experimental. See [SECURITY.md](SECURITY.md) for the current threat-model
  posture and how to report a vulnerability.
- **Observability.** `Manager.metrics` / `ShardedManager.metrics` expose
  cumulative counters (accepted, closed, errors, bytes read/written) plus a live
  connection gauge for health/metrics endpoints.

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

**The two Python rows above predate the per-worker thread-registration fix**
(workers used to register with CPython once per request instead of once per
thread). On Linux the same change took a trivial-handler `ShardedManager` from
126K to 648K req/s, so the `ShardedManager` figure in particular understates
the current code by a wide margin; these macOS numbers have not been re-run.
The C and C++ rows are unaffected. See
[Free-threaded CPython](#free-threaded-cpython-313t-and-later) for current,
reproducible Python numbers.

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

#### Free-threaded CPython (3.13t and later)

`ShardedManager` is a thread-per-core design, so the GIL is precisely what
holds it back: with a pure-Python handler, workers contend to run one handler
at a time and adding workers makes throughput *worse*. On a free-threaded
interpreter that inverts. The wheel opts in (nanobind's `FREE_THREADED`), so
`sys._is_gil_enabled()` stays `False` after importing nanosrv.

Measured on Linux x86_64 (i7-12650H, 16 threads), the same CPython 3.13.15
built both ways, 64 connections, handler doing ~120us of pure-Python work:

| Workers | 3.13 (GIL) | 3.13t (free-threaded) |
|---|---|---|
| 1 | 9,388 rps | 6,806 rps |
| 2 | 8,482 rps | 12,702 rps |
| 4 | 7,181 rps | 20,612 rps |
| 8 | 7,151 rps | 30,832 rps |
| 16 | 6,726 rps | **33,296 rps** |

Best configuration to best configuration, that is **3.6x**. The GIL column
*degrades* as workers are added, which is what a thread-per-core design under
a global lock must do. Reproduce with:

```bash
uv python install 3.13t
uv run python scripts/bench_freethreading.py
```

Free-threaded CPython executes single-threaded Python roughly 30% slower
(the same handler costs 119.5us there against 93us under the GIL), so one
worker is slower and the crossover is at two.

**Your handler really is concurrent.** With no GIL, handlers on different
workers run at the same instant rather than interleaving, so shared state in
your handler needs its own locking. The bindings and the C++ core are safe: the
test suite passes on 3.13t, and a 20-second stress run (2.7M HTTP requests,
1.6M WebSocket echoes, 56K cross-thread wakeups across 8 workers,
payload-verified) completes with zero errors.

#### Run benchmarks

```bash
make bench    # builds everything, runs all benchmarks, generates HTML report
```

```bash
# GIL vs free-threaded scaling for ShardedManager (needs `uv python install 3.13t`)
uv run python scripts/bench_freethreading.py
```

This produces terminal output and an HTML report at `build/bench-report.html` with SVG charts and tables.

### When to Use Which

**mongoose 7.21** -- Use when you need the full Mongoose feature set: MQTT, TLS, multipart uploads, SSI, OTA updates, or any of the many protocols and utilities that Mongoose provides out of the box. It is battle-tested, widely deployed, and commercially supported. The performance is identical to mungo-server. Choose this over nanosrv when you need features that were stripped during extraction.

**mungo-server** -- Use when you only need HTTP and WebSocket and want the smallest possible footprint. At ~5.5K lines vs Mongoose's ~33K, it compiles faster, produces a smaller binary, and has less code surface to audit. The API is identical to Mongoose, so migrating between them is trivial. Best for embedded systems, microcontrollers, or any C project where you want a minimal HTTP server with no extras.

**nanosrv-server** -- The default choice for C++ projects. Typed callbacks, RAII, and `std::function` handlers make it safer and more ergonomic than the C API with negligible overhead (~2%). Use this for any single-threaded C++ server where the handler is fast (under ~5us) or where simplicity matters more than multi-core scaling.

**nanosrv-sharded** -- Use when your handler does real CPU work (>10us per request): computation, serialization, database query building, or any blocking operation. The accept-and-hand-off architecture distributes connections across workers for true parallelism. Not worth the overhead for trivial handlers -- the single-threaded server will be faster in that case.

**nanosrv Python Manager** -- The default choice for Python projects. You get 79% of native C throughput with a Pythonic API. The single-threaded event loop avoids GIL contention entirely. Use this for Python HTTP/WebSocket servers, prototyping, scripting, or any case where Python handler logic is the bottleneck (since the GIL already serializes it, multiple threads won't help).

**nanosrv Python ShardedManager** -- The default choice on a free-threaded interpreter (3.13t and later), where Python handler code runs genuinely in parallel: 3.4x a single-threaded `Manager` for a trivial handler and 4.3x for one doing ~100us of pure Python. On a GIL interpreter it still helps when the per-request cost is mostly outside Python -- event loop, parsing, socket I/O, or a handler that calls into C extensions or GIL-releasing I/O (1.7x for a trivial handler at 2-4 workers) -- but not when the handler is pure Python, which the GIL serializes regardless (0.96x). Rule of thumb: free-threaded, use it; GIL and your handler is mostly Python, use `Manager`.

**Decision flowchart:**

```text
Is it a Python project?
  yes --> Free-threaded interpreter (3.13t+)?
            yes --> nanosrv Python ShardedManager
            no  --> Is the handler mostly pure Python?
                      yes --> nanosrv Python Manager
                      no  --> nanosrv Python ShardedManager (2-4 workers)
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

```text
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

```text
make server-build   # build nanosrv-server and nanosrv-sharded via CMake
make server-run     # build and run nanosrv-server
make server-test    # build and run C++ tests via ctest
make server-clean   # remove CMake build directory
make bench          # run wrk benchmarks (builds everything first)
```

### TLS backend (opt-in)

TLS is off by default. To build the C++ library and servers with TLS, configure
the CMake project with the mbedTLS backend:

```text
cmake -B build/cmake -S projects -DNANOSRV_TLS=mbed
cmake --build build/cmake
```

mbedTLS is not vendored. By default it is fetched at configure time via CMake
`FetchContent`, pinned to the v3.6.6 release commit and cached under the build
tree (so the first configure of the `mbed` backend needs network access; the
default `none` build never fetches). `NANOSRV_MBEDTLS_PROVIDER` controls the
source:

```text
-DNANOSRV_MBEDTLS_PROVIDER=auto     # use an installed mbedTLS if found, else fetch (default)
-DNANOSRV_MBEDTLS_PROVIDER=system   # require an installed mbedTLS (error if absent)
-DNANOSRV_MBEDTLS_PROVIDER=fetch    # always fetch the pinned commit
```

An installed mbedTLS is accepted only in the range `>= 3.6, < 4.0` (the backend
targets the mbedTLS 3.x API; 4.x is an API break and is skipped). With the
backend enabled, the C++ test suite adds end-to-end TLS handshake and mutual-TLS
(client-certificate) tests.

## License

**GPL-2.0-only.** nanosrv is a derivative work of
[Mongoose 7.21](https://github.com/cesanta/mongoose), which is dual-licensed
`GPL-2.0-only or commercial`. GPL-2.0-only has no "or any later version" clause,
so the derived code cannot be redistributed under GPL-3.0, MIT, or any other
terms. See [LICENSE](LICENSE) for the text and
[PROVENANCE.md](PROVENANCE.md) for a file-by-file account of what is inherited
from upstream and what is original.

If you need non-copyleft terms, obtain a commercial Mongoose licence from
<https://mongoose.ws/licensing/> first; it covers the upstream portion only.
