#include "nanosrv/nanosrv.hpp"

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <algorithm>
#include <atomic>
#include <mutex>
#include <vector>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

using namespace nanosrv;
using namespace std::chrono_literals;

// ---- Base64 ----

static void test_base64_roundtrip()
{
    auto encoded = base64_encode("nanosrv");
    assert(encoded == "bmFub3Nydg==");

    auto decoded = base64_decode(encoded);
    assert(decoded == "nanosrv");
}

static void test_base64_invalid()
{
    auto result = base64_decode("abc"); // not multiple of 4
    assert(result.empty());

    result = base64_decode("Zm9$"); // invalid character
    assert(result.empty());
}

// ---- HTTP parsing ----

static void test_http_parse_request()
{
    const char* req = "POST /hello?name=nanosrv HTTP/1.1\r\n"
                      "Host: example.com\r\n"
                      "Content-Length: 4\r\n"
                      "\r\n"
                      "Body";
    HttpMessage hm;
    int parsed = http_parse(req, strlen(req), &hm);
    assert(parsed > 0);

    // Use modern string_view API
    assert(hm.method_str() == "POST");
    assert(hm.uri_str() == "/hello");
    assert(hm.query_str() == "name=nanosrv");
    assert(hm.body_str() == "Body");

    // Optional header lookup
    auto host = hm.header("Host");
    assert(host.has_value());
    assert(*host == "example.com");

    auto missing = hm.header("X-Missing");
    assert(!missing.has_value());
}

static void test_http_creds_basic()
{
    const char* req = "GET / HTTP/1.1\r\n"
                      "Authorization: Basic dXNlcjpzZWNyZXQ=\r\n"
                      "\r\n";
    HttpMessage hm;
    assert(http_parse(req, strlen(req), &hm) > 0);

    auto [user, pass] = hm.credentials();
    assert(user == "user");
    assert(pass == "secret");
}

static void test_http_creds_cookie_and_query()
{
    const char* req = "GET /?access_token=querytoken HTTP/1.1\r\n"
                      "Cookie: foo=bar; access_token=cookietoken\r\n"
                      "\r\n";
    HttpMessage hm;
    assert(http_parse(req, strlen(req), &hm) > 0);

    auto [user, pass] = hm.credentials();
    assert(pass == "cookietoken");

    const char* req2 = "GET /?access_token=querytoken HTTP/1.1\r\n\r\n";
    assert(http_parse(req2, strlen(req2), &hm) > 0);
    auto [u2, p2] = hm.credentials();
    assert(p2 == "querytoken");
}

static void test_http_status_code()
{
    const char* resp = "HTTP/1.1 404 Not Found\r\n\r\n";
    HttpMessage hm;
    assert(http_parse(resp, strlen(resp), &hm) > 0);
    assert(hm.status_code() == 404);
}

// ---- StringView ----

static void test_string_view_conversion()
{
    StringView sv("hello", 5);
    assert(sv.size == 5);

    std::string_view std_sv = sv;
    assert(std_sv == "hello");

    Str ms("world");
    StringView sv2(ms);
    assert(std::string_view(sv2) == "world");

    assert(StringView("abc", 3) == StringView("abc", 3));
    assert(!(StringView("abc", 3) == StringView("abd", 3)));
}

// ---- Manager RAII ----

static void test_manager_raii()
{
    {
        Manager mgr;
        mgr.poll(0ms);
        assert(mgr.raw() != nullptr);
    }
}

// ---- Type-safe callbacks ----

static void test_type_safe_callback_general()
{
    // General handler with Event enum (no int cast needed).
    // handler_called must be declared before mgr: mgr_free() runs a final
    // poll during destruction that can deliver a Poll event into this handler,
    // so the captured local has to outlive the Manager.
    bool handler_called = false;
    Manager mgr;

    auto c = mgr.http_listen("http://127.0.0.1:0",
        HandlerFn([&handler_called](Connection& conn, Event ev, [[maybe_unused]] void* data) {
            if (ev == Event::Poll)
                handler_called = true;
            (void)conn;
        }));
    assert(c);
    mgr.poll(0ms);
    assert(handler_called);
}

static void test_typed_http_handler()
{
    // Typed HTTP handler: receives HttpMessage& directly, no casting
    // got_request must outlive mgr (mgr_free() polls during destruction).
    bool got_request = false;
    Manager mgr;

    auto listener = mgr.http_listen("http://127.0.0.1:0",
        Manager::HttpHandler([&got_request](Connection& c, HttpMessage& hm) {
            // This only fires for HTTP messages -- no event check needed
            got_request = true;
            assert(hm.method_str() == "GET" || hm.method_str().empty() == false);
            http_reply(&c, 200, "", "OK");
        }));
    assert(listener);
    // Just polling won't trigger HTTP handler (no actual HTTP request),
    // but it should not crash
    mgr.poll(0ms);
}

// ---- JSON (modern API) ----

static void test_json_number()
{
    auto v = json::number(R"({"a":1,"b":2.5})", "$.a");
    assert(v.has_value());
    assert(*v == 1.0);

    v = json::number(R"({"a":1,"b":2.5})", "$.b");
    assert(*v == 2.5);

    assert(!json::number(R"({"a":"str"})", "$.a").has_value());
    assert(!json::number(R"({})", "$.missing").has_value());
}

static void test_json_boolean()
{
    auto v = json::boolean(R"({"flag":true,"off":false})", "$.flag");
    assert(v.has_value() && *v == true);

    v = json::boolean(R"({"flag":true,"off":false})", "$.off");
    assert(v.has_value() && *v == false);

    assert(!json::boolean(R"({"x":1})", "$.x").has_value());
}

static void test_json_integer()
{
    auto v = json::integer(R"({"x":42})", "$.x");
    assert(v.has_value() && *v == 42);

    assert(!json::integer(R"({})", "$.missing").has_value());
}

static void test_json_string()
{
    auto v = json::string(R"({"name":"nanosrv"})", "$.name");
    assert(v.has_value());
    assert(*v == "nanosrv");

    v = json::string(R"({"name":""})", "$.name");
    assert(v.has_value() && v->empty());

    assert(!json::string(R"({})", "$.nope").has_value());
}

static void test_json_nested()
{
    auto v = json::integer(R"({"a":{"b":{"c":99}}})", "$.a.b.c");
    assert(v.has_value() && *v == 99);
}

static void test_json_array()
{
    auto v0 = json::integer(R"({"arr":[10,20,30]})", "$.arr[0]");
    auto v1 = json::integer(R"({"arr":[10,20,30]})", "$.arr[1]");
    auto v2 = json::integer(R"({"arr":[10,20,30]})", "$.arr[2]");
    assert(v0.has_value() && *v0 == 10);
    assert(v1.has_value() && *v1 == 20);
    assert(v2.has_value() && *v2 == 30);
    assert(!json::integer(R"({"arr":[10]})", "$.arr[1]").has_value());
}

// ---- URL parsing ----

static void test_url_port()
{
    assert(url_port("http://example.com") == 80);
    assert(url_port("https://example.com") == 443);
    assert(url_port("http://example.com:9090") == 9090);
}

static void test_url_class()
{
    auto u = Url::parse("https://example.com:8443/api/v1?q=1");
    assert(u.is_ssl == true);
    assert(u.port == 8443);
    assert(u.host == "example.com");
    assert(u.path.find("/api/v1") != std::string_view::npos);
}

static void test_url_encode_decode()
{
    auto encoded = url_encode("hello world&foo=bar");
    assert(encoded.find('+') == std::string::npos); // spaces become %20
    assert(encoded.find("%20") != std::string::npos || encoded.find('+') != std::string::npos);

    auto decoded = url_decode(encoded);
    assert(decoded == "hello world&foo=bar");
}

// ---- Timer ----

static void test_timer_expired()
{
    uint64_t expiration = 0;
    assert(timer_expired(&expiration, 100, 50) == false);
    assert(expiration == 150);
    assert(timer_expired(&expiration, 100, 100) == false);
    assert(timer_expired(&expiration, 100, 150) == true);
}

static void test_timer_basic()
{
    Timer* head = nullptr;
    Timer t{};
    int count = 0;
    auto cb = [](void* arg) { (*static_cast<int*>(arg))++; };

    timer_init(&head, &t, 100, MG_TIMER_REPEAT, cb, &count);
    timer_poll(&head, 0);
    assert(count == 0);
    timer_poll(&head, 100);
    assert(count == 1);
    timer_poll(&head, 200);
    assert(count == 2);
    timer_free(&head, &t);
}

// ---- Scoped enums ----

