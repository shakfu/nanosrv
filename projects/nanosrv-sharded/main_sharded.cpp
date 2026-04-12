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

    app.add_option("-p,--port", port, "Listen port")
        ->default_val(8000)
        ->check(CLI::Range(1, 65535));
    app.add_option("-t,--threads", threads, "Worker threads (0 = all cores)")
        ->default_val(0);
    app.add_option("-b,--busy", busy, "Microseconds of CPU spin per request")
        ->default_val(0)
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
    mgr.http_listen(url,
        [](Connection& c, HttpMessage& hm) {
            busy_spin(s_busy_us);
            http_reply(&c, 200, "Content-Type: text/plain\r\n", "OK\n");
            (void)hm;
        });

    printf("nanosrv-sharded listening on %s (%u workers)\n",
           url.c_str(), mgr.num_workers());

    std::thread runner([&mgr]() { mgr.run(); });

    while (s_running) {
        std::this_thread::sleep_for(100ms);
    }

    printf("\nShutting down...\n");
    mgr.stop();
    runner.join();
    return 0;
}
