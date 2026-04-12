#pragma once
#include "platform.hpp"

namespace nanosrv {

void* mem_calloc(size_t count, size_t size);
void mem_free(void* ptr);
void bzero_(volatile unsigned char* buf, size_t len);
bool random_(void* buf, size_t len);
char* random_str(char* buf, size_t len);
uint64_t millis(void);

} // namespace nanosrv
