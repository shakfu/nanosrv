#pragma once
#include "types.hpp"
#include "timer.hpp"
#include "fmt.hpp"

#include <atomic>
#include <chrono>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <vector>

#define MG_TIMER_ONCE 0
#define MG_TIMER_REPEAT 1
#define MG_TIMER_RUN_NOW 2
#define MG_TIMER_CALLED 4
#define MG_TIMER_AUTODELETE 8

namespace nanosrv {

struct Connection;
struct HttpMessage;

typedef void (*EventHandler)(struct Connection*, int ev, void* ev_data);

enum class Event : int {
    Error = 0, Open, Poll, Resolve, Connect, Accept, TlsHandshake,
    Read, Write, Close, HttpHeaders, HttpMessage,
    WsOpen, WsMessage, WsControl,
    Wakeup = 20, User = 100
};

enum {
    MG_EV_ERROR,
    MG_EV_OPEN,
    MG_EV_POLL,
    MG_EV_RESOLVE,
    MG_EV_CONNECT,
    MG_EV_ACCEPT,
    MG_EV_TLS_HS,
    MG_EV_READ,
    MG_EV_WRITE,
    MG_EV_CLOSE,
    MG_EV_HTTP_HDRS,
    MG_EV_HTTP_MSG,
    MG_EV_WS_OPEN,
    MG_EV_WS_MSG,
    MG_EV_WS_CTL,
    MG_EV_WAKEUP = 20,
    MG_EV_USER = 100
};

struct Address {
    union {
        uint8_t ip[16];
        uint32_t ip4;
        uint64_t ip6[2];
    } addr;
    uint16_t port;
    uint8_t scope_id;
    bool is_ip6;
};

struct DnsConfig {
    const char* url;
    struct Connection* c;
};

struct DnsMessage {
    uint16_t txnid;
    bool resolved;
    struct Address addr;
    char name[256];
};

struct DnsHeader {
    uint16_t txnid;
    uint16_t flags;
    uint16_t num_questions;
    uint16_t num_answers;
    uint16_t num_authority_prs;
    uint16_t num_other_prs;
};

struct DnsRR {
    uint16_t nlen;
    uint16_t atype;
    uint16_t aclass;
    uint16_t alen;
};

struct Mgr {
    struct Connection* conns;
    struct DnsConfig dns4;
    struct DnsConfig dns6;
    int dnstimeout;
    bool use_dns6;
    unsigned long nextid;
    void* userdata;
    void* tls_ctx;
    uint16_t mqtt_id;
    void* active_dns_requests;
    struct Timer* timers;
    int epoll_fd;
    void* uring;       // io_uring ring (struct io_uring*), NULL if not using io_uring
    void* conn_pool;   // Free-list of recycled Connection objects
    int conn_pool_size; // Current pool size
    void* ifp;
    size_t extraconnsize;
    MG_SOCKET_TYPE pipe;
};

struct Connection {
    struct Connection* next;
    struct Mgr* mgr;
    struct Address loc;
    struct Address rem;
    void* fd;
    unsigned long id;
    struct IOBuffer recv;
    struct IOBuffer send;
    struct IOBuffer prof;
    struct IOBuffer rtls;
    EventHandler fn;
    void* fn_data;
    EventHandler pfn;
    void* pfn_data;
    char data[MG_DATA_SIZE];
    void* tls;
    unsigned is_listening : 1;
    unsigned is_client : 1;
    unsigned is_accepted : 1;
    unsigned is_resolving : 1;
    unsigned is_arplooking : 1;
    unsigned is_connecting : 1;
    unsigned is_tls : 1;
    unsigned is_tls_hs : 1;
    unsigned is_udp : 1;
    unsigned is_websocket : 1;
    unsigned is_mqtt5 : 1;
    unsigned is_hexdumping : 1;
    unsigned is_draining : 1;
    unsigned is_closing : 1;
    unsigned is_full : 1;
    unsigned is_tls_throttled : 1;
    unsigned is_resp : 1;
    unsigned is_readable : 1;
    unsigned is_writable : 1;

