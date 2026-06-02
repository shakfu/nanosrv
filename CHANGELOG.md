# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [0.1.1]

### Added

- Connection idle timeout (`set_idle_timeout(ms)`) on `Manager` and `ShardedManager` (and the Python bindings), closing accepted connections with no read/write activity for the configured period

- Request-receive deadline (`set_request_timeout(ms)`) closing accepted connections that buffer an incomplete request past the deadline (slow-dribble slowloris defense that an idle timeout alone misses)

- Request body cap (`set_max_body_size(bytes)`) rejecting oversized request bodies with HTTP 413, before the body is buffered when the advertised `Content-Length` already exceeds the cap

- Max-connection cap (`set_max_connections(n)`) on `Manager` and `ShardedManager` (and the Python bindings), closing freshly accepted sockets once the live accepted-connection count reaches the cap instead of adopting them; `num_connections` exposes the current count. The sharded cap is global across all workers, enforced at the single acceptor thread (single producer increments, workers decrement on close, so it cannot over-admit)

- Send-buffer high-water mark (`set_max_send_buffer(bytes)`) closing an accepted connection whose unsent outbound backlog exceeds the cap, dropping a slow/stalled reader that would otherwise tie up unbounded send buffering

- Graceful shutdown: `ShardedManager::drain(timeout_ms)` stops accepting new connections, lets workers finish flushing in-flight responses, and then returns from `run()`, closing anything still open after the timeout (0 = wait indefinitely); `Manager::start_drain()` provides the same primitive for the single-threaded loop (close listeners, drain accepted connections, poll until `num_connections()` is 0). Exposed in the Python bindings and wired into both executables as a `--drain-timeout` flag (default 5000 ms; 0 = abrupt stop) on SIGINT/SIGTERM

- AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer build options (`NANOSRV_SANITIZE`, `NANOSRV_TSAN`) and a `cpp.yml` CI workflow that builds the C++ core and runs the unit suite under sanitizers

- libFuzzer harnesses for the HTTP, WebSocket, and DNS parsers (`tests/fuzz/`) with seed corpora and a CI fuzz smoke job

- C++ tests for the sharded accept-and-hand-off concurrency, sharded teardown, connection timeouts, the body cap, the max-connection cap (single and global-sharded), the send-buffer watermark, and graceful drain (in-flight completion and the drain deadline); the IPv4/IPv6 ACL matcher (allow/deny, CIDR, cross-family, malformed), and the TLS listener failing closed; Python regression tests for callback-object lifetime, the new limits, drain, and the TLS-URL rejection

- `tls_available()` predicate (C++ and Python) reporting whether the build has a TLS backend (the default build has none). The Python `http_listen`/`http_listen_event` now raise `RuntimeError` immediately for `https://`/`wss://` URLs instead of failing later at the no-op handshake

- Opt-in mbedTLS TLS backend (`-DNANOSRV_TLS=mbed`), implementing the TLS hooks (`tls_init`/`tls_handshake`/`tls_recv`/`tls_send`/`tls_pending`/`tls_flush`/`tls_ctx_*`) in `tls_mbed.cpp` against the mbedTLS 3.x public API, wired into the existing event-loop buffering (`rtls`, `MG_IO_WAIT` semantics). With the backend enabled `tls_available()` returns true and `https://`/`wss://` listeners and clients work; the default build and the Python wheel still link the no-op stub

- mbedTLS is fetched at configure time via CMake `FetchContent` (pinned to the v3.6.6 release commit) rather than vendored in-repo, with a `find_package` fallback selected by `NANOSRV_MBEDTLS_PROVIDER` (`auto` = use an installed mbedTLS if found else fetch; `system` = require an install; `fetch` = always fetch the pinned commit). An installed mbedTLS is accepted only in `[3.6, 4.0)` -- 4.x is an API break and is skipped (under `auto`) or rejected with a clear error (under `system`)

- C++ integration tests exercising a real TLS handshake end to end over loopback with client-side certificate verification, and mutual TLS (client certificate) with both a positive case and a negative case proving the server enforces the client-cert requirement (no plaintext crosses when the client presents no certificate); both compiled only when a TLS backend is present

- `scripts/ab_bench.py` (and a `make ab-bench` target): a reusable strict A/B benchmark harness that builds a baseline from any git ref in an isolated worktree through the same CMake pipeline as the working tree, runs wrk against both interleaved over N reps, and reports per-rep numbers, means, percentage delta, and a noise-aware no-regression verdict -- replacing the ad-hoc shell A/B runs

