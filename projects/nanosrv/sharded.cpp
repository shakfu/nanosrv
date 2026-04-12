#include "nanosrv/nanosrv.hpp"
#include <chrono>
#include <mutex>
#include <queue>

using namespace std::chrono_literals;

namespace nanosrv {

// Pending connection: FD + addresses stolen from the acceptor.
struct PendingConn {
    MG_SOCKET_TYPE fd;
    Address rem;
    Address loc;
};

// Per-worker queue of connections to adopt.
struct WorkerQueue {
    std::mutex mu;
    std::queue<PendingConn> pending;
};

ShardedManager::ShardedManager(unsigned num_threads)
{
    if (num_threads == 0)
        num_threads = std::thread::hardware_concurrency();
    if (num_threads == 0)
        num_threads = 2;  // Fallback

    workers_.reserve(num_threads);
    for (unsigned i = 0; i < num_threads; i++)
        workers_.push_back(std::make_unique<Manager>());
}

ShardedManager::~ShardedManager()
{
    stop();
}

void ShardedManager::http_listen(std::string_view url, HttpHandler handler)
{
    // Shared handler and per-worker queues
    auto shared_handler = std::make_shared<HttpHandler>(std::move(handler));
    auto queues = std::make_shared<std::vector<WorkerQueue>>(workers_.size());
    queues_ = queues;
    handler_ = shared_handler;

    std::string url_str(url);

    // Set up a timer on each worker that drains its queue every 1ms.
    // Adopted FDs get the HTTP protocol handler and the user's callback.
    for (size_t i = 0; i < workers_.size(); i++) {
        auto* mgr_raw = workers_[i]->raw();
        auto* q = &(*queues)[i];
        auto h = shared_handler;

        // Typed HTTP trampoline for adopted connections.
        // Each adopted connection gets its own copy of fn/fn_data.
        struct AdoptCtx {
            std::shared_ptr<HttpHandler> handler;
            WorkerQueue* queue;
            Mgr* mgr;
        };

        auto* ctx = new AdoptCtx{h, q, mgr_raw};

        // Use a timer (1ms repeat) to drain the queue.
        timer_add(mgr_raw, 1, MG_TIMER_REPEAT,
            [](void* arg) {
                auto* ctx = static_cast<AdoptCtx*>(arg);
                PendingConn pc;
                for (;;) {
                    {
                        std::lock_guard<std::mutex> lock(ctx->queue->mu);
                        if (ctx->queue->pending.empty()) break;
                        pc = ctx->queue->pending.front();
                        ctx->queue->pending.pop();
                    }
                    // Adopt the FD into this worker's event loop.
                    // http_handler_adopt sets up the HTTP protocol handler
                    // and the user's callback.
                    auto* handler_copy = new HttpHandler(*ctx->handler);
                    auto* c = wrapfd(ctx->mgr, static_cast<int>(pc.fd),
                        [](Connection* c, int ev, void* ev_data) {
                            auto* fn = static_cast<HttpHandler*>(c->fn_data);
                            if (ev == MG_EV_HTTP_MSG) {
                                (*fn)(*c, *static_cast<HttpMessage*>(ev_data));
                            }
                            if (ev == MG_EV_CLOSE) {
                                delete static_cast<HttpHandler*>(c->fn_data);
                                c->fn_data = nullptr;
                            }
                        },
                        handler_copy);
                    if (c) {
                        c->rem = pc.rem;
                        c->loc = pc.loc;
                        c->is_accepted = 1;
                        c->pfn = http_cb;
                    } else {
                        delete handler_copy;
                        close(static_cast<int>(pc.fd));
                    }
                }
            },
            ctx);
    }

    // The acceptor listens on the URL. On MG_EV_ACCEPT, steal the FD
    // and push it to a worker queue round-robin.
    struct AcceptCtx {
        std::shared_ptr<std::vector<WorkerQueue>> queues;
        std::atomic<unsigned>* next;
        unsigned num_workers;
    };
    auto* actx = new AcceptCtx{queues, &next_, static_cast<unsigned>(workers_.size())};

    acceptor_.http_listen(url_str,
        HandlerFn([actx](Connection& c, Event ev, void*) {
            if (ev == Event::Accept) {
                // Steal the FD from the accepted connection
                Address rem = c.rem;
                Address loc = c.loc;
                MG_SOCKET_TYPE fd = detach_fd(&c);
                c.is_closing = 1;  // Tell the event loop to clean up this connection

                if (fd != MG_INVALID_SOCKET) {
                    // Round-robin to a worker
                    unsigned idx = actx->next->fetch_add(1, std::memory_order_relaxed)
                                   % actx->num_workers;
                    auto& q = (*actx->queues)[idx];
                    std::lock_guard<std::mutex> lock(q.mu);
                    q.pending.push({fd, rem, loc});
                }
            }
            // Ignore other events on accepted connections (they'll be closed)
        }));
}

void ShardedManager::run()
{
    running_ = true;

    // Start worker threads
    threads_.reserve(workers_.size());
    for (auto& worker : workers_) {
        threads_.emplace_back([this, &worker]() {
            while (running_) {
                worker->poll(5ms);
            }
        });
    }

    // Acceptor runs on the calling thread
    while (running_) {
        acceptor_.poll(5ms);
    }

    // Join all worker threads before returning
    for (auto& t : threads_) {
        if (t.joinable())
            t.join();
    }
    threads_.clear();
}

void ShardedManager::stop()
{
    running_ = false;
}

} // namespace nanosrv