static void test_scoped_enums()
{
    // Event enum values match the old MG_EV_* values
    assert(static_cast<int>(Event::Error) == MG_EV_ERROR);
    assert(static_cast<int>(Event::HttpMessage) == MG_EV_HTTP_MSG);
    assert(static_cast<int>(Event::Close) == MG_EV_CLOSE);
    assert(static_cast<int>(Event::Wakeup) == 20);
    assert(static_cast<int>(Event::User) == 100);

    // WsOpcode
    assert(static_cast<int>(WsOpcode::Text) == WEBSOCKET_OP_TEXT);
    assert(static_cast<int>(WsOpcode::Binary) == WEBSOCKET_OP_BINARY);
    assert(static_cast<int>(WsOpcode::Close) == WEBSOCKET_OP_CLOSE);

    // LogLevel
    assert(static_cast<int>(LogLevel::Error) == MG_LL_ERROR);
    assert(static_cast<int>(LogLevel::Verbose) == MG_LL_VERBOSE);

    // TimerMode
    assert(static_cast<unsigned>(TimerMode::Once) == MG_TIMER_ONCE);
    assert(static_cast<unsigned>(TimerMode::Repeat) == MG_TIMER_REPEAT);
}

// ---- Str constructors ----

static void test_str_constructors()
{
    Str s;
    assert(s.buf == nullptr && s.len == 0);

    Str s2("hello");
    assert(s2.len == 5);

    Str s3("hello world", 5);
    assert(s3.len == 5);

    Str s4("");
    assert(s4.len == 0 && s4.buf != nullptr);
}

// ---- IP ACL matching (IPv4 + IPv6) ----

static void test_check_ip_acl()
{
    auto A = [](const char* s) {
        Address a;
        memset(&a, 0, sizeof(a));
        assert(aton(Str(s), &a));
        return a;
    };

    Address v4 = A("10.0.0.5");
    Address v4b = A("192.168.1.1");

    // Empty ACL allows everyone; a non-empty ACL defaults to deny.
    assert(check_ip_acl(Str(""), &v4) == 1);
    assert(check_ip_acl(Str("+10.0.0.0/8"), &v4) == 1);
    assert(check_ip_acl(Str("+192.168.0.0/16"), &v4) == 0);

    // Last matching entry wins; bare address is a host route.
    assert(check_ip_acl(Str("-10.0.0.0/8,+10.0.0.5"), &v4) == 1);
    assert(check_ip_acl(Str("+10.0.0.0/8,-10.0.0.5"), &v4) == 0);
    assert(check_ip_acl(Str("+10.0.0.5"), &v4) == 1);
    assert(check_ip_acl(Str("+10.0.0.6"), &v4) == 0);

    // IPv6 -- the regression fix. The old code returned early for any IPv6 peer,
    // so a restrictive ACL silently failed open. It must now actually match.
    Address v6 = A("2001:db8::1");
    Address v6other = A("2001:dead::1");
    assert(check_ip_acl(Str("+2001:db8::/32"), &v6) == 1);
    assert(check_ip_acl(Str("+2001:db8::/32"), &v6other) == 0);
    assert(check_ip_acl(Str("-2001:db8::/32"), &v6) == 0);
    assert(check_ip_acl(Str("+2001:db8::1"), &v6) == 1);
    assert(check_ip_acl(Str("+::/0"), &v6) == 1);  // allow all IPv6

    // Cross-family: an entry only applies to its own family, so a single-family
    // allow ACL DENIES the other family (it must not fail open).
    assert(check_ip_acl(Str("+10.0.0.0/8"), &v6) == 0);
    assert(check_ip_acl(Str("+2001:db8::/32"), &v4) == 0);

    // Mixed ACL: each family honored independently.
    Str mixed("+10.0.0.0/8,+2001:db8::/32");
    assert(check_ip_acl(mixed, &v4) == 1);
    assert(check_ip_acl(mixed, &v6) == 1);
    assert(check_ip_acl(mixed, &v4b) == 0);
    assert(check_ip_acl(mixed, &v6other) == 0);

    // Malformed ACLs report an error (< 0), never a silent allow.
    assert(check_ip_acl(Str("10.0.0.0/8"), &v4) < 0);     // missing +/- flag
    assert(check_ip_acl(Str("+999.0.0.0/8"), &v4) < 0);   // bad address
    assert(check_ip_acl(Str("+10.0.0.0/40"), &v4) < 0);   // IPv4 prefix > 32
    assert(check_ip_acl(Str("+2001:db8::/200"), &v6) < 0); // IPv6 prefix > 128
}

// ---- TLS listener fails closed ----

#if MG_TLS == MG_TLS_NONE
// With no TLS backend, a TLS listener must be refused rather than created --
// otherwise it would accept connections and serve cleartext (accepted conns
// have is_tls reset to 0) on a port intended for TLS.
static void test_listen_tls_unavailable_fails_closed()
{
    assert(!tls_available());
    Manager mgr;

    auto tls_ref = mgr.http_listen("https://127.0.0.1:18903",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(!tls_ref);  // refused: no listener created

    auto wss_ref = mgr.http_listen("wss://127.0.0.1:18903",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(!wss_ref);  // wss too

    // A plaintext listener on the same port still works.
    auto ok = mgr.http_listen("http://127.0.0.1:18903",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(ok);
}
#else
// With a TLS backend compiled in, the fail-closed guard must NOT trigger: a TLS
// listener is created normally. (The handshake path needs real certs and is
// exercised separately.)
static void test_listen_tls_unavailable_fails_closed()
{
    assert(tls_available());
    Manager mgr;

    auto tls_ref = mgr.http_listen("https://127.0.0.1:18903",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(tls_ref);  // accepted: backend is available

    auto wss_ref = mgr.http_listen("wss://127.0.0.1:18904",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(wss_ref);
}
#endif

// ---- TLS handshake, end to end ----

#if MG_TLS != MG_TLS_NONE
// Drive a real TLS handshake between a nanosrv server and a nanosrv client over
// loopback, then exchange application bytes across the encrypted channel. This
// is the proof that the compiled-in backend (tls_init/handshake/recv/send) works
// together with the event loop's rtls buffering -- not just that listeners are
// created. Client verification is enabled (the self-signed cert is its own CA,
// matched against hostname "localhost") so the certificate path is exercised too.

// Self-signed P-256 cert (CN=localhost, SAN DNS:localhost + IP:127.0.0.1),
// valid for 100 years, generated with openssl. Key is unencrypted PKCS#8.
static const char* kTlsCert = R"PEM(-----BEGIN CERTIFICATE-----
MIIBmzCCAUGgAwIBAgIUFrt3rSagxHhy/YmYO3eNvzc52/4wCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MCAXDTI2MDYwMjAyNDAzMVoYDzIxMjYwNTA5
MDI0MDMxWjAUMRIwEAYDVQQDDAlsb2NhbGhvc3QwWTATBgcqhkjOPQIBBggqhkjO
PQMBBwNCAAQFCCg0+XCbw14kqC1c/j1ISLD22+nzyzwhRPTbtDxzKnn0MtnYHOU9
xETYuU3Ik8lok4DNOmyqkVKoo4gk3C6To28wbTAdBgNVHQ4EFgQUVVT2w1oXbY7F
rcvh2O0DZV8q7QAwHwYDVR0jBBgwFoAUVVT2w1oXbY7Frcvh2O0DZV8q7QAwDwYD
VR0TAQH/BAUwAwEB/zAaBgNVHREEEzARgglsb2NhbGhvc3SHBH8AAAEwCgYIKoZI
zj0EAwIDSAAwRQIgAfQMCH4aabNptOO8r8lCo+YcqW61UPGbUOcox8x2ZwECIQCn
KWKm4TvhsRCX59ePfCiWqP95nuA9vajBrY0mXO5SmQ==
-----END CERTIFICATE-----
)PEM";

static const char* kTlsKey = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQgtFklKvZdVm4ojOkn
x5+up0Im77q8xNPrVY563MBkMlahRANCAAQFCCg0+XCbw14kqC1c/j1ISLD22+nz
yzwhRPTbtDxzKnn0MtnYHOU9xETYuU3Ik8lok4DNOmyqkVKoo4gk3C6T
-----END PRIVATE KEY-----
)PEM";

struct TlsHandshakeState {
    bool server_hs = false;
    bool client_hs = false;
    bool server_err = false;
    bool client_err = false;
    std::string server_got;  // plaintext the server decrypted
    std::string client_got;  // plaintext the client decrypted (the echo)
};

static void drain_recv(Connection* c, std::string& into)
{
    into.append(reinterpret_cast<const char*>(c->recv.buf), c->recv.len);
    iobuf_del(&c->recv, 0, c->recv.len);
}

static void tls_handshake_server_ev(Connection* c, int ev, void*)
{
    auto* st = static_cast<TlsHandshakeState*>(c->fn_data);
    if (ev == MG_EV_ACCEPT) {
        TlsOpts opts{};
        opts.cert = Str(kTlsCert);
        opts.key = Str(kTlsKey);
        tls_init(c, &opts);
    } else if (ev == MG_EV_TLS_HS) {
        st->server_hs = true;
    } else if (ev == MG_EV_READ) {
        drain_recv(c, st->server_got);
        send_data(c, st->server_got.data(), st->server_got.size());  // echo
    } else if (ev == MG_EV_ERROR) {
        st->server_err = true;
    }
}

static void tls_handshake_client_ev(Connection* c, int ev, void*)
{
    auto* st = static_cast<TlsHandshakeState*>(c->fn_data);
    if (ev == MG_EV_CONNECT) {
        TlsOpts opts{};
        opts.ca = Str(kTlsCert);     // self-signed cert is its own trust anchor
        opts.name = Str("localhost");  // verified against the cert SAN
        tls_init(c, &opts);
    } else if (ev == MG_EV_TLS_HS) {
        st->client_hs = true;
        send_data(c, "ping", 4);     // first app bytes over the encrypted link
    } else if (ev == MG_EV_READ) {
        drain_recv(c, st->client_got);
    } else if (ev == MG_EV_ERROR) {
        st->client_err = true;
    }
}

static void test_tls_handshake_end_to_end()
{
    assert(tls_available());
    Manager mgr;
    TlsHandshakeState st;
    const char* url = "https://127.0.0.1:18905";

    auto* lsn = listen_(mgr.raw(), url, tls_handshake_server_ev, &st);
    assert(lsn != nullptr);
    auto* cl = connect(mgr.raw(), url, tls_handshake_client_ev, &st);
    assert(cl != nullptr);

    // Pump the loop until the echo round-trips, bounded by a wall-clock deadline
    // so a handshake failure surfaces as a failed assert rather than a hang.
    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        mgr.poll(10ms);
        if (st.client_err || st.server_err)
            break;
        if (st.client_got.size() >= 4)
            break;
    }

    assert(!st.server_err);
    assert(!st.client_err);
    assert(st.server_hs);            // server completed the handshake
    assert(st.client_hs);            // client completed the handshake (verified)
    assert(st.server_got == "ping"); // plaintext arrived intact server-side
    assert(st.client_got == "ping"); // and the echo decrypted intact client-side
}

// ---- Mutual TLS (client certificate) ----

// A second self-signed P-256 identity (CN=test-client), used as the client's
// certificate. Like the server cert it is CA:TRUE, so each side can trust the
// other's cert directly as a one-element chain.
static const char* kClientCert = R"PEM(-----BEGIN CERTIFICATE-----
MIIBgzCCASmgAwIBAgIUCL73yTbfhGsrEzPVhk6R6maAXiYwCgYIKoZIzj0EAwIw
FjEUMBIGA1UEAwwLdGVzdC1jbGllbnQwIBcNMjYwNjAyMDMwNDQ5WhgPMjEyNjA1
MDkwMzA0NDlaMBYxFDASBgNVBAMMC3Rlc3QtY2xpZW50MFkwEwYHKoZIzj0CAQYI
KoZIzj0DAQcDQgAECF/Fm+1aykDY3ZBSLbfPJbNS7WwOymDzQLhcMhi46txHc7/e
ABGOx4r9eNqWRA05b5mO+/6uwBfdfxlZGohbb6NTMFEwHQYDVR0OBBYEFJKwnslZ
tK2KxyX+FjkY7bAUPQk8MB8GA1UdIwQYMBaAFJKwnslZtK2KxyX+FjkY7bAUPQk8
MA8GA1UdEwEB/wQFMAMBAf8wCgYIKoZIzj0EAwIDSAAwRQIhAIp/I0y2neqjCO54
kxgh+rMMPNnmqgosEjid6IB5wy0IAiBHbZTl7QS1ZMF4l5gjV5w5reZS30wEfiZy
2eksJ68jgA==
-----END CERTIFICATE-----
)PEM";

static const char* kClientKey = R"PEM(-----BEGIN PRIVATE KEY-----
MIGHAgEAMBMGByqGSM49AgEGCCqGSM49AwEHBG0wawIBAQQg4awTUUjQnQU76i4v
EpK6iyyQdlrOWTNnPwaqmBOvPamhRANCAAQIX8Wb7VrKQNjdkFItt88ls1LtbA7K
YPNAuFwyGLjq3Edzv94AEY7Hiv142pZEDTlvmY77/q7AF91/GVkaiFtv
-----END PRIVATE KEY-----
)PEM";

