"""Tests for nanosrv nanobind extension module."""

import sys
import threading
import time
import urllib.request
import urllib.error

import pytest

import nanosrv


# ---------------------------------------------------------------------------
# Enum tests
# ---------------------------------------------------------------------------


class TestEnums:
    def test_event_values(self):
        assert nanosrv.Event.Error.value == 0
        assert nanosrv.Event.Open.value == 1
        assert nanosrv.Event.HttpMessage.value == 11
        assert nanosrv.Event.WsOpen.value == 12
        assert nanosrv.Event.WsMessage.value == 13
        assert nanosrv.Event.Wakeup.value == 20
        assert nanosrv.Event.User.value == 100

    def test_ws_opcode_values(self):
        assert nanosrv.WsOpcode.Text.value == 1
        assert nanosrv.WsOpcode.Binary.value == 2
        assert nanosrv.WsOpcode.Close.value == 8
        assert nanosrv.WsOpcode.Ping.value == 9
        assert nanosrv.WsOpcode.Pong.value == 10

    def test_log_level_values(self):
        # Off, not None: `LogLevel.None` is a SyntaxError in Python, so the
        # zero level was previously reachable only via getattr.
        assert nanosrv.LogLevel.Off.value == 0
        # The old name survives as an alias for existing getattr() call sites.
        assert getattr(nanosrv.LogLevel, "None") is nanosrv.LogLevel.Off
        assert nanosrv.LogLevel.Error.value == 1
        assert nanosrv.LogLevel.Info.value == 2
        assert nanosrv.LogLevel.Debug.value == 3
        assert nanosrv.LogLevel.Verbose.value == 4


# ---------------------------------------------------------------------------
# URL parsing tests
# ---------------------------------------------------------------------------


class TestUrl:
    def test_parse_http(self):
        u = nanosrv.Url.parse("http://example.com:8080/path")
        assert u.host == "example.com"
        assert u.port == 8080
        assert u.path == "/path"
        assert u.is_ssl is False

    def test_parse_https(self):
        u = nanosrv.Url.parse("https://example.com/secure")
        assert u.host == "example.com"
        assert u.port == 443
        assert u.is_ssl is True

    def test_parse_default_port(self):
        u = nanosrv.Url.parse("http://localhost/")
        assert u.host == "localhost"
        assert u.port == 80

    def test_repr(self):
        u = nanosrv.Url.parse("http://localhost:9999/test")
        r = repr(u)
        assert "localhost" in r
        assert "9999" in r


# ---------------------------------------------------------------------------
# Base64 tests
# ---------------------------------------------------------------------------


class TestBase64:
    def test_encode(self):
        assert nanosrv.base64_encode("hello") == "aGVsbG8="

    def test_decode(self):
        # base64 decodes to bytes, not text -- the payload need not be UTF-8.
        assert nanosrv.base64_decode("aGVsbG8=") == b"hello"

    def test_roundtrip(self):
        original = "The quick brown fox jumps over the lazy dog"
        assert (
            nanosrv.base64_decode(nanosrv.base64_encode(original)) == original.encode()
        )

    def test_empty(self):
        assert nanosrv.base64_encode("") == ""
        assert nanosrv.base64_decode("") == b""

    def test_binary_roundtrip(self):
        """Arbitrary bytes survive; previously decode() raised on non-UTF-8."""
        original = bytes(range(256))
        assert nanosrv.base64_decode(nanosrv.base64_encode(original)) == original

    def test_encode_accepts_bytes_like(self):
        assert nanosrv.base64_encode(b"hello") == "aGVsbG8="
        assert nanosrv.base64_encode(bytearray(b"hello")) == "aGVsbG8="
        assert nanosrv.base64_encode(memoryview(b"hello")) == "aGVsbG8="


# ---------------------------------------------------------------------------
# URL encode/decode tests
# ---------------------------------------------------------------------------


class TestUrlEncodeDecode:
    def test_encode(self):
        result = nanosrv.url_encode("hello world")
        assert result == "hello%20world"

    def test_decode(self):
        result = nanosrv.url_decode("hello%20world")
        assert result == "hello world"

    def test_roundtrip(self):
        original = "key=value&foo=bar baz"
        assert nanosrv.url_decode(nanosrv.url_encode(original)) == original

    def test_special_chars(self):
        encoded = nanosrv.url_encode("a+b=c&d")
        assert "%" in encoded

    def test_decode_non_utf8_does_not_raise(self):
        """A percent sequence that is not valid UTF-8 must not raise."""
        decoded = nanosrv.url_decode("%ff%fe")
        assert decoded.encode("utf-8", "surrogateescape") == b"\xff\xfe"

    def test_decode_bytes_returns_raw(self):
        assert nanosrv.url_decode_bytes("%ff%fe") == b"\xff\xfe"

    def test_encode_accepts_bytes(self):
        assert nanosrv.url_encode(b"hello world") == "hello%20world"


# ---------------------------------------------------------------------------
# JSON parsing tests
# ---------------------------------------------------------------------------


class TestJson:
    def test_number(self):
        result = nanosrv.json.number('{"x": 3.14}', "$.x")
        assert result is not None
        assert abs(result - 3.14) < 1e-9

    def test_boolean(self):
        assert nanosrv.json.boolean('{"flag": true}', "$.flag") is True
        assert nanosrv.json.boolean('{"flag": false}', "$.flag") is False

    def test_integer(self):
        result = nanosrv.json.integer('{"n": 42}', "$.n")
        assert result == 42

    def test_string(self):
        result = nanosrv.json.string('{"name": "nanosrv"}', "$.name")
        assert result == "nanosrv"

    def test_missing_path(self):
        assert nanosrv.json.number('{"x": 1}', "$.y") is None
        assert nanosrv.json.string('{"x": 1}', "$.y") is None

    def test_nested(self):
        j = '{"a": {"b": 99}}'
        assert nanosrv.json.integer(j, "$.a.b") == 99


