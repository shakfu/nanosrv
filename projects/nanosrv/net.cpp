#include "nanosrv/nanosrv.hpp"
#include <new>

namespace nanosrv {

size_t conn_vprintf(struct Connection* c, const char* fmt, va_list* ap)
{
    size_t old = c->send.len;
    size_t expected = vxprintf(pfn_iobuf, &c->send, fmt, ap);
    size_t actual = c->send.len - old;
    if (actual != expected) {
        error(c, "OOM");
        c->send.len = old;
        actual = 0;
    }
    return actual;
}

size_t conn_printf(struct Connection* c, const char* fmt, ...)
{
    size_t len = 0;
    va_list ap;
    va_start(ap, fmt);
    len = conn_vprintf(c, fmt, &ap);
    va_end(ap);
    return len;
}

static bool atonl(struct Str str, struct Address* addr)
{
    uint32_t localhost = mg_htonl(MG_IPV4_LOCALHOST);
    if (str_casecmp(str, Str("localhost")) != 0)
        return false;
    memcpy(addr->addr.ip, &localhost, sizeof(uint32_t));
    addr->is_ip6 = false;
    return true;
}

static bool atone(struct Str str, struct Address* addr)
{
    if (str.len > 0)
        return false;
    memset(addr->addr.ip, 0, sizeof(addr->addr.ip));
    addr->is_ip6 = false;
    return true;
}

static bool aton4(struct Str str, struct Address* addr)
{
    uint8_t data[4] = { 0, 0, 0, 0 };
    size_t i, num_dots = 0;
    for (i = 0; i < str.len; i++) {
        if (str.buf[i] >= '0' && str.buf[i] <= '9') {
            int octet = data[num_dots] * 10 + (str.buf[i] - '0');
            if (octet > MG_IPV4_OCTET_MAX)
                return false;
            data[num_dots] = static_cast<uint8_t>(octet);
        } else if (str.buf[i] == '.') {
            if (num_dots >= MG_IPV4_NUM_DOTS || i == 0 || str.buf[i - 1] == '.')
                return false;
            num_dots++;
        } else {
            return false;
        }
    }
    if (num_dots != MG_IPV4_NUM_DOTS || str.buf[i - 1] == '.')
        return false;
    memcpy(&addr->addr.ip, data, sizeof(data));
    addr->is_ip6 = false;
    return true;
}

static bool v4mapped(struct Str str, struct Address* addr)
{
    int i;
    uint32_t ipv4;
    if (str.len < MG_IPV6_V4MAPPED_MIN_LEN)
        return false;
    if (str.buf[0] != ':' || str.buf[1] != ':' || str.buf[6] != ':')
        return false;
    for (i = 2; i < 6; i++) {
        if (str.buf[i] != 'f' && str.buf[i] != 'F')
            return false;
    }
    // struct Str s = str_n(&str.buf[7], str.len - 7);
    if (!aton4(str_n(&str.buf[7], str.len - 7), addr))
        return false;
    memcpy(&ipv4, addr->addr.ip, sizeof(ipv4));
    memset(addr->addr.ip, 0, sizeof(addr->addr.ip));
    addr->addr.ip[MG_IPV6_V4MAPPED_PAD_OFFSET] = addr->addr.ip[MG_IPV6_V4MAPPED_PAD_OFFSET + 1] = MG_IPV4_OCTET_MAX;
    memcpy(&addr->addr.ip[MG_IPV6_V4MAPPED_DATA_OFFSET], &ipv4, MG_DNS_IPV4_ADDR_LEN);
    addr->is_ip6 = true;
    return true;
}

static bool aton6(struct Str str, struct Address* addr)
{
    size_t i, j = 0, n = 0, dc = MG_IPV6_NO_DOUBLE_COLON;
    addr->scope_id = 0;
    if (str.len > 2 && str.buf[0] == '[')
        str.buf++, str.len -= 2;
    if (v4mapped(str, addr))
        return true; // sets addr->is_ip6
    for (i = 0; i < str.len; i++) {
        if ((str.buf[i] >= '0' && str.buf[i] <= '9')
            || (str.buf[i] >= 'a' && str.buf[i] <= 'f')
            || (str.buf[i] >= 'A' && str.buf[i] <= 'F')) {
            unsigned long val = 0; // TODO(): This loops on chars, refactor
            if (i > j + 3)
                return false;
            // MG_DEBUG(("%lu %lu [%.*s]", i, j, (int) (i - j + 1),
            // &str.buf[j]));
            str_to_num(str_n(&str.buf[j], i - j + 1), 16, &val,
                          sizeof(val));
            addr->addr.ip[n] = static_cast<uint8_t>((val >> 8) & MG_IPV4_OCTET_MAX);
            addr->addr.ip[n + 1] = static_cast<uint8_t>(val & MG_IPV4_OCTET_MAX);
        } else if (str.buf[i] == ':') {
            j = i + 1;
            if (i > 0 && str.buf[i - 1] == ':') {
                dc = n; // Double colon
                if (i > 1 && str.buf[i - 2] == ':')
                    return false;
            } else if (i > 0) {
                n += 2;
            }
            if (n > MG_IPV6_ADDR_BYTES)
                return false;
            addr->addr.ip[n] = addr->addr.ip[n + 1] = 0; // For trailing ::
        } else if (str.buf[i] == '%') { // Scope ID, last in string
            if (str_to_num(str_n(&str.buf[i + 1], str.len - i - 1), 10,
                              &addr->scope_id, sizeof(uint8_t))) {
                addr->is_ip6 = true;
                return true;
            } else {
                return false;
            }
        } else {
            return false;
        }
    }
    if (n < MG_IPV6_ADDR_BYTES && dc == MG_IPV6_NO_DOUBLE_COLON)
        return false;
    if (n < MG_IPV6_ADDR_BYTES) {
        memmove(&addr->addr.ip[dc + (MG_IPV6_ADDR_BYTES - n)], &addr->addr.ip[dc], n - dc + 2);
        memset(&addr->addr.ip[dc], 0, MG_IPV6_ADDR_BYTES - n);
    }

    addr->is_ip6 = true;
    return true;
}

bool aton(struct Str str, struct Address* addr)
{
    // MG_INFO(("[%.*s]", (int) str.len, str.buf));
    return atone(str, addr) || atonl(str, addr) || aton4(str, addr)
        || aton6(str, addr);
}

// Connection pool: max 64 recycled connections per manager
static constexpr int CONN_POOL_MAX = 64;

struct Connection* alloc_conn(struct Mgr* mgr)
{
    Connection* c = nullptr;
    size_t total = sizeof(Connection) + mgr->extraconnsize;

