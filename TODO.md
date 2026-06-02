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

- [ ] **DNS transaction-ID de-duplication is a stub** (`projects/nanosrv/dns.cpp:~299`).
  When prior requests exist the new txnid is set to `reqs->txnid + 1` instead of
  scanning for a free id, so concurrent lookups can collide and ids are more
  guessable than the random path implies. Implement "scan existing reqs, repeat
  on collision".

- [ ] **WebSocket RFC 6455 enforcement** (`projects/nanosrv/ws.cpp`). `ws_process`
  does not require client->server frames to be masked (5.1) nor that control
  frames (0x8-0xA) are FIN=1 with payload <= 125 bytes (5.5). Not a memory-safety
  issue (bounds checks hold), but non-conforming peers are accepted. Enforce the
  rules and close with status 1002 on violation.

- [ ] **Header-count overflow silently truncates** (`projects/nanosrv/http.cpp:285`,
  `MG_MAX_HTTP_HEADERS` = 30). Headers past the 30th are dropped rather than
  rejected, which can discard a security-relevant header (a second
  `Content-Length`, an `Authorization`). Reject with 431 when the count is
  exceeded.

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

- [ ] **Python version matrix mismatch** (`pyproject.toml` requires `>=3.10` but
  `.github/workflows/ci-py.yml` still tests Python 3.9). Align the matrix with
  `requires-python`. (`release.yml` already builds cp310+.)

- [ ] **Logging defaults verbose and always compiled in** (`projects/nanosrv/log.cpp`). Default the runtime level to Info/Error and add a compile-time floor that compiles out sub-threshold `MG_DEBUG`/`MG_VERBOSE` calls.

- [ ] **Connect timeout for client-initiated connections** (the timeout work so far covers accepted/server connections only). A hung outbound `connect()` is not
  bounded.

- [ ] **Observability hooks.** No metrics/health surface (connection counts, error rates). Add lightweight counters/callbacks.

## P3 -- Quality, tests & portability

- [ ] **Enable `-Wconversion` / `-Wsign-conversion` / `-Wshadow`** and triage. The code is pointer- and size-arithmetic-heavy; these flags may surface real
  truncation bugs. Fix the genuine ones, cast the benign ones. (Low risk, high
  signal -- a good next pick.)

- [ ] **`static_assert` the `platform.hpp` config matrix** so an inconsistent
  `MG_ARCH`/`MG_ENABLE_*` combination fails at compile time, not late and obscurely.

- [ ] **Targeted unit/e2e tests** beyond the existing fuzz harness:

  - WebSocket framing (0-byte payload, 16-bit and 64-bit extended length, masking, control frames) and an end-to-end upgrade/send/receive test.

  - HTTP edge cases: chunked decoding, multipart boundaries, header-limit handling, 4xx/5xx paths.

  - Custom formatter/string layer (`fmt`/`dtoa`/`str`): pathological floats,
    large-input `match`/`span`.

- [ ] **Per-poll cap on `alloca` event arrays** (`projects/nanosrv/sock.cpp`,
  epoll/kqueue/poll paths). Sizing scales with connection count; chunk it to bound stack use under very high connection counts.

- [ ] **Socket-FD `int` casts assume POSIX** (`wrapfd(..., static_cast<int>(fd))`). False for Windows `SOCKET`; only relevant if Windows support is intended (CI
  builds Windows wheels, so the C++ core's Windows portability is nominally in
  scope but untested at the C++ level).

## P4 -- Cosmetic / nice-to-have

- [ ] **Use `std::bit_cast` (or `std::isinf`/`std::isnan`)** instead of `union`
  type-punning in the float formatter (`projects/nanosrv/fmt.cpp`,
  `xisinf`/`xisnan`) for strict conformance.

- [ ] **Clarify cryptic hex offsets** (`projects/nanosrv/str.cpp`, `c - '7'` /
  `c - 'W'`); the explicit `c - 'A' + 10` form prevents a future edit error.

- [ ] **Docs**: add a `SECURITY.md` and an explicit "not for hostile networks yet" note; document the callback object-lifetime contract (handles handed to a
  handler are valid only during the call).

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
