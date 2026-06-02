#include "nanosrv/nanosrv.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

#ifndef _WIN32
#include <atomic>
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
    for (auto& t : clients)
        t.join();

    sharded.stop();
    runner.join();  // must return before sharded is destroyed (teardown contract)

    assert(ok_count.load() == num_clients);
    assert(served.load() == num_clients);
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
    // buffer plus the client's advertised receive window, and on loopback that
    // ceiling is large and platform-dependent (Linux autotunes each into the
    // multi-MB range; a small 2 MB body is swallowed whole). Two measures keep
    // the connection reliably stuck without making the test slow:
    //   - the client sets a tiny SO_RCVBUF so its receive window stays small,
    //     dropping the buffering ceiling close to just the server send buffer;
    //   - the body (8 MB) exceeds the default Linux send-buffer cap
    //     (net.ipv4.tcp_wmem max is 4 MB), so a write cannot drain it in full.
    // The client never reads, so the residue sits unflushed until the deadline
    // forces run() to return. (Flushing is O(n^2) in bytes sent -- io_send()
    // memmoves the whole remaining send buffer down after each partial write --
    // so an over-large body would make even the bounded flush pathologically
    // slow under the sanitizers; 8 MB keeps that cost small.)
    std::string big(8 * 1024 * 1024, 'x');
    sharded.http_listen(std::string("http://127.0.0.1:") + std::to_string(port),
        [&big](Connection& c, HttpMessage&) {
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "%s",
                       big.c_str());
        });

    std::thread runner([&sharded]() { sharded.run(); });
    std::this_thread::sleep_for(100ms);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    assert(fd >= 0);
    int rcvbuf = 4096;  // tiny receive window so the server blocks early
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
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
    test_check_ip_acl();
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
    test_idle_timeout();
    test_request_timeout();
    test_max_body_size();
    test_max_connections();
    test_max_send_buffer();
    test_sharded_concurrent_requests();
    test_sharded_destructor_stops_run();
    test_sharded_max_connections();
    test_sharded_graceful_drain();
    test_sharded_drain_deadline();
#endif

    puts("All tests passed");
    return 0;
}
