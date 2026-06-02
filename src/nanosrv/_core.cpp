#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <nanosrv/nanosrv.hpp>

#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace nb = nanobind;
using namespace nb::literals;

// ---------------------------------------------------------------------------
// Callback lifetime (fixes H1)
// ---------------------------------------------------------------------------
//
// A Python callable handed to http_listen must stay alive as long as the
// listener. It is captured (by value) into the C++ handler lambda, which the
// event loop owns via the listener connection's fn_data, so the natural
// lifetime is "until the listener closes". We hold the callable in a
// shared_ptr<nb::object> whose ownership is exactly the handler's; no extra
// reference is leaked.
//
// The one subtlety is the GIL: poll()/run() release the GIL, and the listener
// (hence the handler, hence this shared_ptr) may be destroyed while the GIL is
// not held -- e.g. when the Manager is torn down, or if the listener closes
// mid-poll. Destroying an nb::object calls Py_DECREF, which requires the GIL.
// The custom deleter below acquires the GIL before destroying the object, so
// teardown is safe regardless of who drops the last reference. (gil_scoped_acquire
// is reentrant, so this is also fine when the GIL is already held.)
static std::shared_ptr<nb::object> make_callback(nb::object callback)
{
    return std::shared_ptr<nb::object>(new nb::object(std::move(callback)),
                                       [](nb::object* o) {
                                           nb::gil_scoped_acquire acquire;
                                           delete o;
                                       });
}

// ---------------------------------------------------------------------------
// Scoped-validity wrappers (fixes H2)
// ---------------------------------------------------------------------------
//
// Connection, HttpMessage and WsMessage handed to a handler are only valid for
// the duration of that handler call: HttpMessage/WsMessage are transient (the
// struct lives on the C++ stack and its fields point into the connection's recv
// buffer), and the Connection may be freed by the event loop once it closes.
// Previously these were passed to Python as raw non-owning pointers, so a
// handler that stored `conn` or `msg` and touched it later was a use-after-free.
//
// Each wrapper now shares a small "alive" token with the trampoline that created
// it. The trampoline sets the token false as soon as the handler returns, so any
// stored wrapper raises RuntimeError on use instead of dereferencing freed or
// transient memory. To act on a connection after the handler returns, store
// `conn.id` and use Manager.wakeup(id) -- the intended mechanism for that.

using Alive = std::shared_ptr<bool>;

[[noreturn]] static void raise_expired(const char* what)
{
    throw std::runtime_error(
        std::string(what) +
        " used outside its handler scope (it is only valid during the "
        "callback). Keep conn.id and use Manager.wakeup(id) instead.");
}

struct PyConn {
    nanosrv::Connection* c = nullptr;
    Alive alive;
    nanosrv::Connection* get() const
    {
        if (!alive || !*alive)
            raise_expired("Connection");
        return c;
    }
};

struct PyHttpMsg {
    nanosrv::HttpMessage* hm = nullptr;
    Alive alive;
    nanosrv::HttpMessage* get() const
    {
        if (!alive || !*alive)
            raise_expired("HttpMessage");
        return hm;
    }
};

struct PyWsMsg {
    nanosrv::WsMessage* wm = nullptr;
    Alive alive;
    nanosrv::WsMessage* get() const
    {
        if (!alive || !*alive)
            raise_expired("WsMessage");
        return wm;
    }
};

// ---------------------------------------------------------------------------
// Thin wrappers where the C++ API uses varargs or needs argument adaptation
// ---------------------------------------------------------------------------

static void py_http_reply(PyConn& c, int status, const std::string& headers,
                          const std::string& body)
{
    nanosrv::http_reply(c.get(), status, headers.c_str(), "%.*s",
                        static_cast<int>(body.size()), body.data());
}

static void py_http_reply_ref(nanosrv::ConnectionRef& ref, int status,
                              const std::string& headers,
                              const std::string& body)
{
    nanosrv::http_reply(&*ref, status, headers.c_str(), "%.*s",
                        static_cast<int>(body.size()), body.data());
}

static size_t py_ws_send_text(PyConn& c, const std::string& data)
{
    return nanosrv::ws_send(c.get(), data.data(), data.size(), WEBSOCKET_OP_TEXT);
}

static size_t py_ws_send_binary(PyConn& c, nb::bytes data)
{
    return nanosrv::ws_send(c.get(), data.c_str(), data.size(), WEBSOCKET_OP_BINARY);
}

static size_t py_ws_send_op(PyConn& c, nb::bytes data, int op)
{
    return nanosrv::ws_send(c.get(), data.c_str(), data.size(), op);
}

