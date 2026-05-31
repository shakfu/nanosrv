#include "nanosrv/nanosrv.hpp"
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace nanosrv {

using HttpHandler = Manager::HttpHandler;

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

// Sentinel id used only to wake a worker/acceptor poll(); it intentionally
// matches no real connection, so wufn() consumes it and returns (see sock.cpp).
static constexpr unsigned long WAKE_ID = 1;

// Interrupt a manager's poll() from another thread. No-op until wakeup_init()
// has set up the pipe. wakeup() does a socket send(), which is thread-safe.
static void wake_mgr(Mgr* mgr)
{
    if (mgr->pipe != MG_INVALID_SOCKET)
        wakeup(mgr, WAKE_ID, "", 0);
}

// Adopt every FD currently queued for this worker into its event loop. Runs on
// the worker thread only. Each adopted connection gets its own copy of the
// user's handler as fn_data and the HTTP protocol handler as pfn.
static void drain_worker(Mgr* mgr, WorkerQueue* q, const HttpHandler& handler)
{
    for (;;) {
        PendingConn pc;
        {
            std::lock_guard<std::mutex> lock(q->mu);
            if (q->pending.empty())
                break;
            pc = q->pending.front();
            q->pending.pop();
        }

        auto* handler_copy = new HttpHandler(handler);
        auto* c = wrapfd(mgr, static_cast<int>(pc.fd),
            [](Connection* c, int ev, void* ev_data) {
                auto* fn = static_cast<HttpHandler*>(c->fn_data);
                if (ev == MG_EV_HTTP_MSG)
                    (*fn)(*c, *static_cast<HttpMessage*>(ev_data));
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
}

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
    // Wait for an in-flight run() (possibly on another thread) to finish before
    // members are destroyed. run() clears its threads and sets run_active_ false
    // as its last act, after which it touches no members.
    while (run_active_.load(std::memory_order_acquire))
        std::this_thread::yield();
}

void ShardedManager::http_listen(std::string_view url, HttpHandler handler)
{
    auto queues = std::make_shared<std::vector<WorkerQueue>>(workers_.size());
    queues_ = queues;
    handler_ = std::make_shared<HttpHandler>(std::move(handler));

    // Raw worker manager pointers for cross-thread wakeups. Valid as long as
    // this ShardedManager (hence workers_) is alive, which outlives the acceptor.
    std::vector<Mgr*> worker_mgrs;
    worker_mgrs.reserve(workers_.size());
    for (auto& w : workers_)
        worker_mgrs.push_back(w->raw());

    const unsigned num_workers = static_cast<unsigned>(workers_.size());
    std::atomic<unsigned>* next = &next_;
    std::string url_str(url);

    // The acceptor listens on the URL. On MG_EV_ACCEPT, steal the FD, hand it to
    // a worker queue round-robin, and wake that worker. All state is captured by
    // value in the std::function (owned by the acceptor's listener connection),
    // so there is nothing to leak.
    acceptor_.http_listen(url_str,
        HandlerFn([queues, next, num_workers, worker_mgrs](
                      Connection& c, Event ev, void*) {
            if (ev != Event::Accept)
                return;  // accepted conns are detached/closed; ignore the rest

            Address rem = c.rem;
            Address loc = c.loc;
            MG_SOCKET_TYPE fd = detach_fd(&c);
            c.is_closing = 1;  // let the acceptor loop clean up this connection
            if (fd == MG_INVALID_SOCKET)
                return;

            unsigned idx =
                next->fetch_add(1, std::memory_order_relaxed) % num_workers;
            {
                std::lock_guard<std::mutex> lock((*queues)[idx].mu);
                (*queues)[idx].pending.push({fd, rem, loc});
            }
            wake_mgr(worker_mgrs[idx]);  // drain promptly instead of on a timer
        }));
}

void ShardedManager::run()
{
    run_active_.store(true, std::memory_order_release);
    running_.store(true);

    // Cross-thread wakeup pipes must exist before any thread polls. Created on
    // this thread before workers start, so there is no concurrent access.
    for (auto& w : workers_)
        wakeup_init(w->raw());
    wakeup_init(acceptor_.raw());

    // One worker thread per worker Manager. Each drains its queue, then polls;
    // the poll wakes immediately when the acceptor hands off (wake_mgr) and
    // otherwise after the fallback timeout.
    threads_.reserve(workers_.size());
    for (size_t i = 0; i < workers_.size(); i++) {
        Manager* w = workers_[i].get();
        auto queues = queues_;
        auto handler = handler_;
        threads_.emplace_back([this, w, queues, handler, i]() {
            while (running_.load()) {
                if (queues && handler)
                    drain_worker(w->raw(), &(*queues)[i], *handler);
                w->poll(1000);
            }
        });
    }

    // Acceptor runs on the calling thread.
    while (running_.load())
        acceptor_.poll(1000);

    // Stop and join workers before returning.
    for (auto& w : workers_)
        wake_mgr(w->raw());
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();

    // Close any FDs still queued at shutdown -- no worker will adopt them now.
    if (queues_) {
        for (auto& q : *queues_) {
            std::lock_guard<std::mutex> lock(q.mu);
            while (!q.pending.empty()) {
                close(static_cast<int>(q.pending.front().fd));
                q.pending.pop();
            }
        }
    }

    run_active_.store(false, std::memory_order_release);
}

void ShardedManager::stop()
{
    running_.store(false);
    // Wake the acceptor and workers so their polls return promptly instead of
    // waiting out the fallback timeout.
    for (auto& w : workers_)
        wake_mgr(w->raw());
    wake_mgr(acceptor_.raw());
}

} // namespace nanosrv
