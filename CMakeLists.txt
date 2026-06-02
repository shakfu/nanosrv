cmake_minimum_required(VERSION 3.15...3.31)
project(${SKBUILD_PROJECT_NAME} VERSION ${SKBUILD_PROJECT_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The nanosrv static library is linked into the _core *shared* module below.
# On ELF targets (Linux) a static archive can only be linked into a shared
# object if its objects were compiled position-independent; otherwise the link
# fails with "relocation R_X86_64_PC32 ... can not be used when making a shared
# object; recompile with -fPIC". macOS compiles PIC unconditionally so this is
# not needed there, but the flag is harmless and keeps the wheel build portable.
# Scoped to this (wheel) build only -- the standalone executables built from
# projects/CMakeLists.txt do not need it.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

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