struct MtlsState {
    bool present_client_cert = true;  // toggled off for the enforcement case
    bool server_hs = false;
    bool client_hs = false;
    bool server_err = false;
    bool client_err = false;
    std::string server_got;
    std::string client_got;
};

static void mtls_server_ev(Connection* c, int ev, void*)
{
    auto* st = static_cast<MtlsState*>(c->fn_data);
    if (ev == MG_EV_ACCEPT) {
        TlsOpts opts{};
        opts.cert = Str(kTlsCert);
        opts.key = Str(kTlsKey);
        opts.ca = Str(kClientCert);  // require + verify the client's cert
        tls_init(c, &opts);
    } else if (ev == MG_EV_TLS_HS) {
        st->server_hs = true;
    } else if (ev == MG_EV_READ) {
        drain_recv(c, st->server_got);
        send_data(c, st->server_got.data(), st->server_got.size());
    } else if (ev == MG_EV_ERROR) {
        st->server_err = true;
    }
}

static void mtls_client_ev(Connection* c, int ev, void*)
{
    auto* st = static_cast<MtlsState*>(c->fn_data);
    if (ev == MG_EV_CONNECT) {
        TlsOpts opts{};
        opts.ca = Str(kTlsCert);       // trust the server's self-signed cert
        opts.name = Str("localhost");
        if (st->present_client_cert) {
            opts.cert = Str(kClientCert);
            opts.key = Str(kClientKey);
        }
        tls_init(c, &opts);
    } else if (ev == MG_EV_TLS_HS) {
        st->client_hs = true;
        send_data(c, "ping", 4);
    } else if (ev == MG_EV_READ) {
        drain_recv(c, st->client_got);
    } else if (ev == MG_EV_ERROR) {
        st->client_err = true;
    }
}

// Pump a fresh client/server pair to a terminal state (success or error),
// bounded by a wall-clock deadline.
static void run_mtls_exchange(MtlsState& st, const char* url)
{
    Manager mgr;
    auto* lsn = listen_(mgr.raw(), url, mtls_server_ev, &st);
    assert(lsn != nullptr);
    auto* cl = connect(mgr.raw(), url, mtls_client_ev, &st);
    assert(cl != nullptr);

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
        mgr.poll(10ms);
        if (st.client_err || st.server_err)
            break;
        if (st.client_got.size() >= 4)
            break;
    }
}

static void test_tls_mutual_auth()
{
    assert(tls_available());

    // Positive: client presents a trusted cert -> the server (VERIFY_REQUIRED)
    // accepts it, both sides complete the handshake, and the echo round-trips.
    {
        MtlsState st;
        st.present_client_cert = true;
        run_mtls_exchange(st, "https://127.0.0.1:18906");
        assert(!st.server_err);
        assert(!st.client_err);
        assert(st.server_hs);
        assert(st.client_hs);
        assert(st.server_got == "ping");
        assert(st.client_got == "ping");
    }

    // Negative: client presents NO cert. The server must refuse the handshake.
    // (Under TLS 1.3 the client may finish its own side and even send early data
    // before the server's alert arrives, so client_hs is not asserted; what must
    // hold is that the server rejects the handshake and accepts no plaintext.)
    // The explicit server_err assert keeps this from passing vacuously if the
    // connection had simply never been established.
    {
        MtlsState st;
        st.present_client_cert = false;
        run_mtls_exchange(st, "https://127.0.0.1:18907");
        assert(st.server_err);          // server actively rejected the peer
        assert(!st.server_hs);          // server never finished the handshake
        assert(st.server_got.empty());  // no plaintext reached the server
        assert(st.client_got.empty());  // and the client got no echo
    }
}
#endif

// ---- Connection methods ----

static void test_connection_send_bytes()
{
    // sent_ok must outlive mgr: mgr_free()'s teardown poll can fire this handler.
    bool sent_ok = false;
    Manager mgr;

    auto c = mgr.http_listen("http://127.0.0.1:0",
        HandlerFn([&sent_ok](Connection& conn, Event ev, [[maybe_unused]] void* data) {
            if (ev == Event::Poll)
                sent_ok = true;
            (void)conn;
        }));
    assert(c);
    mgr.poll(0ms);
    assert(sent_ok);
}

// ---- HTTP header-count limit (431 path) ----

// Exactly MG_MAX_HTTP_HEADERS headers must parse; one more must be rejected with
// the distinct too-many-headers sentinel (which http_cb answers as 431) rather
// than being silently truncated.
static void test_http_header_limit()
{
    auto build = [](int n) {
        std::string r = "GET / HTTP/1.1\r\n";
        for (int i = 0; i < n; i++)
            r += "H" + std::to_string(i) + ": v\r\n";
        r += "\r\n";
        return r;
    };
    HttpMessage hm;
    std::string ok = build(MG_MAX_HTTP_HEADERS);
    assert(http_parse(ok.c_str(), ok.size(), &hm) > 0);

    std::string over = build(MG_MAX_HTTP_HEADERS + 1);
    assert(http_parse(over.c_str(), over.size(), &hm)
           == MG_HTTP_TOO_MANY_HEADERS);
}

// ---- Multipart form parsing ----

