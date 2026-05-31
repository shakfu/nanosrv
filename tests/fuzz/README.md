# Fuzz targets

libFuzzer harnesses for the three parsers that consume untrusted bytes:

| Target      | Entry point                          | Source         |
|-------------|--------------------------------------|----------------|
| `fuzz_http` | `nanosrv::http_parse`                | `fuzz_http.cpp`|
| `fuzz_ws`   | `ws_process` (via `ws_process_for_test`) | `fuzz_ws.cpp` |
| `fuzz_dns`  | `nanosrv::dns_parse`                 | `fuzz_dns.cpp` |

`ws_process` is `static` in `ws.cpp`; it is reached through a thin wrapper
(`ws_process_for_test`) compiled only when `NANOSRV_EXPOSE_INTERNALS` is defined,
which the `NANOSRV_FUZZ` CMake option sets automatically.

## Build

libFuzzer ships with upstream LLVM clang and Linux distro clang. **Apple clang
does not bundle libFuzzer** -- on macOS use Homebrew LLVM (`brew install llvm`)
and point CMake at it.

```bash
# Linux / Homebrew-LLVM clang
CC=clang CXX=clang++ cmake -B build/fuzz -S projects -DNANOSRV_FUZZ=ON
cmake --build build/fuzz --target fuzz_http fuzz_ws fuzz_dns
```

`NANOSRV_FUZZ=ON` implies `NANOSRV_SANITIZE=ON`: the library is built with
AddressSanitizer + UndefinedBehaviorSanitizer + `fuzzer-no-link`, and each target
links its own libFuzzer `main` via `-fsanitize=fuzzer`.

For a sanitized build of just the unit tests (no fuzzing, works with Apple clang):

```bash
cmake -B build/asan -S projects -DNANOSRV_SANITIZE=ON
cmake --build build/asan --target nanosrv-tests
ctest --test-dir build/asan -R unit_tests --output-on-failure
```

## Run

```bash
# Seeded run, 60 seconds, stop with non-zero exit on any finding
./build/fuzz_http -max_total_time=60 -error_exitcode=1 tests/fuzz/corpus/http
./build/fuzz_ws   -max_total_time=60 -error_exitcode=1 tests/fuzz/corpus/ws
./build/fuzz_dns  -max_total_time=60 -error_exitcode=1 tests/fuzz/corpus/dns
```

A crash writes a `crash-<hash>` (or `leak-`/`timeout-`) reproducer to the working
directory. Replay it with `./build/fuzz_http crash-<hash>`.

The `corpus/<target>/` directories hold seed inputs; libFuzzer also writes newly
discovered interesting inputs back into them, so committing the grown corpus
speeds up later runs.

## CI

`.github/workflows/cpp.yml` runs the sanitized unit suite and a 60s seeded smoke
run of each target on every push/PR. These are smoke runs, not a substitute for
sustained fuzzing -- run the targets locally for minutes-to-hours when touching a
parser.