# ---------------------------------------------------------------------------
# Logging tests
# ---------------------------------------------------------------------------


class TestLogging:
    def test_set_get_log_level(self):
        original = nanosrv.get_log_level()
        nanosrv.set_log_level(nanosrv.LogLevel.Debug)
        assert nanosrv.get_log_level() == nanosrv.LogLevel.Debug
        nanosrv.set_log_level(original)

    def test_all_levels(self):
        for level in [
            nanosrv.LogLevel.Off,
            nanosrv.LogLevel.Error,
            nanosrv.LogLevel.Info,
            nanosrv.LogLevel.Debug,
            nanosrv.LogLevel.Verbose,
        ]:
            nanosrv.set_log_level(level)
            assert nanosrv.get_log_level() == level
        nanosrv.set_log_level(getattr(nanosrv.LogLevel, "None"))


# ---------------------------------------------------------------------------
# Millis test
# ---------------------------------------------------------------------------


class TestMillis:
    def test_returns_positive(self):
        t = nanosrv.millis()
        assert t > 0

    def test_monotonic(self):
        t1 = nanosrv.millis()
        t2 = nanosrv.millis()
        assert t2 >= t1


class TestTls:
    def test_tls_unavailable_in_default_build(self):
        # The default build links the no-op TLS stub.
        assert nanosrv.tls_available() is False

    def test_https_listen_raises(self):
        # A TLS URL must raise up front (clear error) rather than failing later
        # at the stub handshake.
        mgr = nanosrv.Manager()
        for url in ("https://127.0.0.1:18370", "wss://127.0.0.1:18370"):
            with pytest.raises(RuntimeError, match="TLS is not available"):
                mgr.http_listen(url, lambda conn, msg: None)
            with pytest.raises(RuntimeError, match="TLS is not available"):
                mgr.http_listen_event(url, lambda conn, ev, data: None)

    def test_sharded_https_listen_raises(self):
        mgr = nanosrv.ShardedManager(2)
        with pytest.raises(RuntimeError, match="TLS is not available"):
            mgr.http_listen("https://127.0.0.1:18371", lambda conn, msg: None)

    def test_plain_url_still_listens(self):
        # Non-TLS schemes are unaffected by the guard.
        mgr = nanosrv.Manager()
        ref = mgr.http_listen("http://127.0.0.1:18372", lambda conn, msg: None)
        assert ref is not None

    def test_url_parse_does_not_raise(self):
        # Parsing a TLS URL is fine; only listening on one is rejected.
        u = nanosrv.Url.parse("https://example.com:443/x")
        assert u.is_ssl is True


# ---------------------------------------------------------------------------
# Manager tests
# ---------------------------------------------------------------------------


class TestManager:
    def test_create(self):
        mgr = nanosrv.Manager()
        assert mgr is not None

    def test_poll(self):
        mgr = nanosrv.Manager()
        mgr.poll(1)  # should not block significantly


# ---------------------------------------------------------------------------
# HTTP server integration test
# ---------------------------------------------------------------------------


class TestHttpServer:
    def test_request_response(self):
        mgr = nanosrv.Manager()
        received = {}

        def handler(conn, msg):
            received["method"] = msg.method
            received["uri"] = msg.uri
            received["body"] = msg.body
            conn.http_reply(200, "Content-Type: text/plain\r\n", f"echo:{msg.uri}")

        ref = mgr.http_listen("http://0.0.0.0:18321", handler)
        assert ref

        # Run the server in a background thread
        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()

        try:
            # Give the server a moment to start
            time.sleep(0.05)
            resp = urllib.request.urlopen("http://127.0.0.1:18321/hello")
            body = resp.read().decode()
            assert body == "echo:/hello"
            assert resp.status == 200

            assert received["method"] == "GET"
            assert received["uri"] == "/hello"
        finally:
            stop.set()
            t.join(timeout=2)

    def test_post_body(self):
        mgr = nanosrv.Manager()

        def handler(conn, msg):
            conn.http_reply(200, "", msg.body)

        mgr.http_listen("http://0.0.0.0:18322", handler)

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()

        try:
            time.sleep(0.05)
            req = urllib.request.Request(
                "http://127.0.0.1:18322/echo",
                data=b"test-payload",
                method="POST",
            )
            resp = urllib.request.urlopen(req)
            assert resp.read() == b"test-payload"
        finally:
            stop.set()
            t.join(timeout=2)

    def test_http_message_properties(self):
        mgr = nanosrv.Manager()
        captured = {}

        def handler(conn, msg):
            captured["repr"] = repr(msg)
            captured["query"] = msg.query
            h = msg.header("User-Agent")
            captured["user_agent"] = h
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://0.0.0.0:18323", handler)

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()

        try:
            time.sleep(0.05)
            resp = urllib.request.urlopen("http://127.0.0.1:18323/path?key=val")
            resp.read()
            assert "GET" in captured["repr"]
            assert "key=val" in captured["query"]
            assert captured["user_agent"] is not None
        finally:
            stop.set()
            t.join(timeout=2)

    def test_metrics_move_on_request(self):
        mgr = nanosrv.Manager()

        # Baseline: everything zero before any traffic.
        base = mgr.metrics
        assert base.accepted == 0
        assert base.closed == 0
        assert base.bytes_read == 0
        assert base.bytes_written == 0
        assert base.active == 0

        def handler(conn, msg):
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://0.0.0.0:18324", handler)

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()

        try:
            time.sleep(0.05)
            resp = urllib.request.urlopen("http://127.0.0.1:18324/hello")
            resp.read()
            # Give the loop a moment to observe the accept and the close.
            time.sleep(0.1)
            m = mgr.metrics
            assert m.accepted >= 1
            assert m.bytes_read > 0
            assert m.bytes_written > 0
            assert repr(m).startswith("Metrics(")
        finally:
            stop.set()
            t.join(timeout=2)


