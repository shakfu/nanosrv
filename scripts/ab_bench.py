#!/usr/bin/env python3
"""Strict A/B benchmark harness for the nanosrv C++ servers.

Builds a baseline ("OLD") from a git ref through the *same* CMake pipeline as the
working-tree build ("NEW"), then runs wrk against both, interleaved over several
reps, and reports per-rep numbers, means, and a no-regression verdict.

Why a git worktree: building OLD in an isolated worktree guarantees both binaries
come from an identical toolchain/flags, so the only difference measured is the
source change -- not the compiler invocation. (A bare `clang++ -O2` baseline would
measure flag differences instead.)

Examples
--------
    # Compare the working tree against HEAD for both servers (defaults).
    python scripts/ab_bench.py

    # Compare against a specific commit, single-threaded server only, longer runs.
    python scripts/ab_bench.py --ref v0.1.0 --targets nanosrv-server \
        --duration 10s --reps 5

    # Pass extra server flags (applied to every target).
    python scripts/ab_bench.py --extra-args "--max-connections 10000"

Requires: wrk, cmake, git, curl-free readiness probe (uses a TCP connect).
"""

from __future__ import annotations

import argparse
import re
import shutil
import socket
import statistics
import subprocess
import sys
import tempfile
import time
from dataclasses import dataclass, field
from pathlib import Path

# --------------------------------------------------------------------------- #
# Shell / build helpers
# --------------------------------------------------------------------------- #


def sh(cmd: list[str], cwd: Path | None = None, quiet: bool = True) -> str:
    """Run a command, raising on failure, returning stdout."""
    res = subprocess.run(
        cmd, cwd=cwd, text=True, capture_output=True, check=False
    )
    if res.returncode != 0:
        out = (res.stdout or "") + (res.stderr or "")
        if quiet:
            # Surface the tail so build failures are diagnosable.
            out = "\n".join(out.splitlines()[-25:])
        raise RuntimeError(f"command failed ({res.returncode}): {' '.join(cmd)}\n{out}")
    return res.stdout


def repo_root() -> Path:
    return Path(sh(["git", "rev-parse", "--show-toplevel"]).strip())


def cmake_build(source_root: Path) -> Path:
    """Configure + build the C++ servers under `source_root`.

    Mirrors `make server-build`: configures `<root>/projects` into
    `<root>/build/cmake`, whose RUNTIME_OUTPUT_DIRECTORY places binaries in
    `<root>/build/`. Returns that build/ directory.
    """
    build_cmake = source_root / "build" / "cmake"
    sh(["cmake", "-B", str(build_cmake), "-S", str(source_root / "projects")])
    sh(["cmake", "--build", str(build_cmake)])
    return source_root / "build"


# --------------------------------------------------------------------------- #
# wrk driving + parsing
# --------------------------------------------------------------------------- #

_RE_RPS = re.compile(r"Requests/sec:\s*([\d.]+)")
_RE_LAT = re.compile(r"Latency\s+([\d.]+)(us|ms|s)")
_RE_P99 = re.compile(r"\s99%\s+([\d.]+)(us|ms|s)")
_UNIT_MS = {"us": 1e-3, "ms": 1.0, "s": 1000.0}


def _to_ms(value: str, unit: str) -> float:
    return float(value) * _UNIT_MS[unit]


@dataclass
class Sample:
    rps: float
    lat_ms: float  # average latency, milliseconds
    p99_ms: float


def parse_wrk(out: str) -> Sample | None:
    m_rps = _RE_RPS.search(out)
    m_lat = _RE_LAT.search(out)  # first "Latency" line is the Thread Stats avg
    m_p99 = _RE_P99.search(out)
    if not (m_rps and m_lat and m_p99):
        return None
    return Sample(
        rps=float(m_rps.group(1)),
        lat_ms=_to_ms(m_lat.group(1), m_lat.group(2)),
        p99_ms=_to_ms(m_p99.group(1), m_p99.group(2)),
    )


