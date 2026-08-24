#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: util ----

void bzero_(volatile unsigned char* buf, size_t len)
{
    if (buf != NULL) {
        while (len--)
            *buf++ = 0;
    }
}

#if MG_ENABLE_CUSTOM_RANDOM
#else
bool random_(void* buf, size_t len)
{
    bool success = false;
    unsigned char* p = (unsigned char*)buf;
#if MG_ARCH == MG_ARCH_ESP32
    while (len--)
        *p++ = (unsigned char)(esp_random() & 255);
    success = true;
#elif MG_ARCH == MG_ARCH_CUBE && defined(HAL_RNG_MODULE_ENABLED)
    extern RNG_HandleTypeDef hrng;
    for (size_t n = 0; n < len; n += sizeof(uint32_t)) {
        uint32_t r = HAL_RNG_ReadLastRandomNumber(&hrng);
        memcpy((char*)buf + n, &r, n + sizeof(r) > len ? len - n : sizeof(r));
    }
    success = true;
#elif MG_ARCH == MG_ARCH_PICOSDK
    while (len--)
        *p++ = (unsigned char)(get_rand_32() & 255);
    success = true;
#elif MG_ARCH == MG_ARCH_ZEPHYR
#if MG_TLS == MG_TLS_BUILTIN                                                  \
    || (MG_TLS == MG_TLS_MBED                                                 \
        && (!defined(MBEDTLS_VERSION_NUMBER)                                  \
            || MBEDTLS_VERSION_NUMBER < 0x04000000))
    return (sys_csrand_get(buf, len) == 0); // do not fallback on reseed error
#else
    sys_rand_get(buf, len);
    success = true;
#endif
#elif MG_ARCH == MG_ARCH_WIN32
#if defined(_MSC_VER) && _MSC_VER < 1700
    static bool initialised = false;
    static HCRYPTPROV hProv;
    // CryptGenRandom() implementation earlier than 2008 is weak, see
    // https://en.wikipedia.org/wiki/CryptGenRandom
    if (!initialised) {
        initialised = CryptAcquireContext(&hProv, NULL, NULL, PROV_RSA_FULL,
                                          CRYPT_VERIFYCONTEXT);
    }
    if (initialised)
        success = CryptGenRandom(hProv, len, p);
#else
    size_t i;
    for (i = 0; i < len; i++) {
        unsigned int rand_v;
        if (rand_s(&rand_v) == 0) {
            p[i] = (unsigned char)(rand_v & 255);
        } else {
            break;
        }
    }
    success = (i == len);
#endif

#elif MG_ARCH == MG_ARCH_UNIX
    FILE* fp = fopen("/dev/urandom", "rb");
    if (fp != NULL) {
        if (fread(buf, 1, len, fp) == len)
            success = true;
        fclose(fp);
    }
#endif
    // If everything above did not work, fallback to a pseudo random generator
    if (success == false) {
        MG_ERROR(("Weak RNG: using rand()"));
        while (len--)
            *p++ = (unsigned char)(rand() & 255);
    }
    return success;
}
#endif

char* random_str(char* buf, size_t len)
{
    size_t i;
    random_(buf, len);
    for (i = 0; i < len; i++) {
        uint8_t c = ((uint8_t*)buf)[i] % 62U;
        buf[i] = i == len - 1 ? (char)'\0'            // 0-terminate last byte
            : c < 26          ? (char)('a' + c)       // lowercase
            : c < 52          ? (char)('A' + c - 26)  // uppercase
                              : (char)('0' + c - 52); // numeric
    }
    return buf;
}

uint32_t crc32_(uint32_t crc, const char* buf, size_t len)
{
    static const uint32_t crclut[16] = {
        // table for polynomial 0xEDB88320 (reflected)
        0x00000000, 0x1DB71064, 0x3B6E20C8, 0x26D930AC, 0x76DC4190, 0x6B6B51F4,
        0x4DB26158, 0x5005713C, 0xEDB88320, 0xF00F9344, 0xD6D6A3E8, 0xCB61B38C,
        0x9B64C2B0, 0x86D3D2D4, 0xA00AE278, 0xBDBDF21C
    };
    crc = ~crc;
    while (len--) {
        uint8_t b = *(uint8_t*)buf++;
        crc = crclut[(crc ^ b) & 0x0F] ^ (crc >> 4);
        crc = crclut[(crc ^ (b >> 4)) & 0x0F] ^ (crc >> 4);
    }
    return ~crc;
}

// Compare the leading `bits` bits of two network-order address byte arrays.
static bool ip_prefix_eq(const uint8_t* a, const uint8_t* b, int bits)
{
    int whole = bits / 8;
    int rem = bits % 8;
    for (int i = 0; i < whole; i++)
        if (a[i] != b[i])
            return false;
    if (rem != 0) {
        uint8_t mask = static_cast<uint8_t>(0xffU << (8 - rem));
        if ((a[whole] & mask) != (b[whole] & mask))
            return false;
    }
    return true;
}