static void py_ws_upgrade(PyConn& c, PyHttpMsg& hm, const std::string& headers)
{
    nanosrv::ws_upgrade(c.get(), hm.get(), "%s", headers.c_str());
}

// Reject TLS URLs up front with a clear Python exception when this build has no
// TLS backend, instead of silently failing later at the (stub) handshake.
static void require_tls_available(std::string_view url)
{
    if (nanosrv::url_is_ssl(std::string(url).c_str()) && !nanosrv::tls_available())
        throw std::runtime_error(
            "TLS is not available in this build; https:// and wss:// URLs are "
            "not supported. Use http:// or ws://, or check nanosrv.tls_available().");
}

static bool py_conn_send_bytes(PyConn& c, std::string_view data)
{
    return c.get()->send_bytes(data);
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

NB_MODULE(_core, m) {
    m.doc() = "nanosrv -- Python bindings for the nanosrv embedded server library";

    // -----------------------------------------------------------------------
    // Enums
    // -----------------------------------------------------------------------
    nb::enum_<nanosrv::Event>(m, "Event")
        .value("Error", nanosrv::Event::Error)
        .value("Open", nanosrv::Event::Open)
        .value("Poll", nanosrv::Event::Poll)
        .value("Resolve", nanosrv::Event::Resolve)
        .value("Connect", nanosrv::Event::Connect)
        .value("Accept", nanosrv::Event::Accept)
        .value("TlsHandshake", nanosrv::Event::TlsHandshake)
        .value("Read", nanosrv::Event::Read)
        .value("Write", nanosrv::Event::Write)
        .value("Close", nanosrv::Event::Close)
        .value("HttpHeaders", nanosrv::Event::HttpHeaders)
        .value("HttpMessage", nanosrv::Event::HttpMessage)
        .value("WsOpen", nanosrv::Event::WsOpen)
        .value("WsMessage", nanosrv::Event::WsMessage)
        .value("WsControl", nanosrv::Event::WsControl)
        .value("Wakeup", nanosrv::Event::Wakeup)
        .value("User", nanosrv::Event::User);

    nb::enum_<WsOpcode>(m, "WsOpcode")
        .value("Continue", WsOpcode::Continue)
        .value("Text", WsOpcode::Text)
        .value("Binary", WsOpcode::Binary)
        .value("Close", WsOpcode::Close)
        .value("Ping", WsOpcode::Ping)
        .value("Pong", WsOpcode::Pong);

    nb::enum_<nanosrv::LogLevel>(m, "LogLevel")
        .value("None", nanosrv::LogLevel::None)
        .value("Error", nanosrv::LogLevel::Error)
        .value("Info", nanosrv::LogLevel::Info)
        .value("Debug", nanosrv::LogLevel::Debug)
        .value("Verbose", nanosrv::LogLevel::Verbose);

    // -----------------------------------------------------------------------
    // Url
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::Url>(m, "Url")
        .def_ro("host", &nanosrv::Url::host)
        .def_ro("port", &nanosrv::Url::port)
        .def_ro("path", &nanosrv::Url::path)
        .def_ro("is_ssl", &nanosrv::Url::is_ssl)
        .def_static("parse", &nanosrv::Url::parse, "url"_a)
        .def("__repr__", [](const nanosrv::Url& u) {
            return std::string("Url(host='") + std::string(u.host) +
                   "', port=" + std::to_string(u.port) +
                   ", path='" + std::string(u.path) +
                   "', is_ssl=" + (u.is_ssl ? "True" : "False") + ")";
        });

    // -----------------------------------------------------------------------
    // HttpMessage (read-only view of an incoming HTTP request/response).
    // Valid only during the handler call that receives it.
    // -----------------------------------------------------------------------
    nb::class_<PyHttpMsg>(m, "HttpMessage")
        .def_prop_ro("method", [](const PyHttpMsg& m) {
            return m.get()->method_str();
        })
        .def_prop_ro("uri", [](const PyHttpMsg& m) {
            return m.get()->uri_str();
        })
        .def_prop_ro("query", [](const PyHttpMsg& m) {
            return m.get()->query_str();
        })
        .def_prop_ro("body", [](const PyHttpMsg& m) {
            return m.get()->body_str();
        })
        .def_prop_ro("status_code", [](const PyHttpMsg& m) {
            return m.get()->status_code();
        })
        .def("header", [](const PyHttpMsg& m, const char* name) {
            return m.get()->header(name);
        }, "name"_a,
           "Return the value of an HTTP header, or None if not present.")
        .def("credentials", [](const PyHttpMsg& m) {
            return m.get()->credentials();
        }, "Return (user, password) from Authorization header.")
        .def("__repr__", [](const PyHttpMsg& m) {
            auto* hm = m.get();
            return std::string("HttpMessage(method='") +
                   std::string(hm->method_str()) + "', uri='" +
                   std::string(hm->uri_str()) + "')";
        });

    // -----------------------------------------------------------------------
    // WsMessage (read-only view of a WebSocket frame).
    // Valid only during the handler call that receives it.
    // -----------------------------------------------------------------------
    nb::class_<PyWsMsg>(m, "WsMessage")
        .def_prop_ro("data", [](const PyWsMsg& m) {
            auto* wm = m.get();
            return std::string_view(wm->data.buf, wm->data.len);
        })
        .def_prop_ro("flags", [](const PyWsMsg& m) -> int {
            return m.get()->flags;
        })
        .def_prop_ro("opcode", [](const PyWsMsg& m) -> WsOpcode {
            return static_cast<WsOpcode>(m.get()->flags & 0x0f);
        });

    // -----------------------------------------------------------------------
    // Connection (passed to event handlers, used to send responses).
    // Valid only during the handler call that receives it; to act on a
    // connection later, keep conn.id and use Manager.wakeup(id).
    // -----------------------------------------------------------------------
    nb::class_<PyConn>(m, "Connection")
        .def_prop_ro("id", [](const PyConn& c) {
            return c.get()->id;
        })
        .def_prop_ro("is_websocket", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_websocket);
        })
        .def_prop_ro("is_listening", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_listening);
        })
        .def_prop_ro("is_client", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_client);
        })
        .def_prop_ro("is_accepted", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_accepted);
        })
        .def_prop_ro("is_tls", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_tls);
        })
        .def_prop_ro("is_closing", [](const PyConn& c) {
            return static_cast<bool>(c.get()->is_closing);
        })
        .def("send_bytes", &py_conn_send_bytes, "data"_a,
             "Send raw bytes on the connection.")
        .def("close", [](PyConn& c) { c.get()->set_closing(); },
             "Mark the connection for closing.")
        .def("http_reply", &py_http_reply,
             "status"_a, "headers"_a = "", "body"_a = "",
             "Send an HTTP response.")
        .def("ws_send_text", &py_ws_send_text, "data"_a,
             "Send a WebSocket text frame.")
        .def("ws_send_binary", &py_ws_send_binary, "data"_a,
             "Send a WebSocket binary frame.")
        .def("ws_send", &py_ws_send_op, "data"_a, "opcode"_a,
             "Send a WebSocket frame with explicit opcode.")
        .def("ws_upgrade", &py_ws_upgrade, "hm"_a, "headers"_a = "",
             "Upgrade an HTTP connection to WebSocket.");

    // -----------------------------------------------------------------------
    // ConnectionRef (non-owning handle returned by listen/connect)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::ConnectionRef>(m, "ConnectionRef")
        .def_prop_ro("id", &nanosrv::ConnectionRef::id)
        .def("__bool__", [](const nanosrv::ConnectionRef& r) {
            return static_cast<bool>(r);
        })
        .def("send_bytes", &nanosrv::ConnectionRef::send_bytes, "data"_a)
        .def("close", &nanosrv::ConnectionRef::set_closing)
        .def("http_reply", &py_http_reply_ref,
             "status"_a, "headers"_a = "", "body"_a = "");

    // -----------------------------------------------------------------------
    // Manager (single-threaded event loop)
    // -----------------------------------------------------------------------
    using HttpHandler = nanosrv::Manager::HttpHandler;
    using HandlerFn = nanosrv::HandlerFn;

    nb::class_<nanosrv::Manager>(m, "Manager")
        .def(nb::init<>())
        .def("poll", [](nanosrv::Manager& mgr, int timeout_ms) {
            nb::gil_scoped_release release;
            mgr.poll(timeout_ms);
        }, "timeout_ms"_a = 1000,
           "Poll the event loop once. Releases the GIL while waiting.")
        .def("http_listen",
             [](nanosrv::Manager& mgr, std::string_view url,
                nb::object callback) -> nanosrv::ConnectionRef {
                 require_tls_available(url);
                 auto cb = make_callback(std::move(callback));

                 HttpHandler handler =
                     [cb](nanosrv::Connection& c, nanosrv::HttpMessage& hm) {
                         nb::gil_scoped_acquire acquire;
                         auto alive = std::make_shared<bool>(true);
                         try {
                             // Pass by value (temporaries): nanobind moves each
                             // wrapper into a Python-owned object that outlives
                             // this call, so a stored wrapper stays valid memory
                             // and is gated only by the shared `alive` token.
                             (*cb)(PyConn{&c, alive}, PyHttpMsg{&hm, alive});
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                         *alive = false;  // invalidate any stored wrappers
                     };
                 return mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen for HTTP connections. handler(conn, msg) is called "
             "for each complete HTTP message.")
        .def("http_listen_event",
             [](nanosrv::Manager& mgr, std::string_view url,
                nb::object callback) -> nanosrv::ConnectionRef {
                 require_tls_available(url);
                 auto cb = make_callback(std::move(callback));

                 HandlerFn handler =
                     [cb](nanosrv::Connection& c, nanosrv::Event ev,
                          void* ev_data) {
                         nb::gil_scoped_acquire acquire;
                         auto alive = std::make_shared<bool>(true);
                         try {
                             if (ev == nanosrv::Event::HttpMessage ||
                                 ev == nanosrv::Event::WsOpen) {
                                 (*cb)(PyConn{&c, alive}, ev,
                                       PyHttpMsg{
                                           static_cast<nanosrv::HttpMessage*>(ev_data),
                                           alive});
                             } else if (ev == nanosrv::Event::WsMessage) {
                                 (*cb)(PyConn{&c, alive}, ev,
                                       PyWsMsg{
                                           static_cast<nanosrv::WsMessage*>(ev_data),
                                           alive});
                             } else {
                                 (*cb)(PyConn{&c, alive}, ev, nb::none());
                             }
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                         *alive = false;  // invalidate any stored wrappers
                     };
                 return mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen with full event handler. handler(conn, event, data) is "
             "called for every event.")
        .def("wakeup", &nanosrv::Manager::wakeup,
             "conn_id"_a, "data"_a = std::string_view{},
             "Wakeup a connection by ID.")
        .def("set_idle_timeout", &nanosrv::Manager::set_idle_timeout, "ms"_a,
             "Close accepted connections idle (no I/O) for `ms` milliseconds. "
             "0 disables (the default). Also reaps idle WebSockets, so use "
             "application keepalive for those.")
        .def_prop_ro("idle_timeout", &nanosrv::Manager::idle_timeout)
        .def("set_request_timeout", &nanosrv::Manager::set_request_timeout,
             "ms"_a,
             "Close accepted connections that buffer a partial request without "
             "completing it within `ms` milliseconds (slow-dribble defense). "
             "0 disables (the default). Set generously for large uploads.")
        .def_prop_ro("request_timeout", &nanosrv::Manager::request_timeout)
        .def("set_max_body_size", &nanosrv::Manager::set_max_body_size,
             "bytes"_a,
             "Reject request bodies larger than `bytes` with HTTP 413. An "
             "oversized Content-Length is rejected before the body is buffered. "
             "0 disables (the default).")
        .def_prop_ro("max_body_size", &nanosrv::Manager::max_body_size)
        .def("set_max_connections", &nanosrv::Manager::set_max_connections,
             "n"_a,
             "Cap the number of simultaneously accepted connections at `n`. "
             "When the cap is reached, newly accepted sockets are closed "
             "immediately instead of adopted. 0 disables (the default).")
        .def_prop_ro("max_connections", &nanosrv::Manager::max_connections)
        .def_prop_ro("num_connections", &nanosrv::Manager::num_connections,
             "Current number of live accepted connections.")
        .def("set_max_send_buffer", &nanosrv::Manager::set_max_send_buffer,
             "bytes"_a,
             "Close an accepted connection whose unsent outbound backlog "
             "exceeds `bytes` (drops a slow/stalled reader). 0 disables (the "
             "default).")
        .def_prop_ro("max_send_buffer", &nanosrv::Manager::max_send_buffer)
        .def("start_drain", &nanosrv::Manager::start_drain,
             "Begin a graceful shutdown: close every listener (stop accepting) "
             "and mark each accepted connection draining so it finishes its "
             "current response and then closes. Keep calling poll() until "
             "num_connections reaches 0, then stop.");

    // -----------------------------------------------------------------------
    // ShardedManager (multi-threaded event loop)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::ShardedManager>(m, "ShardedManager")
        .def(nb::init<unsigned>(), "num_threads"_a = 0,
             "Create a sharded manager. 0 = use hardware concurrency.")
        .def("http_listen",
             [](nanosrv::ShardedManager& mgr, std::string_view url,
                nb::object callback) {
                 require_tls_available(url);
                 auto cb = make_callback(std::move(callback));

                 HttpHandler handler =
                     [cb](nanosrv::Connection& c, nanosrv::HttpMessage& hm) {
                         nb::gil_scoped_acquire acquire;
                         auto alive = std::make_shared<bool>(true);
                         try {
                             (*cb)(PyConn{&c, alive}, PyHttpMsg{&hm, alive});
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                         *alive = false;
                     };
                 mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>())
        .def("run", [](nanosrv::ShardedManager& mgr) {
            nb::gil_scoped_release release;
            mgr.run();
        }, "Start worker threads and acceptor loop. Blocks until stop().")
        .def("stop", &nanosrv::ShardedManager::stop,
             "Signal all workers to stop immediately, abandoning in-flight "
             "requests.")
        .def("drain", &nanosrv::ShardedManager::drain, "timeout_ms"_a = 5000,
             "Begin a graceful shutdown: stop accepting, let workers finish "
             "in-flight responses, then return from run(). Connections still "
             "open after timeout_ms are closed (0 = wait indefinitely). Returns "
             "immediately; call from another thread while run() is executing.")
        .def("set_idle_timeout", &nanosrv::ShardedManager::set_idle_timeout,
             "ms"_a,
             "Close accepted connections idle for `ms` ms on every worker. "
             "Set before run(). 0 disables (the default).")
        .def("set_request_timeout",
             &nanosrv::ShardedManager::set_request_timeout, "ms"_a,
             "Request-receive deadline (ms) on every worker. Set before run(). "
             "0 disables (the default).")
        .def("set_max_body_size",
             &nanosrv::ShardedManager::set_max_body_size, "bytes"_a,
             "Maximum request body size (bytes) on every worker; larger bodies "
             "get HTTP 413. Set before run(). 0 disables (the default).")
        .def("set_max_connections",
             &nanosrv::ShardedManager::set_max_connections, "n"_a,
             "Global cap on simultaneously accepted connections across all "
             "workers, enforced at the acceptor. Set before run(). 0 disables "
             "(the default).")
        .def_prop_ro("max_connections",
             &nanosrv::ShardedManager::max_connections)
        .def_prop_ro("num_connections",
             &nanosrv::ShardedManager::num_connections,
             "Current number of live connections across all workers.")
        .def("set_max_send_buffer",
             &nanosrv::ShardedManager::set_max_send_buffer, "bytes"_a,
             "Send-buffer high-water mark (bytes) on every worker; a connection "
             "whose outbound backlog exceeds it is closed. Set before run(). "
             "0 disables (the default).")
        .def_prop_ro("num_workers", &nanosrv::ShardedManager::num_workers);

    // -----------------------------------------------------------------------
    // Utility functions
    // -----------------------------------------------------------------------

    // Base64
    m.def("base64_encode",
          nb::overload_cast<std::string_view>(&nanosrv::base64_encode),
          "input"_a, "Base64-encode a string.");
    m.def("base64_decode",
          nb::overload_cast<std::string_view>(&nanosrv::base64_decode),
          "input"_a, "Base64-decode a string.");

    // URL encode/decode
    m.def("url_encode",
          nb::overload_cast<std::string_view>(&nanosrv::url_encode),
          "input"_a, "URL-encode a string.");
    m.def("url_decode",
          nb::overload_cast<std::string_view>(&nanosrv::url_decode),
          "input"_a, "URL-decode a string.");

    // JSON (nanosrv::json namespace)
    auto json_mod = m.def_submodule("json", "JSON parsing utilities");
    json_mod.def("number", &nanosrv::json::number, "json"_a, "path"_a,
                 "Extract a number from JSON at the given path.");
    json_mod.def("boolean", &nanosrv::json::boolean, "json"_a, "path"_a,
                 "Extract a boolean from JSON at the given path.");
    json_mod.def("integer", &nanosrv::json::integer, "json"_a, "path"_a,
                 "Extract an integer from JSON at the given path.");
    json_mod.def("string", &nanosrv::json::string, "json"_a, "path"_a,
                 "Extract a string from JSON at the given path.");

    // URL parsing
    // (Url class already registered above with Url.parse static method)

    // Logging
    m.def("set_log_level", [](nanosrv::LogLevel level) {
        nanosrv::log_level = static_cast<int>(level);
    }, "level"_a, "Set the nanosrv log verbosity level.");

    m.def("get_log_level", []() -> nanosrv::LogLevel {
        return static_cast<nanosrv::LogLevel>(nanosrv::log_level);
    }, "Get the current nanosrv log verbosity level.");

    // Time
    m.def("millis", &nanosrv::millis, "Return current time in milliseconds.");
    m.def("tls_available", &nanosrv::tls_available,
          "Whether this build has a working TLS backend. False in the default "
          "build, so https:// and wss:// URLs are not supported and listening "
          "on one raises RuntimeError.");
}
