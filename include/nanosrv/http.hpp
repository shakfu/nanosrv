#pragma once
#include "net.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace nanosrv {

struct HttpHeader {
    struct Str name;
    struct Str value;
};

struct HttpMessage {
    struct Str method, uri, query, proto;
    struct HttpHeader headers[MG_MAX_HTTP_HEADERS];
    struct Str body;
    struct Str head;
    struct Str message;

    // -- Modern C++ API methods --
    std::optional<std::string_view> header(const char* name) const;
    std::string_view method_str() const { return {method.buf, method.len}; }
    std::string_view uri_str() const { return {uri.buf, uri.len}; }
    std::string_view query_str() const { return {query.buf, query.len}; }
    std::string_view body_str() const { return {body.buf, body.len}; }
    int status_code() const;
    std::pair<std::string, std::string> credentials() const;
};

// http_parse() return sentinel: the request carried more than
// MG_MAX_HTTP_HEADERS headers. Distinct from the generic parse-error (-1) so
// the caller can answer 431 (Request Header Fields Too Large) instead of
// silently truncating security-relevant headers.
constexpr int MG_HTTP_TOO_MANY_HEADERS = -2;

[[nodiscard]] int http_parse(const char* s, size_t len, struct HttpMessage*);
[[nodiscard]] int http_get_request_len(const unsigned char* buf, size_t buf_len);
void http_printf_chunk(struct Connection* cnn, const char* fmt, ...);
void http_write_chunk(struct Connection* c, const char* buf, size_t len);
// Streamed-response prologues; see the definitions in http.cpp. Follow
// either with http_write_chunk() per piece and a zero-length chunk to end.
void http_start_chunked(struct Connection* c, int code, const char* headers);
void http_start_sse(struct Connection* c, const char* headers);
[[nodiscard]] struct Connection* http_listen(struct Mgr*, const char* url,
                                     EventHandler fn, void* fn_data);
void http_reply(struct Connection*, int status_code, const char* headers,
                   const char* body_fmt, ...);
// Binary-safe counterpart to http_reply(); see the definition in http.cpp for
// why the formatted variant cannot carry a body containing NUL bytes.
void http_reply_bytes(struct Connection* c, int status_code,
                      const char* headers, const void* body, size_t len);
[[nodiscard]] struct Str* http_get_header(struct HttpMessage*, const char* name);
struct Str http_var(struct Str buf, struct Str name);
int http_get_var(const struct Str*, const char* name, char*, size_t);
int url_decode(const char* s, size_t n, char* to, size_t to_len, int form);
int http_status(const struct HttpMessage* hm);
struct Connection* http_connect(struct Mgr*, const char* url,
                                      EventHandler fn, void* fn_data);
void http_bauth(struct Connection*, const char* user, const char* pass);
struct HttpPart {
    struct Str name;
    struct Str filename;
    struct Str body;
};
size_t http_next_multipart(struct Str body, size_t ofs,
                              struct HttpPart* part);
void http_creds(struct HttpMessage* hm, char* user, size_t userlen,
                   char* pass, size_t passlen);
struct Str http_get_header_var(struct Str s, struct Str v);
size_t url_encode(const char* s, size_t sl, char* buf, size_t len);
bool wakeup_init(struct Mgr* mgr);

// Internal: the HTTP protocol handler used by http_listen.
// Exposed so that ShardedManager can install it on adopted connections.
void http_cb(struct Connection* c, int ev, void* ev_data);

// Type-safe callback wrapper. HandlerFn is defined in net.hpp.

[[nodiscard]] Connection* http_listen(Manager& mgr, const char* url, HandlerFn handler);
[[nodiscard]] Connection* http_connect(Manager& mgr, const char* url, HandlerFn handler);

// Modern API: std::string returning overloads
std::string url_encode(std::string_view input);
std::string url_decode(std::string_view input);

// Template definition for Manager::http_listen -- zero-overhead dispatch.
// Defined here because it needs the http_listen() free function declaration above.
template<typename F>
    requires (!std::same_as<std::decay_t<F>, HandlerFn>
          && !std::same_as<std::decay_t<F>,
                           std::function<void(Connection&, HttpMessage&)>>)
ConnectionRef Manager::http_listen(std::string_view url, F&& handler) {
    using Handler = std::decay_t<F>;
    auto* h = new Handler(std::forward<F>(handler));

    // Stateless trampoline -- decays to a raw function pointer.
    // The compiler can inline the user's handler through this.
    EventHandler trampoline = [](Connection* c, int ev, void* ev_data) {
        auto* fn = static_cast<Handler*>(c->fn_data);
        if constexpr (std::is_invocable_v<Handler, Connection&, HttpMessage&>) {
            if (ev == MG_EV_HTTP_MSG)
                (*fn)(*c, *static_cast<HttpMessage*>(ev_data));
        } else {
            (*fn)(*c, static_cast<Event>(ev), ev_data);
        }
        if (ev == MG_EV_CLOSE && c->is_listening) {
            // Only free the handler when the listener itself closes.
            // Accepted connections share the listener's fn_data pointer.
            delete static_cast<Handler*>(c->fn_data);
            c->fn_data = nullptr;
        }
    };

    std::string url_z(url);
    auto* c = nanosrv::http_listen(raw(), url_z.c_str(), trampoline, h);
    if (c == nullptr) delete h;
    return ConnectionRef(c);
}

} // namespace nanosrv
