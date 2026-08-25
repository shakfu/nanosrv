# nanosrv -- TODO

Open items distilled from the code reviews, in priority order. Items already
completed (all High findings H1-H5; M1 body cap; M2 sharded wakeup; the stale
header tree, README paths, and prompt shutdown; CI sanitizer/TSan/fuzz harness;
connection idle + request-receive timeouts) are intentionally omitted.

The 2026-08-24 review (formerly `REVIEW.md`) has been folded into this file:
its findings are the "Code review" sections below, and its strategic argument
is "Direction and specialization". Section numbers like (4.6) refer to that
review and are kept only so older notes and commit messages still resolve.

Priority tiers: **P1** security / user-facing correctness, **P2** robustness &
production-readiness, **P3** quality, tests & portability, **P4** cosmetic /
nice-to-have.

---

## P1 -- Security / correctness footguns

- [x] **IPv6 ACL bypass** (`projects/nanosrv/util.cpp`, `check_ip_acl`).
  Rewrote `check_ip_acl` to match IPv4 and IPv6 uniformly via a bitwise prefix
  compare over the network-order address bytes; entries (`+net/prefix` /
  `-net/prefix`) apply only to peers of their own family, and malformed ACLs
  return an error instead of failing open. Declared in `net.hpp` and covered by
  C++ tests (allow/deny, CIDR, cross-family, malformed); clean under ASAN/UBSan.
  NOTE: still a building block -- not wired into the accept path; documented in
  the README.

- [x] **TLS is a non-functional stub with no signal** (`projects/nanosrv/tls_dummy.cpp`,
  `include/nanosrv/tls.hpp`). Added a `tls_available()` predicate (false in the
  default build, exposed in the Python module). The Python `http_listen`/
  `http_listen_event` now raise `RuntimeError` immediately for `https://`/`wss://`
  URLs instead of failing later at the handshake. README documents the
  limitation. Covered by Python tests.

- [x] **TLS listener silently downgrades to cleartext** (`projects/nanosrv/sock.cpp:596-602`).
  An accepted connection inherited `is_tls` from its listener, but because no
  handler calls `tls_init`, `is_tls_hs` stayed 0 and the accept path reset
  `c->is_tls = 0` -- so a C++ `http_listen("https://...")` served plaintext on a
  TLS-intended port. Fixed by failing closed in `listen_` (`net.cpp`): when
  `url_is_ssl(url) && !tls_available()` it logs and returns null, so a TLS URL
  never yields a cleartext listener. Covered by a C++ test (`https://`/`wss://`
  listen returns a null/invalid listener; `http://` still works). This is the
  C++-side counterpart of the Python `tls_available()` guard above.

- [x] **DNS transaction-ID de-duplication is a stub** (`projects/nanosrv/dns.cpp`).
  `sendnsreq` now picks a random txnid and rescans the whole outstanding-request
  list on every collision, so concurrent lookups never share an id (bounded at
  65536 attempts). Replaces the old `reqs->txnid + 1` stub.

- [x] **WebSocket RFC 6455 enforcement** (`projects/nanosrv/ws.cpp`). `ws_cb` now
  rejects unmasked client->server frames (5.1) and control frames (0x8-0xA) that
  are fragmented or carry >125 bytes (5.5), closing with status 1002 via a
  `ws_close_with_code` helper. Covered by a C++ e2e echo test (all payload-length
  encodings + ping/pong) and a raw-socket unmasked-frame-rejection test.

- [x] **Header-count overflow silently truncates** (`projects/nanosrv/http.cpp`,
  `MG_MAX_HTTP_HEADERS` = 30). `http_parse_headers` now detects overflow and
  `http_parse` returns a distinct `MG_HTTP_TOO_MANY_HEADERS` sentinel; `http_cb`
  answers 431 instead of proceeding with a truncated header set. Covered by a
  parser-level test.

## P2 -- Robustness & production-readiness

