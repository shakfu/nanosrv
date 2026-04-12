"""Minimal pynanosrv HTTP server for benchmarking."""
import sys
import signal
import nanosrv

def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8000
    url = f"http://0.0.0.0:{port}"

    nanosrv.set_log_level(nanosrv.LogLevel.Error)
    mgr = nanosrv.Manager()
    listener = mgr.http_listen(url, lambda conn, msg: (
        conn.http_reply(200, "Content-Type: text/plain\r\n",
                        f"pynanosrv ready\nMethod: {msg.method}, URI: {msg.uri}\n")
    ))
    if not listener:
        print(f"Failed to listen on {url}", file=sys.stderr)
        sys.exit(1)

    running = True
    def stop(signo, frame):
        nonlocal running
        running = False
    signal.signal(signal.SIGINT, stop)
    signal.signal(signal.SIGTERM, stop)

    print(f"pynanosrv listening on {url}")
    while running:
        mgr.poll(100)

if __name__ == "__main__":
    main()