- Opt-in `--idle-timeout`, `--request-timeout`, `--max-body`, `--max-connections`, and `--max-send-buffer` flags on `nanosrv-server` and `nanosrv-sharded` (default 0 = disabled), wiring the connection-hardening limits through to the standalone executables; defaults are off so benchmarks are unaffected. A strict A/B benchmark of the full Unreleased tree against the prior release (both binaries built through the same CMake pipeline via `scripts/ab_bench.py`, `wrk -t4 -c100 -d8s`, 3 reps) confirmed no regression: `nanosrv-server` ~199k req/s (-1.3%) and `nanosrv-sharded` ~186k req/s (+0.2%), both within run-to-run variance (the server delta is smaller than the spread of the baseline's own reps). None of the added work -- the connection-counter atomics, the per-poll limit checks, or the drain-mode flag checks -- touches the per-request hot path

### Changed

- `ShardedManager` now hands off accepted connections via an event-driven wakeup instead of a 1 ms polling timer, captures per-listen state by value (nothing heap-allocated to leak), and tears down cleanly via a run-state-guarded destructor; `stop()` wakes the worker and acceptor loops for prompt shutdown

- The `Connection`, `HttpMessage`, and `WsMessage` objects handed to Python handlers are now valid only for the duration of the callback and raise `RuntimeError` if used afterwards, instead of dangling; listener callbacks are held for exactly the listener's lifetime

- README: corrected the C++ server source paths and documented the new connection-hardening options

- Renamed CMake target `mungo-server` (was `nanosrv-c`) to avoid confusion with the nanosrv library family

- Renamed all `pynanosrv` references to `nanosrv` -- the Python package was already named `nanosrv` in pyproject.toml

- All build outputs (executables, static libraries) now go to `./build/` instead of scattered CMake sub-directories

- Benchmark script uses explicit binary path variables and updated labels

### Removed

- Stale duplicate header tree under `projects/nanosrv/include/` that had diverged from the canonical `include/` headers and was not on the build's include path

### Fixed

- Stack-use-after-scope where a handler wrote to a captured local that was destroyed before the `Manager` teardown poll fired (caught by the new sanitizer build)

- Per-listener Python callback reference leak -- the callback is now released when the listener closes instead of leaking for the process lifetime

- Sharded per-listener context (`AcceptCtx`/`AdoptCtx`) leak

- Body-cap rejection no longer lets the connection-close path re-deliver the rejected request to the handler

- IPv6 ACL bypass in `check_ip_acl`: the function returned early for any IPv6 peer, so a restrictive ACL silently failed open for half the address space. It now matches IPv4 and IPv6 uniformly via bitwise prefix comparison over the network-order address bytes, supports `+net/prefix` / `-net/prefix` entries of either family (entries only apply to a peer of their own family), and reports malformed ACLs rather than allowing. (The function is a building block and is still not wired into the accept path -- documented as such.)

- Latent data race in `ShardedManager` (caught by the ThreadSanitizer build): `stop()`/`drain()` could read a worker's wakeup-pipe handle while `run()` was still initializing it on another thread. Cross-thread wakeups are now gated on a release/acquire "pipes ready" flag, so the pipe initialization happens-before any external wake (the wake is skipped until the pipes are up; the loops still react on their next fallback poll)

- `make bench` failed because the bench script referenced a non-existent `nanosrv-c` binary

- `make server-run` pointed to wrong binary path

### Security

- Hardened against connection-exhaustion and slowloris attacks via the idle and request-receive timeouts, the max-connection cap, and the send-buffer watermark (slow-reader defense), and against oversized-body denial of service via the request body cap

- Closed an IPv6 ACL fail-open (`check_ip_acl` now enforces IPv6, not just IPv4), and made TLS unavailability explicit (`tls_available()` plus an upfront error on TLS URLs) so `https://`/`wss://` no longer appear to be accepted only to fail at runtime

- Closed a cleartext-downgrade footgun: a C++ `listen_("https://...")` with no TLS backend would create a listener that served plaintext HTTP on a TLS-intended port (accepted connections had `is_tls` reset to 0). `listen_` now fails closed for TLS URLs when `tls_available()` is false, so this matches the Python-side rejection

- Eliminated a use-after-free exposure of transient C++ request and connection objects to Python handlers (objects stored beyond the callback now raise rather than dereferencing freed memory)

- Real in-process TLS termination is now available as an opt-in mbedTLS backend (previously TLS was only ever a no-op stub), removing the hard requirement for a front-facing TLS proxy in C++ deployments. Server-side certificate auth and mutual TLS are covered by integration tests, including a negative test confirming a missing client certificate is rejected before any application data is exchanged

## [0.1.0]

### Added

- Python bindings for the nanosrv embedded server library via nanobind

- `Manager` class -- single-threaded event loop with GIL-releasing `poll()`

- `ShardedManager` class -- multi-threaded event loop with `run()`/`stop()`

- `Connection` and `ConnectionRef` classes for HTTP replies, WebSocket send, and raw byte I/O

- `HttpMessage` read-only view with `method`, `uri`, `query`, `body`, `status_code`, `header()`, `credentials()`

- `WsMessage` read-only view with `data`, `flags`, `opcode`

- `http_listen()` typed handler and `http_listen_event()` full event handler on `Manager`

- WebSocket support: `ws_upgrade()`, `ws_send_text()`, `ws_send_binary()`, `ws_send()`

- `Url.parse()` for URL parsing (host, port, path, is_ssl)

- `base64_encode()` / `base64_decode()` utility functions

- `url_encode()` / `url_decode()` utility functions

- `json` submodule with path-based extraction: `string()`, `number()`, `integer()`, `boolean()`

- `set_log_level()` / `get_log_level()` for controlling nanosrv log verbosity

- `millis()` for current time in milliseconds

- `Event`, `WsOpcode`, and `LogLevel` enums

- Standalone C++ server executables: `nanosrv-server` (single-threaded) and `nanosrv-sharded` (multi-threaded)

- CLI11 argument parsing for `nanosrv-server` and `nanosrv-sharded` with short flags (`-p`, `-b`, `-t`), validation, and `--help`

- Colored terminal output via rang (bold help text, bold red errors)

- README for each sub-project (`projects/mungo`, `projects/nanosrv`, `projects/nanosrv-exe`, `projects/nanosrv-sharded`)

- Full pytest test suite covering enums, URL parsing, base64, URL encode/decode, JSON, logging, millis, Manager, and HTTP integration

- Build system using scikit-build-core with nanobind

- Makefile with targets for build, test, lint, format, typecheck, QA, wheel, sdist, and publish

- `Makefile.server` for building standalone C++ servers via CMake

- Benchmark scripts (`bench.sh`, `bench_pynanosrv_server.py`, `bench_pynanosrv_sharded.py`) for wrk-based performance comparison of all server implementations

- GitHub Actions CI workflows for Python tests and build/publish

- MIT license