static void test_http_multipart()
{
    // Two parts: a plain field and a file part with a filename.
    const char* body =
        "--xyz\r\n"
        "Content-Disposition: form-data; name=\"a\"\r\n"
        "\r\n"
        "AAA\r\n"
        "--xyz\r\n"
        "Content-Disposition: form-data; name=\"b\"; filename=\"f.txt\"\r\n"
        "\r\n"
        "BBBB\r\n"
        "--xyz--\r\n";
    Str b = str_n(body, strlen(body));

    HttpPart part;
    size_t ofs = http_next_multipart(b, 0, &part);
    assert(ofs > 0);
    assert(part.name.len == 1 && part.name.buf[0] == 'a');
    assert(part.body.len == 3 && memcmp(part.body.buf, "AAA", 3) == 0);
    assert(part.filename.len == 0);

    ofs = http_next_multipart(b, ofs, &part);
    assert(ofs > 0);
    assert(part.name.len == 1 && part.name.buf[0] == 'b');
    assert(part.filename.len == 5 && memcmp(part.filename.buf, "f.txt", 5) == 0);
    assert(part.body.len == 4 && memcmp(part.body.buf, "BBBB", 4) == 0);

    // No more parts.
    assert(http_next_multipart(b, ofs, &part) == 0);
}

// ---- Float formatting (custom dtoa) ----

static void test_float_formatting()
{
    char buf[64];

    snprintf_(buf, sizeof(buf), "%g", 0.0);
    assert(strcmp(buf, "0") == 0);
    snprintf_(buf, sizeof(buf), "%g", 1.5);
    assert(strcmp(buf, "1.5") == 0);
    snprintf_(buf, sizeof(buf), "%g", -2.25);
    assert(strcmp(buf, "-2.25") == 0);

    // Pathological values must render as words, never garbage.
    double inf = HUGE_VAL;
    snprintf_(buf, sizeof(buf), "%g", inf);
    assert(strcmp(buf, "inf") == 0);
    snprintf_(buf, sizeof(buf), "%g", -inf);
    assert(strcmp(buf, "-inf") == 0);
    double nan = inf - inf;  // NaN
    snprintf_(buf, sizeof(buf), "%g", nan);
    assert(strcmp(buf, "nan") == 0);
}

// ---- str match/span ----

static void test_str_match_span()
{
    // Glob-style matcher: '?' one char, '*' spans within a segment (not '/'),
    // '#' spans anything including '/'.
    assert(match(Str("abc"), Str("abc"), nullptr));
    assert(match(Str("abc"), Str("a?c"), nullptr));
    assert(!match(Str("abc"), Str("abx"), nullptr));
    assert(match(Str("abcdef"), Str("a*f"), nullptr));
    assert(!match(Str("a/b"), Str("a*b"), nullptr));  // '*' stops at '/'
    assert(match(Str("a/b"), Str("a#b"), nullptr));   // '#' crosses '/'

    // '#' capture must handle a large input without walking off the buffer.
    std::string big(4096, 'z');
    std::string subject = "/p/" + big;
    Str cap;
    assert(match(Str(subject.c_str()), Str("/p/#"), &cap));
    assert(cap.len == big.size());

    // span() splits on the first separator; the remainder keeps the tail.
    Str head, tail;
    assert(span(Str("key=value=extra"), &head, &tail, '='));
    assert(head.len == 3 && memcmp(head.buf, "key", 3) == 0);
    assert(tail.len == 11 && memcmp(tail.buf, "value=extra", 11) == 0);
}

#ifndef _WIN32
// Minimal blocking HTTP client: connect, send a GET, return true iff the
// response status line carries "200". Uses a recv timeout so a hung worker
// fails the test instead of hanging it.
static bool http_get_200(uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return false;

    struct timeval tv{};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");

    bool ok = false;
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
        const char* req =
            "GET /x HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
        if (send(fd, req, strlen(req), 0) == static_cast<ssize_t>(strlen(req))) {
            char buf[512];
            ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);
            if (n > 0) {
                buf[n] = '\0';
                ok = strstr(buf, " 200 ") != nullptr;
            }
        }
    }
    close(fd);
    return ok;
}

// H3: exercise the sharded accept-and-hand-off path under real concurrency.
// One acceptor distributes FDs to N workers; many simultaneous clients must
// each be served. Run under ASan/TSan this covers the cross-thread FD handoff
// (detach_fd -> per-worker queue -> wrapfd adopt -> http_cb -> reply -> close)
// and the per-listen context ownership (no leaks).
static void test_sharded_concurrent_requests()
{
    const uint16_t port = 18890;
    const int num_workers = 4;
    const int num_clients = 32;

    ShardedManager sharded(num_workers);
    assert(sharded.num_workers() == num_workers);

    std::atomic<int> served{0};
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [&served](Connection& c, HttpMessage&) {
            served.fetch_add(1, std::memory_order_relaxed);
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "ok");
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(100ms);  // let workers + acceptor start polling

    std::atomic<int> ok_count{0};
    std::vector<std::thread> clients;
    clients.reserve(num_clients);
    for (int i = 0; i < num_clients; i++)
        clients.emplace_back([&ok_count, port]() {
            if (http_get_200(port))
                ok_count.fetch_add(1, std::memory_order_relaxed);
        });
    // Read the aggregate metrics while workers are actively serving. This
    // exercises the cross-thread counter read (worker threads mutate the atomic
    // counters concurrently) -- it must be race-free under TSan.
    for (int i = 0; i < 5; i++) {
        (void)sharded.metrics();
        std::this_thread::sleep_for(2ms);
    }

    for (auto& t : clients)
        t.join();

    sharded.stop();
    runner.join();  // must return before sharded is destroyed (teardown contract)

    assert(ok_count.load() == num_clients);
    assert(served.load() == num_clients);

    // After the drain, the aggregate counters must reflect all the traffic.
    Metrics m = sharded.metrics();
    assert(m.accepted >= static_cast<uint64_t>(num_clients));
    assert(m.closed >= static_cast<uint64_t>(num_clients));
    assert(m.bytes_read > 0 && m.bytes_written > 0);
}

// ---- HTTP chunked request decoding (end-to-end) ----

// A chunked request body must be de-chunked and delivered as a contiguous body
// to the handler. Drives a raw socket to send the chunk framing verbatim.
static void test_http_chunked_request()
{
    const uint16_t port = 18901;
    Manager mgr;
    std::string got_body;
    bool got_msg = false;

    auto ref = mgr.http_listen(
        std::string("http://127.0.0.1:") + std::to_string(port),
        HandlerFn([&](Connection& c, Event ev, void* ed) {
            if (ev == Event::HttpMessage) {
                auto* hm = static_cast<HttpMessage*>(ed);
                got_body.assign(hm->body.buf, hm->body.len);
                got_msg = true;
                http_reply(&c, 200, "", "ok");
            }
        }));
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread poller([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10ms);
    });

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct timeval tv{};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
        const char* req =
            "POST / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "4\r\nWiki\r\n"
            "5\r\npedia\r\n"
            "0\r\n\r\n";
        (void)send(fd, req, strlen(req), 0);
        char buf[128];
        (void)recv(fd, buf, sizeof(buf), 0);  // wait for the reply
    }
    close(fd);

    stop.store(true);
    poller.join();

    assert(got_msg);
    assert(got_body == "Wikipedia");  // the two chunks, concatenated
}

// ---- WebSocket end-to-end ----

// Client and server share one Manager and one event loop. The client sends
// messages spanning all three payload-length encodings (7-bit, 16-bit and
// 64-bit) plus a 0-byte frame and a Ping; the server echoes each TEXT frame.
// Exercises framing (masking on the client side, unmasking on the server,
// extended lengths, control frames) through the real send/parse path.
struct WsEchoState {
    int server_opens = 0;
    int client_opens = 0;
    int client_pongs = 0;
    std::vector<size_t> echoed;   // sizes of TEXT echoes the client received
    std::vector<size_t> sizes;    // sizes to send on open
};

static void ws_echo_client_ev(Connection* c, int ev, void* ed)
{
    auto* st = static_cast<WsEchoState*>(c->fn_data);
    if (ev == MG_EV_WS_OPEN) {
        st->client_opens++;
        for (size_t sz : st->sizes) {
            std::string payload(sz, 'x');
            ws_send(c, payload.data(), payload.size(), WEBSOCKET_OP_TEXT);
        }
        ws_send(c, "hi", 2, WEBSOCKET_OP_PING);
    } else if (ev == MG_EV_WS_MSG) {
        auto* m = static_cast<WsMessage*>(ed);
        st->echoed.push_back(m->data.len);
    } else if (ev == MG_EV_WS_CTL) {
        auto* m = static_cast<WsMessage*>(ed);
        if ((m->flags & 15) == WEBSOCKET_OP_PONG)
            st->client_pongs++;
    }
}

