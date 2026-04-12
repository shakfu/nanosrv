#pragma once
#include "fmt.hpp"

namespace nanosrv {

enum class LogLevel : int { None = 0, Error, Info, Debug, Verbose };

extern int log_level;

void log(const char* fmt, ...);
void log_prefix(int ll, const char* file, int line, const char* fname);
void hexdump(const void* buf, size_t len);
void log_set_fn(PrintFn fn, void* param);

} // namespace nanosrv
