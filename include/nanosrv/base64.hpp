#pragma once
#include "platform.hpp"

#include <string>
#include <string_view>

namespace nanosrv {

size_t base64_update(unsigned char ch, char* to, size_t n);
size_t base64_final(char* to, size_t n);
size_t base64_encode(const unsigned char* p, size_t n, char* to, size_t dl);
size_t base64_decode(const char* src, size_t n, char* dst, size_t dl);

// Modern API
std::string base64_encode(std::string_view input);
std::string base64_decode(std::string_view input);

} // namespace nanosrv
