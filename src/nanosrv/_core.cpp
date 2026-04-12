#include <nanobind/nanobind.h>
#include <nanobind/stl/function.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/pair.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>

#include <nanosrv/nanosrv.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace nb = nanobind;
using namespace nb::literals;

// ---------------------------------------------------------------------------
// Thin wrappers where the C++ API uses varargs or needs GIL management
// ---------------------------------------------------------------------------

static void py_http_reply(nanosrv::Connection& c, int status,
                          const std::string& headers,
                          const std::string& body) {
    nanosrv::http_reply(&c, status, headers.c_str(), "%.*s",
                      static_cast<int>(body.size()), body.data());
}

static void py_http_reply_ref(nanosrv::ConnectionRef& ref, int status,
                              const std::string& headers,
                              const std::string& body) {
    py_http_reply(*ref, status, headers, body);
}

static size_t py_ws_send_text(nanosrv::Connection& c, const std::string& data) {
    return nanosrv::ws_send(&c, data.data(), data.size(), WEBSOCKET_OP_TEXT);
}

static size_t py_ws_send_binary(nanosrv::Connection& c, nb::bytes data) {
    return nanosrv::ws_send(&c, data.c_str(), data.size(), WEBSOCKET_OP_BINARY);
}

static size_t py_ws_send_op(nanosrv::Connection& c, nb::bytes data, int op) {
    return nanosrv::ws_send(&c, data.c_str(), data.size(), op);
}

static void py_ws_upgrade(nanosrv::Connection& c, nanosrv::HttpMessage& hm,
                          const std::string& headers) {
    nanosrv::ws_upgrade(&c, &hm, "%s", headers.c_str());
}

// ---------------------------------------------------------------------------
// Module definition
// ---------------------------------------------------------------------------

NB_MODULE(_core, m) {
    m.doc() = "pynanosrv -- Python bindings for the nanosrv embedded server library";

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
    // HttpMessage (read-only view of an incoming HTTP request/response)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::HttpMessage>(m, "HttpMessage")
        .def_prop_ro("method", &nanosrv::HttpMessage::method_str)
        .def_prop_ro("uri", &nanosrv::HttpMessage::uri_str)
        .def_prop_ro("query", &nanosrv::HttpMessage::query_str)
        .def_prop_ro("body", &nanosrv::HttpMessage::body_str)
        .def_prop_ro("status_code", &nanosrv::HttpMessage::status_code)
        .def("header", &nanosrv::HttpMessage::header, "name"_a,
             "Return the value of an HTTP header, or None if not present.")
        .def("credentials", &nanosrv::HttpMessage::credentials,
             "Return (user, password) from Authorization header.")
        .def("__repr__", [](const nanosrv::HttpMessage& hm) {
            return std::string("HttpMessage(method='") +
                   std::string(hm.method_str()) + "', uri='" +
                   std::string(hm.uri_str()) + "')";
        });

    // -----------------------------------------------------------------------
    // WsMessage (read-only view of a WebSocket frame)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::WsMessage>(m, "WsMessage")
        .def_prop_ro("data", [](const nanosrv::WsMessage& wm) {
            return std::string_view(wm.data.buf, wm.data.len);
        })
        .def_prop_ro("flags", [](const nanosrv::WsMessage& wm) -> int {
            return wm.flags;
        })
        .def_prop_ro("opcode", [](const nanosrv::WsMessage& wm) -> WsOpcode {
            return static_cast<WsOpcode>(wm.flags & 0x0f);
        });

    // -----------------------------------------------------------------------
    // Connection (passed to event handlers, used to send responses)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::Connection>(m, "Connection")
        .def_prop_ro("id", [](const nanosrv::Connection& c) {
            return c.id;
        })
        .def_prop_ro("is_websocket", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_websocket);
        })
        .def_prop_ro("is_listening", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_listening);
        })
        .def_prop_ro("is_client", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_client);
        })
        .def_prop_ro("is_accepted", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_accepted);
        })
        .def_prop_ro("is_tls", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_tls);
        })
        .def_prop_ro("is_closing", [](const nanosrv::Connection& c) {
            return static_cast<bool>(c.is_closing);
        })
        .def("send_bytes", &nanosrv::Connection::send_bytes, "data"_a,
             "Send raw bytes on the connection.")
        .def("close", &nanosrv::Connection::set_closing,
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
                 // Prevent the Python callback from being garbage-collected
                 // while the listener is alive. We leak one ref; the C++
                 // side will call the captured shared_ptr destructor when
                 // the listener connection closes.
                 auto shared_cb = std::make_shared<nb::object>(std::move(callback));
                 shared_cb->inc_ref();  // prevent GC

                 HttpHandler handler =
                     [shared_cb](nanosrv::Connection& c,
                                 nanosrv::HttpMessage& hm) {
                         nb::gil_scoped_acquire acquire;
                         try {
                             (*shared_cb)(&c, &hm);
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                     };
                 return mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen for HTTP connections. handler(conn, msg) is called "
             "for each complete HTTP message.")
        .def("http_listen_event",
             [](nanosrv::Manager& mgr, std::string_view url,
                nb::object callback) -> nanosrv::ConnectionRef {
                 auto shared_cb = std::make_shared<nb::object>(std::move(callback));
                 shared_cb->inc_ref();

                 HandlerFn handler =
                     [shared_cb](nanosrv::Connection& c, nanosrv::Event ev,
                                 void* ev_data) {
                         nb::gil_scoped_acquire acquire;
                         try {
                             if (ev == nanosrv::Event::HttpMessage) {
                                 auto* hm = static_cast<nanosrv::HttpMessage*>(ev_data);
                                 (*shared_cb)(&c, ev, hm);
                             } else if (ev == nanosrv::Event::WsOpen) {
                                 auto* hm = static_cast<nanosrv::HttpMessage*>(ev_data);
                                 (*shared_cb)(&c, ev, hm);
                             } else if (ev == nanosrv::Event::WsMessage) {
                                 auto* wm = static_cast<nanosrv::WsMessage*>(ev_data);
                                 (*shared_cb)(&c, ev, wm);
                             } else {
                                 (*shared_cb)(&c, ev, nb::none());
                             }
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                     };
                 return mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen with full event handler. handler(conn, event, data) is "
             "called for every event.")
        .def("wakeup", &nanosrv::Manager::wakeup,
             "conn_id"_a, "data"_a = std::string_view{},
             "Wakeup a connection by ID.");

    // -----------------------------------------------------------------------
    // ShardedManager (multi-threaded event loop)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::ShardedManager>(m, "ShardedManager")
        .def(nb::init<unsigned>(), "num_threads"_a = 0,
             "Create a sharded manager. 0 = use hardware concurrency.")
        .def("http_listen",
             [](nanosrv::ShardedManager& mgr, std::string_view url,
                nb::object callback) {
                 auto shared_cb = std::make_shared<nb::object>(std::move(callback));
                 shared_cb->inc_ref();  // prevent GC while workers hold refs

                 HttpHandler handler =
                     [shared_cb](nanosrv::Connection& c,
                                 nanosrv::HttpMessage& hm) {
                         nb::gil_scoped_acquire acquire;
                         try {
                             (*shared_cb)(&c, &hm);
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                     };
                 mgr.http_listen(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>())
        .def("run", [](nanosrv::ShardedManager& mgr) {
            nb::gil_scoped_release release;
            mgr.run();
        }, "Start worker threads and acceptor loop. Blocks until stop().")
        .def("stop", &nanosrv::ShardedManager::stop,
             "Signal all workers to stop.")
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
}
