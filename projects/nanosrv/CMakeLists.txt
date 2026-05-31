cmake_minimum_required(VERSION 3.15)
project(nanosrv VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Static library
# ---------------------------------------------------------------------------
add_library(nanosrv
    base64.cpp
    crypto/sha1.cpp
    dns.cpp
    event.cpp
    fmt.cpp
    http.cpp
    iobuf.cpp
    json.cpp
    log.cpp
    net.cpp
    printf.cpp
    sharded.cpp
    sock.cpp
    str.cpp
    timer.cpp
    tls_dummy.cpp
    url.cpp
    util.cpp
    ws.cpp
)
target_include_directories(nanosrv
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)
target_compile_options(nanosrv PRIVATE -Wall -Wextra -O2)

# ---------------------------------------------------------------------------
# Sanitizers / fuzzing (opt-in; off by default so normal/Release builds are
# unaffected)
# ---------------------------------------------------------------------------
option(NANOSRV_SANITIZE "Build the library and tests with ASan+UBSan" OFF)
option(NANOSRV_FUZZ "Build libFuzzer targets (implies sanitizers; clang only)" OFF)

# Fuzzing requires the internal-parser wrapper and an instrumented library.
if(NANOSRV_FUZZ)
    set(NANOSRV_SANITIZE ON CACHE BOOL "" FORCE)
    target_compile_definitions(nanosrv PUBLIC NANOSRV_EXPOSE_INTERNALS)
    # fuzzer-no-link instruments the library for coverage without pulling in a
    # second main(); each fuzz executable links libFuzzer's main via -fsanitize=fuzzer.
    target_compile_options(nanosrv PUBLIC -fsanitize=fuzzer-no-link)
    target_link_options(nanosrv PUBLIC -fsanitize=fuzzer-no-link)
endif()

if(NANOSRV_SANITIZE)
    # PUBLIC so the flags propagate to test and fuzz executables that link nanosrv.
    target_compile_options(nanosrv PUBLIC
        -fsanitize=address,undefined -fno-sanitize-recover=all
        -fno-omit-frame-pointer -g -O1)
    target_link_options(nanosrv PUBLIC -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
set(_test_src "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/test_all.cpp")
if(EXISTS "${_test_src}")
    enable_testing()
    add_executable(nanosrv-tests "${_test_src}")
    target_link_libraries(nanosrv-tests PRIVATE nanosrv)
    target_compile_definitions(nanosrv-tests PRIVATE TESTING)
    add_test(NAME unit_tests COMMAND nanosrv-tests)
endif()

# ---------------------------------------------------------------------------
# Fuzz targets
# ---------------------------------------------------------------------------
if(NANOSRV_FUZZ)
    enable_testing()
    add_subdirectory(
        "${CMAKE_CURRENT_SOURCE_DIR}/../../tests/fuzz"
        "${CMAKE_BINARY_DIR}/fuzz")
endif()
