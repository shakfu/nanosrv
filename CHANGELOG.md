# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- Connection idle timeout (`set_idle_timeout(ms)`) on `Manager` and `ShardedManager` (and the Python bindings), closing accepted connections with no read/write activity for the configured period
- Request-receive deadline (`set_request_timeout(ms)`) closing accepted connections that buffer an incomplete request past the deadline (slow-dribble slowloris defense that an idle timeout alone misses)
- Request body cap (`set_max_body_size(bytes)`) rejecting oversized request bodies with HTTP 413, before the body is buffered when the advertised `Content-Length` already exceeds the cap
- AddressSanitizer/UndefinedBehaviorSanitizer and ThreadSanitizer build options (`NANOSRV_SANITIZE`, `NANOSRV_TSAN`) and a `cpp.yml` CI workflow that builds the C++ core and runs the unit suite under sanitizers
- libFuzzer harnesses for the HTTP, WebSocket, and DNS parsers (`tests/fuzz/`) with seed corpora and a CI fuzz smoke job
- C++ tests for the sharded accept-and-hand-off concurrency, sharded teardown, connection timeouts, and the body cap; Python regression tests for callback-object lifetime and the new limits
- `TODO.md` tracking prioritized remaining work
- CLI11 argument parsing for `nanosrv-server` and `nanosrv-sharded` with short flags (`-p`, `-b`, `-t`), validation, and `--help`
- Colored terminal output via rang (bold help text, bold red errors)
- README for each sub-project (`projects/mungo`, `projects/nanosrv`, `projects/nanosrv-exe`, `projects/nanosrv-sharded`)

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
- `make bench` failed because the bench script referenced a non-existent `nanosrv-c` binary
- `make server-run` pointed to wrong binary path

### Security

- Hardened against connection-exhaustion and slowloris attacks via the idle and request-receive timeouts, and against oversized-body denial of service via the request body cap
- Eliminated a use-after-free exposure of transient C++ request and connection objects to Python handlers (objects stored beyond the callback now raise rather than dereferencing freed memory)

## [0.1.0] - 2026-04-07

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
- Full pytest test suite covering enums, URL parsing, base64, URL encode/decode, JSON, logging, millis, Manager, and HTTP integration
- Build system using scikit-build-core with nanobind
- Makefile with targets for build, test, lint, format, typecheck, QA, wheel, sdist, and publish
- `Makefile.server` for building standalone C++ servers via CMake
- Benchmark scripts (`bench.sh`, `bench_pynanosrv_server.py`, `bench_pynanosrv_sharded.py`) for wrk-based performance comparison of all server implementations
- GitHub Actions CI workflows for Python tests and build/publish
- MIT license
