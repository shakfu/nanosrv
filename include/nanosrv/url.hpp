#pragma once
#include "types.hpp"

#include <cstdint>
#include <string_view>

namespace nanosrv {

unsigned short url_port(const char* url);
int url_is_ssl(const char* url);
struct Str url_host(const char* url);
const char* url_uri(const char* url);

struct Url {
    std::string_view host;
    uint16_t port = 0;
    std::string_view path;
    bool is_ssl = false;

    static Url parse(std::string_view url);
};

} // namespace nanosrv