static void test_ws_echo_e2e()
{
    const uint16_t port = 18898;
    Manager mgr;
    WsEchoState st;
    st.sizes = {0, 5, 200, 70000};  // 7-bit, 7-bit, 16-bit, 64-bit lengths

    auto ref = mgr.http_listen(
        std::string("http://127.0.0.1:") + std::to_string(port),
        HandlerFn([&st](Connection& c, Event ev, void* ed) {
            if (ev == Event::HttpMessage) {
                st.server_opens++;
                ws_upgrade(&c, static_cast<HttpMessage*>(ed), nullptr);
            } else if (ev == Event::WsMessage) {
                auto* m = static_cast<WsMessage*>(ed);
                ws_send(&c, m->data.buf, m->data.len, WEBSOCKET_OP_TEXT);
            }
        }));
    assert(ref);

    auto* cl = ws_connect(mgr.raw(),
                          (std::string("ws://127.0.0.1:") + std::to_string(port))
                              .c_str(),
                          ws_echo_client_ev, &st, nullptr);
    assert(cl != nullptr);

    auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline
           && (st.echoed.size() < st.sizes.size() || st.client_pongs == 0)) {
        mgr.poll(10ms);
    }

    assert(st.client_opens == 1);
    assert(st.echoed.size() == st.sizes.size());
    for (size_t i = 0; i < st.sizes.size(); i++)
        assert(st.echoed[i] == st.sizes[i]);  // each length round-tripped intact
    assert(st.client_pongs >= 1);             // Ping was answered with a Pong
}

// ---- WebSocket RFC 6455 enforcement ----

// After a valid handshake, an unmasked client->server frame violates RFC 6455
// 5.1 and the server must close the connection (with status 1002). Drives a raw
// socket so we can emit a deliberately non-conforming frame.
static void test_ws_unmasked_frame_rejected()
{
    const uint16_t port = 18899;
    Manager mgr;
    auto ref = mgr.http_listen(
        std::string("http://127.0.0.1:") + std::to_string(port),
        HandlerFn([](Connection& c, Event ev, void* ed) {
            if (ev == Event::HttpMessage)
                ws_upgrade(&c, static_cast<HttpMessage*>(ed), nullptr);
        }));
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread poller([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10ms);
    });

    bool server_closed = false;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct timeval tv{};
    tv.tv_sec = 3;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0) {
        const char* hs =
            "GET / HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
            "\r\n";
        (void)send(fd, hs, strlen(hs), 0);

        // Read the 101 handshake response (terminated by a blank line).
        char resp[512];
        std::string acc;
        while (acc.find("\r\n\r\n") == std::string::npos) {
            ssize_t n = recv(fd, resp, sizeof(resp), 0);
            if (n <= 0)
                break;
            acc.append(resp, static_cast<size_t>(n));
        }
        assert(acc.find(" 101 ") != std::string::npos);

        // Send an UNMASKED text frame: FIN|TEXT (0x81), len 3, no mask bit.
        unsigned char frame[] = {0x81, 0x03, 'a', 'b', 'c'};
        (void)send(fd, frame, sizeof(frame), 0);

        // The server must close. We may first receive its Close frame, then EOF.
        for (int i = 0; i < 100 && !server_closed; i++) {
            char buf[64];
            ssize_t n = recv(fd, buf, sizeof(buf), 0);
            if (n == 0) {
                server_closed = true;
            } else if (n < 0) {
                break;  // recv timeout: server never closed
            }
        }
    }
    close(fd);

    stop.store(true);
    poller.join();
    assert(server_closed);
}

// Observability counters: a full HTTP round-trip must move the cumulative
// metrics (accepted/closed/bytes) and leave the live gauge back at zero.
static void test_metrics()
{
    const uint16_t port = 18896;
    Manager mgr;
    Metrics before = mgr.metrics();
    assert(before.accepted == 0 && before.closed == 0);
    assert(before.bytes_read == 0 && before.bytes_written == 0);
    assert(before.active == 0);

    auto ref = mgr.http_listen(
        std::string("http://127.0.0.1:") + std::to_string(port),
        HandlerFn([](Connection& c, Event ev, void*) {
            if (ev == Event::HttpMessage)
                http_reply(&c, 200, "", "ok");
        }));
    assert(ref);

    // Client runs in a background thread while the main thread drives the loop.
    std::atomic<bool> got{false};
    std::thread client([&got, port]() { got.store(http_get_200(port)); });

    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline && !got.load())
        mgr.poll(20ms);
    client.join();
    // A few more polls so the "Connection: close" teardown completes.
    for (int i = 0; i < 10; i++)
        mgr.poll(20ms);

    assert(got.load());
    Metrics after = mgr.metrics();
    assert(after.accepted >= 1);       // the client connection was accepted
    assert(after.closed >= 1);         // and closed after the response
    assert(after.bytes_read > 0);      // the request was read
    assert(after.bytes_written > 0);   // the response was written
    assert(after.active == 0);         // live gauge back to zero
}

// Connection idle timeout: an accepted connection that sends nothing must be
// closed by the event loop after the configured idle period. Defends against
// connect-and-idle resource exhaustion.
static void test_idle_timeout()
{
    // closed must outlive mgr (mgr_free()'s teardown poll can fire the handler).
    std::atomic<int> closed{0};
    Manager mgr;
    mgr.set_idle_timeout(150);  // ms
    assert(mgr.idle_timeout() == 150);

    auto ref = mgr.http_listen("http://127.0.0.1:18892",
        HandlerFn([&closed](Connection&, Event ev, void*) {
            if (ev == Event::Close)
                closed.fetch_add(1, std::memory_order_relaxed);
        }));
    assert(ref);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(18892);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);

    // Drive the loop; the client never sends, so the server must close it.
    // Poll for up to ~2s (well beyond the 150ms timeout) checking for EOF.
    bool server_closed = false;
    for (int i = 0; i < 100 && !server_closed; i++) {
        mgr.poll(20ms);
        char buf[16];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0)
            server_closed = true;  // orderly shutdown: server closed the conn
        else
            std::this_thread::sleep_for(10ms);
    }
    close(fd);

    assert(server_closed);          // the idle connection was reaped
    assert(closed.load() >= 1);     // and a Close event fired for it
}

// Request-receive deadline: a connection that dribbles bytes but never completes
// a request must be reaped, even though the dribble keeps the idle timer fresh.
static void test_request_timeout()
{
    std::atomic<int> closed{0};
    Manager mgr;
    mgr.set_request_timeout(200);  // ms to complete a request
    mgr.set_idle_timeout(5000);    // generous: must NOT be what closes us
    assert(mgr.request_timeout() == 200);

    auto ref = mgr.http_listen("http://127.0.0.1:18893",
        HandlerFn([&closed](Connection&, Event ev, void*) {
            if (ev == Event::Close)
                closed.fetch_add(1, std::memory_order_relaxed);
        }));
    assert(ref);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(18893);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);

    // Send an incomplete request, then trickle bytes that never finish it. The
    // trickle keeps the idle clock fresh, so only the request deadline can reap
    // this connection -- and it must, well before the 5s idle timeout.
    const char* partial = "GET / HTTP/1.1\r\nHost: x\r\n";
    assert(send(fd, partial, strlen(partial), 0) > 0);

    bool server_closed = false;
    for (int i = 0; i < 80 && !server_closed; i++) {
        mgr.poll(20ms);
        if (i % 3 == 0) {
            char b = 'X';
            (void)send(fd, &b, 1, 0);  // keep the connection active
        }
        char buf[16];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0)
            server_closed = true;
        else
            std::this_thread::sleep_for(10ms);
    }
    close(fd);

    assert(server_closed);       // the never-completing request was reaped
    assert(closed.load() >= 1);
}

// Client connect timeout: a hung outbound connect() to a black-holed peer must
// be reaped by the event loop after connect_timeout_ms, rather than lingering
// for the (much longer) OS-default SYN timeout.
struct ConnectTimeoutState {
    std::atomic<int> closed{0};
    std::atomic<int> connected{0};
};

static void connect_timeout_ev(Connection* c, int ev, void*)
{
    auto* st = static_cast<ConnectTimeoutState*>(c->fn_data);
    if (ev == MG_EV_CONNECT)
        st->connected.fetch_add(1, std::memory_order_relaxed);
    else if (ev == MG_EV_CLOSE)
        st->closed.fetch_add(1, std::memory_order_relaxed);
}

static void test_connect_timeout()
{
    ConnectTimeoutState st;
    Manager mgr;
    mgr.set_connect_timeout(200);  // ms
    assert(mgr.connect_timeout() == 200);

    // 192.0.2.0/24 is TEST-NET-1 (RFC 5737): reserved and not routable, so the
    // SYN goes unanswered and the connect stays pending until we reap it. Using
    // an IP literal skips DNS, so only the connect phase is exercised.
    auto* c = connect(mgr.raw(), "tcp://192.0.2.1:9", connect_timeout_ev, &st);
    assert(c != nullptr);

    // Pump the loop past the 200ms deadline (bounded well under any OS SYN
    // timeout). The connection must be closed without ever connecting.
    auto deadline = std::chrono::steady_clock::now() + 3s;
    while (std::chrono::steady_clock::now() < deadline
           && st.closed.load() == 0) {
        mgr.poll(20ms);
    }

    assert(st.connected.load() == 0);  // never established
    assert(st.closed.load() >= 1);     // reaped by the connect timeout
}