    // -- Modern C++ API methods --
    bool send_bytes(std::string_view data);
    size_t write_fmt(const char* fmt, ...);
    void set_closing() { is_closing = 1; }
};

void call(struct Connection* c, int ev, void* ev_data);
void error(struct Connection* c, const char* fmt, ...);

void mgr_poll(struct Mgr*, int ms);
void mgr_init(struct Mgr*);
void mgr_free(struct Mgr*);

[[nodiscard]] struct Connection* listen_(struct Mgr*, const char* url,
                                EventHandler fn, void* fn_data);
[[nodiscard]] bool send_data(struct Connection*, const void*, size_t);
size_t conn_printf(struct Connection*, const char* fmt, ...);
size_t conn_vprintf(struct Connection*, const char* fmt, va_list* ap);
bool aton(struct Str str, struct Address* addr);
bool wakeup(struct Mgr*, unsigned long id, const void* buf, size_t len);

[[nodiscard]] struct Connection* alloc_conn(struct Mgr*);
void close_conn(struct Connection* c);
[[nodiscard]] bool open_listener(struct Connection* c, const char* url);

// Detach the socket FD from a connection without closing it.
// Removes the FD from kqueue/epoll and sets c->fd to MG_INVALID_SOCKET.
// Returns the raw FD that was detached.  Caller takes ownership.
MG_SOCKET_TYPE detach_fd(struct Connection* c);

[[nodiscard]] struct Connection* connect(struct Mgr*, const char* url,
                                 EventHandler fn, void* fn_data);
[[nodiscard]] struct Connection* connect_svc(struct Mgr*, const char* url,
                                     EventHandler fn, void* fn_data,
                                     EventHandler pfn, void* pfn_data);
void connect_resolved(struct Connection*);
struct Connection* wrapfd(struct Mgr* mgr, int fd,
                                EventHandler fn, void* fn_data);
long io_recv(struct Connection* c, void* buf, size_t len);
long io_send(struct Connection* c, const void* buf, size_t len);

void resolve(struct Connection*, const char* url);
void resolve_cancel(struct Connection*);
bool dns_parse(const uint8_t* buf, size_t len, struct DnsMessage*);
size_t dns_parse_rr(const uint8_t* buf, size_t len, size_t ofs,
                       bool is_question, struct DnsRR*);

struct Timer* timer_add(struct Mgr* mgr, uint64_t milliseconds,
                              unsigned flags, void (*fn)(void*), void* arg);

// General event handler: receives typed Event instead of raw int.
// The void* ev_data depends on the event type (e.g., HttpMessage* for HttpMessage).
using HandlerFn = std::function<void(Connection&, Event, void*)>;

// Non-owning handle to a Connection managed by the event loop.
// Provides a safe interface -- the connection may be closed at any time
// by the event loop, so this should not be stored beyond the current
// event handler scope.
class ConnectionRef {
public:
    ConnectionRef() = default;
    explicit ConnectionRef(Connection* c) : c_(c) {}

    explicit operator bool() const { return c_ != nullptr; }
    Connection* get() const { return c_; }
    Connection& operator*() const { return *c_; }
    Connection* operator->() const { return c_; }

    // Convenience: common operations
    bool send_bytes(std::string_view data) { return c_->send_bytes(data); }
    size_t write_fmt(const char* fmt, ...);
    void set_closing() { c_->set_closing(); }

    unsigned long id() const { return c_->id; }

private:
    Connection* c_ = nullptr;
};

// Legacy handler with int event code (for backward compatibility)
using RawHandlerFn = std::function<void(Connection&, int, void*)>;

// RAII wrapper around Mgr. Calls mgr_init on construction,
// mgr_free on destruction. Non-copyable, non-movable (connections
// hold back-pointers to the manager).
class Manager {
public:
    Manager() { mgr_init(&mgr_); }
    ~Manager() { mgr_free(&mgr_); }

    Manager(const Manager&) = delete;
    Manager& operator=(const Manager&) = delete;
    Manager(Manager&&) = delete;
    Manager& operator=(Manager&&) = delete;

    // Access the underlying Mgr (needed by all C-level APIs)
    struct Mgr* raw() { return &mgr_; }
    const struct Mgr* raw() const { return &mgr_; }

    // Convenience: poll the event loop
    void poll(int timeout_ms) { mgr_poll(&mgr_, timeout_ms); }

    void poll(std::chrono::milliseconds timeout) { mgr_poll(&mgr_, static_cast<int>(timeout.count())); }

    // std::function overloads (type-erased, slight overhead from virtual call)
    using HttpHandler = std::function<void(Connection&, HttpMessage&)>;
    ConnectionRef http_listen(std::string_view url, HandlerFn handler);
    ConnectionRef http_listen(std::string_view url, HttpHandler handler);
    ConnectionRef connect(std::string_view url, HandlerFn handler);

    // Template overload declared in http.hpp (needs http_listen free function visible)
    template<typename F>
        requires (!std::same_as<std::decay_t<F>, HandlerFn>
              && !std::same_as<std::decay_t<F>, HttpHandler>)
    ConnectionRef http_listen(std::string_view url, F&& handler);

    // Wakeup a connection by ID
    void wakeup(unsigned long conn_id, std::string_view data = {});

private:
    struct Mgr mgr_;
};

// Multi-threaded sharded event loop with accept-and-hand-off.
// A single acceptor thread accepts connections and distributes the FDs
// round-robin to worker threads. Each worker runs its own single-threaded
// event loop and adopts handed-off connections.
struct WorkerQueue;  // defined in sharded.cpp

class ShardedManager {
public:
    using HttpHandler = Manager::HttpHandler;

    explicit ShardedManager(unsigned num_threads = 0);  // 0 = hardware_concurrency
    ~ShardedManager();

    ShardedManager(const ShardedManager&) = delete;
    ShardedManager& operator=(const ShardedManager&) = delete;

    // Listen for HTTP connections. Workers handle accepted connections.
    void http_listen(std::string_view url, HttpHandler handler);

    // Start worker threads + acceptor loop. Blocks until stop() is called.
    void run();

    // Signal all workers to stop (call from signal handler or another thread).
    void stop();

    unsigned num_workers() const { return static_cast<unsigned>(workers_.size()); }

private:
    std::vector<std::unique_ptr<Manager>> workers_;
    std::vector<std::thread> threads_;
    Manager acceptor_;
    std::atomic<bool> running_{false};
    std::atomic<unsigned> next_{0};
    std::shared_ptr<std::vector<WorkerQueue>> queues_;
    std::shared_ptr<HttpHandler> handler_;
};

} // namespace nanosrv
