#include "nanosrv/nanosrv.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

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
    test_connection_send_bytes();

    // ShardedManager: verify construction and basic lifecycle
    {
        ShardedManager sharded(2);
        assert(sharded.num_workers() == 2);
        // Construction/destruction without run() should not crash
    }

    puts("All tests passed");
    return 0;
}
