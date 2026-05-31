// Fuzz target for the DNS message parser.
//
// Drives nanosrv::dns_parse() with arbitrary bytes. This is the highest-value
// DNS target because dns_parse() consumes untrusted packets from the network
// and walks name-compression pointers (dns_parse_name_depth) and a variable
// answer/question count -- exactly the inputs most likely to trip a pointer or
// recursion bug. The recursion-depth and answer-count caps are what this target
// is meant to stress.
//
// dns_parse() does not mutate its input.
//
// Build: configure with -DNANOSRV_FUZZ=ON using clang, then run e.g.
//   ./fuzz_dns -max_total_time=60 tests/fuzz/corpus/dns

#include "nanosrv/nanosrv.hpp"

#include <cstddef>
#include <cstdint>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    nanosrv::DnsMessage dm;
    (void)nanosrv::dns_parse(data, size, &dm);
    return 0;
}