# ---------------------------------------------------------------------------
# Connection properties test
# ---------------------------------------------------------------------------


class TestConnectionProperties:
    def test_connection_flags(self):
        mgr = nanosrv.Manager()
        props = {}

        def handler(conn, msg):
            props["id"] = conn.id
            props["is_websocket"] = conn.is_websocket
            props["is_listening"] = conn.is_listening
            props["is_tls"] = conn.is_tls
            props["is_accepted"] = conn.is_accepted
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://0.0.0.0:18324", handler)

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()

        try:
            time.sleep(0.05)
            urllib.request.urlopen("http://127.0.0.1:18324/")
            time.sleep(0.05)

            assert props["id"] > 0
            assert props["is_websocket"] is False
            assert (
                props["is_listening"] is False
            )  # handler conn is accepted, not the listener
            assert props["is_tls"] is False
            assert props["is_accepted"] is True
        finally:
            stop.set()
            t.join(timeout=2)


# ---------------------------------------------------------------------------
# Callback object lifetime (H2): conn/msg handed to a handler are valid only
# during that call. Storing one and using it afterwards must raise, not crash
# (use-after-free). To act on a connection later, keep conn.id + Manager.wakeup.
# ---------------------------------------------------------------------------


class TestCallbackObjectLifetime:
    def _run_request(self, port, handler):
        mgr = nanosrv.Manager()
        mgr.http_listen(f"http://0.0.0.0:{port}", handler)
        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(10)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        try:
            time.sleep(0.05)
            urllib.request.urlopen(f"http://127.0.0.1:{port}/x").read()
            time.sleep(0.05)
        finally:
            stop.set()
            t.join(timeout=2)

    def test_stored_message_raises_after_callback(self):
        escaped = {}

        def handler(conn, msg):
            escaped["msg"] = msg  # smuggle the transient view out
            conn.http_reply(200, "", "ok")

        self._run_request(18331, handler)

        assert "msg" in escaped
        # Using the stored HttpMessage after the callback must raise, not crash.
        with pytest.raises(RuntimeError):
            _ = escaped["msg"].uri
        with pytest.raises(RuntimeError):
            _ = escaped["msg"].header("Host")

    def test_stored_connection_raises_after_callback(self):
        escaped = {}

        def handler(conn, msg):
            escaped["conn"] = conn  # smuggle the connection out
            escaped["id"] = conn.id  # reading inside the callback is fine
            conn.http_reply(200, "", "ok")

        self._run_request(18332, handler)

        assert escaped["id"] > 0
        # Acting on the stored Connection after the callback must raise.
        with pytest.raises(RuntimeError):
            escaped["conn"].http_reply(200, "", "late")
        with pytest.raises(RuntimeError):
            _ = escaped["conn"].id

    def test_many_listeners_do_not_accumulate_errors(self):
        # Exercises the callback-lifetime path repeatedly. With the previous
        # per-listener ref leak this still "worked", but the test documents that
        # repeated listen/handler creation is well-behaved.
        for i in range(50):
            mgr = nanosrv.Manager()
            mgr.http_listen(
                f"http://0.0.0.0:{18400 + i}",
                lambda conn, msg: conn.http_reply(200, "", "ok"),
            )
            mgr.poll(0)
            del mgr

    def test_idle_timeout_closes_silent_connection(self):
        # An accepted connection that sends nothing must be reaped after the
        # idle timeout. We connect a raw socket, send nothing, and expect the
        # server to close it (recv returns b"" / EOF).
        import socket

        mgr = nanosrv.Manager()
        mgr.set_idle_timeout(150)
        assert mgr.idle_timeout == 150
        mgr.http_listen(
            "http://127.0.0.1:18360", lambda conn, msg: conn.http_reply(200, "", "ok")
        )

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(20)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        try:
            time.sleep(0.05)
            s = socket.create_connection(("127.0.0.1", 18360), timeout=2)
            s.settimeout(2.0)
            # Send nothing; the server should close the idle connection.
            data = s.recv(16)  # blocks until server closes -> returns b""
            s.close()
            assert data == b"", f"expected server to close idle conn, got {data!r}"
        finally:
            stop.set()
            t.join(timeout=2)

    def test_request_timeout_closes_dribbling_connection(self):
        # A connection that trickles bytes but never completes a request must be
        # reaped by the request deadline, even though the trickle keeps the idle
        # timer fresh. idle_timeout is set high so only request_timeout can close.
        import socket

        mgr = nanosrv.Manager()
        mgr.set_request_timeout(200)
        mgr.set_idle_timeout(5000)
        assert mgr.request_timeout == 200
        mgr.http_listen(
            "http://127.0.0.1:18361", lambda conn, msg: conn.http_reply(200, "", "ok")
        )

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(20)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        try:
            time.sleep(0.05)
            s = socket.create_connection(("127.0.0.1", 18361), timeout=2)
            s.settimeout(3.0)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n")  # incomplete request
            # The server should close us via the request deadline (~200ms),
            # well before the 5s idle timeout. recv returns b"" on close.
            data = s.recv(16)
            s.close()
            assert data == b"", f"expected request-deadline close, got {data!r}"
        finally:
            stop.set()
            t.join(timeout=2)

    def test_max_body_size_rejects_with_413(self):
        # An oversized Content-Length must get a 413 (and never reach the
        # handler), while a body under the cap is served normally.
        import urllib.error

        mgr = nanosrv.Manager()
        mgr.set_max_body_size(1024)
        assert mgr.max_body_size == 1024
        served = {"n": 0}

        def handler(conn, msg):
            served["n"] += 1
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://127.0.0.1:18363", handler)
        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(20)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        try:
            time.sleep(0.05)

            # Oversized body -> 413, handler not invoked.
            req = urllib.request.Request(
                "http://127.0.0.1:18363/big",
                data=b"x" * 5000,
                method="POST",
            )
            try:
                urllib.request.urlopen(req)
                assert False, "expected HTTP 413"
            except urllib.error.HTTPError as e:
                assert e.code == 413, f"expected 413, got {e.code}"

            # Small body under the cap -> served.
            small = urllib.request.Request(
                "http://127.0.0.1:18363/ok",
                data=b"hello",
                method="POST",
            )
            resp = urllib.request.urlopen(small)
            assert resp.read() == b"ok"
            assert served["n"] == 1  # only the small request reached the handler
        finally:
            stop.set()
            t.join(timeout=2)

    def test_max_connections_caps_accepts(self):
        # Once the live accepted-connection count hits the cap, further accepts
        # are closed immediately. Three idle clients against a cap of two: the
        # count holds at two and exactly one client is dropped (EOF).
        import socket

        mgr = nanosrv.Manager()
        mgr.set_max_connections(2)
        assert mgr.max_connections == 2
        mgr.http_listen(
            "http://127.0.0.1:18364", lambda conn, msg: conn.http_reply(200, "", "ok")
        )

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(20)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        socks = []
        try:
            time.sleep(0.05)
            for _ in range(3):
                s = socket.create_connection(("127.0.0.1", 18364), timeout=2)
                s.setblocking(False)
                socks.append(s)
            time.sleep(0.5)  # let the acceptor process all three

            assert mgr.num_connections == 2  # cap held

            eofs = 0
            for s in socks:
                try:
                    if s.recv(16) == b"":
                        eofs += 1  # server closed this one (rejected)
                except BlockingIOError:
                    pass  # still open, no data pending
            assert eofs == 1, f"expected one rejected conn, got {eofs}"
        finally:
            for s in socks:
                s.close()
            stop.set()
            t.join(timeout=2)

    def test_max_send_buffer_drops_slow_reader(self):
        # A reader that does not drain its socket must be dropped once the
        # server's unsent backlog exceeds the cap. The connection closes
        # mid-stream, so the client reads far less than the full body.
        import socket

        mgr = nanosrv.Manager()
        mgr.set_max_send_buffer(64 * 1024)  # 64 KB
        assert mgr.max_send_buffer == 64 * 1024

        body_size = 4 * 1024 * 1024  # 4 MB, far over the cap
        body = "x" * body_size
        mgr.http_listen(
            "http://127.0.0.1:18365",
            lambda conn, msg: conn.http_reply(
                200, "Content-Type: text/plain\r\n", body
            ),
        )

        stop = threading.Event()

        def poll_loop():
            while not stop.is_set():
                mgr.poll(20)

        t = threading.Thread(target=poll_loop, daemon=True)
        t.start()
        try:
            time.sleep(0.05)
            s = socket.create_connection(("127.0.0.1", 18365), timeout=3)
            s.settimeout(5.0)
            s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")

            # Read until EOF. The server drops us mid-response, so the total is
            # far below the 4 MB body.
            total = 0
            while True:
                chunk = s.recv(64 * 1024)
                if chunk == b"":
                    break
                total += len(chunk)
            s.close()
            assert total < body_size, (
                f"expected truncated response, read {total} of {body_size}"
            )
            assert mgr.num_connections == 0
        finally:
            stop.set()
            t.join(timeout=2)

    def test_start_drain_single_threaded(self):
        # Manager.start_drain() closes listeners (stops accepting) and drains
        # accepted connections. Driven on the poll thread, so it is race-free.
        import socket

        mgr = nanosrv.Manager()
        mgr.http_listen(
            "http://127.0.0.1:18367", lambda conn, msg: conn.http_reply(200, "", "ok")
        )

        # Open an idle keep-alive connection and let it be accepted.
        s = socket.create_connection(("127.0.0.1", 18367), timeout=2)
        for _ in range(10):
            mgr.poll(20)
        assert mgr.num_connections == 1

        # Begin draining on this same thread, then poll until it has drained.
        mgr.start_drain()
        for _ in range(50):
            mgr.poll(20)
            if mgr.num_connections == 0:
                break
        assert mgr.num_connections == 0  # idle connection closed by the drain
        s.close()

        # The listener is closed, so new connections are refused.
        for _ in range(5):
            mgr.poll(20)
        refused = False
        try:
            s2 = socket.create_connection(("127.0.0.1", 18367), timeout=1)
            s2.close()
        except OSError:
            refused = True
        assert refused, "expected listener closed after drain"

    def test_sharded_drain_finishes_inflight(self):
        # ShardedManager.drain() (thread-safe) must let an in-flight request
        # finish, then make run() return on its own.

        mgr = nanosrv.ShardedManager(2)
        served = {"n": 0}

        def handler(conn, msg):
            time.sleep(0.3)  # still in flight when drain() is called
            served["n"] += 1
            conn.http_reply(200, "Content-Type: text/plain\r\n", "ok")

        mgr.http_listen("http://127.0.0.1:18368", handler)
        runner = threading.Thread(target=mgr.run, daemon=True)
        runner.start()
        time.sleep(0.1)

        result = {}

        def client():
            try:
                r = urllib.request.urlopen("http://127.0.0.1:18368/", timeout=5)
                result["code"] = r.getcode()
                result["body"] = r.read()
            except Exception as e:  # noqa: BLE001
                result["err"] = e

        ct = threading.Thread(target=client)
        ct.start()
        time.sleep(0.1)  # request is sent, handler is sleeping
        mgr.drain(3000)  # graceful: let the in-flight request complete

        ct.join()
        runner.join(timeout=5)
        assert not runner.is_alive(), "run() did not return after drain"
        assert result.get("code") == 200, f"got {result}"
        assert result.get("body") == b"ok"
        assert served["n"] == 1
        assert mgr.num_connections == 0

    def test_callback_is_released_when_listener_closes(self):
        # H1 regression: the listener must hold the callback while open and
        # release it when the Manager is destroyed. The previous binding leaked
        # one reference per listen (inc_ref with no matching dec_ref), so the
        # callback was never collected. weakref makes that directly observable.
        import weakref
        import gc

        class CB:
            def __call__(self, conn, msg):
                conn.http_reply(200, "", "ok")

        cb = CB()
        ref = weakref.ref(cb)
        mgr = nanosrv.Manager()
        mgr.http_listen("http://0.0.0.0:18350", cb)
        mgr.poll(0)

        del cb  # drop our strong ref; the listener still holds one
        gc.collect()
        assert ref() is not None, "callback dropped while listener is open"

        del mgr  # closing the listener must release the callback
        gc.collect()
        assert ref() is None, "callback leaked after Manager teardown"