    // Try pool first (only if no extraconnsize -- pool entries are fixed size)
    if (mgr->conn_pool != nullptr && mgr->extraconnsize == 0) {
        c = static_cast<Connection*>(mgr->conn_pool);
        mgr->conn_pool = c->next;
        mgr->conn_pool_size--;
        memset(c, 0, total);
        new (c) Connection{};
    } else {
        void* mem = ::operator new(total, std::nothrow);
        if (mem == nullptr)
            return nullptr;
        memset(mem, 0, total);
        c = new (mem) Connection{};
    }

    c->mgr = mgr;
    c->send.align = c->recv.align = c->rtls.align = MG_IO_SIZE;
    c->id = ++mgr->nextid;
    c->last_active = millis();  // start the idle clock at creation
    MG_PROF_INIT(c);
    return c;
}

void close_conn(struct Connection* c)
{
    resolve_cancel(c); // Close any pending DNS query
    LIST_DELETE(struct Connection, &c->mgr->conns, c);
    if (c == c->mgr->dns4.c)
        c->mgr->dns4.c = NULL;
    if (c == c->mgr->dns6.c)
        c->mgr->dns6.c = NULL;
    // Order of operations is important. `MG_EV_CLOSE` event must be fired
    // before we deallocate received data, see #1331
    call(c, MG_EV_CLOSE, NULL);
    MG_DEBUG(("%lu %ld closed", c->id, c->fd));
    MG_PROF_DUMP(c);
    MG_PROF_FREE(c);

    tls_free(c);

    // Return to pool or free
    struct Mgr* mgr = c->mgr;
    c->~Connection();
    if (mgr->extraconnsize == 0 && mgr->conn_pool_size < CONN_POOL_MAX) {
        // Recycle: stash in free-list using the `next` pointer field
        auto* pooled = reinterpret_cast<Connection*>(c);
        pooled->next = static_cast<Connection*>(mgr->conn_pool);
        mgr->conn_pool = pooled;
        mgr->conn_pool_size++;
    } else {
        ::operator delete(c);
    }
}

struct Connection* connect_svc(struct Mgr* mgr, const char* url,
                                     EventHandler fn, void* fn_data,
                                     EventHandler pfn, void* pfn_data)
{
    struct Connection* c = NULL;
    if (url == NULL || url[0] == '\0') {
        MG_ERROR(("null url"));
    } else if ((c = alloc_conn(mgr)) == NULL) {
        MG_ERROR(("OOM"));
    } else {
        LIST_ADD_HEAD(struct Connection, &mgr->conns, c);
        c->is_udp = (strncmp(url, "udp:", 4) == 0);
        c->fd = reinterpret_cast<void*>(static_cast<size_t>(MG_INVALID_SOCKET));
        c->fn = fn;
        c->is_client = true;
        c->fn_data = fn_data;
        c->is_tls = (url_is_ssl(url) != 0);
        c->pfn = pfn;
        c->pfn_data = pfn_data;
        call(c, MG_EV_OPEN, const_cast<char*>(url));
        MG_DEBUG(("%lu %ld %s", c->id, c->fd, url));
        resolve(c, url);
    }
    return c;
}

struct Connection* connect(struct Mgr* mgr, const char* url,
                                 EventHandler fn, void* fn_data)
{
    return connect_svc(mgr, url, fn, fn_data, NULL, NULL);
}

struct Connection* listen_(struct Mgr* mgr, const char* url,
                                EventHandler fn, void* fn_data)
{
    struct Connection* c = NULL;
    if ((c = alloc_conn(mgr)) == NULL) {
        MG_ERROR(("OOM %s", url));
    } else if (!open_listener(c, url)) {
        MG_ERROR(("Failed: %s", url));
        MG_PROF_FREE(c);
        mem_free(c);
        c = NULL;
    } else {
        c->is_listening = 1;
        c->is_udp = strncmp(url, "udp:", 4) == 0;
        LIST_ADD_HEAD(struct Connection, &mgr->conns, c);
        c->fn = fn;
        c->fn_data = fn_data;
        c->is_tls = (url_is_ssl(url) != 0);
        call(c, MG_EV_OPEN, NULL);
        MG_DEBUG(("%lu %ld %s", c->id, c->fd, url));
    }
    return c;
}

struct Connection* wrapfd(struct Mgr* mgr, int fd,
                                EventHandler fn, void* fn_data)
{
    struct Connection* c = alloc_conn(mgr);
    if (c != NULL) {
        c->fd = reinterpret_cast<void*>(static_cast<size_t>(fd));
        c->fn = fn;
        c->fn_data = fn_data;
        MG_EPOLL_ADD(c);
        MG_KQUEUE_ADD(c);
        call(c, MG_EV_OPEN, NULL);
        LIST_ADD_HEAD(struct Connection, &mgr->conns, c);
    }
    return c;
}

struct Timer* timer_add(struct Mgr* mgr, uint64_t milliseconds,
                              unsigned flags, void (*fn)(void*), void* arg)
{
    struct Timer* t = static_cast<struct Timer*>(mem_calloc(1, sizeof(*t)));
    if (t != NULL) {
        flags |= MG_TIMER_AUTODELETE; // We have alloc'ed it, so autodelete
        timer_init(&mgr->timers, t, milliseconds, flags, fn, arg);
    }
    return t;
}

long io_recv(struct Connection* c, void* buf, size_t len)
{
    if (c->rtls.len == 0)
        return MG_IO_WAIT;
    if (len > c->rtls.len)
        len = c->rtls.len;
    memcpy(buf, c->rtls.buf, len);
    iobuf_del(&c->rtls, 0, len);
    return static_cast<long>(len);
}

void mgr_free(struct Mgr* mgr)
{
    struct Connection* c;
    struct Timer *tmp, *t = mgr->timers;
    while (t != NULL)
        tmp = t->next, mem_free(t), t = tmp;
    mgr->timers = NULL; // Important. Next call to poll won't touch timers
    for (c = mgr->conns; c != NULL; c = c->next)
        c->is_closing = 1;
    mgr_poll(mgr, 0);
    MG_DEBUG(("All connections closed"));
    // Drain connection pool
    while (mgr->conn_pool != nullptr) {
        auto* c = static_cast<Connection*>(mgr->conn_pool);
        mgr->conn_pool = c->next;
        ::operator delete(c);
    }
    mgr->conn_pool_size = 0;
#if MG_ENABLE_IO_URING
    if (mgr->uring != nullptr) {
        io_uring_queue_exit(static_cast<struct io_uring*>(mgr->uring));
        mem_free(mgr->uring);
        mgr->uring = nullptr;
    }
#elif MG_ENABLE_EPOLL || MG_ENABLE_KQUEUE
    if (mgr->epoll_fd >= 0)
        close(mgr->epoll_fd), mgr->epoll_fd = -1;
#endif
    tls_ctx_free(mgr);
}

void mgr_init(struct Mgr* mgr)
{
    memset(mgr, 0, sizeof(*mgr));
#if MG_ENABLE_IO_URING
    mgr->epoll_fd = -1;
    mgr->uring = mem_calloc(1, sizeof(struct io_uring));
    if (mgr->uring != nullptr) {
        if (io_uring_queue_init(256, static_cast<struct io_uring*>(mgr->uring), 0) < 0) {
            MG_ERROR(("io_uring_queue_init failed errno %d", errno));
            mem_free(mgr->uring);
            mgr->uring = nullptr;
        }
    }
#elif MG_ENABLE_EPOLL
    if ((mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC)) < 0)
        MG_ERROR(("epoll_create1 errno %d", errno));
#elif MG_ENABLE_KQUEUE
    if ((mgr->epoll_fd = kqueue()) < 0)
        MG_ERROR(("kqueue errno %d", errno));
#else
    mgr->epoll_fd = -1;
#endif
#if MG_ARCH == MG_ARCH_WIN32 && MG_ENABLE_WINSOCK
    // clang-format off
  { WSADATA data; WSAStartup(MAKEWORD(2, 2), &data); }
    // clang-format on
#elif MG_ARCH == MG_ARCH_UNIX
    // Ignore SIGPIPE signal, so if client cancels the request, it
    // won't kill the whole process.
    signal(SIGPIPE, SIG_IGN);
#endif
    mgr->pipe = MG_INVALID_SOCKET;
    mgr->dnstimeout = MG_DEFAULT_DNS_TIMEOUT_MS;
    mgr->dns4.url = MG_DEFAULT_DNS4_URL;
    mgr->dns6.url = MG_DEFAULT_DNS6_URL;
    tls_ctx_init(mgr);
    MG_DEBUG(("MG_IO_SIZE: %lu, TLS: %s", MG_IO_SIZE,
              MG_TLS == MG_TLS_NONE          ? "none"
                  : MG_TLS == MG_TLS_MBED    ? "MbedTLS"
                  : MG_TLS == MG_TLS_OPENSSL ? "OpenSSL"
                  : MG_TLS == MG_TLS_BUILTIN ? "builtin"
                  : MG_TLS == MG_TLS_WOLFSSL ? "WolfSSL"
                                             : "custom"));
}

