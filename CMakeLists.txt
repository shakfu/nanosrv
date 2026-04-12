cmake_minimum_required(VERSION 3.15...3.31)
project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Build the nanosrv library (needed by the Python extension)
add_subdirectory(projects/nanosrv)

# nanobind Python extension
find_package(Python REQUIRED COMPONENTS Interpreter Development.Module)
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_core src/nanosrv/_core.cpp)
target_link_libraries(_core PRIVATE nanosrv)

if(MSVC)
    target_compile_options(_core PRIVATE /W4)
else()
    target_compile_options(_core PRIVATE -Wall -Wextra)
endif()

install(TARGETS _core DESTINATION nanosrv)
