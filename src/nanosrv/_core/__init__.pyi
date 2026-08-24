"""nanosrv -- Python bindings for the nanosrv embedded server library"""

import enum

from nanosrv._core import json as json

class Event(enum.Enum):
    Error = 0

    Open = 1

    Poll = 2

    Resolve = 3

    Connect = 4

    Accept = 5

    TlsHandshake = 6

    Read = 7

    Write = 8

    Close = 9

    HttpHeaders = 10

    HttpMessage = 11

    WsOpen = 12

    WsMessage = 13

    WsControl = 14

    Wakeup = 20

    User = 100

class WsOpcode(enum.Enum):
    Continue = 0

    Text = 1

    Binary = 2

    Close = 8

    Ping = 9

    Pong = 10

class LogLevel(enum.Enum):
    Off = 0

    Error = 1

    Info = 2

    Debug = 3

    Verbose = 4

class Url:
    @property
    def host(self) -> str: ...
    @property
    def port(self) -> int: ...
    @property
    def path(self) -> str: ...
    @property
    def is_ssl(self) -> bool: ...
    @staticmethod
    def parse(url: str) -> Url: ...
    def __repr__(self) -> str: ...

class HttpMessage:
    @property
    def method(self) -> object:
        """Request method as str (surrogateescape-decoded; never raises)."""

    @property
    def uri(self) -> object:
        """Request URI as str (surrogateescape-decoded; never raises)."""

    @property
    def query(self) -> object:
        """Query string as str (surrogateescape-decoded; never raises)."""

    @property
    def body(self) -> bytes:
        """Request body as bytes. Use .text for a UTF-8 decoded str."""

    @property
    def text(self) -> str:
        """
        Request body decoded strictly as UTF-8. Raises UnicodeDecodeError on binary input -- use .body when the payload may not be text.
        """

    @property
    def status_code(self) -> int: ...
    def header(self, name: str) -> str | None:
        """Return the value of an HTTP header as str, or None if not present."""

    def credentials(self) -> tuple[str, str]:
        """Return (user, password) from Authorization header."""

    def __repr__(self) -> str: ...

class WsMessage:
    @property
    def data(self) -> bytes:
        """
        Frame payload as bytes -- binary frames included. Use .text for a UTF-8 decoded str.
        """

    @property
    def text(self) -> str:
        """
        Frame payload decoded strictly as UTF-8. Raises UnicodeDecodeError on a binary frame.
        """

    @property
    def flags(self) -> int: ...
    @property
    def opcode(self) -> WsOpcode: ...

class Connection:
    @property
    def id(self) -> int: ...
    @property
    def is_websocket(self) -> bool: ...
    @property
    def is_listening(self) -> bool: ...
    @property
    def is_client(self) -> bool: ...
    @property
    def is_accepted(self) -> bool: ...
    @property
    def is_tls(self) -> bool: ...
    @property
    def is_closing(self) -> bool: ...
    @property
    def send_queue_len(self) -> int:
        """
        Bytes buffered for sending but not yet written to the socket. Watch this to apply backpressure when producing a stream faster than the peer reads it; set_max_send_buffer() is the hard limit.
        """

    def send_bytes(self, data: bytes | bytearray | memoryview | str) -> bool:
        """
        Send raw bytes on the connection. Accepts bytes, bytearray, memoryview or str (encoded UTF-8).
        """

    def close(self) -> None:
        """
        Close the connection immediately, discarding anything still buffered for sending. To close after the response has been flushed, use drain().
        """

    def drain(self) -> None:
        """
        Close the connection once everything buffered has been written. This is what you want after send_bytes() or a streamed response -- close() would drop the unsent tail.
        """

    def http_reply(
        self,
        status: int,
        headers: bytes | bytearray | memoryview | str = "",
        body: bytes | bytearray | memoryview | str = b"",
    ) -> None:
        """
        Send a complete HTTP response. body accepts bytes, bytearray, memoryview or str (encoded UTF-8).
        """

    def start_chunked(
        self, status: int = 200, headers: bytes | bytearray | memoryview | str = ""
    ) -> None:
        """
        Begin a chunked response: sends the status line, your headers, and Transfer-Encoding: chunked. Follow with write_chunk() per piece and write_chunk(b'') to finish.
        """

    def start_sse(self, headers: bytes | bytearray | memoryview | str = "") -> None:
        """
        Begin a Server-Sent Events stream (text/event-stream, no-cache, chunked). Follow with sse_send() per event.
        """

    def write_chunk(self, data: bytes | bytearray | memoryview | str) -> None:
        """
        Write one chunk of a chunked response. An empty payload emits the terminating chunk and completes the response.
        """

    def sse_send(
        self,
        data: bytes | bytearray | memoryview | str,
        event: str | None = None,
        id: str | None = None,
        retry: int | None = None,
    ) -> None:
        """
        Send one SSE event. Multi-line data is split into one data: line per line, as the format requires.
        """

    def ws_send_text(self, data: bytes | bytearray | memoryview | str) -> int:
        """Send a WebSocket text frame. Accepts str or bytes-like."""

    def ws_send_binary(self, data: bytes | bytearray | memoryview | str) -> int:
        """Send a WebSocket binary frame. Accepts bytes-like or str."""

    def ws_send(self, data: bytes | bytearray | memoryview | str, opcode: int) -> int:
        """Send a WebSocket frame with explicit opcode."""

    def ws_upgrade(
        self, hm: HttpMessage, headers: bytes | bytearray | memoryview | str = ""
    ) -> None:
        """Upgrade an HTTP connection to WebSocket."""

