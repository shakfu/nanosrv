# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

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
- Benchmark scripts (`bench.sh`, `bench_pynanosrv_server.py`, `bench_pynanosrv_sharded.py`) for wrk-based performance comparison
- GitHub Actions CI workflows for Python tests and build/publish
- MIT license
