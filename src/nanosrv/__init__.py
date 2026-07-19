"""
nanosrv - Python bindings for the nanosrv embedded server library.

Example usage:
    >>> import nanosrv
    >>> mgr = nanosrv.Manager()
    >>> mgr.http_listen("http://0.0.0.0:8080", lambda conn, msg: (
    ...     conn.http_reply(200, "", f"Hello from nanosrv! You requested {msg.uri}")
    ... ))
    >>> # mgr.poll(1000)  # call in a loop to drive the event loop
"""

from nanosrv._core import (
    # Enums
    Event,
    WsOpcode,
    LogLevel,
    # Core classes
    Manager,
    ShardedManager,
    Connection,
    ConnectionRef,
    # HTTP / WebSocket data
    HttpMessage,
    WsMessage,
    # URL parsing
    Url,
    # Utilities
    base64_encode,
    base64_decode,
    url_encode,
    url_decode,
    set_log_level,
    get_log_level,
    millis,
    tls_available,
    # JSON submodule
    json,
)

__all__ = [
    "Event",
    "WsOpcode",
    "LogLevel",
    "Manager",
    "ShardedManager",
    "Connection",
    "ConnectionRef",
    "HttpMessage",
    "WsMessage",
    "Url",
    "base64_encode",
    "base64_decode",
    "url_encode",
    "url_decode",
    "set_log_level",
    "get_log_level",
    "millis",
    "tls_available",
    "json",
]
__version__ = "0.2.0"
