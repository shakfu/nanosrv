"""Minimal nanosrv (Python) ShardedManager HTTP server for benchmarking."""
import sys
import signal
import threading
import nanosrv

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    url = f"http://0.0.0.0:{port}"

    nanosrv.set_log_level(nanosrv.LogLevel.Error)
    # Optional second argument: worker count (default 0 = hardware concurrency).
    # Worth varying: on a GIL interpreter the peak is at 2-4 workers and more
    # than that costs throughput, while a free-threaded one keeps scaling.
    workers = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    mgr = nanosrv.ShardedManager(workers)
    # Same response as every other benchmark server: the comparison is only
    # meaningful if they all do identical work. This used to reply with a
    # constant "OK\n" while the others formatted method and URI into the body.
    mgr.http_listen(
        url,
        lambda conn, msg: conn.http_reply(
            200,
            "Content-Type: text/plain\r\n",
            f"nanosrv-sharded ready\nMethod: {msg.method}, URI: {msg.uri}\n",
        ),
    )

    print(f"nanosrv-sharded listening on {url} ({mgr.num_workers} workers)")

    runner = threading.Thread(target=mgr.run, daemon=True)
    runner.start()

    evt = threading.Event()
    signal.signal(signal.SIGINT, lambda *_: evt.set())
    signal.signal(signal.SIGTERM, lambda *_: evt.set())
    evt.wait()

    print("\nShutting down...")
    mgr.stop()
    runner.join(timeout=5)

if __name__ == "__main__":
    main()
