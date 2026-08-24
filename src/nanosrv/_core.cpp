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
// Per-worker-thread Python state
// ---------------------------------------------------------------------------
//
// A ShardedManager worker is a plain C++ thread, unknown to CPython. Every
// callback therefore did PyGILState_Ensure()/Release(), and because the
// GILState counter fell back to zero each time, CPython *created and destroyed
// a PyThreadState per request*. Under the GIL that costs a few microseconds;
// on a free-threaded interpreter it was catastrophic -- around 185us per
// request with one worker, against ~3us on a GIL build.
//
// Instead, register the thread once when it starts and keep its thread state
// alive for the thread's lifetime, holding the GILState counter at one so no
// per-request Release can destroy it. The thread is left *detached* between
// callbacks (PyEval_SaveThread), which matters: an attached thread parked in
// epoll_wait would never reach a safe point, and a free-threaded
// stop-the-world GC would wait on it forever. Each callback's
// gil_scoped_acquire then only attaches an existing state rather than building
// a new one.
static thread_local PyGILState_STATE tl_gilstate;
static thread_local PyThreadState* tl_saved = nullptr;
static thread_local bool tl_registered = false;

static void worker_thread_start()
{
    if (tl_registered)
        return;
    tl_gilstate = PyGILState_Ensure();  // creates this thread's PyThreadState
    tl_saved = PyEval_SaveThread();     // detach it, but keep it alive
    tl_registered = true;
}

static void worker_thread_stop()
{
    if (!tl_registered)
        return;
    PyEval_RestoreThread(tl_saved);     // re-attach to balance the SaveThread
    PyGILState_Release(tl_gilstate);    // counter reaches 0: state destroyed
    tl_saved = nullptr;
    tl_registered = false;
}

// ---------------------------------------------------------------------------
// Payload interop: bytes in, bytes out
// ---------------------------------------------------------------------------
//
// Wire payloads are bytes, not text. These previously crossed the boundary as
// std::string / std::string_view, which nanobind maps to `str` with strict
// UTF-8 -- so a binary request body or a binary WebSocket frame raised
// UnicodeDecodeError inside the handler, and every send path rejected `bytes`
// outright (including the method named send_bytes).
//
// Now: payload-carrying properties return `bytes`, with a `.text` companion
// that decodes strictly and raises where that is the caller's explicit choice.
// Every send accepts bytes, bytearray, memoryview or str (str is encoded
// UTF-8).
//
// Protocol metadata that is textual by definition (method, URI, query, header
// values) stays `str`, but is decoded with `surrogateescape` rather than
// strictly: a malformed request must not raise inside a handler. Invalid bytes
// survive as lone surrogates and round-trip out again with
// s.encode("utf-8", "surrogateescape").

static nb::bytes to_bytes(std::string_view s)
{
    return nb::bytes(s.empty() ? "" : s.data(), s.size());
}

static nb::object decode(std::string_view s, const char* errors)
{
    if (s.empty())
        return nb::str("");
    PyObject* o = PyUnicode_DecodeUTF8(s.data(),
                                       static_cast<Py_ssize_t>(s.size()),
                                       errors);
    if (o == nullptr)
        throw nb::python_error();
    return nb::steal(o);
}

// Lenient: never raises. For protocol metadata handed to a handler.
static nb::object to_text(std::string_view s) { return decode(s, "surrogateescape"); }

// Strict: raises UnicodeDecodeError. For .text, where the caller asked for text.
static nb::object to_text_strict(std::string_view s) { return decode(s, "strict"); }