- [x] **Per-connection / max-connection limits.** Added `set_max_connections(n)`
  (single-`Manager` cap enforced in `accept_conn`; global-across-workers cap on
  `ShardedManager` enforced at the acceptor via a shared atomic) and
  `set_max_send_buffer(bytes)`, a close-over-watermark backpressure policy that
  drops a slow reader once its unsent outbound backlog exceeds the cap. Exposed
  in the Python bindings and as opt-in CLI flags; covered by C++ and Python tests
  and verified clean under TSAN and ASAN/UBSan.

- [x] **Graceful shutdown / drain.** Added `ShardedManager::drain(timeout_ms)`
  (stop accepting at the acceptor, let workers finish flushing in-flight
  responses via per-connection `is_draining`, then return from `run()`; force
  close anything still open past the timeout) and `Manager::start_drain()` for
  the single-threaded loop. Exposed in the Python bindings and as a
  `--drain-timeout` flag on both executables. Covered by C++ and Python tests
  and verified clean under TSAN and ASAN/UBSan; the TSAN run also surfaced and
  fixed a pre-existing wakeup-pipe race in `stop()`/`drain()`.

- [x] **Python version matrix mismatch** (`.github/workflows/ci-py.yml`). Matrix
  now tests `["3.10", "3.12"]`, aligned with `requires-python >=3.10`.

- [x] **Logging defaults verbose and always compiled in** (`projects/nanosrv/log.cpp`,
  `platform.hpp`). Default runtime level is now `MG_LL_INFO`, and a compile-time
  floor `MG_LOG_LEVEL_MAX` (Info in `NDEBUG`/release, Verbose in debug;
  overridable) dead-codes sub-threshold `MG_DEBUG`/`MG_VERBOSE` sites so they
  cannot be re-enabled at runtime.

- [x] **Connect timeout for client-initiated connections.** Added
  `connect_timeout_ms` (default `MG_DEFAULT_CONNECT_TIMEOUT_MS` = 30 s) armed in
  the poll loop for client connections still resolving/connecting; exposed as
  `set_connect_timeout()` in C++ and Python. Covered by a C++ test against a
  black-holed TEST-NET address.

- [x] **Observability hooks.** Added atomic cumulative counters on `Mgr`
  (accepted/closed/errors/bytes read/written), a `Metrics` snapshot exposed via
  `Manager::metrics()` and `ShardedManager::metrics()` (aggregated across
  workers, TSan-clean), and the Python `Manager.metrics` / `ShardedManager.metrics`
  properties. Covered by C++ and Python tests.

## P3 -- Quality, tests & portability

- [x] **Enable `-Wconversion` / `-Wsign-conversion` / `-Wshadow`** and triage.
  Enabled on the `nanosrv` target (non-MSVC). The pervasive explicit casts left
  the library clean apart from two benign shadows (`net.cpp`, since renamed).
  Verified clean locally on the kqueue path; the Linux epoll/io_uring legs are
  validated by CI.

- [x] **`static_assert` the `platform.hpp` config matrix.** Poller selectors are
  normalized to 0/1, then static_asserts enforce a valid `MG_ARCH`, mutual
  exclusion of the readiness backends, no POSIX poller under Win32, and an
  enabled socket backend. Verified to fire on a bad combination.

- [x] **Targeted unit/e2e tests** beyond the fuzz harness:
  - WebSocket end-to-end upgrade/echo across all payload-length encodings
    (0-byte, 7-bit, 16-bit, 64-bit), masking, and ping/pong; plus a raw-socket
    unmasked-frame-rejection test.
  - HTTP edge cases: chunked-request decoding (e2e), multipart boundaries,
    header-limit -> 431 handling.
  - Formatter/string layer: pathological floats (inf/-inf/nan) and large-input
    `match`/`span`.

- [x] **Per-poll cap on `alloca` event arrays** (`projects/nanosrv/sock.cpp`).
  epoll/kqueue now harvest into a fixed-size `MG_IO_POLL_BATCH` (256) stack array
  (level-triggered, so remaining events return next poll); the poll() path falls
  back to a heap buffer past that size. Bounds stack use to a constant.

- [x] **Socket-FD `int` casts assume POSIX.** `wrapfd` now takes `MG_SOCKET_TYPE`
  instead of `int`, so a full-width Windows `SOCKET` handle is stored verbatim
  (via `S2PTR`) rather than truncated; call sites drop the `int` casts and
  `sharded.cpp` uses `closesocket()` for un-adopted FDs.

