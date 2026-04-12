#pragma once
#include "platform.hpp"
#include <string_view>
#include <string>

namespace nanosrv {

struct Str {
    char* buf = nullptr;
    size_t len = 0;

    // Default
    Str() = default;

    // Construct from null-terminated C string (replaces the mg_str(s) macro)
    Str(const char* s) : buf(const_cast<char*>(s)), len(s ? strlen(s) : 0) {}

    // Construct from pointer + length
    Str(const char* s, size_t n) : buf(const_cast<char*>(s)), len(n) {}

    // Construct from mutable pointer + length
    Str(char* b, size_t n) : buf(b), len(n) {}
};



// Modern C++ string view with mutable data pointer (matches Str semantics).
// Implicitly convertible to std::string_view for const contexts.
struct StringView {
    char* data = nullptr;
    size_t size = 0;

    constexpr StringView() = default;
    constexpr StringView(char* d, size_t s) : data(d), size(s) {}
    constexpr StringView(struct Str s) : data(s.buf), size(s.len) {}

    // Construct from string literal / const char* (casts away const to match
    // Str semantics -- the pointer is not used for mutation in practice)
    StringView(const char* s)
        : data(const_cast<char*>(s)), size(s ? strlen(s) : 0) {}
    StringView(const char* s, size_t n) : data(const_cast<char*>(s)), size(n) {}

    constexpr operator std::string_view() const { return {data, size}; }

    // Convert to Str. Uses a method name to avoid Str macro expansion.
    Str to_str() const { return Str(data, size); }

    constexpr bool empty() const { return size == 0; }
    constexpr char operator[](size_t i) const { return data[i]; }

    std::string str() const { return std::string(data, size); }

    constexpr auto operator<=>(const StringView& other) const {
        std::string_view a{data, size}, b{other.data, other.size};
        return a <=> b;
    }
    constexpr bool operator==(const StringView& other) const {
        std::string_view a{data, size}, b{other.data, other.size};
        return a == b;
    }
};

struct Str str_s(const char* s);
struct Str str_n(const char* s, size_t n);
int casecmp(const char* s1, const char* s2);
int str_cmp(const struct Str str1, const struct Str str2);
int str_casecmp(const struct Str str1, const struct Str str2);
struct Str str_dup(const struct Str s);
bool match(struct Str str, struct Str pattern, struct Str* caps);
bool span(struct Str s, struct Str* a, struct Str* b, char delim);
bool str_to_num(struct Str, int base, void* val, size_t val_len);

struct IOBuffer {
    unsigned char* buf = nullptr;
    size_t size = 0;
    size_t len = 0;
    size_t align = 0;

    IOBuffer() = default;
    ~IOBuffer();

    // Move-only
    IOBuffer(IOBuffer&& other) noexcept;
    IOBuffer& operator=(IOBuffer&& other) noexcept;
    IOBuffer(const IOBuffer&) = delete;
    IOBuffer& operator=(const IOBuffer&) = delete;
};

[[nodiscard]] bool iobuf_init(struct IOBuffer*, size_t, size_t);
[[nodiscard]] bool iobuf_resize(struct IOBuffer*, size_t);
void iobuf_free(struct IOBuffer*);
size_t iobuf_add(struct IOBuffer*, size_t, const void*, size_t);
size_t iobuf_del(struct IOBuffer*, size_t ofs, size_t len);

} // namespace nanosrv
