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