# ---------------------------------------------------------------------------
# Binary payloads
#
# Every payload path used to cross the boundary as str: a binary request body
# or WebSocket frame raised UnicodeDecodeError inside the handler, and the send
# paths rejected bytes outright. These pin the bytes contract down.
# ---------------------------------------------------------------------------

import socket  # noqa: E402
import base64 as _b64  # noqa: E402
import os as _os  # noqa: E402
from contextlib import contextmanager  # noqa: E402

BINARY = bytes([0x89, 0xFF, 0xFE, 0x00, 0x41, 0x80])


@contextmanager
def _polling(mgr):
    """Drive a Manager's event loop on a background thread for the block."""
    stop = threading.Event()

    def loop():
        while not stop.is_set():
            mgr.poll(10)

    t = threading.Thread(target=loop, daemon=True)
    t.start()
    time.sleep(0.05)
    try:
        yield
    finally:
        stop.set()
        t.join(timeout=2)


@contextmanager
def _running(mgr):
    """Run a ShardedManager for the block, stopping it on exit."""
    runner = threading.Thread(target=mgr.run, daemon=True)
    runner.start()
    time.sleep(0.15)
    try:
        yield
    finally:
        mgr.stop()
        runner.join(timeout=5)


def _post(url, data, timeout=5):
    req = urllib.request.Request(url, data=data, method="POST")
    return urllib.request.urlopen(req, timeout=timeout)


