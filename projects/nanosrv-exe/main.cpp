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

    app.add_option("-p,--port", port, "Listen port")
        ->default_val(8000)
        ->check(CLI::Range(1, 65535));
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
    Manager mgr;

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
    printf("\nShutting down (%d)\n", s_signal);
    return 0;
}