// Borrowed, zero-copy view over a bytes-like or str argument. Valid while the
// Python argument is alive, which for a function parameter is the whole call.
class Payload {
public:
    explicit Payload(nb::handle h)
    {
        PyObject* o = h.ptr();
        if (o == nullptr || o == Py_None) {
            return;  // treat None as empty, so headers=None is not a footgun
        }
        if (PyUnicode_Check(o)) {
            Py_ssize_t n = 0;
            const char* p = PyUnicode_AsUTF8AndSize(o, &n);
            if (p != nullptr) {
                view_ = std::string_view(p, static_cast<size_t>(n));
                return;
            }
            // Lone surrogates (e.g. a string that came back out of to_text):
            // re-encode losslessly instead of failing.
            PyErr_Clear();
            PyObject* b = PyUnicode_AsEncodedString(o, "utf-8", "surrogateescape");
            if (b == nullptr)
                throw nb::python_error();
            tmp_.assign(PyBytes_AsString(b), static_cast<size_t>(PyBytes_Size(b)));
            Py_DECREF(b);
            view_ = tmp_;
            return;
        }
        if (PyObject_CheckBuffer(o)
            && PyObject_GetBuffer(o, &buf_, PyBUF_SIMPLE) == 0) {
            has_buf_ = true;
            view_ = std::string_view(static_cast<const char*>(buf_.buf),
                                     static_cast<size_t>(buf_.len));
            return;
        }
        PyErr_Clear();
        throw nb::type_error(
            "expected bytes, bytearray, memoryview or str");
    }

    ~Payload()
    {
        if (has_buf_)
            PyBuffer_Release(&buf_);
    }

    Payload(const Payload&) = delete;
    Payload& operator=(const Payload&) = delete;

    std::string_view view() const { return view_; }
    // NUL-terminated copy, for the C APIs that take a const char*.
    std::string str() const { return std::string(view_); }

private:
    Py_buffer buf_{};
    bool has_buf_ = false;
    std::string tmp_;
    std::string_view view_;
};

// ---------------------------------------------------------------------------
// Thin wrappers where the C++ API uses varargs or needs argument adaptation
// ---------------------------------------------------------------------------

// http_reply_bytes(), not http_reply(): the formatted variant runs the body
// through xprintf's "%s", which stops at the first NUL byte even with an
// explicit precision, so it silently truncates any binary payload.
static void py_http_reply(PyConn& c, int status, nb::handle headers,
                          nb::handle body)
{
    auto* conn = c.get();
    std::string hdr = Payload(headers).str();
    Payload b(body);
    nanosrv::http_reply_bytes(conn, status, hdr.c_str(), b.view().data(),
                              b.view().size());
}

static void py_http_reply_ref(nanosrv::ConnectionRef& ref, int status,
                              nb::handle headers, nb::handle body)
{
    std::string hdr = Payload(headers).str();
    Payload b(body);
    nanosrv::http_reply_bytes(&*ref, status, hdr.c_str(), b.view().data(),
                              b.view().size());
}

static size_t py_ws_send_text(PyConn& c, nb::handle data)
{
    auto* conn = c.get();
    Payload p(data);
    return nanosrv::ws_send(conn, p.view().data(), p.view().size(),
                            WEBSOCKET_OP_TEXT);
}

static size_t py_ws_send_binary(PyConn& c, nb::handle data)
{
    auto* conn = c.get();
    Payload p(data);
    return nanosrv::ws_send(conn, p.view().data(), p.view().size(),
                            WEBSOCKET_OP_BINARY);
}

static size_t py_ws_send_op(PyConn& c, nb::handle data, int op)
{
    auto* conn = c.get();
    Payload p(data);
    return nanosrv::ws_send(conn, p.view().data(), p.view().size(), op);
}

static void py_ws_upgrade(PyConn& c, PyHttpMsg& hm, nb::handle headers)
{
    auto* conn = c.get();
    auto* msg = hm.get();
    std::string hdr = Payload(headers).str();
    // "%s" (not hdr.c_str() as the format) so a header value containing a
    // percent sign cannot be interpreted as a conversion.
    nanosrv::ws_upgrade(conn, msg, "%s", hdr.c_str());
}

// --- streamed responses ----------------------------------------------------
//
// The chunked-write primitives existed in C++ (http_write_chunk) but were not
// exposed, so a Python handler had to materialise its entire response body as
// one object before replying. These make incremental responses -- SSE feeds,
// long downloads, token streams -- possible from Python.