def _ws_connect(host, port, timeout=5):
    """Minimal RFC 6455 client handshake; returns the connected socket."""
    s = socket.create_connection((host, port), timeout=timeout)
    key = _b64.b64encode(_os.urandom(16)).decode()
    s.sendall(
        f"GET / HTTP/1.1\r\nHost: {host}\r\nUpgrade: websocket\r\n"
        f"Connection: Upgrade\r\nSec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n".encode()
    )
    deadline = time.time() + timeout
    buf = b""
    while b"\r\n\r\n" not in buf and time.time() < deadline:
        chunk = s.recv(4096)
        if not chunk:
            break
        buf += chunk
    assert b"101" in buf, f"upgrade failed: {buf!r}"
    return s


def _ws_send(sock, payload, opcode=0x2):
    """Send one masked client frame (clients must mask, per RFC 6455 5.1)."""
    mask = _os.urandom(4)
    masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
    n = len(payload)
    assert n < 126, "test helper only encodes 7-bit lengths"
    sock.sendall(bytes([0x80 | opcode, 0x80 | n]) + mask + masked)


def _ws_recv(sock, timeout=5):
    """Read one unmasked server frame; returns (opcode, payload)."""
    sock.settimeout(timeout)
    hdr = sock.recv(2)
    assert len(hdr) == 2, f"short frame header: {hdr!r}"
    opcode = hdr[0] & 0x0F
    n = hdr[1] & 0x7F
    assert n < 126, "test helper only decodes 7-bit lengths"
    payload = b""
    while len(payload) < n:
        chunk = sock.recv(n - len(payload))
        if not chunk:
            break
        payload += chunk
    return opcode, payload