// Evaluate an IP against an ACL of comma-separated entries, each "+<net>" or
// "-<net>" where <net> is an IPv4 or IPv6 address with an optional "/<prefix>"
// (a bare address is a host route -- /32 for IPv4, /128 for IPv6). The last
// matching entry wins; with a non-empty ACL the default is deny. An entry only
// applies to peers of its own address family (IPv4 entries never match IPv6
// peers and vice versa). Both families are matched the same way -- a bitwise
// prefix compare over the network-order address bytes -- so a restrictive ACL
// no longer fails open for IPv6 (previously the IPv6 path returned early).
//
// Returns 1 = allowed, 0 = denied, -1 = malformed entry (missing/bad +/- flag),
// -2 = unparseable address or prefix.
int check_ip_acl(struct Str acl, struct Address* remote_ip)
{
    struct Str entry;
    int allowed = acl.len == 0 ? '+' : '-'; // non-empty ACL: deny by default
    while (span(acl, &entry, &acl, ',')) {
        if (entry.len < 2 || (entry.buf[0] != '+' && entry.buf[0] != '-'))
            return -1;
        char flag = entry.buf[0];

        // Split "<addr>[/<prefix>]" (everything after the +/- flag).
        struct Str spec = str_n(&entry.buf[1], entry.len - 1);
        struct Str addr_str = spec;
        long prefix = -1;
        for (size_t i = 0; i < spec.len; i++) {
            if (spec.buf[i] == '/') {
                addr_str = str_n(spec.buf, i);
                unsigned long p = 0;
                if (!str_to_num(str_n(&spec.buf[i + 1], spec.len - i - 1), 10,
                                &p, sizeof(p)))
                    return -2;
                prefix = static_cast<long>(p);
                break;
            }
        }
        if (addr_str.len == 0)
            return -2;

        struct Address net;
        memset(&net, 0, sizeof(net));
        if (!aton(addr_str, &net))
            return -2;

        int maxbits = net.is_ip6 ? 128 : 32;
        if (prefix < 0)
            prefix = maxbits; // bare address: exact host match
        if (prefix > maxbits)
            return -2;

        // An entry only applies to peers of the same address family.
        if (net.is_ip6 != remote_ip->is_ip6)
            continue;
        if (ip_prefix_eq(remote_ip->addr.ip, net.addr.ip,
                         static_cast<int>(prefix)))
            allowed = flag;
    }
    return allowed == '+';
}

bool path_is_sane(const struct Str path)
{
    const char* s = path.buf;
    size_t n = path.len;
    if (path.buf[0] == '~')
        return false; // Starts with ~
    if (path.buf[0] == '.' && path.buf[1] == '.')
        return false; // Starts with ..
    for (; s[0] != '\0' && n > 0; s++, n--) {
        if ((s[0] == '/' || s[0] == '\\') && n >= 2) { // Subdir?
            if (s[1] == '.' && s[2] == '.')
                return false; // Starts with ..
        }
    }
    return true;
}

