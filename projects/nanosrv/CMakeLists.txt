cmake_minimum_required(VERSION 3.15)
project(nanosrv VERSION 0.1.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# TLS backend selection
# ---------------------------------------------------------------------------
# "none" links the no-op stub (tls_dummy.cpp); tls_available() is false and TLS
# URLs are refused up front. "mbed" links the mbedTLS backend (tls_mbed.cpp)
# against the vendored mbedTLS in thirdparty/.
set(NANOSRV_TLS "none" CACHE STRING "TLS backend: none | mbed")
set_property(CACHE NANOSRV_TLS PROPERTY STRINGS none mbed)

if(NANOSRV_TLS STREQUAL "mbed")
    set(_tls_src tls_mbed.cpp)
else()
    set(_tls_src tls_dummy.cpp)
endif()

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
    ${_tls_src}
    url.cpp
    util.cpp
    ws.cpp
)
target_include_directories(nanosrv
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../include>
)
target_compile_options(nanosrv PRIVATE -Wall -Wextra -O2)

# ---------------------------------------------------------------------------
# mbedTLS backend (opt-in via -DNANOSRV_TLS=mbed)
# ---------------------------------------------------------------------------
if(NANOSRV_TLS STREQUAL "mbed")
    # Where the mbedTLS libraries come from:
    #   auto   - use an installed mbedTLS if its CMake package is found,
    #            otherwise fetch a pinned copy (default)
    #   system - require an installed mbedTLS (hard error if absent)
    #   fetch  - always fetch the pinned copy, ignoring any system install
    set(NANOSRV_MBEDTLS_PROVIDER "auto"
        CACHE STRING "mbedTLS source: auto | system | fetch")
    set_property(CACHE NANOSRV_MBEDTLS_PROVIDER PROPERTY STRINGS auto system fetch)

    # Pinned to the *commit* behind the v3.6.6 release tag (immutable; a tag can
    # in principle be re-pointed).
    set(NANOSRV_MBEDTLS_GIT_TAG "0bebf8b8c7f07abe3571ded48a11aa907a1ffb20"
        CACHE STRING "mbedTLS commit/tag to fetch (default: v3.6.6)")

    # Accepted system version window: this backend is written against the
    # mbedTLS 3.x API (TLS 1.3 on by default since 3.6). mbedTLS 4.x is an API
    # break, so a 4.x system install must be rejected rather than compiled
    # against -- e.g. Homebrew now ships 4.x. The upper bound is enforced
    # manually because the `3.6...<4.0` range form needs CMake >= 3.19 while
    # this project targets 3.15.
    set(_nanosrv_mbedtls_min_version 3.6)
    set(_nanosrv_mbedtls_max_version 4.0)  # exclusive

    # Try an installed mbedTLS first (skips the network fetch) unless the caller
    # forced fetching. The CMake package mbedTLS installs provides the same
    # MbedTLS::* targets as a source build.
    set(_mbedtls_from_system FALSE)
    if(NOT NANOSRV_MBEDTLS_PROVIDER STREQUAL "fetch")
        find_package(MbedTLS ${_nanosrv_mbedtls_min_version} CONFIG QUIET)
        if(MbedTLS_FOUND AND DEFINED MbedTLS_VERSION
           AND NOT MbedTLS_VERSION VERSION_LESS ${_nanosrv_mbedtls_max_version})
            message(STATUS
                "nanosrv: ignoring system mbedTLS ${MbedTLS_VERSION} "
                "(>= ${_nanosrv_mbedtls_max_version} is API-incompatible)")
            set(MbedTLS_FOUND FALSE)
        endif()
        if(MbedTLS_FOUND)
            set(_mbedtls_from_system TRUE)
        elseif(NANOSRV_MBEDTLS_PROVIDER STREQUAL "system")
            message(FATAL_ERROR
                "NANOSRV_MBEDTLS_PROVIDER=system but no mbedTLS in "
                "[${_nanosrv_mbedtls_min_version}, ${_nanosrv_mbedtls_max_version}) "
                "was found. Install one, or use NANOSRV_MBEDTLS_PROVIDER=fetch.")
        endif()
    endif()

    if(_mbedtls_from_system)
        message(STATUS
            "nanosrv: using installed mbedTLS ${MbedTLS_VERSION} (provider=${NANOSRV_MBEDTLS_PROVIDER})")
    else()
        # Fetch at configure time rather than vendoring the source in-repo. The
        # clone is cached in the build tree under _deps/, so reconfiguring does
        # not re-fetch. NOTE: the first fetch needs network access; the default
        # (NANOSRV_TLS=none) build, and therefore the Python wheel, never fetch.
        message(STATUS "nanosrv: fetching mbedTLS pinned at ${NANOSRV_MBEDTLS_GIT_TAG}")
        include(FetchContent)

        # Build only the mbedTLS libraries. These must be set before the content
        # is made available so they take effect inside mbedTLS's own CMake scope.
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        # mbedTLS builds its sources with -Werror by default; disable so a newer
        # host compiler warning cannot fail the dependency build.
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)

        # framework/ is a git submodule and add_subdirectory(framework) is
        # unconditional in mbedTLS, so submodules must be initialized on fetch.
        FetchContent_Declare(
            mbedtls
            GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
            GIT_TAG ${NANOSRV_MBEDTLS_GIT_TAG}
            GIT_SUBMODULES_RECURSE TRUE
            GIT_PROGRESS TRUE
        )
        FetchContent_MakeAvailable(mbedtls)
    endif()

    target_link_libraries(nanosrv
        PUBLIC MbedTLS::mbedtls MbedTLS::mbedx509 MbedTLS::mbedcrypto)
    # PUBLIC so consumers (tests, executables) compile their MG_TLS-conditional
    # code against the same backend the library was built with.
    target_compile_definitions(nanosrv PUBLIC MG_TLS=MG_TLS_MBED)
endif()

# ---------------------------------------------------------------------------
# Sanitizers / fuzzing (opt-in; off by default so normal/Release builds are
# unaffected)
# ---------------------------------------------------------------------------
option(NANOSRV_SANITIZE "Build the library and tests with ASan+UBSan" OFF)
option(NANOSRV_TSAN "Build the library and tests with ThreadSanitizer" OFF)
option(NANOSRV_FUZZ "Build libFuzzer targets (implies sanitizers; clang only)" OFF)

# TSan is mutually exclusive with ASan.
if(NANOSRV_TSAN AND NANOSRV_SANITIZE)
    message(FATAL_ERROR "NANOSRV_TSAN and NANOSRV_SANITIZE are mutually exclusive")
endif()

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

if(NANOSRV_TSAN)
    target_compile_options(nanosrv PUBLIC
        -fsanitize=thread -fno-omit-frame-pointer -g -O1)
    target_link_options(nanosrv PUBLIC -fsanitize=thread)
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
