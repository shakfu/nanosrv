#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

static void pfn_iobuf_private(char ch, void* param, bool expand)
{
    struct IOBuffer* io = static_cast<struct IOBuffer*>(param);
    if (expand && io->len + 2 > io->size) {
        // Grow capacity geometrically. This callback appends one byte at a
        // time, and iobuf_resize() copies the whole buffer on every growth; if
        // we only ever asked for `len + 2` the buffer would be reallocated once
        // per io->align step, making a large printf O(n^2) in the output size
        // (e.g. a 32 MB http_reply body took ~16 s). Requesting 1.5x the
        // current capacity keeps the number of reallocations logarithmic, so
        // building the output is amortized O(n). iobuf_resize() still rounds up
        // to io->align; `len` (the true data length) is unchanged.
        size_t want = io->len + 2;
        size_t grow = io->size + io->size / 2;
        (void)iobuf_resize(io, want > grow ? want : grow);
    }
    if (io->len + 2 <= io->size) {
        io->buf[io->len++] = static_cast<uint8_t>(ch);
        io->buf[io->len] = 0;
    } else if (io->len < io->size) {
        io->buf[io->len++] = 0; // Guarantee to 0-terminate
    }
}

void pfn_iobuf_noresize(char ch, void* param)
{
    pfn_iobuf_private(ch, param, false);
}

void pfn_iobuf(char ch, void* param)
{
    pfn_iobuf_private(ch, param, true);
}

size_t vsnprintf_(char* buf, size_t len, const char* fmt, va_list* ap)
{
    struct IOBuffer io{};
    size_t n;
    io.buf = reinterpret_cast<uint8_t*>(buf), io.size = len;
    n = vxprintf(pfn_iobuf_noresize, &io, fmt, ap);
    if (n < len)
        buf[n] = '\0';
    io.buf = nullptr;  // External buffer, not owned -- prevent destructor free
    return n;
}

size_t snprintf_(char* buf, size_t len, const char* fmt, ...)
{
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    n = vsnprintf_(buf, len, fmt, &ap);
    va_end(ap);
    return n;
}

char* vmprintf(const char* fmt, va_list* ap)
{
    struct IOBuffer io{};
    io.align = 256;
    vxprintf(pfn_iobuf, &io, fmt, ap);
    // Release ownership -- caller takes the buffer
    auto* result = reinterpret_cast<char*>(io.buf);
    io.buf = nullptr;  // Prevent destructor from freeing
    return result;
}

char* mprintf(const char* fmt, ...)
{
    char* s;
    va_list ap;
    va_start(ap, fmt);
    s = vmprintf(fmt, &ap);
    va_end(ap);
    return s;
}

void pfn_stdout(char c, void* param)
{
    putchar(c);
    (void)param;
}

static size_t print_ip4(void (*out)(char, void*), void* arg, uint8_t* p)
{
    return xprintf(out, arg, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
}

static size_t print_ip6(void (*out)(char, void*), void* arg, uint16_t* p)
{
    return xprintf(out, arg, "[%x:%x:%x:%x:%x:%x:%x:%x]", ntohs_(p[0]),
                      ntohs_(p[1]), ntohs_(p[2]), ntohs_(p[3]),
                      ntohs_(p[4]), ntohs_(p[5]), ntohs_(p[6]),
                      ntohs_(p[7]));
}

size_t print_ip4(void (*out)(char, void*), void* arg, va_list* ap)
{
    uint8_t* p = va_arg(*ap, uint8_t*);
    return print_ip4(out, arg, p);
}

size_t print_ip6(void (*out)(char, void*), void* arg, va_list* ap)
{
    uint16_t* p = va_arg(*ap, uint16_t*);
    return print_ip6(out, arg, p);
}

size_t print_ip(void (*out)(char, void*), void* arg, va_list* ap)
{
    struct Address* addr = va_arg(*ap, struct Address*);
    if (addr->is_ip6)
        return print_ip6(out, arg, reinterpret_cast<uint16_t*>(addr->addr.ip));
    return print_ip4(out, arg, reinterpret_cast<uint8_t*>(&addr->addr.ip));
}

size_t print_ip_port(void (*out)(char, void*), void* arg, va_list* ap)
{
    struct Address* a = va_arg(*ap, struct Address*);
    return xprintf(out, arg, "%M:%hu", print_ip, a, ntohs_(a->port));
}

static char esc_char(int c, bool esc)
{
    const char *p, *esc1 = "\b\f\n\r\t\\\"", *esc2 = "bfnrt\\\"";
    for (p = esc ? esc1 : esc2; *p != '\0'; p++) {
        if (*p == c)
            return esc ? esc2[p - esc1] : esc1[p - esc2];
    }
    return 0;
}

static char escape_char(int c) { return esc_char(c, true); }

static size_t qcpy(void (*out)(char, void*), void* ptr, char* buf, size_t len)
{
    size_t i = 0, extra = 0;
    for (i = 0; i < len && buf[i] != '\0'; i++) {
        char c = escape_char(buf[i]);
        if (c) {
            out('\\', ptr), out(c, ptr), extra++;
        } else {
            out(buf[i], ptr);
        }
    }
    return i + extra;
}

static size_t bcpy(void (*out)(char, void*), void* arg, uint8_t* buf,
                   size_t len)
{
    size_t i, j, n = 0;
    const char*
        t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (i = 0; i < len; i += 3) {
        uint8_t c1 = buf[i], c2 = i + 1 < len ? buf[i + 1] : 0,
                c3 = i + 2 < len ? buf[i + 2] : 0;
        char tmp[4] = { 0, 0, '=', '=' };
        tmp[0] = t[c1 >> 2], tmp[1] = t[(c1 & 3) << 4 | (c2 >> 4)];
        if (i + 1 < len)
            tmp[2] = t[(c2 & 15) << 2 | (c3 >> 6)];
        if (i + 2 < len)
            tmp[3] = t[c3 & 63];
        for (j = 0; j < sizeof(tmp) && tmp[j] != '\0'; j++)
            out(tmp[j], arg);
        n += j;
    }
    return n;
}

size_t print_hex(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t bl = static_cast<size_t>(va_arg(*ap, int));
    uint8_t* p = va_arg(*ap, uint8_t*);
    const char* hex = "0123456789abcdef";
    size_t j;
    for (j = 0; j < bl; j++) {
        out(hex[(p[j] >> 4) & 0x0F], arg);
        out(hex[p[j] & 0x0F], arg);
    }
    return 2 * bl;
}

size_t print_base64(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t len = static_cast<size_t>(va_arg(*ap, int));
    uint8_t* buf = va_arg(*ap, uint8_t*);
    return bcpy(out, arg, buf, len);
}

size_t print_esc(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t len = static_cast<size_t>(va_arg(*ap, int));
    char* p = va_arg(*ap, char*);
    if (len == 0)
        len = p == NULL ? 0 : strlen(p);
    return qcpy(out, arg, p, len);
}

} // namespace nanosrv