## P4 -- Cosmetic / nice-to-have

- [x] **Use `std::bit_cast`** instead of `union` type-punning in the float
  formatter (`projects/nanosrv/fmt.cpp`, `xisinf`/`xisnan`).

- [x] **Clarify cryptic hex offsets** (`projects/nanosrv/str.cpp`): `c - '7'` /
  `c - 'W'` are now the explicit `c - 'A' + 10` / `c - 'a' + 10` form.

- [x] **Docs**: added `SECURITY.md` (threat-model posture, "not for hostile
  networks yet", hardening knobs, reporting) and documented the callback
  object-lifetime contract in both `SECURITY.md` and the README API reference.

---

## Code review 2026-08-24 -- Phase 0 (done)

The prerequisites the review identified as blocking everything else, all closed:

- [x] **Licensing (4.1).** Relicensed GPL-2.0-only to match upstream Mongoose
  (`GPL-2.0-only or commercial`, no "or later" clause), replacing the GPL-3.0
  `LICENSE` and the `license = "MIT"` package metadata that PyPI was
  publishing. Added `PROVENANCE.md` with a file-by-file account of what
  descends from upstream, and removed the "rewritten from scratch" claim in
  `projects/nanosrv/README.md`, which a line-level comparison against the
  vendored upstream contradicts.

- [x] **Binary payloads (4.2).** `HttpMessage.body` and `WsMessage.data` return
  `bytes` (with `.text` for a strict UTF-8 decode); every send path accepts
  bytes-like or str. Metadata stays `str` but decodes with `surrogateescape`,
  so a malformed request cannot raise inside a handler. Uncovered and fixed a
  latent truncation on the way: `http_reply()` formats its body through
  `xprintf`'s `%s`, whose `scpy()` stops at the first NUL even with an explicit
  precision -- hence the new length-counted `http_reply_bytes()`.

- [x] **ShardedManager parity (4.3).** Added `http_listen_event()` (WebSocket
  and every other event were unreachable on the sharded path), a routed
  `wakeup()`, and `set_connect_timeout()`. Required making connection ids
  unique across workers -- each worker numbered from 1 independently, so ids
  collided -- via a per-worker id stride, which also makes `id % N` the routing
  key. Fixed a second listener silently replacing the first listener's handler.

- [x] **Streaming (4.4).** `start_chunked()`, `write_chunk()`, `start_sse()`,
  `sse_send()` and `send_queue_len` exposed to Python, plus
  `http_start_chunked()` / `http_start_sse()` in C++. Added `Connection.drain()`
  after finding that `close()` discards unflushed output.

## Code review 2026-08-24 -- findings

- [x] **Free-threading (4.5).** `nanobind_add_module()` now passes
  `FREE_THREADED`, so the module keeps the GIL disabled on 3.13t; `cp313t`
  wheels are in the release matrix and CI runs the suite on 3.13t. Measured
  3.4x over the best GIL configuration with a ~100us pure-Python handler (3.5x
  at ~400us), against a GIL build that gets slower with each added worker;
  `scripts/bench_freethreading.py` reproduces it. Stress-tested for races on
  3.13t (8 workers, HTTP + WebSocket + cross-thread wakeups, payload-verified,
  zero errors). Two findings fell out, below.

- [x] **Type stubs (4.6).** `scripts/gen_stubs.py` generates
  `src/nanosrv/_core/*.pyi` (a stub *package*, because `_core` has a `json`
  submodule that a single `.pyi` cannot resolve) and validates that they parse.
  Payload parameters carry `nb::sig` annotations so they type as
  `bytes | bytearray | memoryview | str` rather than `object`. The mypy
  `ignore_missing_imports` override for `nanosrv._core` is gone.

- [x] **`ShardedManager(1)` pathology on free-threaded CPython -- fixed, and it
  was bigger than it looked.** The cause was as suspected: a worker is not a
  Python thread, so each callback's `PyGILState_Ensure`/`Release` created and
  destroyed a `PyThreadState`. `ShardedManager::set_worker_hooks()` now lets the
  binding register each worker thread once, holding the GILState counter at one
  for the thread's lifetime while leaving the thread detached between callbacks
  (an attached thread parked in `epoll_wait` would stall a free-threaded
  stop-the-world GC). This was not a free-threading quirk but the dominant
  per-request cost on the sharded Python path in every release: trivial handler,
  best worker count, 130K -> 350K req/s under the GIL and 126K -> 648K on 3.13t.
  Covered by a C++ test (hooks fire once per worker, on distinct threads) and a
  Python run/stop-cycle test.

- [x] **nanobind leak warnings at interpreter exit (4.6).** Off by default;
  `NANOSRV_LEAK_WARNINGS=1` restores them for binding development. Covered by a
  subprocess test asserting a quiet exit (the opt-in half skips on
  free-threaded builds, where nanobind reports nothing either way).

- [x] **DNS hardcoded to 8.8.8.8 (4.6).** `/etc/resolv.conf` is now read at
  startup for the first nameserver of each family, with the old constants as a
  fallback. `parse_resolv_conf()` takes contents rather than a path so the
  comment, indentation, family-selection, IPv6-zone and malformed-directive
  cases are all tested without depending on the host.

- [ ] **`MG_MAX_RECV_SIZE` is a compile-time 3 MB cap (4.6).** Fine for
  embedded; fatal for uploads, multi-image payloads, or bulk ingest -- and
  because it is a compile-time constant rather than a knob, a wheel user cannot
  raise it at all. Make it a per-manager runtime setting (alongside
  `set_max_body_size`) or justify the number explicitly in the docs. Whichever
  specialization is pursued, this needs an answer.

- [x] **io_uring auto-detection (4.6).** Now opt-in via
  `-DMG_ENABLE_IO_URING=1`, and that flag actually works: it previously fell
  through to the epoll branch, enabling both backends and tripping the
  mutual-exclusion static_assert, so the documented way to select io_uring
  could never have compiled. Requesting it without liburing now fails with a
  clear `#error`. Still poll-mode only, so it buys nothing over epoll -- noted
  in the README.

- [x] **Benchmarks are macOS-only and not reproducible (4.6).** `make bench`
  now falls back to a bundled C load generator (`scripts/loadgen.c`, with p50/p99
  by reservoir sampling) when `wrk` is absent, so it no longer depends on an
  unlisted external tool. Two methodology defects fixed along the way: the
  sharded servers replied with a constant string while the others formatted a
  body (so the table compared different workloads), and all build trees shared
  one output directory (so a sanitizer build could silently leave an
  instrumented binary where the benchmark looks for a release one). Re-measured
  on Linux, which inverts the macOS conclusions -- sharding is 4.07x mongoose
  for a trivial handler there, not 11% slower. The README carries both results
  and says to measure per platform.

- [ ] **Re-run the benchmarks on macOS with the corrected handlers.** The
  published macOS numbers were taken when the two sharded servers replied with
  a constant string while the other four formatted a body, so that table
  compared different workloads and its "sharding is not worth it below ~10us of
  handler work" conclusion is unproven. The Linux run disagrees sharply (4.07x
  rather than 11% slower), and only a macOS re-run can say how much of that gap
  is the platform and how much was the unequal handler.

