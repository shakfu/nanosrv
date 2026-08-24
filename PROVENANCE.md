# Provenance and licensing

## Summary

nanosrv is a **derivative work of Mongoose 7.21** (Cesanta Software Limited /
Sergey Lyubka) and is distributed under the **GNU General Public License,
version 2 only** -- the same licence under which the upstream code was
obtained. See `LICENSE` for the full text.

Mongoose is dual-licensed as `GPL-2.0-only or commercial`. GPL-2.0-only carries
no "or any later version" clause, so the derived code here **cannot** be
relicensed under GPL-3.0, MIT, or any other terms. If you need nanosrv under
non-copyleft terms, you must first obtain a commercial Mongoose licence from
<https://mongoose.ws/licensing/>; that covers the upstream portion only, and the
original portions listed below would still need to be licensed separately by
their author.

## What comes from where

| Path | Provenance |
|---|---|
| `thirdparty/mongoose/` | Verbatim upstream Mongoose 7.21. `GPL-2.0-only or commercial`. |
| `projects/mungo/mungo.{c,h}` | A ~5.5K-line subset mechanically extracted from Mongoose 7.21 (HTTP + WebSocket only). Derivative work; retains the upstream copyright header and SPDX tag. |
| `projects/nanosrv/`, `include/nanosrv/` | A C++23 port and refactor of that same subset. Substantially derived from Mongoose: the event model, the `Mgr`/`Connection` structure layout, the `MG_*` constants, and much of the parser code descend directly from upstream. Original contributions on top are the typed callback layer, RAII wrappers (`Manager`, `ConnectionRef`), the sharded worker model, and the hardening knobs. Derivative work, GPL-2.0-only. |
| `projects/nanosrv-exe/`, `projects/nanosrv-sharded/` | Original, but link the derived library. GPL-2.0-only. |
| `src/nanosrv/` (nanobind bindings) | Original, but link the derived library. GPL-2.0-only. |
| `tests/`, `scripts/` | Original. GPL-2.0-only. |
| `thirdparty/include/CLI11.hpp` | Upstream CLI11, BSD-3-Clause. |
| `thirdparty/include/rang.hpp` | Upstream rang, Unlicense. |

## A note on an earlier claim

`projects/nanosrv/README.md` previously described libnanosrv as "an independent
C++ implementation ... rewritten from scratch". That was inaccurate and has been
corrected. A line-level comparison against the vendored upstream contradicts it:
in `projects/nanosrv/http.cpp` alone, 128 of 481 substantive lines (27%) appear
verbatim in `thirdparty/mongoose/mongoose.c`, and 268 of 379 multi-character
identifiers are shared. The project is a port, and is licensed accordingly.

## Practical consequences

- Distributing a binary or a wheel built from this tree obliges you to offer the
  corresponding source under GPL-2.0-only.
- Linking `libnanosrv` into a closed-source application is not permitted under
  this licence.
- The published PyPI metadata declares `GPL-2.0-only`. Releases up to and
  including v0.2.0 declared MIT in their metadata, and the repository carried a
  GPL-3.0 `LICENSE`; both were incorrect and are superseded by this document.
