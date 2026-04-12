#pragma once
#include "../platform.hpp"

namespace nanosrv {

typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} Sha1Ctx;

void sha1_init(Sha1Ctx*);
void sha1_update(Sha1Ctx*, const unsigned char* data, size_t len);
void sha1_final(unsigned char digest[20], Sha1Ctx*);

} // namespace nanosrv