#if MG_ENABLE_CUSTOM_MILLIS
#else
uint64_t millis(void)
{
#if MG_ARCH == MG_ARCH_WIN32
    return GetTickCount();
#elif MG_ARCH == MG_ARCH_PICOSDK
    return time_us_64() / 1000;
#elif MG_ARCH == MG_ARCH_ESP8266 || MG_ARCH == MG_ARCH_ESP32                  \
    || MG_ARCH == MG_ARCH_FREERTOS
    return xTaskGetTickCount() * portTICK_PERIOD_MS;
#elif MG_ARCH == MG_ARCH_CUBE
    return (uint64_t)HAL_GetTick();
#elif MG_ARCH == MG_ARCH_THREADX
    return tx_time_get() * (1000 /* MS per SEC */ / TX_TIMER_TICKS_PER_SECOND);
#elif MG_ARCH == MG_ARCH_TIRTOS
    return (uint64_t)Clock_getTicks();
#elif MG_ARCH == MG_ARCH_ZEPHYR
    return (uint64_t)k_uptime_get();
#elif MG_ARCH == MG_ARCH_CMSIS_RTOS1
    return (uint64_t)rt_time_get();
#elif MG_ARCH == MG_ARCH_CMSIS_RTOS2
    return (uint64_t)((osKernelGetTickCount() * 1000) / osKernelGetTickFreq());
#elif MG_ARCH == MG_ARCH_RTTHREAD
    return (uint64_t)((rt_tick_get() * 1000) / RT_TICK_PER_SECOND);
#elif MG_ARCH == MG_ARCH_UNIX && defined(__APPLE__)
    // Apple CLOCK_MONOTONIC_RAW is equivalent to CLOCK_BOOTTIME on linux
    // Apple CLOCK_UPTIME_RAW is equivalent to CLOCK_MONOTONIC_RAW on linux
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000000;
#elif MG_ARCH == MG_ARCH_UNIX
    struct timespec ts = { 0, 0 };
    // See #1615 - prefer monotonic clock
#if defined(CLOCK_MONOTONIC_RAW)
    // Raw hardware-based time that is not subject to NTP adjustment
    clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
#elif defined(CLOCK_MONOTONIC)
    // Affected by the incremental adjustments performed by adjtime and NTP
    clock_gettime(CLOCK_MONOTONIC, &ts);
#else
    // Affected by discontinuous jumps in the system time and by the
    // incremental adjustments performed by adjtime and NTP
    clock_gettime(CLOCK_REALTIME, &ts);
#endif
    return ((uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000);
#elif defined(ARDUINO)
    return (uint64_t)millis();
#else
    return (uint64_t)(time(NULL) * 1000);
#endif
}
#endif

// network format equates big endian order
uint16_t ntohs_(uint16_t net) { return MG_LOAD_BE16(&net); }

uint32_t ntohl_(uint32_t net) { return MG_LOAD_BE32(&net); }

uint64_t ntohll_(uint64_t net) { return MG_LOAD_BE64(&net); }

void delayms(unsigned int ms)
{
    uint64_t to = millis() + ms + 1;
    while (millis() < to)
        (void)0;
}

#if MG_ENABLE_CUSTOM_CALLOC
#else
void* mem_calloc(size_t count, size_t size) { return calloc(count, size); }

void mem_free(void* ptr) { free(ptr); }
#endif


// ---------------------------------------------------------------------------
// System resolver discovery
// ---------------------------------------------------------------------------
//
// The default DNS server used to be a hardcoded MG_DEFAULT_DNS4_URL
// ("udp://8.8.8.8:53"), so outbound name resolution ignored the host's own
// configuration: internal names silently failed to resolve, split-horizon DNS
// was bypassed, queries left the network, and a container or VPN resolver was
// never consulted. Read the system resolver instead, keeping the old constant
// only as a fallback.

std::string parse_resolv_conf(std::string_view contents, bool want_ipv6)
{
    size_t pos = 0;
    while (pos < contents.size()) {
        size_t eol = contents.find('\n', pos);
        std::string_view line = contents.substr(
            pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos);
        pos = (eol == std::string_view::npos) ? contents.size() : eol + 1;

        // Trim leading blanks; skip comments (# and ; are both used).
        size_t b = line.find_first_not_of(" \t\r");
        if (b == std::string_view::npos)
            continue;
        line.remove_prefix(b);
        if (line[0] == '#' || line[0] == ';')
            continue;

        constexpr std::string_view kw = "nameserver";
        if (line.size() <= kw.size() || line.compare(0, kw.size(), kw) != 0)
            continue;
        if (line[kw.size()] != ' ' && line[kw.size()] != '\t')
            continue;  // "nameservers" and friends are not this directive

        std::string_view rest = line.substr(kw.size());
        size_t s0 = rest.find_first_not_of(" \t");
        if (s0 == std::string_view::npos)
            continue;
        rest.remove_prefix(s0);
        size_t s1 = rest.find_first_of(" \t\r#;");
        std::string_view addr =
            (s1 == std::string_view::npos) ? rest : rest.substr(0, s1);
        if (addr.empty())
            continue;

        // resolv.conf may carry an IPv6 zone ("fe80::1%eth0"); keep it, the
        // address parser understands the scope suffix.
        bool is_ipv6 = addr.find(':') != std::string_view::npos;
        if (is_ipv6 != want_ipv6)
            continue;

        std::string url = "udp://";
        if (is_ipv6) {
            url += '[';
            url.append(addr);
            url += ']';
        } else {
            url.append(addr);
        }
        url += ":53";
        return url;
    }
    return {};
}

// Read /etc/resolv.conf and pick the first nameserver of the given family.
// Returns empty on Windows (no such file) or when the file is absent,
// unreadable, or names no server of that family -- the caller falls back.
static std::string read_resolv_conf_field(bool want_ipv6)
{
#if MG_ARCH == MG_ARCH_WIN32
    (void)want_ipv6;
    return {};
#else
    FILE* f = fopen("/etc/resolv.conf", "re");
    if (f == NULL)
        return {};
    std::string contents;
    char buf[512];
    size_t n;
    // resolv.conf is small; cap the read so a pathological file cannot make
    // startup allocate without bound.
    constexpr size_t kMaxRead = 64 * 1024;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        contents.append(buf, n);
        if (contents.size() >= kMaxRead)
            break;
    }
    fclose(f);
    return parse_resolv_conf(contents, want_ipv6);
#endif
}

const char* system_dns_url(bool ipv6)
{
    // Read once per process: resolv.conf is re-read by libc resolvers on
    // change, but a long-lived server that cached a resolver at startup is the
    // established behaviour here, and this keeps the returned pointer stable
    // for the lifetime of every Mgr that stores it.
    static const std::string v4 = [] {
        std::string s = read_resolv_conf_field(false);
        return s.empty() ? std::string(MG_DEFAULT_DNS4_URL) : s;
    }();
    static const std::string v6 = [] {
        std::string s = read_resolv_conf_field(true);
        return s.empty() ? std::string(MG_DEFAULT_DNS6_URL) : s;
    }();
    return ipv6 ? v6.c_str() : v4.c_str();
}

} // namespace nanosrv
