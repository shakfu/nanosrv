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
    // Idle timeout in milliseconds for accepted connections. 0 = disabled.
    // An accepted connection with no read/write activity for this long is
    // closed by the event loop (defends against connect-and-idle exhaustion).
    int idle_timeout_ms;
    // Request-receive deadline (ms) for accepted connections. 0 = disabled.
    // An accepted connection that has buffered a partial request but not
    // completed it within this window is closed (defends against slow-dribble
    // slowloris, which keeps the idle timer alive by trickling bytes).
    int request_timeout_ms;
    // Connect deadline (ms) for client-initiated connections. 0 = disabled.
    // A client connection that has not finished resolving + connecting within
    // this window is closed (bounds a hung outbound connect()). Defaults to
    // MG_DEFAULT_CONNECT_TIMEOUT_MS.
    int connect_timeout_ms;
    // Maximum request body size in bytes for accepted connections. 0 = disabled.
    // A request whose Content-Length (or de-chunked body) exceeds this is
    // rejected with 413; an oversized Content-Length is rejected before the body
    // is buffered.
    size_t max_body_size;
    // Maximum number of simultaneously accepted connections. 0 = disabled.
    // When num_accepted is at the cap, a freshly accepted socket is closed
    // immediately instead of being adopted (defends against accept-flooding).
    int max_connections;
    // Live count of currently accepted connections on this manager; incremented
    // on accept, decremented on close. Used to enforce max_connections.
    int num_accepted;
    // Send-buffer high-water mark in bytes for accepted connections. 0 = disabled.
    // A connection whose unsent outbound backlog (send.len) exceeds this is
    // closed (defends against a slow/stalled reader tying up send buffering).
    size_t max_send_buffer;
    // Lightweight cumulative observability counters. A Manager's own event loop
    // writes these single-threaded, but ShardedManager sums them across worker
    // threads while those workers run, so they are atomic to keep that read
    // race-free (relaxed ordering is sufficient for monotonic counters). The
    // memset in mgr_init zero-initializes them (valid for integral atomics).
    std::atomic<uint64_t> stat_accepted;      // accepted connections, total
    std::atomic<uint64_t> stat_closed;        // accepted connections closed, total
    std::atomic<uint64_t> stat_errors;        // MG_EV_ERROR events raised, total
    std::atomic<uint64_t> stat_bytes_read;    // bytes received off the wire, total
    std::atomic<uint64_t> stat_bytes_written; // bytes written to the wire, total
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
    uint64_t last_active;   // millis() of last I/O; used for idle timeout
    uint64_t recv_deadline; // deadline to complete a buffered request; 0 = none
    uint64_t connect_deadline; // deadline to finish connecting (client); 0 = none
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