class ConnectionRef:
    @property
    def id(self) -> int: ...
    def __bool__(self) -> bool: ...
    def send_bytes(self, data: bytes | bytearray | memoryview | str) -> bool:
        """Send raw bytes. Accepts bytes, bytearray, memoryview or str."""

    def close(self) -> None: ...
    def http_reply(
        self,
        status: int,
        headers: bytes | bytearray | memoryview | str = "",
        body: bytes | bytearray | memoryview | str = b"",
    ) -> None: ...

class Metrics:
    """Snapshot of a manager's cumulative observability counters."""

    @property
    def accepted(self) -> int:
        """Accepted connections, total."""

    @property
    def closed(self) -> int:
        """Accepted connections closed, total."""

    @property
    def errors(self) -> int:
        """MG_EV_ERROR events raised, total."""

    @property
    def bytes_read(self) -> int:
        """Bytes received off the wire, total."""

    @property
    def bytes_written(self) -> int:
        """Bytes written to the wire, total."""

    @property
    def active(self) -> int:
        """Currently live accepted connections."""

    def __repr__(self) -> str: ...

class Manager:
    def __init__(self) -> None: ...
    def poll(self, timeout_ms: int = 1000) -> None:
        """Poll the event loop once. Releases the GIL while waiting."""

    def http_listen(self, url: str, handler: object) -> ConnectionRef:
        """
        Listen for HTTP connections. handler(conn, msg) is called for each complete HTTP message.
        """

    def http_listen_event(self, url: str, handler: object) -> ConnectionRef:
        """
        Listen with full event handler. handler(conn, event, data) is called for every event.
        """

    def wakeup(
        self, conn_id: int, data: bytes | bytearray | memoryview | str = b""
    ) -> bool:
        """
        Deliver Event.Wakeup with `data` to the connection with this id, from any thread. This is how you push to a connection outside its handler -- a stored Connection is invalid once the handler returns. Requires an http_listen_event() handler to observe it. Returns False if the id is unknown or the wakeup pipe is down.
        """

    def set_idle_timeout(self, ms: int) -> None:
        """
        Close accepted connections idle (no I/O) for `ms` milliseconds. 0 disables (the default). Also reaps idle WebSockets, so use application keepalive for those.
        """

    @property
    def idle_timeout(self) -> int: ...
    def set_request_timeout(self, ms: int) -> None:
        """
        Close accepted connections that buffer a partial request without completing it within `ms` milliseconds (slow-dribble defense). 0 disables (the default). Set generously for large uploads.
        """

    @property
    def request_timeout(self) -> int: ...
    def set_connect_timeout(self, ms: int) -> None:
        """
        Close client-initiated connections that do not finish resolving and connecting within `ms` milliseconds (bounds a hung outbound connect). Defaults to 30000; 0 disables.
        """

    @property
    def connect_timeout(self) -> int: ...
    def set_max_body_size(self, bytes: int) -> None:
        """
        Reject request bodies larger than `bytes` with HTTP 413. An oversized Content-Length is rejected before the body is buffered. 0 disables (the default).
        """

    @property
    def max_body_size(self) -> int: ...
    def set_max_connections(self, n: int) -> None:
        """
        Cap the number of simultaneously accepted connections at `n`. When the cap is reached, newly accepted sockets are closed immediately instead of adopted. 0 disables (the default).
        """

    @property
    def max_connections(self) -> int: ...
    @property
    def num_connections(self) -> int:
        """Current number of live accepted connections."""

    @property
    def metrics(self) -> Metrics:
        """Snapshot of cumulative observability counters (see Metrics)."""

    def set_max_send_buffer(self, bytes: int) -> None:
        """
        Close an accepted connection whose unsent outbound backlog exceeds `bytes` (drops a slow/stalled reader). 0 disables (the default).
        """

    @property
    def max_send_buffer(self) -> int: ...
    def start_drain(self) -> None:
        """
        Begin a graceful shutdown: close every listener (stop accepting) and mark each accepted connection draining so it finishes its current response and then closes. Keep calling poll() until num_connections reaches 0, then stop.
        """

