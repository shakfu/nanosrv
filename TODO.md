# TODO

Findings from the 2026-10-04 review. Tags give the evidence behind each item:

- `[repro]`: reproduced against a scratch build.
- `[read]`: from reading the code only; not reproduced.
- `[upstream]`: also present in Mongoose 7.21.

## Critical

- [ ] **HTTP request smuggling via POST/PUT without Content-Length.** `projects/nanosrv/http.cpp:714` adds `n` to `ofs`, and `:783` adds `n + body.len` again. The first `n` bytes of the next pipelined request are skipped, so a body can carry a hidden request. Example: `POST /a` (no CL), then `GET /pub` with a body containing `GET /admin` -> server serves `/admin`. Fix: delete `ofs += n` at `:714`. The 411 in that branch is also unreachable, because `http_parse` sets `body.len = 0` for requests. `[repro]` `[upstream]`

- [ ] **One spoofed datagram redirects all DNS queries.** `sock.cpp:504-516`, `:331-333`. The resolver UDP socket is not `connect()`ed. `recv_raw` copies every sender into `c->rem`, and later queries `sendto` that address. An attacker can then read the txnid and answer every lookup. Fix: `connect()` the UDP socket, or drop datagrams whose source is not the configured resolver. `[repro]` `[upstream]`

## High

- [ ] **Idle timeout closes connections that just received data.** `sock.cpp:1009`, `:125`, `:1048-1050`. `now` is read once per poll; `iolog` then sets `last_active = millis()`, which can be later. `now - last_active` wraps to ~2^64 and exceeds any timeout. The buffered reply is discarded. 60 of 80 concurrent requests failed with a 2 ms handler. `[repro]`

- [ ] **A Python handler exception permanently disables a sharded worker.** `src/nanosrv/_core.cpp:841-843`, `:881-883` (same pattern at `:705-707`, `:750-752`). `e.restore()` leaves the error set on a long-lived thread state. Every later callback on that worker fails silently; with `ShardedManager(1)` all later requests hang. On `Manager`, the next `poll()` raises `SystemError`. The client gets no response and the connection stays open. Fix: `e.discard_as_unraisable()`, then reply 500 or close. `[repro]`

- [ ] **`ConnectionRef` is unchecked: null deref and stale aliasing.** `_core.cpp:271-278`, `:629-642`; `include/nanosrv/net.hpp:283-296`. `Manager.http_listen` on a busy port returns a falsy ref instead of raising; `ref.close()` segfaults the interpreter. After the listener closes, the ref points at a pooled slot, so it acts on whatever connection reuses it (id changed 2 -> 3). `[repro]`

- [ ] **`ShardedManager.http_listen` after `run()` hangs every connection.** `projects/nanosrv/sharded.cpp:171-173`, `:197`, `:257`. Workers copy `queues_` when `run()` starts, so a later first listen leaves them with `nullptr` queues. The acceptor's connection list is also mutated from the caller's thread while the acceptor polls it (data race). Fix: create queues eagerly, or reject listen after run. `[repro]`

- [ ] **Transfer-Encoding + Content-Length accepted, keep-alive kept.** `http.cpp:690-698`. Chunked wins silently. RFC 9112 6.3 requires closing the connection after such a request. A CL-preferring proxy desyncs. Fix: reply 400 and close. `[repro]` `[upstream]`

- [ ] **HTTP/1.0 Transfer-Encoding guard is dead code.** `http.cpp:687` tests `proto.len > 8`; `"HTTP/1.0"` is 8 bytes. Chunked framing is honoured on 1.0 and keep-alive continues. RFC 9112 6.1 requires treating it as faulty framing. Fix: `>= 8` and close. `[repro]` `[upstream]`

- [ ] **TLS send reports unencrypted bytes as sent.** `tls_mbed.cpp:302-308`. On `MBEDTLS_ERR_SSL_WANT_WRITE`, `tls_send` returns `len`, but mbedTLS only took one record (16 KB). `iolog` deletes the rest from `c->send`. Responses over 16 KB to a slow client are truncated. `[read]`