- [ ] **Try per-worker `SO_REUSEPORT` listeners on Linux.** `ShardedManager`
  uses accept-and-hand-off because macOS does not load-balance across
  `SO_REUSEPORT` listeners; Linux does. A per-worker listener there would drop
  the mutex, the queue and the FD re-registration from the accept path
  entirely. Cheap to try, and it only has to beat an already-strong 4.07x.

---

## Direction and specialization

From the 2026-08-24 review, sections 5-7. Kept because the reasoning, not the
conclusion, is what makes these decidable later.

### Why a generic positioning loses

- **The C++ core vs upstream Mongoose.** nanosrv is a subset of Mongoose with a
  nicer C++ skin, derived from Mongoose, distributed under Mongoose's copyleft,
  and (per its own benchmark) within a couple of percent of it single-threaded.
  Its added value is typed callbacks, RAII and sharding -- a real but small
  delta against a battle-tested, commercially supported library with MQTT, TLS,
  multipart, SSI and OTA.
- **The Python binding vs the Python server ecosystem.** No ASGI or WSGI, no
  routing, no TLS in the shipped wheel, no HTTP/2, no static files. uvicorn,
  granian, socketify and Robyn each dominate it on features. Two structural
  advantages survive: it imposes no event loop on your process, and it releases
  the GIL.
