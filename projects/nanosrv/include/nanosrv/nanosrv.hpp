// nanosrv -- C++ wrapper around the Mongoose embedded networking library
// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

// Forward-declare mongoose types so callers don't need the C header.
struct mg_mgr;
struct mg_connection;
struct mg_http_message;
struct mg_ws_message;
struct mg_str;

// Re-export websocket opcodes from mongoose (C macros)
#ifndef WEBSOCKET_OP_TEXT
#define WEBSOCKET_OP_CONTINUE 0
#define WEBSOCKET_OP_TEXT 1
#define WEBSOCKET_OP_BINARY 2
#define WEBSOCKET_OP_CLOSE 8
#define WEBSOCKET_OP_PING 9
#define WEBSOCKET_OP_PONG 10
#endif

// Typed enum mirror for Python bindings
enum class WsOpcode : int {
    Continue = WEBSOCKET_OP_CONTINUE,
    Text     = WEBSOCKET_OP_TEXT,
    Binary   = WEBSOCKET_OP_BINARY,
    Close    = WEBSOCKET_OP_CLOSE,
    Ping     = WEBSOCKET_OP_PING,
    Pong     = WEBSOCKET_OP_PONG,
};

namespace nanosrv {

// -------------------------------------------------------------------------
// Enums
// -------------------------------------------------------------------------

enum class Event : int {
    Error        = 0,   // MG_EV_ERROR
    Open         = 1,   // MG_EV_OPEN
    Poll         = 2,   // MG_EV_POLL
    Resolve      = 3,   // MG_EV_RESOLVE
    Connect      = 4,   // MG_EV_CONNECT
    Accept       = 5,   // MG_EV_ACCEPT
    TlsHandshake = 6,  // MG_EV_TLS_HS
    Read         = 7,   // MG_EV_READ
    Write        = 8,   // MG_EV_WRITE
    Close        = 9,   // MG_EV_CLOSE
    HttpHeaders  = 10,  // MG_EV_HTTP_HDRS
    HttpMessage  = 11,  // MG_EV_HTTP_MSG
    WsOpen       = 12,  // MG_EV_WS_OPEN
    WsMessage    = 13,  // MG_EV_WS_MSG
    WsControl    = 14,  // MG_EV_WS_CTL
    Wakeup       = 19,  // MG_EV_WAKEUP
    User         = 22,  // MG_EV_USER
};

enum class LogLevel : int {
    None    = 0,
    Error   = 1,
    Info    = 2,
    Debug   = 3,
    Verbose = 4,
};

// Exposed so bindings can set it directly.
extern int log_level;

// -------------------------------------------------------------------------
// Lightweight value types
// -------------------------------------------------------------------------

struct Url {
    std::string host;
    unsigned short port{0};
    std::string path;
    bool is_ssl{false};

    static Url parse(std::string_view url);
};

// -------------------------------------------------------------------------
// Read-only views into mongoose message structs
// -------------------------------------------------------------------------

// Wraps mg_http_message (does NOT own memory -- valid only during callback).
struct HttpMessage {
    mg_http_message* raw{nullptr};

    std::string_view method_str() const;
    std::string_view uri_str() const;
    std::string_view query_str() const;
    std::string_view body_str() const;
    int status_code() const;
    std::optional<std::string_view> header(std::string_view name) const;
    std::pair<std::string, std::string> credentials() const;
};

// Wraps mg_ws_message (does NOT own memory -- valid only during callback).
struct WsMessage {
    mg_str data;
    uint8_t flags;
};

// -------------------------------------------------------------------------
// Connection (thin wrapper around mg_connection*)
// -------------------------------------------------------------------------

using Connection = mg_connection;

// Non-owning handle returned by listen/connect.  Stores the connection id
// and the owning manager so we can look the connection up later.
class ConnectionRef {
public:
    ConnectionRef() = default;
    ConnectionRef(mg_mgr* mgr, unsigned long id) : mgr_(mgr), id_(id) {}

    explicit operator bool() const { return mgr_ != nullptr && id_ != 0; }
    unsigned long id() const { return id_; }

    Connection* operator*() const;
    Connection* operator->() const { return **this ? *(*this) : nullptr; }

    void send_bytes(std::string_view data) const;
    void set_closing() const;

private:
    mg_mgr* mgr_{nullptr};
    unsigned long id_{0};

    mg_connection* find() const;
};

// -------------------------------------------------------------------------
// Free functions that operate on Connection / HttpMessage
// -------------------------------------------------------------------------

using HandlerFn = std::function<void(Connection&, Event, void*)>;

void http_reply(Connection* c, int status, const char* headers,
                const char* fmt, ...);
size_t ws_send(Connection* c, const void* buf, size_t len, int op);
void ws_upgrade(Connection* c, HttpMessage* hm, const char* fmt, ...);

// -------------------------------------------------------------------------
// Manager (single-threaded event loop)
// -------------------------------------------------------------------------

class Manager {
public:
    using HttpHandler = std::function<void(Connection&, HttpMessage&)>;

    Manager();
    ~Manager();

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;

    void poll(int timeout_ms = 1000);

    // Listen for HTTP requests.  The simple handler receives only
    // MG_EV_HTTP_MSG events.
    ConnectionRef http_listen(std::string_view url, HttpHandler handler);

    // Listen with a full event handler.
    ConnectionRef http_listen(std::string_view url, HandlerFn handler);

    // Wake up a connection by id.
    bool wakeup(unsigned long conn_id,
                std::string_view data = std::string_view{});

    mg_mgr* raw() { return mgr_; }

private:
    mg_mgr* mgr_;
};

// -------------------------------------------------------------------------
// ShardedManager (multi-threaded: one acceptor + N workers)
// -------------------------------------------------------------------------

class ShardedManager {
public:
    using HttpHandler = Manager::HttpHandler;

    explicit ShardedManager(unsigned num_threads = 0);
    ~ShardedManager();

    ShardedManager(const ShardedManager&) = delete;
    ShardedManager& operator=(const ShardedManager&) = delete;

    void http_listen(std::string_view url, HttpHandler handler);
    void run();   // blocks until stop()
    void stop();
    unsigned num_workers() const { return static_cast<unsigned>(workers_.size()); }

private:
    struct Worker {
        mg_mgr* mgr{nullptr};
        std::thread thread;
        bool running{false};
    };

    std::string listen_url_;
    HttpHandler handler_;
    std::vector<Worker> workers_;
    mg_mgr* acceptor_mgr_{nullptr};
    bool stopping_{false};
};

// -------------------------------------------------------------------------
// Utility functions
// -------------------------------------------------------------------------

std::string base64_encode(std::string_view input);
std::string base64_decode(std::string_view input);
std::string url_encode(std::string_view input);
std::string url_decode(std::string_view input);
uint64_t millis();

namespace json {
    std::optional<double> number(std::string_view json, std::string_view path);
    std::optional<bool> boolean(std::string_view json, std::string_view path);
    std::optional<long> integer(std::string_view json, std::string_view path);
    std::optional<std::string> string(std::string_view json,
                                      std::string_view path);
}  // namespace json

}  // namespace nanosrv