- [ ] **io_uring backend (opt-in) is unsafe.** `sock.cpp:720-772`. `[read]`
  - Each `iotest` adds a new `POLL_ADD` per connection; old ones are never cancelled.
  - `user_data = c` is never revoked, so a completion after `close_conn` writes into freed or pooled memory.
  - Pending polls keep the file open, so `close()` sends no FIN.
  - A full SQ `break`s the loop, so connections past the newest 256 are never polled.
  - `goto poll_fallback` with a null `mgr->uring` spins without doing I/O.

## Medium

- [ ] **Chunk-size line split across reads kills the request.** `http.cpp:615-624` (`skip_chunk`). An incomplete size line (`fff`, `100\r`) returns -1 instead of 0, so the connection closes. Any TCP segment boundary inside a size line triggers it. `len < i + 1` also lets `buf[i + 1]` read 1 byte past the data. Fix: return 0 when `i + 2 > len`. `[repro]` `[upstream]`

- [ ] **411 reply repeats every poll.** `http.cpp:639-641`, `:715-728`. A request line starting `HTTP/` is parsed as a response with `body.len = ~0`. The 411 path neither drains nor discards, so the POLL re-parse (#2796) appends another 411 each tick. 16 copies in 0.4 s. `MG_EV_HTTP_HDRS` also re-fires each poll for any partially buffered body. Fix: discard and drain after 411. `[repro]` `[upstream]`

- [ ] **Truncated body delivered as complete on close.** `http.cpp:681-686`. On `MG_EV_CLOSE`, `body.len` is recomputed from buffered bytes even when Content-Length was declared. `CL: 100` + 3 bytes + `SHUT_WR` -> handler sees a 3-byte body. Fix: only recompute when no CL was declared. `[repro]` `[upstream]`

- [ ] **Buffered TLS tail discarded on drain/close.** `tls_mbed.cpp:302-308` with `sock.cpp:1038-1044`, `:141-143`. `send.len` can reach 0 while mbedTLS still holds ciphertext. The drain check ignores `is_tls_throttled`, and `MG_EPOLL_MOD(c, 0)` drops EPOLLOUT. Up to 16 KB of a reply is lost. `[read]`

- [ ] **TLS fail-closed holds only for the stub backend.** `net.cpp:271`, `sock.cpp:606-607`, `tls_mbed.cpp:202-204`. With `-DNANOSRV_TLS=mbed`, an `https://` listener whose handler does not call `tls_init` on accept serves cleartext. A client `TlsOpts` with empty `ca` silently uses `VERIFY_NONE`. Either fix or document in SECURITY.md. `[read]`

- [ ] **`SO_REUSEPORT` on every listener.** `sock.cpp:289-294`. Not in upstream. Binding a port already in use succeeds instead of `EADDRINUSE`; a second process under the same UID silently takes a share of connections. Sharding does not use it. `[repro]`

- [ ] **kqueue and io_uring reject accepted fds >= `FD_SETSIZE`.** `sock.cpp:567-572`. The guard excludes only POLL and EPOLL. On macOS with a raised `ulimit -n`, concurrency caps at ~1000. `[read]`

- [ ] **`start_drain()` drops requests whose reply is deferred.** `sock.cpp:1043-1044`, `net.hpp:428-435`. Draining closes on `send.len == 0` and ignores `is_resp`. A handler using the documented `wakeup` pattern loses its reply. `[repro]`

- [ ] **Wakeup payloads over ~16 KB are truncated; the bool does not mean delivery.** `sock.cpp:938-1000`, `:363`. The UDP socketpair is read once into at most `MG_IO_SIZE`; 20000 and 60000 bytes both arrive as 16376. `wakeup(999999, ...)` and wakeup to a closed connection both return `True`. Contradicts `net.hpp:363`, the Python docstrings, and the CHANGELOG. Fix the behaviour or the docs; document a size limit. `[repro]`

- [ ] **`ShardedManager(1)` delivers spurious `Wakeup` events.** `sharded.cpp:45`, `:52`, `:134`. Internal `WAKE_ID = 1` collides with worker 0's first connection id when N = 1. Every hand-off, `stop()` or `drain()` sends that connection `Event.Wakeup` with `b''`. `[repro]`

- [ ] **Connections stay open after `run()` returns.** `sharded.cpp:318-339`. `run()` joins workers and closes only queued fds; worker connections and the listener stay open until destruction. The port stays bound. Contradicts the `drain()` docstring. `draining_` is never reset, so a second `run()` after `drain()` exits at once. `AdoptCtx` decrements `live_conns_` after it is destroyed (declared after `workers_`). `[repro]`

- [ ] **`stop()` before `run()` is lost.** `sharded.cpp:240`. `run()` unconditionally stores `running_ = true`, so `stop(); run()` blocks forever. `main_sharded.cpp:120-134` hangs on an early SIGINT. `[repro]`

- [ ] **Sharded listen failure is silent.** `sharded.cpp:197`. The `ConnectionRef` from `acceptor_.http_listen` is discarded; the Python binding returns `None`. A busy port only logs `bind: 98`. `[repro]`

- [ ] **Handlers that close over their manager are never collected.** `_core.cpp:39-46`, every `make_callback` capture. Handlers are held as `nb::object` with no `tp_traverse`. `self.m.http_listen(url, self.handle)` leaks the app, the manager and the bound port. `[repro]`

- [ ] **Use-after-return guard races on free-threaded builds.** `_core.cpp:65-106`, `:844`, `:884`. `alive` is a plain `bool`. On 3.13t, another thread can pass `get()` while the handler is returning, then use a connection the worker is flushing or freeing. Safe under the GIL. `[read]`

- [ ] **`%g` drops sign and exponent of negative numbers.** `fmt.cpp:85-94`. The recursive `dtoa` overwrites the `'-'` and NUL-terminates before `addexp`. `snprintf_("%g", -1.5e20)` -> `"1.5"`. `[repro]` `[upstream]`

- [ ] **Stack overflow in `dtoa` for wide `%g` precision.** `fmt.cpp:88`, `addexp` at `:14-24`. `"%.38g"` with `-1.2345678901234567e300` writes past `buf[40]` (ASan). Only reachable with precision >= ~33. `[repro]` `[upstream]`

- [ ] **JSON integer extraction is UB on out-of-range values.** `json.cpp:445`, `:476`. `static_cast<long>(double)` on `{"a":1e300}` via `nanosrv.json.integer`. Returns `LONG_MIN` on x86. Fix: range-check and return none. `[repro]` `[upstream]`

- [ ] **IPv6 zone in resolv.conf breaks DNS; with `use_dns6` it crashes.** `util.cpp` (`parse_resolv_conf`), `net.cpp:130` (`aton6`), `dns.cpp:278-279`. `aton6` accepts only a numeric scope; `nameserver fe80::1%eth0` fails to parse. With `use_dns6 = true`, `dnsc_init` -> `connect` -> `resolve` -> `sendnsreq` recurses until segfault. The `parse_resolv_conf` comment claims zone support. `[repro]`

## Low

- [ ] **`url_encode(std::string_view)` returns `""` for some inputs.** `http.cpp:976-983` with `:830`. The wrapper allocates `3n + 1`; the bound `n + 4 >= len` needs `3n + 2`. `url_encode("a")`, `"  "`, `"/ "` -> `""`. Python `nanosrv.url_encode` binds this overload (`_core.cpp:972`). Port regression. `[repro]`

- [ ] **`http_get_header_var` over-reads and substring-matches.** `http.cpp:885-892`. An unterminated quoted value returns 1 byte past the input. The name match has no boundary check, so cookie `evil_access_token=x` matches `access_token` in `http_creds` (`:863`). `[repro]` `[upstream]`

- [ ] **WebSocket client accepts a malformed handshake.** `ws.cpp:175`. `if (http_parse(...))` treats -1 and -2 (`MG_HTTP_TOO_MANY_HEADERS`) as success; `MG_EV_WS_OPEN` fires with a partly zeroed `hm`. `[read]` `[upstream]`

- [ ] **WebSocket length cap is weaker than SECURITY.md implies.** `ws.cpp:83-90`. Above 1 GiB `ws_process` returns 0 ("need more data") rather than closing with 1009; only `MG_MAX_RECV_SIZE` (3 MB) and `idle_timeout` bound it. `request_timeout` excludes websockets. On 32-bit `size_t`, the cast truncates before the cap check (0x1_0000_0005 -> 5). `[read]`

- [ ] **Missing RFC 6455 checks.** `ws.cpp:218-273`. `[repro]` `[upstream]`
  - No continuation sequencing: `CONT(fin=0,"XY")`, `CONT(fin=1,"Z")` -> message with `flags='X'` and data `"YZ"`.
  - RSV bits are not checked.
  - Close payload length 1 and invalid close codes are not checked; the payload is echoed verbatim (`:248`).
  - Text frames are not UTF-8 validated.
  - On a 1002 `break` the bad frame is not consumed; the next read unmasks it again and queues another Close.

- [ ] **`check_ip_acl` error codes are truthy.** `util.cpp:147-193`. -1/-2 for a malformed ACL pass `if (check_ip_acl(...))`. "Fails closed" in SECURITY.md depends on the caller. `[read]`

- [ ] **IPv4-mapped addresses bypass IPv4 ACL entries.** With `MG_ENABLE_IPV6=1` and a dual-stack listener, IPv4 peers appear as `::ffff:a.b.c.d`; `-10.0.0.0/8,+0.0.0.0/0,+::/0` admits 10.x. IPv6 is off by default. `[read]`

- [ ] **`listen_` failure frees with the wrong deallocator.** `net.cpp:282`. `alloc_conn` uses `::operator new` (or the pool); the error path calls `mem_free` and skips `~Connection`. ASan `alloc-dealloc-mismatch`. `[repro]`

- [ ] **Lenient base64 decode.** `base64.cpp:93-108` accepts mid-string padding: `"QQ==QUJD"` -> `"AABC"`. `[repro]` `[upstream]`

- [ ] **`lld(INT64_MIN)` negates a signed value.** `fmt.cpp:140`. UBSan. `[repro]` `[upstream]`

- [ ] **JSON accepts a bare `-` as a number.** `json.cpp`. `[repro]`

- [ ] **`path_is_sane` reads `buf[0..2]` without a length check.** `util.cpp:195`. Currently unused; delete it or fix it. `[read]`

- [ ] **`closesocket` vs `close` for queued fds.** `sharded.cpp:332` uses `close(int)`, which is wrong on Windows. `[read]`

- [ ] **`stat_bytes_read` counts decrypted bytes under TLS,** not wire bytes as its comment says. `[read]`

- [ ] **Chunked body over `max_body_size` is rejected only after full buffering** (up to 3 MB). `http.cpp:769`. `[read]`

- [ ] **Stub and doc gaps.** `[read]`
  - `HttpMessage.method`/`uri`/`query` are typed `-> object` (no `nb::sig`).
  - Handlers are typed `object`.
  - `Metrics` is not re-exported from `src/nanosrv/__init__.py`.
  - README:337 says "79% of native C throughput"; README:265 and the CHANGELOG say 97%.
  - The `[tool.mypy]` comment in `pyproject.toml` names `src/nanosrv/_core.pyi`; stubs are in `src/nanosrv/_core/`.

- [ ] **Build and CI.** `[read]`
  - `make test` runs only pytest; the C++ suite needs `make server-test`.
  - `make release` uses BSD-only `sed -i ''`; it fails on GNU sed.
  - `.github/workflows/ci.yml` and `ci-py.yml` are both named "CI" and overlap.
  - CI mypy checks only `src/nanosrv/__init__.py`; `make typecheck` checks all of `src/`.
