#pragma once
#include "types.hpp"

#include <optional>
#include <string>
#include <string_view>

#ifndef MG_JSON_MAX_DEPTH
#define MG_JSON_MAX_DEPTH 30
#endif

namespace nanosrv {

enum { MG_JSON_TOO_DEEP = -1, MG_JSON_INVALID = -2, MG_JSON_NOT_FOUND = -3 };

int json_get(struct Str json, const char* path, int* toklen);
struct Str json_get_tok(struct Str json, const char* path);
bool json_get_num(struct Str json, const char* path, double* v);
bool json_get_bool(struct Str json, const char* path, bool* v);
long json_get_long(struct Str json, const char* path, long dflt);
char* json_get_str(struct Str json, const char* path);
bool json_unescape(struct Str str, char* buf, size_t len);
size_t json_next(struct Str obj, size_t ofs, struct Str* key,
                    struct Str* val);

namespace json {
    std::optional<double> number(std::string_view json, const char* path);
    std::optional<bool> boolean(std::string_view json, const char* path);
    std::optional<long> integer(std::string_view json, const char* path);
    std::optional<std::string> string(std::string_view json, const char* path);
} // namespace json

} // namespace nanosrv