class TestBinaryPayloads:
    def test_request_body_is_bytes(self):
        mgr = nanosrv.Manager()
        seen = {}

        def handler(conn, msg):
            seen["body"] = msg.body
            conn.http_reply(200, "", msg.body)  # echo the raw bytes back

        mgr.http_listen("http://127.0.0.1:18401", handler)
        with _polling(mgr):
            resp = _post("http://127.0.0.1:18401/", BINARY)
            assert resp.read() == BINARY

        assert seen["body"] == BINARY
        assert isinstance(seen["body"], bytes)

    def test_text_decodes_utf8_and_raises_on_binary(self):
        mgr = nanosrv.Manager()
        seen = {}

        def handler(conn, msg):
            try:
                seen["text"] = msg.text
                conn.http_reply(200, "", "decoded")
            except UnicodeDecodeError:
                seen["raised"] = True
                conn.http_reply(400, "", "not text")

        mgr.http_listen("http://127.0.0.1:18402", handler)
        with _polling(mgr):
            # Non-ASCII kept as an escape so the source stays 7-bit clean.
            utf8_text = "h\u00e9llo"
            resp = _post("http://127.0.0.1:18402/", utf8_text.encode())
            assert resp.read() == b"decoded"
            assert seen["text"] == utf8_text

            with pytest.raises(urllib.error.HTTPError) as exc:
                _post("http://127.0.0.1:18402/", BINARY)
            assert exc.value.code == 400
            assert seen.get("raised") is True

    def test_metadata_survives_invalid_utf8_without_raising(self):
        """Header values carry legacy non-UTF-8 bytes in the wild; reading one
        must not raise inside the handler. (A request *line* containing such
        bytes is rejected by the parser before any handler sees it.)"""
        mgr = nanosrv.Manager()
        seen = {}

        def handler(conn, msg):
            seen["hdr"] = msg.header("X-Legacy")  # must not raise
            seen["uri"] = msg.uri
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://127.0.0.1:18403", handler)
        with _polling(mgr):
            s = socket.create_connection(("127.0.0.1", 18403), timeout=5)
            try:
                s.sendall(b"GET /ok HTTP/1.1\r\nHost: x\r\nX-Legacy: caf\xe9\r\n\r\n")
                s.settimeout(5)
                assert b"200" in s.recv(4096)
            finally:
                s.close()

        # surrogateescape is lossless: the raw bytes come back out.
        assert seen["hdr"].encode("utf-8", "surrogateescape") == b"caf\xe9"
        assert seen["uri"] == "/ok"

    def test_malformed_request_line_is_rejected(self):
        """Raw non-ASCII in the request line fails the parse (fails closed)."""
        mgr = nanosrv.Manager()
        seen = {"calls": 0}

        def handler(conn, msg):
            seen["calls"] += 1
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://127.0.0.1:18415", handler)
        with _polling(mgr):
            s = socket.create_connection(("127.0.0.1", 18415), timeout=5)
            try:
                s.sendall(b"GET /bad\xff\xfe HTTP/1.1\r\nHost: x\r\n\r\n")
                time.sleep(0.2)
            finally:
                s.close()

        assert seen["calls"] == 0, "handler ran on an unparseable request"

    def test_send_paths_accept_bytes_like(self):
        mgr = nanosrv.Manager()

        def handler(conn, msg):
            if msg.uri == "/mv":
                conn.http_reply(200, "", memoryview(BINARY))
            elif msg.uri == "/ba":
                conn.http_reply(200, "", bytearray(BINARY))
            elif msg.uri == "/raw":
                # send_bytes must accept bytes -- it used to reject them.
                conn.send_bytes(
                    b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(BINARY)
                    + BINARY
                )
                # drain(), not close(): close() would discard the unsent bytes.
                conn.drain()
            else:
                conn.http_reply(200, "", "str body")

        mgr.http_listen("http://127.0.0.1:18404", handler)
        with _polling(mgr):
            for path in ("/mv", "/ba", "/raw"):
                resp = urllib.request.urlopen(
                    f"http://127.0.0.1:18404{path}", timeout=5
                )
                assert resp.read() == BINARY, path
            resp = urllib.request.urlopen("http://127.0.0.1:18404/s", timeout=5)
            assert resp.read() == b"str body"

    def test_drain_flushes_where_close_would_discard(self):
        mgr = nanosrv.Manager()

        def handler(conn, msg):
            body = b"x" * 64
            conn.send_bytes(
                b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body
            )
            conn.drain()

        mgr.http_listen("http://127.0.0.1:18416", handler)
        with _polling(mgr):
            resp = urllib.request.urlopen("http://127.0.0.1:18416/", timeout=5)
            assert resp.read() == b"x" * 64

    def test_websocket_binary_frame_round_trip(self):
        mgr = nanosrv.Manager()
        seen = {}

        def handler(conn, ev, data):
            if ev == nanosrv.Event.HttpMessage:
                conn.ws_upgrade(data, "")
            elif ev == nanosrv.Event.WsMessage:
                seen["data"] = data.data  # must not raise on a binary frame
                seen["opcode"] = data.opcode
                try:
                    data.text
                except UnicodeDecodeError:
                    seen["text_raised"] = True
                conn.ws_send_binary(data.data)

        mgr.http_listen_event("http://127.0.0.1:18405", handler)
        with _polling(mgr):
            s = _ws_connect("127.0.0.1", 18405)
            try:
                _ws_send(s, BINARY, opcode=0x2)
                opcode, payload = _ws_recv(s)
            finally:
                s.close()

        assert payload == BINARY
        assert opcode == 0x2
        assert seen["data"] == BINARY
        assert seen["opcode"] == nanosrv.WsOpcode.Binary
        assert seen.get("text_raised") is True


# ---------------------------------------------------------------------------
# Interpreter shutdown
# ---------------------------------------------------------------------------


