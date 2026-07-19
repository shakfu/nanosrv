#include "nanosrv/nanosrv.hpp"
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

// Portable socket close: platform.hpp defines closesocket() on Windows; on
// POSIX it maps to close(). Mirrors the fallback in sock.cpp so this TU can
// close an un-adopted FD without truncating a Windows SOCKET handle to int.
#ifndef closesocket
#define closesocket(x) close(x)
#endif

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

// Per-adopted-connection context: the user's handler plus a pointer to the
// shared live-connection counter, decremented when the connection closes so the
// acceptor's global max-connection cap stays accurate.
struct AdoptCtx {
    HttpHandler handler;
    std::atomic<int>* live;
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
static void drain_worker(Mgr* mgr, WorkerQueue* q, const HttpHandler& handler,
                         std::atomic<int>* live)
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

        auto* ctx = new AdoptCtx{handler, live};
        auto* c = wrapfd(mgr, pc.fd,
            [](Connection* c, int ev, void* ev_data) {
                auto* ctx = static_cast<AdoptCtx*>(c->fn_data);
                if (ev == MG_EV_HTTP_MSG)
                    ctx->handler(*c, *static_cast<HttpMessage*>(ev_data));
                if (ev == MG_EV_CLOSE) {
                    if (ctx->live)
                        ctx->live->fetch_sub(1, std::memory_order_relaxed);
                    delete ctx;
                    c->fn_data = nullptr;
                }
            },
            ctx);
        if (c) {
            c->rem = pc.rem;
            c->loc = pc.loc;
            c->is_accepted = 1;
            // Count the hand-off as an accept for metrics. Workers do not run
            // accept_conn (the acceptor does), so this is the sharded-path
            // counterpart to the stat_accepted bump there.
            mgr->stat_accepted.fetch_add(1, std::memory_order_relaxed);
            c->pfn = http_cb;
        } else {
            // Adoption failed: drop the FD and release the slot we reserved at
            // the acceptor so the global cap does not leak a phantom connection.
            if (live)
                live->fetch_sub(1, std::memory_order_relaxed);
            delete ctx;
            closesocket(pc.fd);  // portable + no handle truncation on Windows
        }
    }
}

