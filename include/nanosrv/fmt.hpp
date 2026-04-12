#pragma once
#include "types.hpp"

namespace nanosrv {

typedef void (*PrintFn)(char, void*);
typedef size_t (*PrintMatcher)(PrintFn, void*, va_list*);

size_t vxprintf(void (*)(char, void*), void*, const char* fmt, va_list*);
size_t xprintf(void (*fn)(char, void*), void*, const char* fmt, ...);
size_t vsnprintf_(char* buf, size_t len, const char* fmt, va_list* ap);
size_t snprintf_(char*, size_t, const char* fmt, ...);
char* vmprintf(const char* fmt, va_list* ap);
char* mprintf(const char* fmt, ...);

size_t print_base64(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_esc(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_hex(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_ip(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_ip_port(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_ip4(void (*out)(char, void*), void* arg, va_list* ap);
size_t print_ip6(void (*out)(char, void*), void* arg, va_list* ap);

void pfn_iobuf(char ch, void* param);
void pfn_iobuf_noresize(char ch, void* param);
void pfn_stdout(char c, void* param);

} // namespace nanosrv
