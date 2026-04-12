cmake_minimum_required(VERSION 3.15)
project(nanosrv-sharded LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Multi-threaded sharded server executable
# ---------------------------------------------------------------------------
add_executable(nanosrv-sharded main_sharded.cpp)
target_link_libraries(nanosrv-sharded PRIVATE nanosrv)
target_include_directories(nanosrv-sharded PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../thirdparty/include
)