// Request-body cap: a request advertising a Content-Length over the cap must be
// rejected with 413 before its body is buffered, and the user handler must not
// be invoked for it.
static void test_max_body_size()
{
    std::atomic<int> served{0};
    Manager mgr;
    mgr.set_max_body_size(1024);  // 1 KB cap
    assert(mgr.max_body_size() == 1024);

    auto ref = mgr.http_listen("http://127.0.0.1:18894",
        HandlerFn([&served](Connection& c, Event ev, void*) {
            if (ev == Event::HttpMessage) {
                served.fetch_add(1, std::memory_order_relaxed);
                http_reply(&c, 200, "", "ok");
            }
        }));
    assert(ref);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(18894);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);

    // Advertise an oversized body; send only the headers (no body bytes). The
    // server must reject on the declared Content-Length alone.
    const char* req =
        "POST /x HTTP/1.1\r\nHost: x\r\nContent-Length: 100000\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);

    bool got_413 = false, got_200 = false;
    for (int i = 0; i < 60 && !got_413; i++) {
        mgr.poll(20ms);
        char buf[512];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, MSG_DONTWAIT);
        if (n > 0) {
            buf[n] = '\0';
            if (strstr(buf, "413"))
                got_413 = true;
            if (strstr(buf, " 200 "))
                got_200 = true;
        } else {
            std::this_thread::sleep_for(10ms);
        }
    }
    close(fd);

    assert(got_413);                 // rejected with 413
    assert(!got_200);                // never served a 200
    assert(served.load() == 0);      // handler never saw the oversized request
}

// H3 Stage B: the destructor must shut down an in-flight run() (possibly on
// another thread) on its own -- without an explicit stop()/join beforehand --
// rather than racing teardown against the running loops.
static void test_sharded_destructor_stops_run()
{
    std::thread runner;
    {
        ShardedManager sharded(2);
        sharded.http_listen("http://127.0.0.1:18891",
            [](Connection& c, HttpMessage&) {
                http_reply(&c, 200, "", "ok");
            });
        runner = std::thread([&sharded]() { sharded.run(); });
        std::this_thread::sleep_for(100ms);
        (void)http_get_200(18891);  // prove it served at least one request
        // No explicit stop() or join here: leaving this scope destroys `sharded`,
        // and ~ShardedManager must stop run() and wait for it to finish.
    }
    // By now run() has returned (the destructor waited), so this join is immediate.
    runner.join();
}

// Max-connection cap (single Manager): once the live accepted-connection count
// reaches the cap, further accepted sockets are closed immediately instead of
// adopted. Closing an accepted connection must free a slot for a new one.
static void test_max_connections()
{
    Manager mgr;
    mgr.set_max_connections(2);
    assert(mgr.max_connections() == 2);

    auto ref = mgr.http_listen("http://127.0.0.1:18895",
        HandlerFn([](Connection&, Event, void*) {}));
    assert(ref);

    auto dial = []() {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(18895);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa),
                       sizeof(sa)) == 0);
        return fd;
    };
    auto is_eof = [](int fd) {
        char buf[8];
        return recv(fd, buf, sizeof(buf), MSG_DONTWAIT) == 0;
    };

    // Three idle clients against a cap of two: the third is rejected.
    int a = dial(), b = dial(), c = dial();
    for (int i = 0; i < 40; i++) mgr.poll(20ms);

    assert(mgr.num_connections() == 2);  // cap held, third not adopted
    int eofs = (is_eof(a) ? 1 : 0) + (is_eof(b) ? 1 : 0) + (is_eof(c) ? 1 : 0);
    assert(eofs == 1);                   // exactly one was dropped

    // Close all clients; the server must account every close back to zero.
    close(a); close(b); close(c);
    for (int i = 0; i < 40; i++) mgr.poll(20ms);
    assert(mgr.num_connections() == 0);

    // A slot is free again, so a fresh connection is accepted.
    int d = dial();
    for (int i = 0; i < 40; i++) mgr.poll(20ms);
    assert(mgr.num_connections() == 1);
    close(d);
    for (int i = 0; i < 20; i++) mgr.poll(20ms);
}

// Send-buffer high-water mark (single Manager): a reader that cannot keep up
// must be dropped once the server's unsent backlog exceeds the cap, instead of
// the server buffering the whole response. The connection is closed mid-stream,
// so the client reads far less than the full body before EOF.
static void test_max_send_buffer()
{
    Manager mgr;
    mgr.set_max_send_buffer(64 * 1024);  // 64 KB cap
    // No idle/request timeout: only backpressure can close this connection.

    const size_t body_size = 4 * 1024 * 1024;  // 4 MB, far over the cap
    std::string big(body_size, 'x');
    auto ref = mgr.http_listen("http://127.0.0.1:18896",
        HandlerFn([&big](Connection& c, Event ev, void*) {
            if (ev == Event::HttpMessage)
                http_reply(&c, 200, "Content-Type: text/plain\r\n", "%s",
                           big.c_str());
        }));
    assert(ref);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(18896);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);

    const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);

    // Read in large chunks but far slower than the server would like; the 4 MB
    // backlog stays well above the 64 KB cap, so the server drops us. We see EOF
    // after reading only what was already in flight -- much less than the body.
    size_t total = 0;
    bool server_closed = false;
    for (int i = 0; i < 200 && !server_closed; i++) {
        mgr.poll(20ms);
        char buf[64 * 1024];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0)
            server_closed = true;     // server's FIN: connection was dropped
        else if (n > 0)
            total += static_cast<size_t>(n);
        else
            std::this_thread::sleep_for(10ms);
    }
    close(fd);

    assert(server_closed);            // backpressure closed the slow reader
    assert(total < body_size);        // the full body was never delivered
    assert(mgr.num_connections() == 0);
}

// Global max-connection cap (ShardedManager): enforced at the single acceptor
// across all workers. Excess connections are closed without handoff; a worker
// closing a connection frees a slot.
static void test_sharded_max_connections()
{
    const uint16_t port = 18897;
    ShardedManager sharded(4);
    sharded.set_max_connections(4);
    assert(sharded.max_connections() == 4);

    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [](Connection& c, HttpMessage&) {
            http_reply(&c, 200, "", "ok");
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(100ms);

    // Eight idle clients (send nothing) against a global cap of four.
    std::vector<int> fds;
    for (int i = 0; i < 8; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        assert(fd >= 0);
        struct sockaddr_in sa{};
        sa.sin_family = AF_INET;
        sa.sin_port = htons(port);
        sa.sin_addr.s_addr = inet_addr("127.0.0.1");
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0)
            fds.push_back(fd);
        else
            close(fd);
    }
    std::this_thread::sleep_for(400ms);  // let the acceptor process every accept

    assert(sharded.num_connections() == 4);  // global cap held

    int eofs = 0, alive = 0;
    for (int fd : fds) {
        char buf[8];
        ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
        if (n == 0) eofs++;
        else alive++;
    }
    assert(eofs == 4);   // four rejected without handoff
    assert(alive == 4);  // four adopted

    for (int fd : fds) close(fd);
    sharded.stop();
    runner.join();
}

// Graceful drain: a request in flight when drain() is called must still receive
// its complete response, the acceptor must stop taking new connections, and
// run() must return once every connection has finished and closed.
static void test_sharded_graceful_drain()
{
    const uint16_t port = 18898;
    ShardedManager sharded(2);

    std::atomic<int> served{0};
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [&served](Connection& c, HttpMessage&) {
            // Simulate work still in progress when drain() is called.
            std::this_thread::sleep_for(300ms);
            served.fetch_add(1, std::memory_order_relaxed);
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "ok");
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(100ms);

    // Fire a request, then begin draining while the handler is mid-flight.
    std::atomic<bool> got200{false};
    std::thread client([&got200, port]() { got200.store(http_get_200(port)); });
    std::this_thread::sleep_for(100ms);  // request is sent, handler is sleeping
    sharded.drain(3000);                 // must let the in-flight request finish

    client.join();
    runner.join();  // run() returns on its own once fully drained

    assert(got200.load());                   // in-flight request got its 200
    assert(served.load() == 1);
    assert(sharded.num_connections() == 0);  // everything drained and closed
}

