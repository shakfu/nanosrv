"""Minimal nanosrv (Python) ShardedManager HTTP server for benchmarking."""
import sys
import signal
import threading
import nanosrv

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    url = f"http://0.0.0.0:{port}"

    nanosrv.set_log_level(nanosrv.LogLevel.Error)
    mgr = nanosrv.ShardedManager(0)  # 0 = hardware concurrency
    mgr.http_listen(url, lambda conn, msg: (
        conn.http_reply(200, "Content-Type: text/plain\r\n", "OK\n")
    ))

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