// Mark every accepted connection on a worker as draining. A draining connection
// finishes flushing its current response (the event loop sets is_closing once
// send.len reaches 0) and then closes. The wakeup pipe and any non-accepted
// connection are left untouched. Runs on the worker thread only. Idempotent.
static void mark_draining(Mgr* mgr)
{
    for (Connection* c = mgr->conns; c != nullptr; c = c->next)
        if (c->is_accepted && !c->is_closing)
            c->is_draining = 1;
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
    // Stable addresses of the global-cap state (members of this ShardedManager,
    // which outlives the acceptor's listener connection). Captured by pointer so
    // the acceptor reads the current cap value even if it is set after listen.
    const int* maxp = &max_connections_;
    std::atomic<int>* live = &live_conns_;
    std::atomic<bool>* draining = &draining_;

    // The acceptor listens on the URL. On MG_EV_ACCEPT, steal the FD, hand it to
    // a worker queue round-robin, and wake that worker. All state is captured by
    // value (or by stable pointer for the cap/drain flag) in the std::function
    // (owned by the acceptor's listener connection), so there is nothing to leak.
    acceptor_.http_listen(url_str,
        HandlerFn([queues, next, num_workers, worker_mgrs, maxp, live, draining](
                      Connection& c, Event ev, void*) {
            if (ev != Event::Accept)
                return;  // accepted conns are detached/closed; ignore the rest

            // Graceful drain: stop accepting. Close the connection without
            // handing it off, so existing connections drain undisturbed.
            if (draining->load(std::memory_order_acquire)) {
                c.is_closing = 1;
                return;
            }

            // Global connection cap, enforced on the single acceptor thread.
            // Only the acceptor increments, so this check-then-increment cannot
            // over-admit; workers only decrement on close. Over the cap, leave
            // the connection to be closed by the acceptor loop (no handoff).
            if (*maxp > 0 && live->load(std::memory_order_relaxed) >= *maxp) {
                c.is_closing = 1;
                return;
            }

            Address rem = c.rem;
            Address loc = c.loc;
            MG_SOCKET_TYPE fd = detach_fd(&c);
            c.is_closing = 1;  // let the acceptor loop clean up this connection
            if (fd == MG_INVALID_SOCKET)
                return;

            live->fetch_add(1, std::memory_order_relaxed);  // reserve a slot
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
    // Publish the pipes: a release here pairs with the acquire in stop()/drain(),
    // so their wake_mgr() reads of mgr->pipe see these initialized values.
    pipes_ready_.store(true, std::memory_order_release);

    // One worker thread per worker Manager. Each drains its queue, then polls;
    // the poll wakes immediately when the acceptor hands off (wake_mgr) and
    // otherwise after the fallback timeout.
    threads_.reserve(workers_.size());
    for (size_t i = 0; i < workers_.size(); i++) {
        Manager* w = workers_[i].get();
        auto queues = queues_;
        auto handler = handler_;
        std::atomic<int>* live = &live_conns_;
        threads_.emplace_back([this, w, queues, handler, i, live]() {
            while (running_.load()) {
                if (queues && handler)
                    drain_worker(w->raw(), &(*queues)[i], *handler, live);
                // During a graceful drain, mark connections so they finish their
                // in-flight response and close; poll more often so they close
                // (and live_conns_ drops to 0) promptly.
                bool draining = draining_.load(std::memory_order_acquire);
                if (draining) {
                    mark_draining(w->raw());
                    // Enforce the drain deadline here, not only in the acceptor
                    // loop. A connection with a slow or stalled reader keeps its
                    // socket writable, so this worker busy-flushes it with the
                    // poll returning immediately; when every core is spinning on
                    // such a flush the acceptor thread can be starved long enough
                    // that it never clears running_ before the work completes,
                    // and the deadline is missed by seconds. Checking it on the
                    // worker guarantees a timely stop regardless of scheduling.
                    uint64_t deadline = drain_deadline_.load(std::memory_order_relaxed);
                    if (deadline != 0 && millis() >= deadline) {
                        running_.store(false);
                        break;
                    }
                }
                w->poll(draining ? 50 : 1000);
            }
        });
    }

    // Acceptor runs on the calling thread. During a graceful drain it keeps
    // running (rejecting new connections via the accept handler) until every
    // worker connection has closed, or the drain deadline passes -- then it ends
    // the run by clearing running_, which stops the workers too.
    while (running_.load()) {
        bool draining = draining_.load(std::memory_order_acquire);
        acceptor_.poll(draining ? 50 : 1000);
        if (draining) {
            uint64_t deadline = drain_deadline_.load(std::memory_order_relaxed);
            bool timed_out = deadline != 0 && millis() >= deadline;
            if (live_conns_.load(std::memory_order_relaxed) == 0 || timed_out)
                running_.store(false);
        }
    }

    // No external waker should touch the pipes once the loops are exiting; the
    // wakes just below run on this thread, after wakeup_init, so they are safe.
    pipes_ready_.store(false, std::memory_order_release);

    // Stop and join workers before returning.
    for (auto& w : workers_)
        wake_mgr(w->raw());
    for (auto& t : threads_)
        if (t.joinable())
            t.join();
    threads_.clear();

    // Close any FDs still queued at shutdown -- no worker will adopt them now.
    // Each was counted against the cap at the acceptor, so release its slot.
    if (queues_) {
        for (auto& q : *queues_) {
            std::lock_guard<std::mutex> lock(q.mu);
            while (!q.pending.empty()) {
                close(static_cast<int>(q.pending.front().fd));
                q.pending.pop();
                live_conns_.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }

    run_active_.store(false, std::memory_order_release);
}

// Wake the acceptor and workers so their polls return promptly instead of
// waiting out the fallback timeout. Gated on pipes_ready_ (acquire) so the pipe
// reads in wake_mgr() never race with wakeup_init() in run(); if the pipes are
// not up yet, the loops still observe the new state on their next fallback poll.
void ShardedManager::wake_all()
{
    if (!pipes_ready_.load(std::memory_order_acquire))
        return;
    for (auto& w : workers_)
        wake_mgr(w->raw());
    wake_mgr(acceptor_.raw());
}

void ShardedManager::stop()
{
    running_.store(false);
    wake_all();
}

void ShardedManager::drain(int timeout_ms)
{
    // Publish the deadline before the flag so a loop that observes draining_
    // also sees a valid deadline (release/acquire on draining_ orders both).
    drain_deadline_.store(
        timeout_ms > 0 ? millis() + static_cast<uint64_t>(timeout_ms) : 0,
        std::memory_order_relaxed);
    draining_.store(true, std::memory_order_release);
    // run() ends the run on its own once every connection has closed (or the
    // deadline passes), so -- unlike stop() -- this does not clear running_.
    wake_all();
}

} // namespace nanosrv