// Drain deadline: a connection that cannot finish (a response that never flushes
// because the client never reads) must not hang the drain. The deadline forces
// run() to return.
static void test_sharded_drain_deadline()
{
    const uint16_t port = 18899;
    ShardedManager sharded(2);

    // To exercise the deadline the connection must still have unflushed data
    // when drain() is called: if the whole response fits in the kernel socket
    // buffers it is "sent" at once, the connection closes, live_conns_ hits 0,
    // and drain() returns immediately (so `elapsed >= 250` would fail). The
    // amount that can be buffered before a write blocks is the server send
    // buffer plus the client's advertised receive window, which on loopback is
    // large and platform-dependent (Linux autotunes both into the multi-MB
    // range, so the original 2 MB body was swallowed whole). 32 MB exceeds that
    // ceiling on both Linux and macOS, so with a client that never reads the
    // residue stays unflushed until the deadline forces run() to return.
    //
    // The body is queued with send_bytes() (a single bulk append) rather than
    // http_reply(): this test measures the drain deadline, not response
    // construction, and queuing 32 MB is the point -- not how it is produced.
    //
    // The deadline is what bounds the wait, not the flush: a stalled reader
    // keeps the socket writable, so a worker would otherwise busy-flush the
    // body, and the per-write iobuf compaction in io_send() makes that
    // O(n^2) -- pathologically slow under the sanitizers. drain() enforces its
    // deadline on the workers themselves (see ShardedManager::run()), so the
    // flush is preempted at ~300 ms regardless of how much is left.
    std::string big(32 * 1024 * 1024, 'x');
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [&big](Connection& c, HttpMessage&) { c.send_bytes(big); });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(100ms);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
    const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);
    std::this_thread::sleep_for(150ms);  // let the response queue up unflushed

    // The stuck connection cannot drain; the deadline must force run() to return.
    uint64_t t0 = millis();
    sharded.drain(300);
    runner.join();
    uint64_t elapsed = millis() - t0;
    close(fd);

    assert(elapsed >= 250);    // waited roughly the deadline
    assert(elapsed < 10000);   // but returned -- did not hang on the stuck conn
                               // (generous upper bound: sanitizer builds are slow)
}
#endif  // _WIN32

// ---- System resolver discovery ----

// The default DNS server used to be hardcoded to a public resolver, so
// internal names never resolved and queries left the network. These pin the
// resolv.conf parsing, which is the part that can be tested without depending
// on the host's actual configuration.
static void test_parse_resolv_conf()
{
    // Typical file: comments, options, one nameserver.
    const char* typical =
        "# Generated by NetworkManager\n"
        "search example.com\n"
        "nameserver 192.168.1.1\n"
        "options edns0\n";
    assert(parse_resolv_conf(typical, false) == "udp://192.168.1.1:53");
    assert(parse_resolv_conf(typical, true).empty());  // no IPv6 entry

    // First entry of the requested family wins, regardless of ordering.
    const char* mixed =
        "nameserver fe80::1\n"
        "nameserver 10.0.0.53\n"
        "nameserver 10.0.0.54\n";
    assert(parse_resolv_conf(mixed, false) == "udp://10.0.0.53:53");
    assert(parse_resolv_conf(mixed, true) == "udp://[fe80::1]:53");

    // An IPv6 zone suffix is preserved (the address parser understands it).
    assert(parse_resolv_conf("nameserver fe80::1%eth0\n", true)
           == "udp://[fe80::1%eth0]:53");

    // Comments and indentation.
    assert(parse_resolv_conf("  \tnameserver\t9.9.9.9  # quad9\n", false)
           == "udp://9.9.9.9:53");
    assert(parse_resolv_conf("#nameserver 1.2.3.4\n", false).empty());
    assert(parse_resolv_conf(";nameserver 1.2.3.4\n", false).empty());

    // Directives that merely start with the keyword must not match.
    assert(parse_resolv_conf("nameservers 1.2.3.4\n", false).empty());
    assert(parse_resolv_conf("nameserver\n", false).empty());
    assert(parse_resolv_conf("nameserver   \n", false).empty());

    // Degenerate inputs.
    assert(parse_resolv_conf("", false).empty());
    assert(parse_resolv_conf("\n\n\n", false).empty());
    // A final line without a trailing newline still parses.
    assert(parse_resolv_conf("nameserver 8.8.4.4", false) == "udp://8.8.4.4:53");

    // Whatever this host is configured with, a Manager must end up with a
    // usable UDP URL rather than an empty or malformed one.
    const char* url = system_dns_url(false);
    assert(url != NULL && strncmp(url, "udp://", 6) == 0);
    Manager mgr;
    assert(mgr.raw()->dns4.url != NULL);
    assert(strncmp(mgr.raw()->dns4.url, "udp://", 6) == 0);
}

// ---- Binary-safe reply (http_reply_bytes) ----

// http_reply() formats its body through xprintf's "%s", which stops at the
// first NUL byte even with an explicit precision -- so it silently truncates
// binary payloads. http_reply_bytes() is length-counted; this pins that down.
static void test_http_reply_bytes_binary_safe()
{
    const uint16_t port = 18901;
    const char payload[] = { '\x89', '\xff', '\x00', 'A', '\x80' };
    const size_t payload_len = sizeof(payload);

    Manager mgr;
    auto ref = mgr.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [&](Connection& c, HttpMessage&) {
            http_reply_bytes(&c, 200, "Content-Type: application/octet-stream\r\n",
                             payload, payload_len);
        });
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread loop([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10);
    });
    std::this_thread::sleep_for(50ms);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
    const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);

    char buf[512];
    size_t total = 0;
    for (int i = 0; i < 50 && total < sizeof(buf); i++) {
        struct timeval tv { 0, 100000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, buf + total, sizeof(buf) - total, 0);
        if (n > 0)
            total += static_cast<size_t>(n);
        std::string_view got(buf, total);
        if (got.find("\r\n\r\n") != std::string_view::npos
            && got.size() >= got.find("\r\n\r\n") + 4 + payload_len)
            break;
    }
    close(fd);
    stop.store(true);
    loop.join();

    std::string_view resp(buf, total);
    size_t hdr_end = resp.find("\r\n\r\n");
    assert(hdr_end != std::string_view::npos);
    assert(resp.find("Content-Length: 5") != std::string_view::npos);
    std::string_view body = resp.substr(hdr_end + 4);
    assert(body.size() == payload_len);           // not truncated at the NUL
    assert(memcmp(body.data(), payload, payload_len) == 0);
}

// ---- Streamed responses (chunked prologue + chunk writes) ----

static void test_http_chunked_response()
{
    const uint16_t port = 18902;

    Manager mgr;
    auto ref = mgr.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [](Connection& c, HttpMessage&) {
            http_start_chunked(&c, 200, "Content-Type: text/plain\r\n");
            http_write_chunk(&c, "alpha", 5);
            http_write_chunk(&c, "beta", 4);
            http_write_chunk(&c, "", 0);  // terminating chunk
        });
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread loop([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10);
    });
    std::this_thread::sleep_for(50ms);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
    const char* req = "GET / HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);

    char buf[1024];
    size_t total = 0;
    for (int i = 0; i < 50 && total < sizeof(buf); i++) {
        struct timeval tv { 0, 100000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, buf + total, sizeof(buf) - total, 0);
        if (n > 0)
            total += static_cast<size_t>(n);
        if (std::string_view(buf, total).find("0\r\n\r\n") != std::string_view::npos)
            break;
    }
    close(fd);
    stop.store(true);
    loop.join();

    std::string_view resp(buf, total);
    assert(resp.find("Transfer-Encoding: chunked") != std::string_view::npos);
    // Chunk framing: <hex len>\r\n<data>\r\n, ended by a zero-length chunk.
    assert(resp.find("5\r\nalpha\r\n") != std::string_view::npos);
    assert(resp.find("4\r\nbeta\r\n") != std::string_view::npos);
    assert(resp.find("0\r\n\r\n") != std::string_view::npos);
}

static void test_http_sse_prologue()
{
    const uint16_t port = 18903;

    Manager mgr;
    auto ref = mgr.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [](Connection& c, HttpMessage&) {
            http_start_sse(&c, "");
            const char* ev = "data: tick\n\n";
            http_write_chunk(&c, ev, strlen(ev));
        });
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread loop([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10);
    });
    std::this_thread::sleep_for(50ms);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = inet_addr("127.0.0.1");
    assert(connect(fd, reinterpret_cast<struct sockaddr*>(&sa), sizeof(sa)) == 0);
    const char* req = "GET /events HTTP/1.1\r\nHost: x\r\n\r\n";
    assert(send(fd, req, strlen(req), 0) > 0);

    char buf[1024];
    size_t total = 0;
    for (int i = 0; i < 50 && total < sizeof(buf); i++) {
        struct timeval tv { 0, 100000 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        ssize_t n = recv(fd, buf + total, sizeof(buf) - total, 0);
        if (n > 0)
            total += static_cast<size_t>(n);
        if (std::string_view(buf, total).find("tick") != std::string_view::npos)
            break;
    }
    close(fd);
    stop.store(true);
    loop.join();

    std::string_view resp(buf, total);
    assert(resp.find("Content-Type: text/event-stream") != std::string_view::npos);
    assert(resp.find("Cache-Control: no-cache") != std::string_view::npos);
    assert(resp.find("Transfer-Encoding: chunked") != std::string_view::npos);
    assert(resp.find("data: tick") != std::string_view::npos);
}

// ---- Cross-thread wakeup on a single Manager ----

