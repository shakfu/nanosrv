"""Tests for nanosrv nanobind extension module."""

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
        assert getattr(nanosrv.LogLevel, "None").value == 0
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
        assert nanosrv.base64_decode("aGVsbG8=") == "hello"

    def test_roundtrip(self):
        original = "The quick brown fox jumps over the lazy dog"
        assert nanosrv.base64_decode(nanosrv.base64_encode(original)) == original

    def test_empty(self):
        assert nanosrv.base64_encode("") == ""
        assert nanosrv.base64_decode("") == ""


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
        for level in [getattr(nanosrv.LogLevel, "None"), nanosrv.LogLevel.Error,
                      nanosrv.LogLevel.Info, nanosrv.LogLevel.Debug,
                      nanosrv.LogLevel.Verbose]:
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
            conn.http_reply(200, "Content-Type: text/plain\r\n",
                            f"echo:{msg.uri}")

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
            resp = urllib.request.urlopen(
                "http://127.0.0.1:18323/path?key=val"
            )
            resp.read()
            assert "GET" in captured["repr"]
            assert "key=val" in captured["query"]
            assert captured["user_agent"] is not None
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
            assert props["is_listening"] is False  # handler conn is accepted, not the listener
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
            escaped["msg"] = msg          # smuggle the transient view out
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
            escaped["conn"] = conn        # smuggle the connection out
            escaped["id"] = conn.id       # reading inside the callback is fine
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
        mgr.http_listen("http://127.0.0.1:18360",
                        lambda conn, msg: conn.http_reply(200, "", "ok"))

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
        mgr.http_listen("http://127.0.0.1:18361",
                        lambda conn, msg: conn.http_reply(200, "", "ok"))

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
        mgr.http_listen("http://127.0.0.1:18364",
                        lambda conn, msg: conn.http_reply(200, "", "ok"))

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
                    pass             # still open, no data pending
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
                200, "Content-Type: text/plain\r\n", body),
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
                f"expected truncated response, read {total} of {body_size}")
            assert mgr.num_connections == 0
        finally:
            stop.set()
            t.join(timeout=2)

    def test_start_drain_single_threaded(self):
        # Manager.start_drain() closes listeners (stops accepting) and drains
        # accepted connections. Driven on the poll thread, so it is race-free.
        import socket

        mgr = nanosrv.Manager()
        mgr.http_listen("http://127.0.0.1:18367",
                        lambda conn, msg: conn.http_reply(200, "", "ok"))

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
        import socket

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
        time.sleep(0.1)     # request is sent, handler is sleeping
        mgr.drain(3000)     # graceful: let the in-flight request complete

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

        del cb           # drop our strong ref; the listener still holds one
        gc.collect()
        assert ref() is not None, "callback dropped while listener is open"

        del mgr          # closing the listener must release the callback
        gc.collect()
        assert ref() is None, "callback leaked after Manager teardown"
