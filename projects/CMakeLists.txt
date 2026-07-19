cmake_minimum_required(VERSION 3.15)
project(nano-server VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Place all executables and libraries in the top-level build/ directory
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/.." CACHE INTERNAL "")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/.." CACHE INTERNAL "")

enable_testing()

# Subprojects
add_subdirectory(mungo)
add_subdirectory(nanosrv)
add_subdirectory(nanosrv-exe)
add_subdirectory(nanosrv-sharded)

# Mongoose 7.21 reference server (upstream, for benchmarking)
set(_mongoose_dir "${CMAKE_CURRENT_SOURCE_DIR}/../thirdparty/mongoose")
if(EXISTS "${_mongoose_dir}/mongoose.c")
    add_executable(mongoose-server
        ${_mongoose_dir}/main.c
        ${_mongoose_dir}/mongoose.c
    )
    target_include_directories(mongoose-server PRIVATE ${_mongoose_dir})
    target_compile_options(mongoose-server PRIVATE -Wall -Wextra -O2)
endif()
