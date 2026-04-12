#pragma once
#include "http.hpp"

enum class WsOpcode : uint8_t {
    Continue = 0, Text = 1, Binary = 2, Close = 8, Ping = 9, Pong = 10
};

#define WEBSOCKET_OP_CONTINUE 0
#define WEBSOCKET_OP_TEXT 1
#define WEBSOCKET_OP_BINARY 2
#define WEBSOCKET_OP_CLOSE 8
#define WEBSOCKET_OP_PING 9
#define WEBSOCKET_OP_PONG 10

namespace nanosrv {

struct WsMessage {
    struct Str data;
    uint8_t flags;
};

struct Connection* ws_connect(struct Mgr*, const char* url,
                                    EventHandler fn, void* fn_data,
                                    const char* fmt, ...);
void ws_upgrade(struct Connection*, struct HttpMessage*,
                   const char* fmt, ...);
size_t ws_send(struct Connection*, const void* buf, size_t len, int op);
size_t ws_wrap(struct Connection*, size_t len, int op);
size_t ws_printf(struct Connection* c, int op, const char* fmt, ...);
size_t ws_vprintf(struct Connection* c, int op, const char* fmt,
                     va_list*);

} // namespace nanosrv