// -- Modern C++ API: Connection methods --

bool Connection::send_bytes(std::string_view data)
{
    return send_data(this, data.data(), data.size());
}

size_t Connection::write_fmt(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t len = conn_vprintf(this, fmt, &ap);
    va_end(ap);
    return len;
}

// -- Modern C++ API: Manager methods --

ConnectionRef Manager::http_listen(std::string_view url, HandlerFn handler)
{
    std::string url_z(url);
    return ConnectionRef(nanosrv::http_listen(*this, url_z.c_str(), std::move(handler)));
}

ConnectionRef Manager::connect(std::string_view url, HandlerFn handler)
{
    std::string url_z(url);
    auto* fn = new HandlerFn(std::move(handler));
    auto trampoline = [](struct Connection* c, int ev, void* ev_data) {
        auto* h = static_cast<HandlerFn*>(c->fn_data);
        (*h)(*c, static_cast<Event>(ev), ev_data);
        if (ev == MG_EV_CLOSE) {
            delete h;
            c->fn_data = nullptr;
        }
    };
    auto* c = nanosrv::connect(raw(), url_z.c_str(), trampoline, fn);
    if (c == nullptr)
        delete fn;
    return ConnectionRef(c);
}

size_t ConnectionRef::write_fmt(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    size_t n = conn_vprintf(c_, fmt, &ap);
    va_end(ap);
    return n;
}

void Manager::wakeup(unsigned long conn_id, std::string_view data)
{
    nanosrv::wakeup(raw(), conn_id, data.data(), data.size());
}

} // namespace nanosrv
