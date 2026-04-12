#include "nanosrv/nanosrv.hpp"

#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    int port = 8000;
    unsigned threads = 0; // 0 = hardware_concurrency
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--port") == 0 && i + 1 < argc)
            port = std::atoi(argv[++i]);
        else if (strcmp(argv[i], "--threads") == 0 && i + 1 < argc)
            threads = static_cast<unsigned>(std::atoi(argv[++i]));
        else if (strcmp(argv[i], "--busy") == 0 && i + 1 < argc)
            s_busy_us = std::atoi(argv[++i]);
    }

    log_level = MG_LL_ERROR;

    auto url = "http://0.0.0.0:" + std::to_string(port);

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);

    ShardedManager mgr(threads);
    mgr.http_listen(url,
        [](Connection& c, HttpMessage& hm) {
            busy_spin(s_busy_us);
            http_reply(&c, 200, "Content-Type: text/plain\r\n",
                       "OK\n");
            (void)hm;
        });

    printf("nanosrv-sharded listening on %s (%u workers)\n",
           url.c_str(), mgr.num_workers());

    // run() blocks, but we need to stop on signal.
    // Use a separate thread for run(), main thread waits for signal.
    std::thread runner([&mgr]() { mgr.run(); });

    while (s_running) {
        std::this_thread::sleep_for(100ms);
    }

    printf("\nShutting down...\n");
    mgr.stop();
    runner.join();
    return 0;
}