- **The comparison lab.** No incumbent, genuinely uncommon -- and no users,
  only readers.

Those two surviving advantages are what a specialization should be built on.

**Counter-framing, still worth holding onto.** It is not obvious that
specialization is the binding constraint. The project has no users, so any
niche chosen now is a guess; what it may actually need is a defensible licence
(done), a Python API that can move bytes (done), and **one real deployment**
(still missing). Treat the proposals below as "where to point once the API
works", not as a reason to defer correctness work.

### 6.1 Free-threaded, non-asyncio Python server -- ADOPTED

The primary identity, and the falsification test passed. `ShardedManager` is a
thread-per-core design, which is exactly the shape free-threading rewards and
that asyncio-based servers cannot become. Measured 3.6x the best GIL
configuration on a pure-Python handler and 3.9x a single-threaded `Manager` on
a trivial one. Shipping: `FREE_THREADED` module, `cp313t` wheels, a 3.13t CI
leg, `scripts/bench_freethreading.py`. The remaining work is ordinary
follow-through -- 3.14t wheels, and keeping the stress coverage honest as the
binding grows.

### 6.2 Streaming inference front-end -- open

An OpenAI-compatible HTTP/SSE front-end for local model servers: the network
edge a C++ or Python inference process embeds, rather than putting FastAPI plus
uvicorn in front of it. This is the project's own stated origin --
`projects/mungo/mungo.h` records that the subset was "extracted for use as an
HTTP server suitable for OpenAI-compatible API endpoints". The technical fit is
exact: token generation is a long-lived streaming response whose handler blocks
in native code that releases the GIL, which is where `ShardedManager` wins and
asyncio servers are weakest. The primitives now exist (chunked writes, SSE
helpers, `send_queue_len` for backpressure, metrics for tokens/sec).

Build: a thin `nanosrv.openai` module -- `/v1/chat/completions` with
`stream: true`, `/v1/models`, `[DONE]` framing, cancel-on-disconnect propagated
to the generator, a documented backpressure callback. Needs the
`MG_MAX_RECV_SIZE` item above; multimodal payloads exceed 3 MB.

Risk: crowded above the transport layer (vLLM, llama.cpp's server, TGI,
LiteLLM all ship OpenAI-compatible endpoints). The defensible pitch is narrow
-- *embeddable* transport for people writing their own runtime, not another
gateway -- and GPL-2.0-only is a real obstacle for that audience.

Falsify cheaply: drive a toy generator through `http_write_chunk` and measure
sustained SSE with a slow reader. If backpressure and cancel-on-disconnect are
not clean at 1K concurrent streams, the story does not hold.

### 6.3 In-process sidecar / control plane -- open, next up

"The HTTP endpoint you embed in a process that already has a main loop":
health, metrics, config and live telemetry for long-running Python and C++
jobs, with no framework, no asyncio, and no second process. This is where the
two structural advantages are decisive rather than marginal -- a training loop,
a data pipeline, a game loop or a native daemon cannot host asyncio and will
not add a FastAPI dependency tree for `/healthz`. `Manager.poll(0)` drops into
an existing loop; the sharded runner drops into a thread.

Build: a `nanosrv.sidecar` module -- one-call `serve_in_background(port)`, a
`@route` decorator over the existing `mg_match` matcher (`projects/nanosrv/str.cpp`),
Prometheus text rendering of `Metrics` plus user gauges, default `/healthz` and
`/debug` endpoints, and a WebSocket topic broadcaster (the sharded `wakeup` it
needs now exists).

Risk: low technical risk, low differentiation ceiling -- someone can approximate
it with a thread and `http.server`. The pitch has to be performance and
non-intrusiveness, and it needs one reference deployment to be credible. It is
the cheapest path to "someone else uses this".