// wakeup() exists to act on a connection from another thread while the loop
// polls. Arming the pipe on first use would have allocated a Connection and
// spliced it into mgr->conns from the calling thread while the loop walked
// that list; the pipe is armed in the constructor instead. Run under TSan,
// this test is what proves the point.
static void test_manager_wakeup_cross_thread()
{
    const uint16_t port = 18907;

    Manager mgr;
    std::atomic<unsigned long> conn_id{0};
    std::atomic<int> woken{0};
    std::string payload = "woken-payload";

    auto ref = mgr.http_listen(
        std::string("http://127.0.0.1:") + std::to_string(port),
        HandlerFn([&](Connection& c, Event ev, void* ev_data) {
            if (ev == Event::HttpMessage) {
                conn_id.store(c.id);   // reply from the wakeup, not from here
            } else if (ev == Event::Wakeup) {
                auto* d = static_cast<struct Str*>(ev_data);
                woken.fetch_add(1, std::memory_order_relaxed);
                http_reply(&c, 200, "Content-Type: text/plain\r\n", "%.*s",
                           static_cast<int>(d->len), d->buf);
            }
        }));
    assert(ref);

    std::atomic<bool> stop{false};
    std::thread loop([&mgr, &stop]() {
        while (!stop.load())
            mgr.poll(10);
    });
    std::this_thread::sleep_for(50ms);

    std::atomic<bool> got_200{false};
    std::thread client([&got_200, port]() {
        got_200.store(http_get_200(port));
    });

    // Wait for the request to reach the handler, then wake it from this thread.
    for (int i = 0; i < 200 && conn_id.load() == 0; i++)
        std::this_thread::sleep_for(10ms);
    assert(conn_id.load() != 0);
    assert(mgr.wakeup(conn_id.load(), payload));

    client.join();
    stop.store(true);
    loop.join();

    assert(woken.load() == 1);
    assert(got_200.load());
    assert(!mgr.wakeup(0, "x"));  // id 0 is rejected
}

// ---- ShardedManager: event handlers, unique ids, routed wakeup ----

// http_listen() only ever delivers MG_EV_HTTP_MSG, so WebSocket (and every
// other event) was unreachable on the sharded path. http_listen_event()
// delivers the whole event stream; ids must also be unique across workers, or
// a wakeup cannot be routed to the connection that owns it.
static void test_sharded_event_handler_and_wakeup()
{
    const uint16_t port = 18904;
    const int num_workers = 4;
    const int num_clients = 12;

    ShardedManager sharded(num_workers);

    std::mutex mu;
    std::vector<unsigned long> ids;
    std::atomic<int> woken{0};
    std::atomic<bool> saw_non_http_event{false};

    sharded.http_listen_event(
        std::string("http://127.0.0.1:") + std::to_string(port),
        [&](Connection& c, Event ev, void* ev_data) {
            if (ev == Event::Open || ev == Event::Read)
                saw_non_http_event.store(true);  // http_listen() never shows these
            if (ev == Event::HttpMessage) {
                std::lock_guard<std::mutex> lock(mu);
                ids.push_back(c.id);  // reply later, from the wakeup
            } else if (ev == Event::Wakeup) {
                auto* d = static_cast<struct Str*>(ev_data);
                woken.fetch_add(1, std::memory_order_relaxed);
                http_reply(&c, 200, "Content-Type: text/plain\r\n", "%.*s",
                           static_cast<int>(d->len), d->buf);
            }
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(150ms);

    std::atomic<int> ok_count{0};
    std::vector<std::thread> clients;
    clients.reserve(num_clients);
    for (int i = 0; i < num_clients; i++)
        clients.emplace_back([&ok_count, port]() {
            if (http_get_200(port))
                ok_count.fetch_add(1, std::memory_order_relaxed);
        });

    // Wait for every request to reach a worker, then wake each connection --
    // this is what fails if two workers hand out the same id.
    for (int i = 0; i < 200; i++) {
        {
            std::lock_guard<std::mutex> lock(mu);
            if (ids.size() == static_cast<size_t>(num_clients))
                break;
        }
        std::this_thread::sleep_for(10ms);
    }
    {
        std::lock_guard<std::mutex> lock(mu);
        assert(ids.size() == static_cast<size_t>(num_clients));
        std::vector<unsigned long> sorted = ids;
        std::sort(sorted.begin(), sorted.end());
        assert(std::adjacent_find(sorted.begin(), sorted.end()) == sorted.end()
               && "connection ids collided across workers");
        for (unsigned long id : ids)
            assert(sharded.wakeup(id, "ok") && "wakeup did not route");
    }

    for (auto& t : clients)
        t.join();

    sharded.stop();
    runner.join();

    assert(ok_count.load() == num_clients);
    assert(woken.load() == num_clients);
    assert(saw_non_http_event.load() && "event handler saw only HTTP messages");
    // A wakeup outside run() has no pipe to write to and must say so.
    assert(!sharded.wakeup(1, "x"));
}

// Worker-thread hooks fire exactly once per worker, on the worker thread, and
// the stop hook runs however the loop exits. Language bindings rely on this to
// register a thread with their runtime once instead of once per request.
static void test_sharded_worker_hooks()
{
    const uint16_t port = 18908;
    const unsigned num_workers = 4;

    ShardedManager sharded(num_workers);
    std::atomic<int> starts{0}, stops{0};
    std::mutex mu;
    std::vector<std::thread::id> start_threads;

    sharded.set_worker_hooks(
        [&]() {
            std::lock_guard<std::mutex> lock(mu);
            start_threads.push_back(std::this_thread::get_id());
            starts.fetch_add(1, std::memory_order_relaxed);
        },
        [&]() { stops.fetch_add(1, std::memory_order_relaxed); });

    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [](Connection& c, HttpMessage&) {
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "ok");
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(150ms);
    assert(http_get_200(port));
    sharded.stop();
    runner.join();

    assert(starts.load() == static_cast<int>(num_workers));
    assert(stops.load() == static_cast<int>(num_workers));
    {
        // Each hook ran on a distinct thread: one per worker, not one per call.
        std::lock_guard<std::mutex> lock(mu);
        std::sort(start_threads.begin(), start_threads.end());
        assert(std::adjacent_find(start_threads.begin(), start_threads.end())
               == start_threads.end());
    }
}

// Two listeners with different handlers must both work: the handler travels
// with each handed-off connection, rather than the manager holding one.
static void test_sharded_multiple_listeners()
{
    const uint16_t port_a = 18905, port_b = 18906;
    ShardedManager sharded(2);

    std::atomic<int> hits_a{0}, hits_b{0};
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port_a),
        [&hits_a](Connection& c, HttpMessage&) {
            hits_a.fetch_add(1, std::memory_order_relaxed);
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "A");
        });
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port_b),
        [&hits_b](Connection& c, HttpMessage&) {
            hits_b.fetch_add(1, std::memory_order_relaxed);
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "B");
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(150ms);

    assert(http_get_200(port_a));
    assert(http_get_200(port_b));

    sharded.stop();
    runner.join();

    assert(hits_a.load() == 1 && hits_b.load() == 1);
}

int main()
{
    test_base64_roundtrip();
    test_base64_invalid();
    test_http_parse_request();
    test_http_creds_basic();
    test_http_creds_cookie_and_query();
    test_http_status_code();
    test_string_view_conversion();
    test_manager_raii();
    test_type_safe_callback_general();
    test_typed_http_handler();
    test_json_number();
    test_json_boolean();
    test_json_integer();
    test_json_string();
    test_json_nested();
    test_json_array();
    test_url_port();
    test_url_class();
    test_url_encode_decode();
    test_timer_expired();
    test_timer_basic();
    test_scoped_enums();
    test_str_constructors();
    test_http_header_limit();
    test_http_multipart();
    test_float_formatting();
    test_str_match_span();
    test_check_ip_acl();
    test_parse_resolv_conf();
    test_listen_tls_unavailable_fails_closed();
#if MG_TLS != MG_TLS_NONE
    test_tls_handshake_end_to_end();
    test_tls_mutual_auth();
#endif
    test_connection_send_bytes();

    // ShardedManager: verify construction and basic lifecycle
    {
        ShardedManager sharded(2);
        assert(sharded.num_workers() == 2);
        // Construction/destruction without run() should not crash
    }

#ifndef _WIN32
    test_http_chunked_request();
    test_ws_echo_e2e();
    test_ws_unmasked_frame_rejected();
    test_metrics();
    test_idle_timeout();
    test_request_timeout();
    test_connect_timeout();
    test_max_body_size();
    test_max_connections();
    test_max_send_buffer();
    test_sharded_concurrent_requests();
    test_sharded_destructor_stops_run();
    test_sharded_max_connections();
    test_sharded_graceful_drain();
    test_sharded_drain_deadline();
    test_http_reply_bytes_binary_safe();
    test_manager_wakeup_cross_thread();
    test_http_chunked_response();
    test_http_sse_prologue();
    test_sharded_event_handler_and_wakeup();
    test_sharded_multiple_listeners();
    test_sharded_worker_hooks();
#endif

    puts("All tests passed");
    return 0;
}
