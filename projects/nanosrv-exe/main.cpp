#include "nanosrv/nanosrv.hpp"
#include <CLI11.hpp>
#include <rang.hpp>

#include <chrono>
#include <csignal>
#include <cstdio>
#include <iostream>
#include <string>

using namespace nanosrv;
using namespace std::chrono_literals;

static volatile sig_atomic_t s_signal;
static void handle_signal(int signo) { s_signal = signo; }

static int s_busy_us = 0;

static void busy_spin(int us)
{
    if (us <= 0) return;
    auto start = std::chrono::steady_clock::now();
    auto target = std::chrono::microseconds(us);
    while (std::chrono::steady_clock::now() - start < target) {
        // spin
    }
}

int main(int argc, char** argv)
{
    CLI::App app{"nanosrv-server -- single-threaded HTTP server"};

    int port = 8000;
    int busy = 0;
    int idle_timeout = 0;
    int request_timeout = 0;
    size_t max_body = 0;
    int max_conns = 0;
    size_t max_send = 0;
    int drain_timeout = 5000;

    app.add_option("-p,--port", port, "Listen port")
        ->default_val(8000)
        ->check(CLI::Range(1, 65535));
    app.add_option("-b,--busy", busy, "Microseconds of CPU spin per request")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--idle-timeout", idle_timeout,
                   "Close idle connections after N ms (0 = disabled)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--request-timeout", request_timeout,
                   "Close connections that buffer an incomplete request past N ms (0 = disabled)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--max-body", max_body,
                   "Reject request bodies larger than N bytes with HTTP 413 (0 = disabled)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--max-connections", max_conns,
                   "Cap simultaneously accepted connections; excess are closed (0 = disabled)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--max-send-buffer", max_send,
                   "Close a connection whose unsent outbound backlog exceeds N bytes (0 = disabled)")
        ->default_val(0)
        ->check(CLI::NonNegativeNumber);
    app.add_option("--drain-timeout", drain_timeout,
                   "On shutdown, finish in-flight requests for up to N ms before forcing close (0 = stop immediately)")
        ->default_val(5000)
        ->check(CLI::NonNegativeNumber);

    std::atexit([]() { std::cout << rang::style::reset; });
    try {
        app.parse(argc, argv);
    } catch (const CLI::ParseError& e) {
        if (e.get_exit_code() == 0)
            std::cout << rang::style::bold;
        else
            std::cout << rang::style::bold << rang::fg::red;
        return app.exit(e);
    }

    s_busy_us = busy;
    auto url = "http://0.0.0.0:" + std::to_string(port);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_level = MG_LL_ERROR;
    Manager mgr;

    if (idle_timeout > 0) mgr.set_idle_timeout(idle_timeout);
    if (request_timeout > 0) mgr.set_request_timeout(request_timeout);
    if (max_body > 0) mgr.set_max_body_size(max_body);
    if (max_conns > 0) mgr.set_max_connections(max_conns);
    if (max_send > 0) mgr.set_max_send_buffer(max_send);

    auto listener = mgr.http_listen(url,
        [](Connection& c, HttpMessage& hm) {
            busy_spin(s_busy_us);
            http_reply(&c, 200, "Content-Type: text/plain\r\n",
                       "nanosrv-server ready\nMethod: %.*s, URI: %.*s\n",
                       static_cast<int>(hm.method_str().size()), hm.method_str().data(),
                       static_cast<int>(hm.uri_str().size()), hm.uri_str().data());
        });

    if (!listener) {
        fprintf(stderr, "Failed to listen on %s\n", url.c_str());
        return 1;
    }

    printf("nanosrv-server listening on %s\n", url.c_str());
    while (!s_signal) {
        mgr.poll(100ms);
    }

    if (drain_timeout > 0) {
        // Graceful drain: stop accepting, finish in-flight responses, then exit.
        printf("\nDraining (up to %d ms for in-flight requests)...\n",
               drain_timeout);
        mgr.start_drain();
        uint64_t deadline = millis() + static_cast<uint64_t>(drain_timeout);
        while (mgr.num_connections() > 0 && millis() < deadline)
            mgr.poll(50ms);
    } else {
        printf("\nShutting down (%d)\n", s_signal);
    }
    return 0;
}