### 6.4 Contrarian: ship the parser, not the server -- open

Extract the HTTP/1.1 and RFC 6455 parsers as an I/O-free, fuzz-hardened C++23
library (a `picohttpparser`/`llhttp` equivalent that also does WebSocket
framing) and let the server demonstrate it. The most credible assets here are
the parser correctness work and the harness that proves it, and a parser is the
piece people actually reuse: everyone writing a server already has an event
loop they like. No threading model to argue about, no TLS gap, no GIL.

Cost: smallest audience, and it abandons the sharded work and the binding as
products. It also collides hardest with the licence -- a parser is a component
others link into their code, which is precisely what GPL-2.0-only makes
untenable. Pursue only alongside a clean-room or commercial resolution.

### 6.5 Keep regardless: the comparison lab

The multi-implementation benchmark, `ab_bench.py` and the HTML report let the
project make performance claims that are *checkable*, and they are a legitimate
teaching artifact about binding overhead and event-loop architecture. Do not cut
them for focus. (The reproducibility half of this is now done: no `wrk`
dependency, Linux numbers published, handlers equalized.)

### 6.6 The alternative framing worth deciding deliberately

**Stop forking Mongoose; become the C++ and Python layer on top of it.** Delete
the duplicated core and build `Manager` / `ShardedManager` / the nanobind layer
against upstream `mongoose.c`. Lost: control over the core. Gained: TLS that
works, MQTT and multipart for free, upstream security fixes without a merge, a
licensing story that is simply Mongoose's, and a maintenance burden that drops
by most of the repo. The typed-callback and sharding work -- the parts that are
actually original -- survive intact and become the whole product.

Counter-argument: the fork exists to be small and auditable (5.5K vs 33K
lines), which matters for embedded targets and for the fuzz story. A real
trade, and the answer depends on the specialization: 6.4 needs the fork, 6.1
and 6.3 do not care, 6.2 would probably prefer upstream's TLS. Worth deciding
deliberately rather than by inertia.

## Open questions -- these change the plan

Cannot be resolved from the source; each materially changes the ranking above.

1. **Is a commercial Mongoose licence obtainable and affordable?** If yes, 6.4
   becomes viable and 6.2's audience opens up considerably.

2. **Who is the intended user?** The repo reads as a portfolio and research
   artifact. If that is the goal, 6.5 plus 6.1 is the right answer and the
   licensing question matters much less than it otherwise would.

3. **Was the OpenAI-endpoint note in `projects/mungo/mungo.h` the original
   motivation, or a leftover?** If it is still live, 6.2 should be reconsidered
   ahead of 6.3.

4. **Is Windows a real target?** It constrains the free-threading and
   `SO_REUSEPORT` work and roughly doubles the cost of 6.1.

---

## Investigated and rejected -- do NOT re-litigate

Preserved from the review so these are not re-flagged later. Each was checked
against the source and found to be a non-issue:

- **Unbounded recv buffer / OOM** -- `MG_MAX_RECV_SIZE` = 3 MB caps it
  (`platform.hpp:202`).

- **`Content-Length` integer-overflow smuggling** -- `to_size_t` (`http.cpp:33-38`) rejects overflow before accumulating; it wraps nothing. (The residual "no sane
  cap" concern was M1, now resolved.)

- **WebSocket 64-bit length truncation -> memory corruption** -- `ws_process` caps `data_len` at `MG_WS_MAX_DATA_LEN` (1 GB) and checks `header_len + data_len > len` (`ws.cpp:87-90`); a bad length is a parse failure, not an over-read.

- **WebSocket mask pointer underrun (`p - mask_len`)** -- `header_len` always
  includes `mask_len`, so the pointer stays in-buffer.

- **Dangling `&worker` capture in `ShardedManager::run()`** -- per
  `[expr.prim.lambda.capture]/12`, a reference captured by reference refers to its referent; `workers_` is stable during `run()`. Not a bug (the code has since been rewritten anyway).

- **Data race on `running_`** -- it is `std::atomic<bool>`; default seq_cst is
  correct.