class ShardedManager:
    def __init__(self, num_threads: int = 0) -> None:
        """Create a sharded manager. 0 = use hardware concurrency."""

    def http_listen(self, url: str, handler: object) -> None:
        """
        Listen for HTTP connections. handler(conn, msg) runs on the worker thread owning the connection.
        """

    def http_listen_event(self, url: str, handler: object) -> None:
        """
        Listen with a full event handler, so WebSocket works on the sharded path -- http_listen() only ever delivers HttpMessage. handler(conn, event, data) runs on the worker thread owning the connection, so it must be thread-safe.
        """

    def wakeup(
        self, conn_id: int, data: bytes | bytearray | memoryview | str = b""
    ) -> bool:
        """
        Deliver Event.Wakeup with `data` to the connection with this id, on whichever worker owns it. Thread-safe. Only meaningful between run() and its return; returns False otherwise, or if no worker owns the id.
        """

    def set_connect_timeout(self, ms: int) -> None:
        """
        Connect deadline (ms) for client-initiated connections on every worker. Set before run().
        """

    def run(self) -> None:
        """Start worker threads and acceptor loop. Blocks until stop()."""

    def stop(self) -> None:
        """Signal all workers to stop immediately, abandoning in-flight requests."""

    def drain(self, timeout_ms: int = 5000) -> None:
        """
        Begin a graceful shutdown: stop accepting, let workers finish in-flight responses, then return from run(). Connections still open after timeout_ms are closed (0 = wait indefinitely). Returns immediately; call from another thread while run() is executing.
        """

    def set_idle_timeout(self, ms: int) -> None:
        """
        Close accepted connections idle for `ms` ms on every worker. Set before run(). 0 disables (the default).
        """

    def set_request_timeout(self, ms: int) -> None:
        """
        Request-receive deadline (ms) on every worker. Set before run(). 0 disables (the default).
        """

    def set_max_body_size(self, bytes: int) -> None:
        """
        Maximum request body size (bytes) on every worker; larger bodies get HTTP 413. Set before run(). 0 disables (the default).
        """

    def set_max_connections(self, n: int) -> None:
        """
        Global cap on simultaneously accepted connections across all workers, enforced at the acceptor. Set before run(). 0 disables (the default).
        """

    @property
    def max_connections(self) -> int: ...
    @property
    def num_connections(self) -> int:
        """Current number of live connections across all workers."""

    @property
    def metrics(self) -> Metrics:
        """
        Observability counters aggregated across all workers (see Metrics). Race-free to read while run() is active.
        """

    def set_max_send_buffer(self, bytes: int) -> None:
        """
        Send-buffer high-water mark (bytes) on every worker; a connection whose outbound backlog exceeds it is closed. Set before run(). 0 disables (the default).
        """

    @property
    def num_workers(self) -> int: ...

def base64_encode(input: bytes | bytearray | memoryview | str) -> str:
    """Base64-encode bytes or str (str is encoded UTF-8). Returns str."""

def base64_decode(input: bytes | bytearray | memoryview | str) -> bytes:
    """Base64-decode. Returns bytes."""

def url_encode(input: bytes | bytearray | memoryview | str) -> str:
    """Percent-encode bytes or str (str is encoded UTF-8). Returns str."""

def url_decode(input: bytes | bytearray | memoryview | str) -> str:
    """
    Percent-decode. Returns str, decoded with surrogateescape so a sequence that is not valid UTF-8 does not raise; recover the raw bytes with s.encode('utf-8', 'surrogateescape').
    """

def url_decode_bytes(input: bytes | bytearray | memoryview | str) -> bytes:
    """Percent-decode. Returns the decoded bytes verbatim."""

def set_log_level(level: LogLevel) -> None:
    """Set the nanosrv log verbosity level."""

def get_log_level() -> LogLevel:
    """Get the current nanosrv log verbosity level."""

def millis() -> int:
    """Return current time in milliseconds."""

def tls_available() -> bool:
    """
    Whether this build has a working TLS backend. False in the default build, so https:// and wss:// URLs are not supported and listening on one raises RuntimeError.
    """
