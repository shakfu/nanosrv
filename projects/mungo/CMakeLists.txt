cmake_minimum_required(VERSION 3.15)
project(mungo VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# ---------------------------------------------------------------------------
# Static library (self-contained C extraction of Mongoose 7.21)
# ---------------------------------------------------------------------------
add_library(mungo STATIC mungo.c)
target_include_directories(mungo
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
)
target_compile_options(mungo PRIVATE -Wall -Wextra -O2)

# ---------------------------------------------------------------------------
# Example server executable
# ---------------------------------------------------------------------------
add_executable(mungo-server main.c)
target_link_libraries(mungo-server PRIVATE mungo)