def wait_ready(port: int, timeout_s: float = 5.0) -> bool:
    """Poll a TCP connect until the server accepts, or timeout."""
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return True
        except OSError:
            time.sleep(0.05)
    return False


def run_once(
    binary: Path,
    server_args: list[str],
    wrk_cmd: list[str],
    port: int,
) -> Sample:
    """Launch one server, run wrk against it once, return the parsed sample."""
    proc = subprocess.Popen(
        [str(binary), "-p", str(port), *server_args],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    try:
        if not wait_ready(port):
            raise RuntimeError(f"{binary.name} did not start on port {port}")
        out = subprocess.run(
            wrk_cmd, text=True, capture_output=True, check=False
        ).stdout
        sample = parse_wrk(out)
        if sample is None:
            raise RuntimeError(f"could not parse wrk output:\n{out}")
        return sample
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()
            proc.wait()
        time.sleep(0.4)  # let the port free up before the next launch


# --------------------------------------------------------------------------- #
# Aggregation / reporting
# --------------------------------------------------------------------------- #


@dataclass
class Series:
    old: list[Sample] = field(default_factory=list)
    new: list[Sample] = field(default_factory=list)


def _mean(xs: list[float]) -> float:
    return statistics.fmean(xs) if xs else float("nan")


def _pct_delta(old: float, new: float) -> float:
    return (new - old) / old * 100.0 if old else float("nan")


def report(target: str, series: Series, noise_floor_pct: float) -> bool:
    """Print a per-target report. Returns True if no regression is detected."""
    old_rps = [s.rps for s in series.old]
    new_rps = [s.rps for s in series.new]
    mean_old, mean_new = _mean(old_rps), _mean(new_rps)
    delta = _pct_delta(mean_old, mean_new)

    # Run-to-run spread of the OLD series, as a percentage of its mean. A delta
    # smaller than this is indistinguishable from noise.
    spread_pct = (
        (statistics.pstdev(old_rps) / mean_old * 100.0)
        if len(old_rps) > 1 and mean_old
        else 0.0
    )
    threshold = max(noise_floor_pct, 2.0 * spread_pct)
    ok = abs(delta) <= threshold

    print(f"\n=== {target} ===")
    print(f"  {'rep':<5}{'OLD rps':>12}{'NEW rps':>12}"
          f"{'OLD p99':>12}{'NEW p99':>12}")
    for i, (o, n) in enumerate(zip(series.old, series.new), 1):
        print(f"  {i:<5}{o.rps:>12,.0f}{n.rps:>12,.0f}"
              f"{o.p99_ms:>10.2f}ms{n.p99_ms:>10.2f}ms")
    print(f"  {'mean':<5}{mean_old:>12,.0f}{mean_new:>12,.0f}")
    print(f"  delta: {delta:+.2f}% RPS   (noise threshold +-{threshold:.2f}%, "
          f"OLD spread {spread_pct:.2f}%)")
    print(f"  avg latency: OLD {_mean([s.lat_ms for s in series.old]):.3f}ms  "
          f"NEW {_mean([s.lat_ms for s in series.new]):.3f}ms")
    print(f"  verdict: {'NO REGRESSION (within noise)' if ok else 'REGRESSION?'}")
    return ok


# --------------------------------------------------------------------------- #
# Main
# --------------------------------------------------------------------------- #


def main() -> int:
    p = argparse.ArgumentParser(
        description="Strict A/B benchmark of nanosrv servers (git ref vs working tree).",
        formatter_class=argparse.ArgumentDefaultsHelpFormatter,
    )
    p.add_argument("--ref", default="HEAD",
                   help="git ref to use as the OLD baseline")
    p.add_argument("--targets", default="nanosrv-server,nanosrv-sharded",
                   help="comma-separated binary names in build/ to compare")
    p.add_argument("--port", type=int, default=9876)
    p.add_argument("--threads", type=int, default=4, help="wrk threads (-t)")
    p.add_argument("--connections", type=int, default=100,
                   help="wrk connections (-c)")
    p.add_argument("--duration", default="8s", help="wrk duration (-d)")
    p.add_argument("--reps", type=int, default=3,
                   help="interleaved OLD/NEW reps per target")
    p.add_argument("--path", default="/", help="request path")
    p.add_argument("--extra-args", default="",
                   help="extra args passed to every server (e.g. '-t 8')")
    p.add_argument("--noise-floor", type=float, default=2.0,
                   help="min delta%% treated as noise regardless of spread")
    p.add_argument("--no-build", action="store_true",
                   help="skip building NEW; use existing build/ binaries")
    p.add_argument("--wrk", default="wrk", help="path to the wrk binary")
    p.add_argument("--keep-worktree", action="store_true",
                   help="do not remove the OLD worktree (for debugging)")
    args = p.parse_args()

    for tool in (args.wrk, "cmake", "git"):
        if shutil.which(tool) is None:
            print(f"error: '{tool}' not found on PATH", file=sys.stderr)
            return 2

    root = repo_root()
    targets = [t.strip() for t in args.targets.split(",") if t.strip()]
    server_args = args.extra_args.split() if args.extra_args else []
    url = f"http://127.0.0.1:{args.port}{args.path}"
    wrk_cmd = [args.wrk, f"-t{args.threads}", f"-c{args.connections}",
               f"-d{args.duration}", "--latency", url]

    # --- Build NEW (working tree) ---
    if not args.no_build:
        print(f"Building NEW (working tree) ...")
        new_build = cmake_build(root)
    else:
        new_build = root / "build"

    # --- Build OLD (baseline ref) in an isolated worktree ---
    worktree = Path(tempfile.mkdtemp(prefix="nanosrv-ab-"))
    old_build: Path | None = None
    try:
        print(f"Building OLD ({args.ref}) in worktree {worktree} ...")
        sh(["git", "worktree", "add", "--detach", str(worktree), args.ref],
           cwd=root)
        old_build = cmake_build(worktree)

        # Validate every target exists in both trees before benchmarking.
        plan: list[tuple[str, Path, Path]] = []
        for t in targets:
            old_bin, new_bin = old_build / t, new_build / t
            for label, b in (("OLD", old_bin), ("NEW", new_bin)):
                if not b.exists():
                    print(f"error: {label} binary missing: {b}", file=sys.stderr)
                    return 2
            plan.append((t, old_bin, new_bin))

        print(f"\nwrk -t{args.threads} -c{args.connections} -d{args.duration} "
              f"| {args.reps} reps | baseline {args.ref}")

        results: dict[str, Series] = {t: Series() for t in targets}
        for rep in range(1, args.reps + 1):
            print(f"\n# rep {rep}/{args.reps}")
            for t, old_bin, new_bin in plan:
                # Interleave OLD then NEW so slow drift affects both equally.
                o = run_once(old_bin, server_args, wrk_cmd, args.port)
                n = run_once(new_bin, server_args, wrk_cmd, args.port)
                results[t].old.append(o)
                results[t].new.append(n)
                print(f"  {t:<18} OLD {o.rps:>10,.0f}  NEW {n.rps:>10,.0f} req/s")

        all_ok = True
        for t in targets:
            all_ok &= report(t, results[t], args.noise_floor)

        print(f"\n{'=' * 50}")
        print("OVERALL: no regression" if all_ok
              else "OVERALL: possible regression -- inspect above")
        return 0 if all_ok else 1
    finally:
        if args.keep_worktree:
            print(f"\n(worktree kept at {worktree})")
        else:
            sh(["git", "worktree", "remove", "--force", str(worktree)], cwd=root)


if __name__ == "__main__":
    sys.exit(main())
