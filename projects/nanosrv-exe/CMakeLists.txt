cmake_minimum_required(VERSION 3.15)
project(nanosrv-exe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Single-threaded server executable
# ---------------------------------------------------------------------------
add_executable(nanosrv-server main.cpp)
target_link_libraries(nanosrv-server PRIVATE nanosrv)
