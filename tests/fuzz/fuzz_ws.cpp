// Fuzz target for the WebSocket frame parser (ws_process).
//
// ws_process() is static inside ws.cpp and operates on the internal ws_msg
// struct. It is reached here via nanosrv::ws_process_for_test(), a thin wrapper
// compiled only when NANOSRV_EXPOSE_INTERNALS is defined (set by the
// NANOSRV_FUZZ CMake option). This covers the 7-bit / 16-bit / 64-bit extended
// length paths, the mask-bit unmasking loop, and the length bounds checks.
//
// ws_process() UNMASKS IN PLACE, so the fuzzer input must be copied into a
// writable buffer before each call.
//
// Build: configure with -DNANOSRV_FUZZ=ON using clang, then run e.g.
//   ./fuzz_ws -max_total_time=60 tests/fuzz/corpus/ws

#include "nanosrv/nanosrv.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace nanosrv {
// Defined in ws.cpp under NANOSRV_EXPOSE_INTERNALS.
size_t ws_process_for_test(uint8_t* buf, size_t len);
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    // ws_process mutates the buffer (unmasking), so work on a writable copy.
    std::vector<uint8_t> buf(data, data + size);
    (void)nanosrv::ws_process_for_test(buf.empty() ? nullptr : buf.data(),
                                       buf.size());
    return 0;
}
