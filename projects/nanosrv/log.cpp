#include "nanosrv/nanosrv.hpp"
#include <mutex>

namespace nanosrv {

// ---- module: log ----

int log_level = MG_LL_DEBUG;
static PrintFn s_log_func = pfn_stdout;
static void* s_log_func_param = NULL;
static std::recursive_mutex s_log_mutex;

void log_set_fn(PrintFn fn, void* param)
{
    std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
    s_log_func = fn;
    s_log_func_param = param;
}

static void logc(unsigned char c) { s_log_func(static_cast<char>(c), s_log_func_param); }

static void logs(const char* buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        logc(static_cast<unsigned char>(buf[i]));
}

#if MG_ENABLE_CUSTOM_LOG
// Let user define their own log_prefix() and log()
#else
void log_prefix(int level, const char* file, int line, const char* fname)
{
    std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
    const char* p = strrchr(file, '/');
    char buf[MG_LOG_PREFIX_BUF_SIZE];
    size_t n;
    if (p == NULL)
        p = strrchr(file, '\\');
    n = snprintf_(buf, sizeof(buf), "%-6llx %d %s:%d:%s", millis(), level,
                    p == NULL ? file : p + 1, line, fname);
    if (n > sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    while (n < sizeof(buf))
        buf[n++] = ' ';
    logs(buf, n - 1);
}

void log(const char* fmt, ...)
{
    std::lock_guard<std::recursive_mutex> lock(s_log_mutex);
    va_list ap;
    va_start(ap, fmt);
    vxprintf(s_log_func, s_log_func_param, fmt, &ap);
    va_end(ap);
    logs("\r\n", 2);
}
#endif

static unsigned char nibble(unsigned c)
{
    return (unsigned char)(c < 10 ? c + '0' : c + 'W');
}

#define ISPRINT(x) ((x) >= ' ' && (x) <= '~')

void hexdump(const void* buf, size_t len)
{
    const unsigned char* p = (const unsigned char*)buf;
    unsigned char ascii[MG_HEXDUMP_BYTES_PER_LINE], alen = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        if ((i % MG_HEXDUMP_BYTES_PER_LINE) == 0) {
            // Print buffered ascii chars
            if (i > 0)
                logs("  ", 2), logs((char*)ascii, MG_HEXDUMP_BYTES_PER_LINE), logs("\r\n", 2),
                    alen = 0;
            // Print hex address, then \t
            logc(nibble((i >> 12) & 15)), logc(nibble((i >> 8) & 15)),
                logc(nibble((i >> 4) & 15)), logc('0'), logs("   ", 3);
        }
        logc(nibble(p[i] >> 4)),
            logc(nibble(p[i] & 15));                // Two nibbles, e.g. c5
        logc(' ');                                  // Space after hex number
        ascii[alen++] = ISPRINT(p[i]) ? p[i] : '.'; // Add to the ascii buf
    }
    while (alen < 16)
        logs("   ", 3), ascii[alen++] = ' ';
    logs("  ", 2), logs((char*)ascii, 16), logs("\r\n", 2);
}

} // namespace nanosrv
