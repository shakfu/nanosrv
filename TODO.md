# nanosrv -- TODO

Open items distilled from the code review, in priority order. Items already
completed (all High findings H1-H5; M1 body cap; M2 sharded wakeup; the stale
header tree, README paths, and prompt shutdown; CI sanitizer/TSan/fuzz harness;
connection idle + request-receive timeouts) are intentionally omitted.

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
