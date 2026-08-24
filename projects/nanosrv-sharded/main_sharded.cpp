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

static std::atomic<bool> s_running{true};
static int s_busy_us = 0;
static void handle_signal(int) { s_running = false; }

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
    CLI::App app{"nanosrv-sharded -- multi-threaded HTTP server"};

    int port = 8000;
    unsigned threads = 0;
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
    app.add_option("-t,--threads", threads, "Worker threads (0 = all cores)")
        ->default_val(0);
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
                   "Global cap on simultaneously accepted connections; excess are closed (0 = disabled)")
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

    ShardedManager mgr(threads);

    if (idle_timeout > 0) mgr.set_idle_timeout(idle_timeout);
    if (request_timeout > 0) mgr.set_request_timeout(request_timeout);
    if (max_body > 0) mgr.set_max_body_size(max_body);
    if (max_conns > 0) mgr.set_max_connections(max_conns);
    if (max_send > 0) mgr.set_max_send_buffer(max_send);

    mgr.http_listen(url,
        [](Connection& c, HttpMessage& hm) {
            busy_spin(s_busy_us);
            // Same response as nanosrv-server, mungo-server and mongoose-server
            // (and the Python benchmark servers): a benchmark that compares
            // implementations is only meaningful if they do identical work.
            // This previously replied with a constant "OK\n" while every other
            // server formatted the method and URI into the body, so the
            // comparison was measuring different workloads.
            http_reply(&c, 200, "Content-Type: text/plain\r\n",
                       "nanosrv-sharded ready\nMethod: %.*s, URI: %.*s\n",
                       static_cast<int>(hm.method_str().size()), hm.method_str().data(),
                       static_cast<int>(hm.uri_str().size()), hm.uri_str().data());
        });

    printf("nanosrv-sharded listening on %s (%u workers)\n",
           url.c_str(), mgr.num_workers());

    std::thread runner([&mgr]() { mgr.run(); });

    while (s_running) {
        std::this_thread::sleep_for(100ms);
    }

    if (drain_timeout > 0) {
        printf("\nDraining (up to %d ms for in-flight requests)...\n",
               drain_timeout);
        mgr.drain(drain_timeout);  // finish in-flight, stop accepting, then exit
    } else {
        printf("\nShutting down...\n");
        mgr.stop();                // abrupt: abandon in-flight requests
    }
    runner.join();
    return 0;
}