class TestInterpreterShutdown:
    """A server object alive at interpreter exit is the normal case -- the
    README's own example keeps a module-level Manager and stops with Ctrl-C --
    but nanobind reported each one as a leak, so a new user's first run ended
    in an alarming bug report about nothing."""

    SCRIPT = (
        "import nanosrv\n"
        "mgr = nanosrv.Manager()\n"
        "ref = mgr.http_listen('http://127.0.0.1:18418', lambda c, m: None)\n"
        "sm = nanosrv.ShardedManager(2)\n"
    )

    def _run(self, env_extra=None):
        import subprocess
        import os

        env = dict(os.environ)
        env.pop("NANOSRV_LEAK_WARNINGS", None)
        if env_extra:
            env.update(env_extra)
        return subprocess.run(
            [sys.executable, "-c", self.SCRIPT],
            capture_output=True,
            text=True,
            env=env,
            timeout=60,
        )

    def test_exit_is_quiet_by_default(self):
        result = self._run()
        assert result.returncode == 0, result.stderr
        assert "leaked" not in result.stderr, result.stderr

    def test_leak_warnings_can_be_opted_into(self):
        result = self._run({"NANOSRV_LEAK_WARNINGS": "1"})
        assert result.returncode == 0, result.stderr
        if "leaked" not in result.stderr:
            # Whether nanobind reports anything is its own business and varies
            # by build: a free-threaded interpreter reports nothing at all,
            # where a GIL build reports every live instance. The contract this
            # class guards is the default (quiet); the opt-in can only be
            # checked where the diagnostics exist.
            pytest.skip("this nanobind build reports no leaks at shutdown")
        # Developers chasing a reference-counting bug still get the diagnostics.
        assert "leaked" in result.stderr


# ---------------------------------------------------------------------------
# Streamed responses (chunked / SSE)
# ---------------------------------------------------------------------------


class TestStreaming:
    def test_chunked_response(self):
        mgr = nanosrv.Manager()
        pieces = [b"first-", b"second-", BINARY]

        def handler(conn, msg):
            conn.start_chunked(200, "Content-Type: application/octet-stream\r\n")
            for p in pieces:
                conn.write_chunk(p)
            conn.write_chunk(b"")  # terminating chunk

        mgr.http_listen("http://127.0.0.1:18406", handler)
        with _polling(mgr):
            resp = urllib.request.urlopen("http://127.0.0.1:18406/", timeout=5)
            body = resp.read()

        assert body == b"".join(pieces)

    def test_chunks_are_written_incrementally(self):
        """The point of streaming: bytes reach the client before the handler
        has produced the rest of the response."""
        mgr = nanosrv.Manager()
        release = threading.Event()
        conn_ids = {}

        def handler(conn, ev, data):
            if ev == nanosrv.Event.HttpMessage:
                conn_ids["id"] = conn.id
                conn.start_chunked(200, "")
                conn.write_chunk(b"early")
            elif ev == nanosrv.Event.Wakeup:
                conn.write_chunk(b"late")
                conn.write_chunk(b"")

        mgr.http_listen_event("http://127.0.0.1:18407", handler)
        with _polling(mgr):
            s = socket.create_connection(("127.0.0.1", 18407), timeout=5)
            try:
                s.sendall(b"GET / HTTP/1.1\r\nHost: x\r\n\r\n")
                s.settimeout(5)
                head = b""
                while b"early" not in head:
                    head += s.recv(4096)
                # First chunk arrived while the response is still open.
                assert b"Transfer-Encoding: chunked" in head
                release.set()
                mgr.wakeup(conn_ids["id"], b"")
                rest = b""
                deadline = time.time() + 5
                while b"late" not in rest and time.time() < deadline:
                    rest += s.recv(4096)
                assert b"late" in rest
            finally:
                s.close()

    def test_sse_framing(self):
        mgr = nanosrv.Manager()

        def handler(conn, msg):
            conn.start_sse()
            conn.sse_send("hello", event="greeting", id="1")
            conn.sse_send("line one\nline two")
            conn.write_chunk(b"")

        mgr.http_listen("http://127.0.0.1:18408", handler)
        with _polling(mgr):
            s = socket.create_connection(("127.0.0.1", 18408), timeout=5)
            try:
                s.sendall(b"GET /events HTTP/1.1\r\nHost: x\r\n\r\n")
                s.settimeout(5)
                buf = b""
                deadline = time.time() + 5
                while b"line two" not in buf and time.time() < deadline:
                    chunk = s.recv(4096)
                    if not chunk:
                        break
                    buf += chunk
            finally:
                s.close()

        text = buf.decode()
        assert "Content-Type: text/event-stream" in text
        assert "event: greeting\nid: 1\ndata: hello\n\n" in text
        # A multi-line payload becomes one data: line per line.
        assert "data: line one\ndata: line two\n\n" in text

    def test_send_queue_len_reports_backlog(self):
        mgr = nanosrv.Manager()
        seen = {}

        def handler(conn, msg):
            assert conn.send_queue_len == 0
            conn.http_reply(200, "", "x" * 4096)
            seen["after"] = conn.send_queue_len

        mgr.http_listen("http://127.0.0.1:18409", handler)
        with _polling(mgr):
            urllib.request.urlopen("http://127.0.0.1:18409/", timeout=5).read()

        # The reply is buffered on the connection before the loop writes it.
        assert seen["after"] >= 4096


# ---------------------------------------------------------------------------
# Wakeup: acting on a connection from outside its handler
# ---------------------------------------------------------------------------


