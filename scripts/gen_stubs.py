#!/usr/bin/env python3
"""Regenerate src/nanosrv/_core.pyi from the built extension.

Run after changing the bindings:

    uv run --with nanobind python scripts/gen_stubs.py

The stub is what backs the package's `Typing :: Typed` claim -- the entire
public API lives in the compiled module, so without it the claim is nominal.
Payload signatures come from nb::sig() annotations in _core.cpp; keep those in
step with the C++ or the stub will quietly say `object`.
"""

from __future__ import annotations

import ast
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PKG = ROOT / "src" / "nanosrv"
# Stub *package*, not a single file: _core has a `json` submodule, and a lone
# _core.pyi can only `from nanosrv._core import json` -- which resolves to
# nothing, so type checkers reject it. A directory carrying only .pyi files is
# at most a namespace package, which ranks below an extension module in the
# import system, so it never shadows the compiled _core.
STUB_DIR = PKG / "_core"
STUB = STUB_DIR / "__init__.pyi"

# LogLevel keeps a runtime alias named "None" for the old, syntactically
# unusable member name (see the LogLevel binding in _core.cpp). stubgen sees it
# as an attribute and emits `None = 0`, which is a SyntaxError in a .pyi -- and
# is unrepresentable in any stub, since the name is a keyword. Drop the line:
# the alias still exists at runtime for getattr() callers.
ALIAS_LINE = re.compile(r"^\s*None = \d+\s*$")


def main() -> int:
    legacy = PKG / "_core.pyi"
    if legacy.exists():
        legacy.unlink()  # superseded by the stub package; both would be ambiguous

    subprocess.run(
        [
            sys.executable,
            "-m",
            "nanobind.stubgen",
            "-m",
            "nanosrv._core",
            "-r",
            "-O",
            str(PKG),
        ],
        check=True,
    )

    lines = STUB.read_text().splitlines(keepends=True)
    kept = [ln for ln in lines if not ALIAS_LINE.match(ln)]
    dropped = len(lines) - len(kept)
    STUB.write_text("".join(kept))

    # A stub that does not parse silently disables type checking for everything
    # that imports it, so fail loudly here instead.
    for path in sorted(STUB_DIR.glob("*.pyi")):
        try:
            ast.parse(path.read_text())
        except SyntaxError as exc:
            print(
                f"generated stub does not parse: {path}:{exc.lineno}: {exc.msg}",
                file=sys.stderr,
            )
            return 1

    # Format the output: CI runs `ruff format --check` over src/, and stubgen's
    # own layout does not match it, so an unformatted regeneration would fail
    # the build. Doing it here keeps regeneration idempotent.
    ruff = shutil.which("ruff")
    if ruff:
        subprocess.run([ruff, "format", "-q", str(STUB_DIR)], check=False)
    else:
        print(
            "note: ruff not found; generated stubs are unformatted "
            "and `ruff format --check` will flag them",
            file=sys.stderr,
        )

    written = ", ".join(str(p.relative_to(PKG)) for p in sorted(STUB_DIR.glob("*.pyi")))
    print(
        f"wrote {written} under {STUB_DIR.relative_to(ROOT)} "
        f"(dropped {dropped} unrepresentable alias line(s))"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
