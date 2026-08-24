# Security Policy

## Reporting a vulnerability

Please report suspected vulnerabilities **privately** rather than opening a
public issue. Use GitHub's private vulnerability reporting ("Report a
vulnerability" under the repository's *Security* tab). Include a description, a
minimal reproduction if possible, and the affected version or commit. You will
receive an acknowledgement, and fixes for confirmed issues will be released with
a note in the changelog.

## Threat-model posture

**nanosrv is not yet hardened for direct exposure to hostile networks.** It is a
compact HTTP/WebSocket toolkit intended for trusted or controlled environments,
or behind a hardened reverse proxy that terminates TLS and absorbs abusive
traffic. Treat exposure to untrusted clients as experimental.

What this means in practice:

- **TLS is opt-in and off by default.** The default build links a no-op TLS
  stub; `tls_available()` returns false and a `https://` / `wss://` listen fails
  closed (raising in Python, returning a null listener in C++) rather than
  silently serving cleartext on a TLS-intended port. Build the C++ library with
  `-DNANOSRV_TLS=mbed` for a working mbedTLS backend. The Python wheel ships with
  the stub, so terminate TLS in front of it.
- **IP ACLs are not applied automatically.** `check_ip_acl()` (C++) matches IPv4
  and IPv6 uniformly via a bitwise prefix compare and fails closed on a malformed
  ACL, but no listener wires it into the accept path for you; apply it in your
  handler if you need address filtering.

## Hardening knobs

The event loop provides opt-in limits to blunt common resource-exhaustion
attacks. Set them before serving untrusted traffic:

| Knob | Defends against |
|---|---|
| `set_idle_timeout(ms)` | connect-and-idle exhaustion |
| `set_request_timeout(ms)` | slow-dribble (slowloris) requests |
| `set_connect_timeout(ms)` | hung outbound `connect()` (default 30 s) |
| `set_max_body_size(bytes)` | oversized request bodies (answered 413) |
| `set_max_connections(n)` | accept-flooding |
| `set_max_send_buffer(bytes)` | slow/stalled readers tying up send buffers |

Additional built-in protections that are always on: a bounded receive buffer
(`MG_MAX_RECV_SIZE`, 3 MB), `Content-Length` overflow rejection, a WebSocket
payload-length cap, RFC 6455 masking/control-frame enforcement (non-conforming
peers are closed with status 1002), and rejection of requests carrying more than
`MG_MAX_HTTP_HEADERS` (answered 431 rather than silently truncated).
`ShardedManager::drain()` / `Manager::start_drain()` provide graceful shutdown.

Note that `Connection.close()` is immediate and discards anything still
buffered for sending; `Connection.drain()` closes once the output has been
flushed.

For observability during an incident, `Manager.metrics` /
`ShardedManager.metrics` expose cumulative counters (accepted, closed, errors,
bytes read/written) and a live connection gauge.

## Callback object lifetime

The `Connection`, `HttpMessage`, and `WsMessage` objects passed to a handler are
borrowed views valid **only for the duration of that call** -- they point into
buffers the event loop reuses or frees once the handler returns. Copy out any
bytes you need to keep; do not retain a handle and use it from a later event, a
timer, or another thread. To act on a connection afterwards, keep `conn.id` and
call `wakeup(id, data)` on the `Manager` or `ShardedManager` -- it is
thread-safe and delivers the payload to the handler on the loop that owns the
connection. The Python bindings enforce this by raising on
use-after-return (see `TestCallbackObjectLifetime`). The exception is
`ConnectionRef` (returned by `http_listen()`), which remains valid until its
listener closes.

## Supported versions

nanosrv is pre-1.0 and under active development. Security fixes are applied to
the `main` branch and the latest release; older releases are not maintained.
