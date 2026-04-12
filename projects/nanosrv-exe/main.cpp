#include "nanosrv/nanosrv.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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

static std::string build_listen_url(int argc, char** argv)
{
    int port = 8000;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = std::atoi(argv[++i]);
            if (port < 1 || port > 65535) {
                fprintf(stderr, "Invalid port\n");
                exit(1);
            }
        } else if (strcmp(argv[i], "--busy") == 0 && i + 1 < argc) {
            s_busy_us = std::atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0) {
            fprintf(stderr, "Usage: %s [--port <1-65535>] [--busy <microseconds>]\n", argv[0]);
            exit(0);
        }
    }
    return "http://0.0.0.0:" + std::to_string(port);
}

int main(int argc, char** argv)
{
    auto url = build_listen_url(argc, argv);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    log_level = MG_LL_ERROR;  // Suppress debug logging for performance
    Manager mgr;

    // Typed HTTP handler: no event code check, no void* cast, just the message.
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
