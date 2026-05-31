# Fuzz targets. Only added to the build when NANOSRV_FUZZ=ON (see
# projects/nanosrv/CMakeLists.txt), which also compiles the nanosrv library
# with ASan+UBSan+fuzzer-no-link and defines NANOSRV_EXPOSE_INTERNALS.
#
# libFuzzer ships with clang; these targets require clang. Each executable gets
# its own libFuzzer main via -fsanitize=fuzzer.

foreach(target http ws dns)
    add_executable(fuzz_${target} fuzz_${target}.cpp)
    target_link_libraries(fuzz_${target} PRIVATE nanosrv)
    target_compile_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)
    target_link_options(fuzz_${target} PRIVATE -fsanitize=fuzzer)

    # Smoke-test each fuzzer in ctest: a short, bounded run that fails the test
    # if libFuzzer reports a crash. Real fuzzing runs longer (see README).
    add_test(
        NAME fuzz_${target}_smoke
        COMMAND fuzz_${target} -max_total_time=20 -error_exitcode=1
    )
endforeach()