// Evaluate an IP against an ACL string of comma-separated "+net/prefix" /
// "-net/prefix" entries (IPv4 and IPv6). Returns 1 = allowed, 0 = denied,
// negative = malformed ACL. NOTE: this is a building block; ACLs are not yet
// wired into the accept path (no listener enforces one automatically).
int check_ip_acl(struct Str acl, struct Address* remote_ip);
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
// fd is MG_SOCKET_TYPE (not int) so a full-width Windows SOCKET handle is not
// truncated on the way in; it is stored verbatim in Connection::fd.
struct Connection* wrapfd(struct Mgr* mgr, MG_SOCKET_TYPE fd,
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

// Snapshot of a manager's cumulative counters, for lightweight observability
// (health/metrics endpoints, logging). Counts are monotonic over the manager
// lifetime; `active` is an instantaneous gauge.
struct Metrics {
    uint64_t accepted;      // accepted connections, total
    uint64_t closed;        // accepted connections closed, total
    uint64_t errors;        // MG_EV_ERROR events raised, total
    uint64_t bytes_read;    // bytes received off the wire, total
    uint64_t bytes_written; // bytes written to the wire, total
    int active;             // currently live accepted connections
};

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
    // NOTE: the requires-clause must be spelled identically here and at the
    // out-of-class definition in http.hpp -- MSVC matches a constrained member
    // template to its definition by the textual form of the constraint. The
    // HttpHandler alias is expanded to its std::function type so both sites read
    // the same regardless of scope (the definition lives at namespace scope and
    // cannot name the unqualified member alias).
    template<typename F>
        requires (!std::same_as<std::decay_t<F>, HandlerFn>
              && !std::same_as<std::decay_t<F>,
                               std::function<void(Connection&, HttpMessage&)>>)
    ConnectionRef http_listen(std::string_view url, F&& handler);

    // Wakeup a connection by ID
    void wakeup(unsigned long conn_id, std::string_view data = {});

    // Idle timeout (ms) for accepted connections; 0 disables it (the default).
    // An accepted connection with no read/write activity for this long is closed
    // on the next poll. Note: this also reaps idle established connections such
    // as WebSockets, so those should use application-level keepalive (ping/pong).
    void set_idle_timeout(int ms) { mgr_.idle_timeout_ms = ms; }
    int idle_timeout() const { return mgr_.idle_timeout_ms; }

    // Request-receive deadline (ms) for accepted connections; 0 disables it
    // (the default). An accepted connection that buffers a partial request but
    // does not complete it within this window is closed -- this catches the
    // slow-dribble slowloris that an idle timeout alone misses. Set generously
    // if large request bodies are expected, as it bounds total receive time.
    void set_request_timeout(int ms) { mgr_.request_timeout_ms = ms; }
    int request_timeout() const { return mgr_.request_timeout_ms; }

    // Connect deadline (ms) for client-initiated connections; 0 disables it.
    // Defaults to MG_DEFAULT_CONNECT_TIMEOUT_MS. A client connection that has
    // not finished resolving + connecting within this window is closed, which
    // bounds a hung outbound connect() to an unreachable or black-holed peer.
    void set_connect_timeout(int ms) { mgr_.connect_timeout_ms = ms; }
    int connect_timeout() const { return mgr_.connect_timeout_ms; }

    // Maximum request body size in bytes for accepted connections; 0 disables
    // it (the default). A request advertising a larger Content-Length is
    // rejected with 413 before its body is buffered; a chunked body exceeding
    // the cap is rejected after de-chunking.
    void set_max_body_size(size_t bytes) { mgr_.max_body_size = bytes; }
    size_t max_body_size() const { return mgr_.max_body_size; }

    // Maximum number of simultaneously accepted connections; 0 disables it
    // (the default). When the cap is reached, newly accepted sockets are closed
    // immediately rather than adopted into the event loop.
    void set_max_connections(int n) { mgr_.max_connections = n; }
    int max_connections() const { return mgr_.max_connections; }
    // Current number of live accepted connections.
    int num_connections() const { return mgr_.num_accepted; }

    // Snapshot the cumulative observability counters (see struct Metrics).
    // Cheap; safe to call from a handler or between polls on the loop thread.
    struct Metrics metrics() const {
        using std::memory_order_relaxed;
        return Metrics{mgr_.stat_accepted.load(memory_order_relaxed),
                       mgr_.stat_closed.load(memory_order_relaxed),
                       mgr_.stat_errors.load(memory_order_relaxed),
                       mgr_.stat_bytes_read.load(memory_order_relaxed),
                       mgr_.stat_bytes_written.load(memory_order_relaxed),
                       mgr_.num_accepted};
    }

    // Send-buffer high-water mark in bytes for accepted connections; 0 disables
    // it (the default). A connection whose unsent outbound backlog exceeds this
    // is closed on the next poll (drops a slow/stalled reader).
    void set_max_send_buffer(size_t bytes) { mgr_.max_send_buffer = bytes; }
    size_t max_send_buffer() const { return mgr_.max_send_buffer; }

    // Begin a graceful shutdown: close every listener (stop accepting) and mark
    // every accepted connection draining, so it finishes flushing its current
    // response and then closes. After calling this, keep driving poll() until
    // num_connections() reaches 0 (optionally with your own deadline), then stop.
    // Idempotent; safe to call from a handler or between polls.
    void start_drain() {
        for (struct Connection* c = mgr_.conns; c != nullptr; c = c->next) {
            if (c->is_listening)
                c->is_closing = 1;            // stop accepting new connections
            else if (c->is_accepted && !c->is_closing)
                c->is_draining = 1;           // finish in-flight, then close
        }
    }

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

    // Signal all workers to stop immediately, abandoning in-flight requests
    // (call from a signal handler or another thread).
    void stop();

    // Begin a graceful shutdown: stop accepting new connections, let workers
    // finish flushing in-flight responses, then return from run(). Any connection
    // still open after timeout_ms is closed (0 = wait indefinitely). Safe to call
    // from a signal handler or another thread; returns immediately -- run() carries
    // out the drain and returns once it completes.
    void drain(int timeout_ms = 5000);

    // Idle timeout (ms) for accepted connections, applied to every worker.
    // Set before run() so the value is visible to worker threads without a race.
    void set_idle_timeout(int ms) {
        for (auto& w : workers_)
            w->set_idle_timeout(ms);
    }

    // Request-receive deadline (ms), applied to every worker. Set before run().
    void set_request_timeout(int ms) {
        for (auto& w : workers_)
            w->set_request_timeout(ms);
    }

    // Maximum request body size (bytes), applied to every worker. Set before run().
    void set_max_body_size(size_t bytes) {
        for (auto& w : workers_)
            w->set_max_body_size(bytes);
    }

    // Global cap on simultaneously accepted connections across all workers,
    // enforced at the acceptor thread. 0 disables it. Set before run().
    void set_max_connections(int n) { max_connections_ = n; }
    int max_connections() const { return max_connections_; }
    // Current number of live connections handed off and not yet closed.
    int num_connections() const {
        return live_conns_.load(std::memory_order_relaxed);
    }

    // Send-buffer high-water mark (bytes), applied to every worker. Set before
    // run(). A connection whose outbound backlog exceeds this is closed.
    void set_max_send_buffer(size_t bytes) {
        for (auto& w : workers_)
            w->set_max_send_buffer(bytes);
    }

    unsigned num_workers() const { return static_cast<unsigned>(workers_.size()); }

    // Aggregate observability counters summed across all workers. The per-worker
    // counters are atomic, so this is race-free to call while run() is active.
    // `active` reflects the acceptor's live-connection gauge (num_connections()),
    // which is authoritative across the hand-off boundary.
    struct Metrics metrics() const {
        struct Metrics total{};
        for (auto& w : workers_) {
            struct Metrics m = w->metrics();
            total.accepted += m.accepted;
            total.closed += m.closed;
            total.errors += m.errors;
            total.bytes_read += m.bytes_read;
            total.bytes_written += m.bytes_written;
        }
        total.active = num_connections();
        return total;
    }

private:
    // Wake the acceptor and worker loops if their pipes are initialized.
    void wake_all();

    std::vector<std::unique_ptr<Manager>> workers_;
    std::vector<std::thread> threads_;
    Manager acceptor_;
    std::atomic<bool> running_{false};
    // Set true for the duration of run(); the destructor waits on this so the
    // object is not torn down while run() is still executing on another thread.
    std::atomic<bool> run_active_{false};
    std::atomic<unsigned> next_{0};
    std::shared_ptr<std::vector<WorkerQueue>> queues_;
    std::shared_ptr<HttpHandler> handler_;
    // Global accepted-connection cap, enforced at the single acceptor thread.
    // max_connections_ is the limit (0 = disabled); live_conns_ is the count of
    // connections handed off and not yet closed by a worker. The acceptor reads
    // and increments; workers decrement on close. Single producer (the acceptor)
    // so the check-then-increment cannot over-admit.
    int max_connections_{0};
    std::atomic<int> live_conns_{0};
    // Graceful-drain state, consumed by run()'s loops. draining_ flips the
    // acceptor to reject new connections and the workers to mark their
    // connections draining; drain_deadline_ (millis, 0 = none) bounds the wait.
    std::atomic<bool> draining_{false};
    std::atomic<uint64_t> drain_deadline_{0};
    // Set (release) by run() once every wakeup pipe is initialized, cleared after
    // the loops exit. stop()/drain() read it (acquire) before touching a worker's
    // pipe via wake_mgr(), so the pipe write in wakeup_init() happens-before any
    // cross-thread wake. While false, the wake is skipped -- the loops still react
    // on their next fallback poll.
    std::atomic<bool> pipes_ready_{false};
};

} // namespace nanosrv
