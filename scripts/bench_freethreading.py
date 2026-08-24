#!/usr/bin/env python3
"""Measure what free-threaded CPython does for ShardedManager.

The claim under test: ShardedManager's worker-per-core design is penalised by
the GIL (multiple workers contend to run one Python handler at a time) and
rewarded without it. Only *pure-Python* handler work isolates that -- work that
releases the GIL (native calls, I/O) would scale on a GIL build too.

Usage:
    uv run python scripts/bench_freethreading.py                 # both interpreters
    uv run python scripts/bench_freethreading.py --secs 4        # quicker

Requires a free-threaded interpreter; install one with `uv python install 3.13t`.
Builds a small C load generator (scripts/loadgen.c) on first use, because a
Python client would contend for the very GIL under test.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
WORKERS = [1, 2, 4, 8, 16]

SERVER_SRC = """
import argparse, signal, sys
import nanosrv

ap = argparse.ArgumentParser()
ap.add_argument("--workers", type=int, default=1)
ap.add_argument("--port", type=int, default=18500)
ap.add_argument("--iters", type=int, default=0)
args = ap.parse_args()

def busy():
    x = 0.0
    for i in range(args.iters):
        x += i * 0.5
    return x

mgr = nanosrv.ShardedManager(args.workers)

def handler(conn, msg):
    if args.iters:
        busy()
    conn.http_reply(200, "Content-Type: text/plain\\r\\n", "ok")

mgr.http_listen(f"http://127.0.0.1:{args.port}", handler)
signal.signal(signal.SIGTERM, lambda s, f: mgr.stop())
gil = getattr(sys, "_is_gil_enabled", lambda: True)()
print(f"ready workers={mgr.num_workers} gil={gil}", flush=True)
mgr.run()
"""


def build_loadgen(outdir: str) -> str:
    exe = os.path.join(outdir, "loadgen")
    cc = os.environ.get("CC", "cc")
    if shutil.which(cc) is None:
        sys.exit(f"{cc} not found; set CC to a C compiler")
    subprocess.run(
        [
            cc,
            "-O2",
            "-D_GNU_SOURCE",
            "-o",
            exe,
            os.path.join(HERE, "loadgen.c"),
            "-lpthread",
        ],
        check=True,
    )
    return exe


def calibrate(python: str, iters: int) -> float:
    """Cost of one busy() call, in microseconds. Free-threaded CPython is
    slower single-threaded, so the same iteration count is not the same time --
    report both rather than pretending otherwise."""
    code = (
        "import time\n"
        f"iters={iters}\n"
        "def busy():\n"
        "    x = 0.0\n"
        "    for i in range(iters):\n"
        "        x += i * 0.5\n"
        "    return x\n"
        "busy()\n"
        "best = min((lambda t0: (busy(), time.perf_counter()-t0)[1])(time.perf_counter())\n"
        "           for _ in range(200))\n"
        "print(best * 1e6)\n"
    )
    out = subprocess.run(
        [python, "-c", code], capture_output=True, text=True, check=True
    )
    return float(out.stdout.strip())


def measure(
    python: str,
    server: str,
    loadgen: str,
    workers: int,
    iters: int,
    port: int,
    conns: int,
    secs: float,
) -> dict:
    proc = subprocess.Popen(
        [
            python,
            server,
            "--workers",
            str(workers),
            "--port",
            str(port),
            "--iters",
            str(iters),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )
    try:
        line = proc.stdout.readline().strip()
        if not line.startswith("ready"):
            raise RuntimeError(f"server did not start: {line!r}")
        gil = line.split("gil=")[1]
        time.sleep(0.3)
        subprocess.run(
            [loadgen, "127.0.0.1", str(port), str(conns), "2"],
            capture_output=True,
            check=True,
        )  # warm-up
        out = subprocess.run(
            [loadgen, "127.0.0.1", str(port), str(conns), str(secs)],
            capture_output=True,
            text=True,
            check=True,
        )
        result = json.loads(out.stdout)
        result["gil"] = gil
        return result
    finally:
        proc.send_signal(signal.SIGTERM)
        try:
            proc.wait(timeout=10)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait(timeout=5)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--gil-python",
        default=sys.executable,
        help="interpreter with the GIL (default: this one)",
    )
    ap.add_argument(
        "--ft-python",
        default=None,
        help="free-threaded interpreter (default: autodetect python3.13t)",
    )
    ap.add_argument(
        "--iters",
        type=int,
        default=5000,
        help="pure-Python loop iterations per request (~100us)",
    )
    ap.add_argument("--conns", type=int, default=64)
    ap.add_argument("--secs", type=float, default=8)
    ap.add_argument("--port", type=int, default=18500)
    args = ap.parse_args()

    ft = args.ft_python or shutil.which("python3.13t") or shutil.which("python3.14t")
    interpreters = {"GIL": args.gil_python}
    if ft:
        interpreters["free-threaded"] = ft
    else:
        print(
            "no free-threaded interpreter found (`uv python install 3.13t`); "
            "measuring the GIL build only\n"
        )

    tmp = tempfile.mkdtemp(prefix="nanosrv-ft-")
    server = os.path.join(tmp, "server.py")
    with open(server, "w") as f:
        f.write(SERVER_SRC)
    loadgen = build_loadgen(tmp)

    port = args.port
    results: dict = {}
    for name, python in interpreters.items():
        us = calibrate(python, args.iters)
        print(
            f"{name}: {python}\n  one handler call = {us:.1f}us "
            f"({args.iters} pure-Python iterations)"
        )
        results[name] = {"handler_us": us, "workers": {}}
        for w in WORKERS:
            port += 1
            r = measure(
                python, server, loadgen, w, args.iters, port, args.conns, args.secs
            )
            results[name]["workers"][w] = r
            print(
                f"  workers={w:2d}  {r['rps']:9.0f} rps  "
                f"lat={r['mean_latency_us']:9.1f}us  errors={r['errors']}  gil={r['gil']}",
                flush=True,
            )
        print()

    header = "".join(f"{('w=' + str(w)):>10s}" for w in WORKERS)
    print(f"{'interpreter':16s}{header}{'best':>12s}")
    bests = {}
    for name, data in results.items():
        row = data["workers"]
        bests[name] = max(r["rps"] for r in row.values())
        cells = "".join(f"{row[w]['rps']:10.0f}" for w in WORKERS)
        print(f"{name:16s}{cells}{bests[name]:12.0f}")

    if len(bests) == 2:
        speedup = bests["free-threaded"] / bests["GIL"]
        print(f"\nbest free-threaded / best GIL: {speedup:.2f}x")
        print(
            "Compare best configuration to best configuration: the GIL build peaks "
            "at 1-2 workers\nand degrades from there, so a same-worker-count "
            "comparison flatters free-threading."
        )

    shutil.rmtree(tmp, ignore_errors=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
