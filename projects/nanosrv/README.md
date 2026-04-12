# libnanosrv

A C++23 HTTP/WebSocket server library. This is the core library that nanosrv-server, nanosrv-sharded, and the Python bindings all link against.

libnanosrv is an independent C++ implementation inspired by Mongoose's event-loop architecture but rewritten from scratch with typed callbacks, RAII resource management, and a sharded multi-threading model.

## Features

- HTTP/1.1 request/response with `std::function<void(Connection&, HttpMessage&)>` callbacks
- WebSocket (RFC 6455) -- text, binary, ping/pong, upgrade
- `Manager` -- single-threaded event loop with `poll()`
- `ShardedManager` -- accept-and-hand-off architecture with N worker threads
- `Connection` and `ConnectionRef` with RAII lifetime management
- Typed enums (`Event`, `WsOpcode`, `LogLevel`) instead of raw integer constants
- DNS resolution, JSON path extraction, Base64, URL encoding
- `std::chrono` duration support for timeouts

## Public headers

The public API is in `include/nanosrv/`:

| Header | Contents |
|---|---|
| `nanosrv.hpp` | Umbrella include |
| `net.hpp` | `Manager`, `ShardedManager`, `Connection`, `ConnectionRef`, `Event` enum |
| `http.hpp` | `HttpMessage`, `http_listen()`, `http_reply()`, `http_get_header()` |
| `ws.hpp` | `WsMessage`, `ws_upgrade()`, `ws_send()`, `WsOpcode` enum |
| `json.hpp` | `json_get_num()`, `json_get_str()`, `json_get_bool()`, `json_get_long()` |
| `base64.hpp` | `base64_encode()`, `base64_decode()` |
| `url.hpp` | `Url` parse result with host, port, path, is_ssl |
| `log.hpp` | `log_level`, `LogLevel` enum |
| `types.hpp` | Core types: `Str`, `IOBuf`, `Address` |
| `timer.hpp` | Timer utilities |

## Usage

```cpp
#include "nanosrv/nanosrv.hpp"

using namespace nanosrv;

Manager mgr;
mgr.http_listen("http://0.0.0.0:8080",
    [](Connection& c, HttpMessage& hm) {
        http_reply(&c, 200, "Content-Type: text/plain\r\n",
                   "Hello from %.*s\n",
                   static_cast<int>(hm.uri_str().size()),
                   hm.uri_str().data());
    });

while (running) {
    mgr.poll(std::chrono::milliseconds(100));
}
```

## Building

Built as a static library (`libnanosrv.a`) via the top-level CMake project:

```bash
make server-build
```

The library is at `build/cmake/nanosrv/libnanosrv.a`. Link against it and add `include/` to your include path.

## Design

- **No dependency on mungo.** libnanosrv is a full C++ rewrite -- it does not link against the C extraction. The networking, HTTP parsing, and WebSocket framing are all reimplemented in C++.
- **Mongoose-compatible constants.** The `MG_EV_*` integer constants are defined alongside the typed `Event` enum for interoperability with code that uses the C-style dispatch pattern.
- **C++23.** Uses concepts, `std::chrono` literals, `std::string_view`, and other modern C++ facilities.