static void py_start_chunked(PyConn& c, int status, nb::handle headers)
{
    auto* conn = c.get();
    std::string hdr = Payload(headers).str();
    nanosrv::http_start_chunked(conn, status, hdr.c_str());
}

static void py_start_sse(PyConn& c, nb::handle headers)
{
    auto* conn = c.get();
    std::string hdr = Payload(headers).str();
    nanosrv::http_start_sse(conn, hdr.c_str());
}

static void py_write_chunk(PyConn& c, nb::handle data)
{
    auto* conn = c.get();
    Payload p(data);
    nanosrv::http_write_chunk(conn, p.view().data(), p.view().size());
}

// One SSE event, emitted as a single chunk. Multi-line payloads are split into
// one `data:` line each, as the format requires; a bare newline terminates.
static void py_sse_send(PyConn& c, nb::handle data, nb::handle event,
                        nb::handle id, nb::handle retry)
{
    auto* conn = c.get();
    std::string frame;
    if (!event.is_none())
        frame += "event: " + Payload(event).str() + "\n";
    if (!id.is_none())
        frame += "id: " + Payload(id).str() + "\n";
    if (!retry.is_none())
        frame += "retry: " + std::to_string(nb::cast<long>(retry)) + "\n";

    std::string_view body = Payload(data).view();
    size_t pos = 0;
    do {
        size_t nl = body.find('\n', pos);
        std::string_view line = body.substr(
            pos, nl == std::string_view::npos ? std::string_view::npos : nl - pos);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        frame += "data: ";
        frame.append(line);
        frame += "\n";
        pos = (nl == std::string_view::npos) ? nl : nl + 1;
    } while (pos != std::string_view::npos);
    frame += "\n";

    nanosrv::http_write_chunk(conn, frame.data(), frame.size());
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

static bool py_conn_send_bytes(PyConn& c, nb::handle data)
{
    auto* conn = c.get();
    Payload p(data);
    return conn->send_bytes(p.view());
}

static bool py_ref_send_bytes(nanosrv::ConnectionRef& ref, nb::handle data)
{
    Payload p(data);
    return ref.send_bytes(p.view());
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

    // "Off", not "None": a member literally named None cannot be written as
    // LogLevel.None in Python (it is a keyword -- the expression is a
    // SyntaxError), so the level was reachable only through
    // getattr(LogLevel, "None"), and no type stub could name it either. The old
    // name is kept as an alias so existing getattr() call sites keep working.
    auto log_level = nb::enum_<nanosrv::LogLevel>(m, "LogLevel")
        .value("Off", nanosrv::LogLevel::None)
        .value("Error", nanosrv::LogLevel::Error)
        .value("Info", nanosrv::LogLevel::Info)
        .value("Debug", nanosrv::LogLevel::Debug)
        .value("Verbose", nanosrv::LogLevel::Verbose);
    log_level.attr("None") = log_level.attr("Off");

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
            return to_text(m.get()->method_str());
        }, "Request method as str (surrogateescape-decoded; never raises).")
        .def_prop_ro("uri", [](const PyHttpMsg& m) {
            return to_text(m.get()->uri_str());
        }, "Request URI as str (surrogateescape-decoded; never raises).")
        .def_prop_ro("query", [](const PyHttpMsg& m) {
            return to_text(m.get()->query_str());
        }, "Query string as str (surrogateescape-decoded; never raises).")
        .def_prop_ro("body", [](const PyHttpMsg& m) {
            return to_bytes(m.get()->body_str());
        }, "Request body as bytes. Use .text for a UTF-8 decoded str.")
        .def_prop_ro("text", [](const PyHttpMsg& m) {
            return to_text_strict(m.get()->body_str());
        }, nb::sig("def text(self) -> str"),
           "Request body decoded strictly as UTF-8. Raises UnicodeDecodeError "
           "on binary input -- use .body when the payload may not be text.")
        .def_prop_ro("status_code", [](const PyHttpMsg& m) {
            return m.get()->status_code();
        })
        .def("header", [](const PyHttpMsg& m, const char* name) -> nb::object {
            auto v = m.get()->header(name);
            if (!v)
                return nb::none();
            return to_text(*v);
        }, "name"_a, nb::sig("def header(self, name: str) -> str | None"),
           "Return the value of an HTTP header as str, or None if not present.")
        .def("credentials", [](const PyHttpMsg& m) {
            auto [user, pass] = m.get()->credentials();
            return nb::make_tuple(to_text(user), to_text(pass));
        }, nb::sig("def credentials(self) -> tuple[str, str]"),
           "Return (user, password) from Authorization header.")
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
            return to_bytes(std::string_view(wm->data.buf, wm->data.len));
        }, "Frame payload as bytes -- binary frames included. Use .text for a "
           "UTF-8 decoded str.")
        .def_prop_ro("text", [](const PyWsMsg& m) {
            auto* wm = m.get();
            return to_text_strict(std::string_view(wm->data.buf, wm->data.len));
        }, nb::sig("def text(self) -> str"),
           "Frame payload decoded strictly as UTF-8. Raises "
           "UnicodeDecodeError on a binary frame.")
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
        .def_prop_ro("send_queue_len", [](const PyConn& c) {
            return c.get()->send.len;
        }, "Bytes buffered for sending but not yet written to the socket. "
           "Watch this to apply backpressure when producing a stream faster "
           "than the peer reads it; set_max_send_buffer() is the hard limit.")
        .def("send_bytes", &py_conn_send_bytes, "data"_a,
             nb::sig("def send_bytes(self, data: bytes | bytearray | memoryview | str) -> bool"),
             "Send raw bytes on the connection. Accepts bytes, bytearray, "
             "memoryview or str (encoded UTF-8).")
        .def("close", [](PyConn& c) { c.get()->set_closing(); },
             "Close the connection immediately, discarding anything still "
             "buffered for sending. To close after the response has been "
             "flushed, use drain().")
        .def("drain", [](PyConn& c) { c.get()->is_draining = 1; },
             "Close the connection once everything buffered has been written. "
             "This is what you want after send_bytes() or a streamed "
             "response -- close() would drop the unsent tail.")
        .def("http_reply", &py_http_reply,
             "status"_a, "headers"_a = nb::str(""), "body"_a = nb::bytes("", 0),
             nb::sig("def http_reply(self, status: int, headers: bytes | bytearray | memoryview | str = '', "
                     "body: bytes | bytearray | memoryview | str = b'') -> None"),
             "Send a complete HTTP response. body accepts bytes, bytearray, "
             "memoryview or str (encoded UTF-8).")
        .def("start_chunked", &py_start_chunked,
             "status"_a = 200, "headers"_a = nb::str(""),
             nb::sig("def start_chunked(self, status: int = 200, "
                     "headers: bytes | bytearray | memoryview | str = '') -> None"),
             "Begin a chunked response: sends the status line, your headers, "
             "and Transfer-Encoding: chunked. Follow with write_chunk() per "
             "piece and write_chunk(b'') to finish.")
        .def("start_sse", &py_start_sse, "headers"_a = nb::str(""),
             nb::sig("def start_sse(self, headers: bytes | bytearray | memoryview | str = '') -> None"),
             "Begin a Server-Sent Events stream (text/event-stream, no-cache, "
             "chunked). Follow with sse_send() per event.")
        .def("write_chunk", &py_write_chunk, "data"_a,
             nb::sig("def write_chunk(self, data: bytes | bytearray | memoryview | str) -> None"),
             "Write one chunk of a chunked response. An empty payload emits "
             "the terminating chunk and completes the response.")
        .def("sse_send", &py_sse_send,
             "data"_a, "event"_a = nb::none(), "id"_a = nb::none(),
             "retry"_a = nb::none(),
             nb::sig("def sse_send(self, data: bytes | bytearray | memoryview | str, event: str | None = None, "
                     "id: str | None = None, retry: int | None = None) -> None"),
             "Send one SSE event. Multi-line data is split into one data: "
             "line per line, as the format requires.")
        .def("ws_send_text", &py_ws_send_text, "data"_a,
             nb::sig("def ws_send_text(self, data: bytes | bytearray | memoryview | str) -> int"),
             "Send a WebSocket text frame. Accepts str or bytes-like.")
        .def("ws_send_binary", &py_ws_send_binary, "data"_a,
             nb::sig("def ws_send_binary(self, data: bytes | bytearray | memoryview | str) -> int"),
             "Send a WebSocket binary frame. Accepts bytes-like or str.")
        .def("ws_send", &py_ws_send_op, "data"_a, "opcode"_a,
             nb::sig("def ws_send(self, data: bytes | bytearray | memoryview | str, opcode: int) -> int"),
             "Send a WebSocket frame with explicit opcode.")
        .def("ws_upgrade", &py_ws_upgrade, "hm"_a, "headers"_a = nb::str(""),
             nb::sig("def ws_upgrade(self, hm: HttpMessage, "
                     "headers: bytes | bytearray | memoryview | str = '') -> None"),
             "Upgrade an HTTP connection to WebSocket.");

    // -----------------------------------------------------------------------
    // ConnectionRef (non-owning handle returned by listen/connect)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::ConnectionRef>(m, "ConnectionRef")
        .def_prop_ro("id", &nanosrv::ConnectionRef::id)
        .def("__bool__", [](const nanosrv::ConnectionRef& r) {
            return static_cast<bool>(r);
        })
        .def("send_bytes", &py_ref_send_bytes, "data"_a,
             nb::sig("def send_bytes(self, data: bytes | bytearray | memoryview | str) -> bool"),
             "Send raw bytes. Accepts bytes, bytearray, memoryview or str.")
        .def("close", &nanosrv::ConnectionRef::set_closing)
        .def("http_reply", &py_http_reply_ref,
             "status"_a, "headers"_a = nb::str(""),
             "body"_a = nb::bytes("", 0),
             nb::sig("def http_reply(self, status: int, headers: bytes | bytearray | memoryview | str = '', "
                     "body: bytes | bytearray | memoryview | str = b'') -> None"));

    // -----------------------------------------------------------------------
    // Metrics snapshot (observability)
    // -----------------------------------------------------------------------
    nb::class_<nanosrv::Metrics>(m, "Metrics",
        "Snapshot of a manager's cumulative observability counters.")
        .def_prop_ro("accepted",
                     [](const nanosrv::Metrics& s) { return s.accepted; },
                     "Accepted connections, total.")
        .def_prop_ro("closed",
                     [](const nanosrv::Metrics& s) { return s.closed; },
                     "Accepted connections closed, total.")
        .def_prop_ro("errors",
                     [](const nanosrv::Metrics& s) { return s.errors; },
                     "MG_EV_ERROR events raised, total.")
        .def_prop_ro("bytes_read",
                     [](const nanosrv::Metrics& s) { return s.bytes_read; },
                     "Bytes received off the wire, total.")
        .def_prop_ro("bytes_written",
                     [](const nanosrv::Metrics& s) { return s.bytes_written; },
                     "Bytes written to the wire, total.")
        .def_prop_ro("active",
                     [](const nanosrv::Metrics& s) { return s.active; },
                     "Currently live accepted connections.")
        .def("__repr__", [](const nanosrv::Metrics& s) {
            return std::string("Metrics(accepted=") + std::to_string(s.accepted)
                + ", closed=" + std::to_string(s.closed)
                + ", errors=" + std::to_string(s.errors)
                + ", bytes_read=" + std::to_string(s.bytes_read)
                + ", bytes_written=" + std::to_string(s.bytes_written)
                + ", active=" + std::to_string(s.active) + ")";
        });

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
                             } else if (ev == nanosrv::Event::Wakeup) {
                                 // The wakeup payload was previously dropped,
                                 // which made wakeup() a bare signal. Hand it
                                 // over as bytes; it is copied out of the pipe
                                 // buffer, so it outlives the callback.
                                 auto* d = static_cast<nanosrv::Str*>(ev_data);
                                 (*cb)(PyConn{&c, alive}, ev,
                                       to_bytes(std::string_view(
                                           d->buf, d->len)));
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
        .def("wakeup", [](nanosrv::Manager& mgr, unsigned long conn_id,
                          nb::handle data) {
            Payload p(data);
            return mgr.wakeup(conn_id, p.view());
        }, "conn_id"_a, "data"_a = nb::bytes("", 0),
           nb::sig("def wakeup(self, conn_id: int, data: bytes | bytearray | memoryview | str = b'') -> bool"),
           "Deliver Event.Wakeup with `data` to the connection with this id, "
           "from any thread. This is how you push to a connection outside its "
           "handler -- a stored Connection is invalid once the handler "
           "returns. Requires an http_listen_event() handler to observe it. "
           "Returns False if the id is unknown or the wakeup pipe is down.")
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
        .def("set_connect_timeout", &nanosrv::Manager::set_connect_timeout,
             "ms"_a,
             "Close client-initiated connections that do not finish resolving "
             "and connecting within `ms` milliseconds (bounds a hung outbound "
             "connect). Defaults to 30000; 0 disables.")
        .def_prop_ro("connect_timeout", &nanosrv::Manager::connect_timeout)
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
        .def_prop_ro("metrics", &nanosrv::Manager::metrics,
             "Snapshot of cumulative observability counters (see Metrics).")
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
        .def("__init__",
             [](nanosrv::ShardedManager* self, unsigned num_threads) {
                 new (self) nanosrv::ShardedManager(num_threads);
                 // Register each worker thread with CPython once, rather than
                 // once per request (see worker_thread_start above).
                 self->set_worker_hooks(worker_thread_start, worker_thread_stop);
             },
             "num_threads"_a = 0,
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
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen for HTTP connections. handler(conn, msg) runs on the "
             "worker thread owning the connection.")
        .def("http_listen_event",
             [](nanosrv::ShardedManager& mgr, std::string_view url,
                nb::object callback) {
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
                             } else if (ev == nanosrv::Event::Wakeup) {
                                 auto* d = static_cast<nanosrv::Str*>(ev_data);
                                 (*cb)(PyConn{&c, alive}, ev,
                                       to_bytes(std::string_view(d->buf, d->len)));
                             } else {
                                 (*cb)(PyConn{&c, alive}, ev, nb::none());
                             }
                         } catch (nb::python_error& e) {
                             e.restore();
                         }
                         *alive = false;
                     };
                 mgr.http_listen_event(url, std::move(handler));
             },
             "url"_a, "handler"_a, nb::keep_alive<0, 1>(),
             "Listen with a full event handler, so WebSocket works on the "
             "sharded path -- http_listen() only ever delivers HttpMessage. "
             "handler(conn, event, data) runs on the worker thread owning the "
             "connection, so it must be thread-safe.")
        .def("wakeup", [](nanosrv::ShardedManager& mgr, unsigned long conn_id,
                          nb::handle data) {
            Payload p(data);
            return mgr.wakeup(conn_id, p.view());
        }, "conn_id"_a, "data"_a = nb::bytes("", 0),
           nb::sig("def wakeup(self, conn_id: int, data: bytes | bytearray | memoryview | str = b'') -> bool"),
           "Deliver Event.Wakeup with `data` to the connection with this id, "
           "on whichever worker owns it. Thread-safe. Only meaningful between "
           "run() and its return; returns False otherwise, or if no worker "
           "owns the id.")
        .def("set_connect_timeout",
             &nanosrv::ShardedManager::set_connect_timeout, "ms"_a,
             "Connect deadline (ms) for client-initiated connections on every "
             "worker. Set before run().")
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
        .def_prop_ro("metrics", &nanosrv::ShardedManager::metrics,
             "Observability counters aggregated across all workers (see "
             "Metrics). Race-free to read while run() is active.")
        .def("set_max_send_buffer",
             &nanosrv::ShardedManager::set_max_send_buffer, "bytes"_a,
             "Send-buffer high-water mark (bytes) on every worker; a connection "
             "whose outbound backlog exceeds it is closed. Set before run(). "
             "0 disables (the default).")
        .def_prop_ro("num_workers", &nanosrv::ShardedManager::num_workers);

    // -----------------------------------------------------------------------
    // Utility functions
    // -----------------------------------------------------------------------

    // Base64. Encoding accepts arbitrary bytes; decoding produces arbitrary
    // bytes, so it returns `bytes` (as Python's own base64 module does) --
    // returning str made b64 of any non-UTF-8 payload undecodable.
    m.def("base64_encode", [](nb::handle input) {
        Payload p(input);
        return nanosrv::base64_encode(p.view());
    }, "input"_a, nb::sig("def base64_encode(input: bytes | bytearray | memoryview | str) -> str"),
       "Base64-encode bytes or str (str is encoded UTF-8). Returns str.");
    m.def("base64_decode", [](nb::handle input) {
        Payload p(input);
        return to_bytes(nanosrv::base64_decode(p.view()));
    }, "input"_a, nb::sig("def base64_decode(input: bytes | bytearray | memoryview | str) -> bytes"),
       "Base64-decode. Returns bytes.");

    // URL encode/decode
    m.def("url_encode", [](nb::handle input) {
        Payload p(input);
        return nanosrv::url_encode(p.view());
    }, "input"_a, nb::sig("def url_encode(input: bytes | bytearray | memoryview | str) -> str"),
       "Percent-encode bytes or str (str is encoded UTF-8). Returns str.");
    m.def("url_decode", [](nb::handle input) {
        Payload p(input);
        return to_text(nanosrv::url_decode(p.view()));
    }, "input"_a, nb::sig("def url_decode(input: bytes | bytearray | memoryview | str) -> str"),
       "Percent-decode. Returns str, decoded with surrogateescape so a "
       "sequence that is not valid UTF-8 does not raise; recover the raw "
       "bytes with s.encode('utf-8', 'surrogateescape').");
    m.def("url_decode_bytes", [](nb::handle input) {
        Payload p(input);
        return to_bytes(nanosrv::url_decode(p.view()));
    }, "input"_a, nb::sig("def url_decode_bytes(input: bytes | bytearray | memoryview | str) -> bytes"),
       "Percent-decode. Returns the decoded bytes verbatim.");

    // JSON (nanosrv::json namespace)
    // Each accepts bytes or str, so a request body (now bytes) can be parsed
    // without a decode step.
    auto json_mod = m.def_submodule("json", "JSON parsing utilities");
    json_mod.def("number", [](nb::handle json, const char* path) {
        Payload p(json);
        return nanosrv::json::number(p.view(), path);
    }, "json"_a, "path"_a,
       nb::sig("def number(json: bytes | bytearray | memoryview | str, path: str) -> float | None"),
       "Extract a number from JSON at the given path.");
    json_mod.def("boolean", [](nb::handle json, const char* path) {
        Payload p(json);
        return nanosrv::json::boolean(p.view(), path);
    }, "json"_a, "path"_a,
       nb::sig("def boolean(json: bytes | bytearray | memoryview | str, path: str) -> bool | None"),
       "Extract a boolean from JSON at the given path.");
    json_mod.def("integer", [](nb::handle json, const char* path) {
        Payload p(json);
        return nanosrv::json::integer(p.view(), path);
    }, "json"_a, "path"_a,
       nb::sig("def integer(json: bytes | bytearray | memoryview | str, path: str) -> int | None"),
       "Extract an integer from JSON at the given path.");
    json_mod.def("string", [](nb::handle json, const char* path) -> nb::object {
        Payload p(json);
        auto v = nanosrv::json::string(p.view(), path);
        if (!v)
            return nb::none();
        return to_text(*v);
    }, "json"_a, "path"_a,
       nb::sig("def string(json: bytes | bytearray | memoryview | str, path: str) -> str | None"),
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