class TestWakeup:
    def test_manager_wakeup_delivers_payload(self):
        mgr = nanosrv.Manager()
        seen = {}
        got_request = threading.Event()

        def handler(conn, ev, data):
            if ev == nanosrv.Event.HttpMessage:
                seen["id"] = conn.id
                got_request.set()  # reply later, from the wakeup
            elif ev == nanosrv.Event.Wakeup:
                seen["payload"] = data
                conn.http_reply(200, "", data)

        mgr.http_listen_event("http://127.0.0.1:18410", handler)
        with _polling(mgr):
            result = {}

            def client():
                try:
                    r = urllib.request.urlopen("http://127.0.0.1:18410/", timeout=5)
                    result["body"] = r.read()
                except Exception as e:  # noqa: BLE001
                    result["err"] = e

            t = threading.Thread(target=client)
            t.start()
            assert got_request.wait(5), "handler never saw the request"
            assert mgr.wakeup(seen["id"], BINARY) is True
            t.join(timeout=5)

        assert result.get("body") == BINARY, f"got {result}"
        assert seen["payload"] == BINARY
        assert isinstance(seen["payload"], bytes)

    def test_wakeup_unknown_id_is_reported(self):
        mgr = nanosrv.Manager()
        mgr.http_listen("http://127.0.0.1:18411", lambda c, m: None)
        with _polling(mgr):
            # An id that matches nothing is delivered to no one, but the pipe
            # write itself succeeds; id 0 is rejected outright.
            assert mgr.wakeup(0, b"x") is False


# ---------------------------------------------------------------------------
# ShardedManager parity: WebSocket and wakeup on the multi-threaded path
# ---------------------------------------------------------------------------


class TestShardedParity:
    def test_websocket_over_sharded_manager(self):
        """http_listen_event is what makes WebSocket possible here at all --
        http_listen only ever delivers HttpMessage."""
        mgr = nanosrv.ShardedManager(4)
        seen = {}
        lock = threading.Lock()

        def handler(conn, ev, data):
            if ev == nanosrv.Event.HttpMessage:
                conn.ws_upgrade(data, "")
            elif ev == nanosrv.Event.WsOpen:
                with lock:
                    seen["opened"] = True
            elif ev == nanosrv.Event.WsMessage:
                conn.ws_send_binary(data.data)

        mgr.http_listen_event("http://127.0.0.1:18412", handler)
        with _running(mgr):
            s = _ws_connect("127.0.0.1", 18412)
            try:
                _ws_send(s, BINARY, opcode=0x2)
                opcode, payload = _ws_recv(s)
            finally:
                s.close()

        assert payload == BINARY
        assert opcode == 0x2
        assert seen.get("opened") is True

    def test_repeated_run_stop_cycles(self):
        """Each run() creates fresh worker threads, and each worker registers a
        Python thread state on start and releases it on exit. Cycling run/stop
        must not strand or double-free one."""
        mgr = nanosrv.ShardedManager(2)
        mgr.http_listen(
            "http://127.0.0.1:18417", lambda c, m: c.http_reply(200, "", "ok")
        )
        for _ in range(3):
            runner = threading.Thread(target=mgr.run, daemon=True)
            runner.start()
            time.sleep(0.3)
            body = urllib.request.urlopen("http://127.0.0.1:18417/", timeout=5).read()
            assert body == b"ok"
            mgr.stop()
            runner.join(timeout=5)
            assert not runner.is_alive(), "run() did not return after stop()"

    def test_connection_ids_are_unique_across_workers(self):
        """Each worker used to number its connections from 1 independently, so
        ids collided and could not identify a connection."""
        mgr = nanosrv.ShardedManager(4)
        ids = []
        lock = threading.Lock()

        def handler(conn, msg):
            with lock:
                ids.append(conn.id)
            conn.http_reply(200, "", "ok")

        mgr.http_listen("http://127.0.0.1:18413", handler)
        with _running(mgr):
            threads = []
            for _ in range(16):
                t = threading.Thread(
                    target=lambda: urllib.request.urlopen(
                        "http://127.0.0.1:18413/", timeout=5
                    ).read()
                )
                t.start()
                threads.append(t)
            for t in threads:
                t.join(timeout=10)

        assert len(ids) == 16, f"only {len(ids)} requests handled"
        assert len(set(ids)) == len(ids), f"duplicate connection ids: {ids}"

    def test_sharded_wakeup_routes_to_the_owning_worker(self):
        mgr = nanosrv.ShardedManager(4)
        pending = {}
        lock = threading.Lock()
        arrived = threading.Event()

        def handler(conn, ev, data):
            if ev == nanosrv.Event.HttpMessage:
                with lock:
                    # Path identifies the client; id routes the wakeup back.
                    pending[msg_path(data)] = conn.id
                    if len(pending) == 4:
                        arrived.set()
            elif ev == nanosrv.Event.Wakeup:
                conn.http_reply(200, "", data)

        def msg_path(msg):
            return msg.uri

        mgr.http_listen_event("http://127.0.0.1:18414", handler)
        results = {}

        def client(path):
            try:
                r = urllib.request.urlopen(f"http://127.0.0.1:18414{path}", timeout=10)
                results[path] = r.read()
            except Exception as e:  # noqa: BLE001
                results[path] = e

        with _running(mgr):
            paths = [f"/c{i}" for i in range(4)]
            threads = [threading.Thread(target=client, args=(p,)) for p in paths]
            for t in threads:
                t.start()
            assert arrived.wait(10), f"only {len(pending)} requests arrived"

            # Each connection is woken with a payload unique to its path; if the
            # wakeup were routed to the wrong worker or the wrong connection,
            # the bodies would not match up.
            with lock:
                for path, conn_id in pending.items():
                    assert mgr.wakeup(conn_id, path.encode()) is True
            for t in threads:
                t.join(timeout=10)

        for path in paths:
            assert results.get(path) == path.encode(), f"{path}: {results}"
