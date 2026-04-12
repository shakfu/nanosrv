# mungo

A minimal C HTTP/WebSocket server library extracted from [Mongoose 7.21](https://github.com/cesanta/mongoose).

Mongoose is a battle-tested embedded networking library (~33K lines) supporting HTTP, WebSocket, MQTT, TLS, multipart uploads, SSI, OTA updates, and dozens of embedded platforms. mungo strips it down to the core needed for HTTP and WebSocket servers: **~5.5K lines** in a single `mungo.c`/`mungo.h` pair with the same callback-based API.

## What's included

- HTTP/1.1 request/response handling
- WebSocket (RFC 6455) -- text, binary, ping/pong, upgrade
- DNS resolution
- JSON path-based extraction
- Base64 and URL percent-encoding
- SHA-1 (required for WebSocket handshake)
- Event loop (`mg_mgr_poll()`)
- Timers
- Logging

## What's removed

- **Protocols:** MQTT/MQTT5, SNTP, CoAP
- **TLS:** All implementations (OpenSSL, mbedTLS, WolfSSL) -- stubs remain for API compatibility
- **HTTP extras:** directory listing, multipart forms, SSI, OTA updates, range requests
- **Embedded/RTOS:** lwIP, FreeRTOS-TCP, Zephyr, ESP32, ARM Cortex-M, STM32 ethernet drivers
- **Filesystems:** FatFS, packed FS, directory walking
- **Crypto:** SHA-256, UECC, Chacha20

## API

The API is identical to Mongoose. Callbacks receive `(struct mg_connection *, int ev, void *ev_data)` and you dispatch on event codes (`MG_EV_HTTP_MSG`, `MG_EV_WS_MSG`, etc.). Migrating between mungo and full Mongoose is trivial -- swap the header and link target.

## Example

```c
#include "mungo.h"

static void handler(struct mg_connection *c, int ev, void *ev_data) {
    if (ev == MG_EV_HTTP_MSG) {
        mg_http_reply(c, 200, "Content-Type: text/plain\r\n", "Hello\n");
    }
}

int main(void) {
    struct mg_mgr mgr;
    mg_mgr_init(&mgr);
    mg_http_listen(&mgr, "http://0.0.0.0:8080", handler, NULL);
    for (;;) mg_mgr_poll(&mgr, 100);
    mg_mgr_free(&mgr);
    return 0;
}
```

## Building

mungo is built as part of the nanosrv project:

```bash
make server-build    # builds mungo-server along with other targets
```

Or standalone:

```bash
cc -O2 -o mungo-server main.c mungo.c -Wall -Wextra
```

## Use cases

- **Readable reference.** 5.5K lines is auditable in an afternoon; 33K is not. Useful for understanding how callback-based event-loop HTTP servers work at the socket level.
- **Minimal C embedding.** When you need an HTTP endpoint in a C application (status page, JSON API, health check) without pulling in 33K lines of library code.
- **WebSocket test harness.** A known-simple WebSocket server in under 6K lines for testing clients in other languages.
- **Starting point for further extraction.** Strip WebSocket, JSON, or DNS to get an even smaller HTTP-only core.

## Relationship to nanosrv

mungo was the first step in creating nanosrv: extract a minimal C core from Mongoose, then rewrite it as a C++ library (libnanosrv) with typed callbacks, RAII, and sharded multi-threading. libnanosrv is an independent C++ implementation -- it does not link against mungo. mungo remains as a standalone C library.

## License

SPDX-License-Identifier: GPL-2.0-only or commercial

Dual-licensed under the GNU General Public License v2 and a commercial license from [Cesanta](https://www.mongoose.ws/licensing/), matching the upstream Mongoose licensing terms.
