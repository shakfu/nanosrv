// Fuzz target for the HTTP request/response parser.
//
// Drives nanosrv::http_parse() with arbitrary bytes. The handshake path used
// by the WebSocket upgrade also flows through http_parse(), so this target
// covers request-line parsing, header parsing (including the MG_MAX_HTTP_HEADERS
// truncation boundary), Content-Length handling, and the chunked/multipart
// helpers reachable from a parsed message.
//
// Build: configure with -DNANOSRV_FUZZ=ON using clang, then run e.g.
//   ./fuzz_http -max_total_time=60 tests/fuzz/corpus/http
//
// http_parse() does not mutate its input, so the fuzzer buffer is passed as-is.

#include "nanosrv/nanosrv.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    nanosrv::HttpMessage hm;
    int n = nanosrv::http_parse(reinterpret_cast<const char*>(data), size, &hm);

    // Exercise the accessors / follow-on parsers on a successfully parsed
    // message so their pointer arithmetic is covered too.
    if (n > 0) {
        (void)hm.method_str();
        (void)hm.uri_str();
        (void)hm.query_str();
        (void)hm.body_str();
        (void)hm.status_code();
        (void)hm.header("Content-Type");
        (void)hm.credentials();
    }
    return 0;
}
