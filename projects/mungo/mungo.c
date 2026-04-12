// nanosrv.c -- clean extraction of a subset of Mongoose 7.21
// Extracted modules: base64, dns, event, fmt, http, iobuf, json, log,
//                    net, printf, sock, str, timer, tls_dummy, url, util

#include "mungo.h"

// ---- module: base64 ----

static int mg_base64_encode_single(int c)
{
    if (c < 26) {
        return c + 'A';
    } else if (c < 52) {
        return c - 26 + 'a';
    } else if (c < 62) {
        return c - 52 + '0';
    } else {
        return c == 62 ? '+' : '/';
    }
}

static int mg_base64_decode_single(int c)
{
    if (c >= 'A' && c <= 'Z') {
        return c - 'A';
    } else if (c >= 'a' && c <= 'z') {
        return c + 26 - 'a';
    } else if (c >= '0' && c <= '9') {
        return c + 52 - '0';
    } else if (c == '+') {
        return 62;
    } else if (c == '/') {
        return 63;
    } else if (c == '=') {
        return 64;
    } else {
        return -1;
    }
}

size_t mg_base64_update(unsigned char ch, char* to, size_t n)
{
    unsigned long rem = (n & 3) % 3;
    if (rem == 0) {
        to[n] = (char)mg_base64_encode_single(ch >> 2);
        to[++n] = (char)((ch & 3) << 4);
    } else if (rem == 1) {
        to[n] = (char)mg_base64_encode_single(to[n] | (ch >> 4));
        to[++n] = (char)((ch & 15) << 2);
    } else {
        to[n] = (char)mg_base64_encode_single(to[n] | (ch >> 6));
        to[++n] = (char)mg_base64_encode_single(ch & 63);
        n++;
    }
    return n;
}

size_t mg_base64_final(char* to, size_t n)
{
    size_t saved = n;
    // printf("---[%.*s]\n", n, to);
    if (n & 3)
        n = mg_base64_update(0, to, n);
    if ((saved & 3) == 2)
        n--;
    // printf("    %d[%.*s]\n", n, n, to);
    while (n & 3)
        to[n++] = '=';
    to[n] = '\0';
    return n;
}

size_t mg_base64_encode(const unsigned char* p, size_t n, char* to, size_t dl)
{
    size_t i, len = 0;
    if (dl > 0)
        to[0] = '\0';
    if (dl < ((n / 3) + (n % 3 ? 1 : 0)) * 4 + 1)
        return 0;
    for (i = 0; i < n; i++)
        len = mg_base64_update(p[i], to, len);
    len = mg_base64_final(to, len);
    return len;
}

size_t mg_base64_decode(const char* src, size_t n, char* dst, size_t dl)
{
    const char* end = src == NULL ? NULL : src + n; // Cannot add to NULL
    size_t len = 0;
    if (dl < n / 4 * 3 + 1)
        goto fail;
    while (src != NULL && src + 3 < end) {
        int a = mg_base64_decode_single(src[0]),
            b = mg_base64_decode_single(src[1]),
            c = mg_base64_decode_single(src[2]),
            d = mg_base64_decode_single(src[3]);
        if (a == 64 || a < 0 || b == 64 || b < 0 || c < 0 || d < 0) {
            goto fail;
        }
        dst[len++] = (char)((a << 2) | (b >> 4));
        if (src[2] != '=') {
            dst[len++] = (char)((b << 4) | (c >> 2));
            if (src[3] != '=')
                dst[len++] = (char)((c << 6) | d);
        }
        src += 4;
    }
    dst[len] = '\0';
    return len;
fail:
    if (dl > 0)
        dst[0] = '\0';
    return 0;
}

// ---- module: dns ----

struct dns_data {
    struct dns_data* next;
    struct mg_connection* c;
    uint64_t expire;
    uint16_t txnid;
};

static void mg_sendnsreq(struct mg_connection*, struct mg_str*, int,
                         struct mg_dns*, bool);

static void mg_dns_free(struct dns_data** head, struct dns_data* d)
{
    LIST_DELETE(struct dns_data, head, d);
    mg_free(d);
}

void mg_resolve_cancel(struct mg_connection* c)
{
    struct dns_data *tmp, *d;
    struct dns_data** head = (struct dns_data**)&c->mgr->active_dns_requests;
    for (d = *head; d != NULL; d = tmp) {
        tmp = d->next;
        if (d->c == c)
            mg_dns_free(head, d);
    }
}

static size_t mg_dns_parse_name_depth(const uint8_t* s, size_t len, size_t ofs,
                                      char* to, size_t tolen, size_t j,
                                      int depth)
{
    size_t i = 0;
    if (tolen > 0 && depth == 0)
        to[0] = '\0';
    if (depth > 5)
        return 0;
    // MG_INFO(("ofs %lx %x %x", (unsigned long) ofs, s[ofs], s[ofs + 1]));
    while (ofs + i + 1 < len) {
        size_t n = s[ofs + i];
        if (n == 0) {
            i++;
            break;
        }
        if (n & 0xc0) {
            size_t ptr = (((n & 0x3f) << 8) | s[ofs + i + 1]); // 12 is hdr len
            // MG_INFO(("PTR %lx", (unsigned long) ptr));
            if (ptr + 1 < len && (s[ptr] & 0xc0) == 0
                && mg_dns_parse_name_depth(s, len, ptr, to, tolen, j,
                                           depth + 1)
                    == 0)
                return 0;
            i += 2;
            break;
        }
        if (ofs + i + n + 1 >= len)
            return 0;
        if (j > 0) {
            if (j < tolen)
                to[j] = '.';
            j++;
        }
        if (j + n < tolen)
            memcpy(&to[j], &s[ofs + i + 1], n);
        j += n;
        i += n + 1;
        if (j < tolen)
            to[j] = '\0'; // Zero-terminate this chunk
                          // MG_INFO(("--> [%s]", to));
    }
    if (tolen > 0)
        to[tolen - 1] = '\0'; // Make sure it is nul-term
    return i;
}

static size_t mg_dns_parse_name(const uint8_t* s, size_t n, size_t ofs,
                                char* dst, size_t dstlen)
{
    return mg_dns_parse_name_depth(s, n, ofs, dst, dstlen, 0, 0);
}

size_t mg_dns_parse_rr(const uint8_t* buf, size_t len, size_t ofs,
                       bool is_question, struct mg_dns_rr* rr)
{
    const uint8_t *s = buf + ofs, *e = &buf[len];

    memset(rr, 0, sizeof(*rr));
    if (len < sizeof(struct mg_dns_header))
        return 0; // Too small
    if (len > 512)
        return 0; //  Too large, we don't expect that
    if (s >= e)
        return 0; //  Overflow

    if ((rr->nlen = (uint16_t)mg_dns_parse_name(buf, len, ofs, NULL, 0)) == 0)
        return 0;
    s += rr->nlen + 4;
    if (s > e)
        return 0;
    rr->atype = (uint16_t)(((uint16_t)s[-4] << 8) | s[-3]);
    rr->aclass = (uint16_t)(((uint16_t)s[-2] << 8) | s[-1]);
    if (is_question)
        return (size_t)(rr->nlen + 4);

    s += 6;
    if (s > e)
        return 0;
    rr->alen = (uint16_t)(((uint16_t)s[-2] << 8) | s[-1]);
    if (s + rr->alen > e)
        return 0;
    return (size_t)(rr->nlen + rr->alen + 10);
}

bool mg_dns_parse(const uint8_t* buf, size_t len, struct mg_dns_message* dm)
{
    const struct mg_dns_header* h = (struct mg_dns_header*)buf;
    struct mg_dns_rr rr;
    size_t i, n, num_answers, ofs = sizeof(*h);
    bool is_response;
    memset(dm, 0, sizeof(*dm));

    if (len < sizeof(*h))
        return 0; // Too small, headers dont fit
    if (mg_ntohs(h->num_questions) > 1)
        return 0; // Sanity
    num_answers = mg_ntohs(h->num_answers);
    if (num_answers > 10) {
        MG_DEBUG(("Got %u answers, ignoring beyond 10th one", num_answers));
        num_answers = 10; // Sanity cap
    }
    dm->txnid = mg_ntohs(h->txnid);
    is_response = mg_ntohs(h->flags) & 0x8000;

    for (i = 0; i < mg_ntohs(h->num_questions); i++) {
        if ((n = mg_dns_parse_rr(buf, len, ofs, true, &rr)) == 0)
            return false;
        // MG_INFO(("Q %lu %lu %hu/%hu", ofs, n, rr.atype, rr.aclass));
        mg_dns_parse_name(buf, len, ofs, dm->name, sizeof(dm->name));
        ofs += n;
    }

    if (!is_response) {
        // For queries, there is no need to parse the answers. In this way,
        // we also ensure the domain name (dm->name) is parsed from
        // the question field.
        return true;
    }

    for (i = 0; i < num_answers; i++) {
        if ((n = mg_dns_parse_rr(buf, len, ofs, false, &rr)) == 0)
            return false;
        // MG_INFO(("A -- %lu %lu %hu/%hu %s", ofs, n, rr.atype, rr.aclass,
        // dm->name));
        mg_dns_parse_name(buf, len, ofs, dm->name, sizeof(dm->name));
        ofs += n;

        if (rr.alen == 4 && rr.atype == MG_DNS_RTYPE_A && rr.aclass == 1) {
            dm->addr.is_ip6 = false;
            memcpy(&dm->addr.addr.ip, &buf[ofs - 4], 4);
            dm->resolved = true;
            break; // Return success
        } else if (rr.alen == 16 && rr.atype == MG_DNS_RTYPE_AAAA
                   && rr.aclass == 1) {
            dm->addr.is_ip6 = true;
            memcpy(&dm->addr.addr.ip, &buf[ofs - 16], 16);
            dm->resolved = true;
            break; // Return success
        }
    }
    return true;
}

static void dns_cb(struct mg_connection* c, int ev, void* ev_data)
{
    struct dns_data *d, *tmp;
    struct dns_data** head = (struct dns_data**)&c->mgr->active_dns_requests;
    if (ev == MG_EV_POLL) {
        uint64_t now = *(uint64_t*)ev_data;
        for (d = *head; d != NULL; d = tmp) {
            tmp = d->next;
            // MG_DEBUG ("%lu %lu dns poll", d->expire, now));
            if (now > d->expire)
                mg_error(d->c, "DNS timeout");
        }
    } else if (ev == MG_EV_READ) {
        struct mg_dns_message dm;
        int resolved = 0;
        if (mg_dns_parse(c->recv.buf, c->recv.len, &dm) == false) {
            MG_ERROR(("Unexpected DNS response:"));
            mg_hexdump(c->recv.buf, c->recv.len);
        } else {
            // MG_VERBOSE(("%s %d", dm.name, dm.resolved));
            for (d = *head; d != NULL; d = tmp) {
                tmp = d->next;
                // MG_INFO(("d %p %hu %hu", d, d->txnid, dm.txnid));
                if (dm.txnid != d->txnid)
                    continue;
                if (d->c->is_resolving) {
                    if (dm.resolved) {
                        dm.addr.port = d->c->rem.port; // Save port
                        d->c->rem = dm.addr;           // Copy resolved address
                        MG_DEBUG(("%lu %s is %M", d->c->id, dm.name,
                                  mg_print_ip, &d->c->rem));
                        mg_connect_resolved(d->c);
#if MG_ENABLE_IPV6
                    } else if (dm.addr.is_ip6 == false && dm.name[0] != '\0'
                               && c->mgr->use_dns6 == false) {
                        struct mg_str x = mg_str(dm.name);
                        mg_sendnsreq(d->c, &x, c->mgr->dnstimeout,
                                     &c->mgr->dns6, true);
#endif
                    } else {
                        mg_error(d->c, "%s DNS lookup failed", dm.name);
                    }
                } else {
                    MG_ERROR(("%lu already resolved", d->c->id));
                }
                mg_dns_free(head, d);
                resolved = 1;
            }
        }
        if (!resolved)
            MG_ERROR(("stray DNS reply"));
        c->recv.len = 0;
    } else if (ev == MG_EV_CLOSE) {
        for (d = *head; d != NULL; d = tmp) {
            tmp = d->next;
            mg_error(d->c, "DNS error");
            mg_dns_free(head, d);
        }
    }
}

static bool mg_dns_send(struct mg_connection* c, const struct mg_str* name,
                        uint16_t txnid, bool ipv6)
{
    struct {
        struct mg_dns_header header;
        uint8_t data[256];
    } pkt;

    size_t i, n;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.txnid = mg_htons(txnid);
    pkt.header.flags = mg_htons(0x100);
    pkt.header.num_questions = mg_htons(1);
    for (i = n = 0; i < sizeof(pkt.data) - 5; i++) {
        if (name->buf[i] == '.' || i >= name->len) {
            pkt.data[n] = (uint8_t)(i - n);
            memcpy(&pkt.data[n + 1], name->buf + n, i - n);
            n = i + 1;
        }
        if (i >= name->len)
            break;
    }
    memcpy(&pkt.data[n], "\x00\x00\x01\x00\x01", 5); // A query
    n += 5;
    if (ipv6)
        pkt.data[n - 3] = 0x1c; // AAAA query
    // memcpy(&pkt.data[n], "\xc0\x0c\x00\x1c\x00\x01", 6);  // AAAA query
    // n += 6;
    return mg_send(c, &pkt, sizeof(pkt.header) + n);
}

bool mg_dnsc_init(struct mg_mgr* mgr, struct mg_dns* dnsc);

bool mg_dnsc_init(struct mg_mgr* mgr, struct mg_dns* dnsc)
{
    if (dnsc->url == NULL) {
        mg_error(0, "DNS server URL is NULL. Call mg_mgr_init()");
        return false;
    }
    if (dnsc->c == NULL) {
        dnsc->c = mg_connect(mgr, dnsc->url, NULL, NULL);
        if (dnsc->c == NULL)
            return false;
        dnsc->c->pfn = dns_cb;
    }
    return true;
}

static void mg_sendnsreq(struct mg_connection* c, struct mg_str* name, int ms,
                         struct mg_dns* dnsc, bool ipv6)
{
    struct dns_data* d = NULL;
    if (!mg_dnsc_init(c->mgr, dnsc)) {
        mg_error(c, "resolver");
    } else if ((d = (struct dns_data*)mg_calloc(1, sizeof(*d))) == NULL) {
        mg_error(c, "resolve OOM");
    } else {
        struct dns_data* reqs = (struct dns_data*)c->mgr->active_dns_requests;
        uint16_t id;
        mg_random(&id, sizeof(uint16_t));
        // TODO(): traverse reqs and check id != reqs->txnid; repeat otherwise
        if (reqs != NULL)
            id = (uint16_t)(reqs->txnid + 1); // no collision
        d->txnid = id;
        d->next = reqs;
        c->mgr->active_dns_requests = d;
        d->expire = mg_millis() + (uint64_t)ms;
        d->c = c;
        c->is_resolving = 1;
        MG_VERBOSE(("%lu resolving %.*s @ %s, txnid %hu", c->id,
                    (int)name->len, name->buf, dnsc->url, d->txnid));
        if (!mg_dns_send(dnsc->c, name, d->txnid, ipv6)) {
            mg_error(dnsc->c, "DNS send");
        }
    }
}

void mg_resolve(struct mg_connection* c, const char* url)
{
    struct mg_str host = mg_url_host(url);
    c->rem.port = mg_htons(mg_url_port(url));
    if (mg_aton(host, &c->rem)) {
        // host is an IP address, do not fire name resolution
        mg_connect_resolved(c);
    } else {
        // host is not an IP, send DNS resolution request
        struct mg_dns* dns = c->mgr->use_dns6 ? &c->mgr->dns6 : &c->mgr->dns4;
        mg_sendnsreq(c, &host, c->mgr->dnstimeout, dns, c->mgr->use_dns6);
    }
}

// ---- module: event ----

void mg_call(struct mg_connection* c, int ev, void* ev_data)
{
#if MG_ENABLE_PROFILE
    const char* names[] = { "EV_ERROR",     "EV_OPEN",     "EV_POLL",
                            "EV_RESOLVE",   "EV_CONNECT",  "EV_ACCEPT",
                            "EV_TLS_HS",    "EV_READ",     "EV_WRITE",
                            "EV_CLOSE",     "EV_HTTP_MSG", "EV_HTTP_CHUNK",
                            "EV_WS_OPEN",   "EV_WS_MSG",   "EV_WS_CTL",
                            "EV_MQTT_CMD",  "EV_MQTT_MSG", "EV_MQTT_OPEN",
                            "EV_SNTP_TIME", "EV_USER" };
    if (ev != MG_EV_POLL && ev < (int)(sizeof(names) / sizeof(names[0]))) {
        MG_PROF_ADD(c, names[ev]);
    }
#endif
    // Fire protocol handler first, user handler second. See #2559
    if (c->pfn != NULL)
        c->pfn(c, ev, ev_data);
    if (c->fn != NULL)
        c->fn(c, ev, ev_data);
}

void mg_error(struct mg_connection* c, const char* fmt, ...)
{
    char buf[64];
    va_list ap;
    va_start(ap, fmt);
    mg_vsnprintf(buf, sizeof(buf), fmt, &ap);
    va_end(ap);
    MG_ERROR(("%lu %ld %s", c->id, c->fd, buf));
    c->is_closing = 1;            // Set is_closing before sending MG_EV_CALL
    mg_call(c, MG_EV_ERROR, buf); // Let user handler override it
}

// ---- module: fmt ----

static bool is_digit(int c) { return c >= '0' && c <= '9'; }

static int addexp(char* buf, int e, int sign)
{
    int n = 0;
    buf[n++] = 'e';
    buf[n++] = (char)sign;
    if (e > 400)
        return 0;
    if (e < 10)
        buf[n++] = '0';
    if (e >= 100)
        buf[n++] = (char)(e / 100 + '0'), e -= 100 * (e / 100);
    if (e >= 10)
        buf[n++] = (char)(e / 10 + '0'), e -= 10 * (e / 10);
    buf[n++] = (char)(e + '0');
    return n;
}

static int xisinf(double x)
{
    union {
        double f;
        uint64_t u;
    } ieee754;

    ieee754.f = x;
    return ((unsigned)(ieee754.u >> 32) & 0x7fffffff) == 0x7ff00000
        && ((unsigned)ieee754.u == 0);
}

static int xisnan(double x)
{
    union {
        double f;
        uint64_t u;
    } ieee754;

    ieee754.f = x;
    return ((unsigned)(ieee754.u >> 32) & 0x7fffffff)
        + ((unsigned)ieee754.u != 0)
        > 0x7ff00000;
}

static size_t mg_dtoa(char* dst, size_t dstlen, double d, int width, bool tz)
{
    char buf[40];
    int i, s = 0, n = 0, e = 0;
    double t, mul, saved;
    if (d == 0.0)
        return mg_snprintf(dst, dstlen, "%s", "0");
    if (xisinf(d))
        return mg_snprintf(dst, dstlen, "%s", d > 0 ? "inf" : "-inf");
    if (xisnan(d))
        return mg_snprintf(dst, dstlen, "%s", "nan");
    if (d < 0.0)
        d = -d, buf[s++] = '-';

    // Round
    saved = d;
    if (tz) {
        mul = 1.0;
        while (d >= 10.0 && d / mul >= 10.0)
            mul *= 10.0;
    } else {
        mul = 0.1;
    }

    while (d <= 1.0 && d / mul <= 1.0)
        mul /= 10.0;
    for (i = 0, t = mul * 5; i < width; i++)
        t /= 10.0;

    d += t;

    // Calculate exponent, and 'mul' for scientific representation
    mul = 1.0;
    while (d >= 10.0 && d / mul >= 10.0)
        mul *= 10.0, e++;
    while (d < 1.0 && d / mul < 1.0)
        mul /= 10.0, e--;
    // printf(" --> %g %d %g %g\n", saved, e, t, mul);

    if (tz && e >= width && width > 1) {
        n = (int)mg_dtoa(buf, sizeof(buf), saved / mul, width, tz);
        // printf(" --> %.*g %d [%.*s]\n", 10, d / t, e, n, buf);
        n += addexp(buf + s + n, e, '+');
        return mg_snprintf(dst, dstlen, "%.*s", n, buf);
    } else if (tz && e <= -width && width > 1) {
        n = (int)mg_dtoa(buf, sizeof(buf), saved / mul, width, tz);
        // printf(" --> %.*g %d [%.*s]\n", 10, d / mul, e, n, buf);
        n += addexp(buf + s + n, -e, '-');
        return mg_snprintf(dst, dstlen, "%.*s", n, buf);
    } else {
        int targ_width = width;
        for (i = 0, t = mul; t >= 1.0 && s + n < (int)sizeof(buf); i++) {
            int ch = (int)(d / t);
            if (n > 0 || ch > 0)
                buf[s + n++] = (char)(ch + '0');
            d -= ch * t;
            t /= 10.0;
        }
        // printf(" --> [%g] -> %g %g (%d) [%.*s]\n", saved, d, t, n, s + n,
        // buf);
        if (n == 0)
            buf[s++] = '0';
        while (t >= 1.0 && n + s < (int)sizeof(buf))
            buf[n++] = '0', t /= 10.0;
        if (s + n < (int)sizeof(buf))
            buf[n + s++] = '.';
        // printf(" 1--> [%g] -> [%.*s]\n", saved, s + n, buf);
        if (!tz && n > 0)
            targ_width = width + n;
        for (i = 0, t = 0.1; s + n < (int)sizeof(buf) && n < targ_width; i++) {
            int ch = (int)(d / t);
            buf[s + n++] = (char)(ch + '0');
            d -= ch * t;
            t /= 10.0;
        }
    }

    while (tz && n > 0 && buf[s + n - 1] == '0')
        n--; // Trim trailing zeroes
    if (tz && n > 0 && buf[s + n - 1] == '.')
        n--; // Trim trailing dot
    n += s;
    if (n >= (int)sizeof(buf))
        n = (int)sizeof(buf) - 1;
    buf[n] = '\0';
    return mg_snprintf(dst, dstlen, "%s", buf);
}

static size_t mg_lld(char* buf, int64_t val, bool is_signed, bool is_hex)
{
    const char* letters = "0123456789abcdef";
    uint64_t v = (uint64_t)val;
    size_t s = 0, n, i;
    if (is_signed && val < 0)
        buf[s++] = '-', v = (uint64_t)(-val);
    // This loop prints a number in reverse order. I guess this is because we
    // write numbers from right to left: least significant digit comes last.
    // Maybe because we use Arabic numbers, and Arabs write RTL?
    if (is_hex) {
        for (n = 0; v; v >>= 4)
            buf[s + n++] = letters[v & 15];
    } else {
        for (n = 0; v; v /= 10)
            buf[s + n++] = letters[v % 10];
    }
    // Reverse a string
    for (i = 0; i < n / 2; i++) {
        char t = buf[s + i];
        buf[s + i] = buf[s + n - i - 1], buf[s + n - i - 1] = t;
    }
    if (val == 0)
        buf[n++] = '0'; // Handle special case
    return n + s;
}

static size_t scpy(void (*out)(char, void*), void* ptr, char* buf, size_t len)
{
    size_t i = 0;
    while (i < len && buf[i] != '\0')
        out(buf[i++], ptr);
    return i;
}

size_t mg_xprintf(void (*out)(char, void*), void* ptr, const char* fmt, ...)
{
    size_t len = 0;
    va_list ap;
    va_start(ap, fmt);
    len = mg_vxprintf(out, ptr, fmt, &ap);
    va_end(ap);
    return len;
}

size_t mg_vxprintf(void (*out)(char, void*), void* param, const char* fmt,
                   va_list* ap)
{
    size_t i = 0, n = 0;
    while (fmt[i] != '\0') {
        if (fmt[i] == '%') {
            size_t j, k, x = 0, is_long = 0, w = 0 /* width */,
                         pr = ~0U /* prec */;
            char pad = ' ', minus = 0, c = fmt[++i];
            if (c == '#')
                x++, c = fmt[++i];
            if (c == '-')
                minus++, c = fmt[++i];
            if (c == '0')
                pad = '0', c = fmt[++i];
            while (is_digit(c))
                w *= 10, w += (size_t)(c - '0'), c = fmt[++i];
            if (c == '.') {
                c = fmt[++i];
                if (c == '*') {
                    pr = (size_t)va_arg(*ap, int);
                    c = fmt[++i];
                } else {
                    pr = 0;
                    while (is_digit(c))
                        pr *= 10, pr += (size_t)(c - '0'), c = fmt[++i];
                }
            }
            while (c == 'h')
                c = fmt[++i]; // Treat h and hh as int
            if (c == 'l') {
                is_long++, c = fmt[++i];
                if (c == 'l')
                    is_long++, c = fmt[++i];
            }
            if (c == 'p')
                x = 1, is_long = 1;
            if (c == 'd' || c == 'u' || c == 'x' || c == 'X' || c == 'p'
                || c == 'g' || c == 'f') {
                bool s = (c == 'd'), h = (c == 'x' || c == 'X' || c == 'p');
                char tmp[40];
                size_t xl = x ? 2 : 0;
                if (c == 'g' || c == 'f') {
                    double v = va_arg(*ap, double);
                    if (pr == ~0U)
                        pr = 6;
                    k = mg_dtoa(tmp, sizeof(tmp), v, (int)pr, c == 'g');
                } else if (is_long == 2) {
                    int64_t v = va_arg(*ap, int64_t);
                    k = mg_lld(tmp, v, s, h);
                } else if (is_long == 1) {
                    long v = va_arg(*ap, long);
                    k = mg_lld(tmp, s ? (int64_t)v : (int64_t)(unsigned long)v,
                               s, h);
                } else {
                    int v = va_arg(*ap, int);
                    k = mg_lld(tmp, s ? (int64_t)v : (int64_t)(unsigned)v, s,
                               h);
                }
                for (j = 0; j < xl && w > 0; j++)
                    w--;
                for (j = 0; pad == ' ' && !minus && k < w && j + k < w; j++)
                    n += scpy(out, param, &pad, 1);
                n += scpy(out, param, (char*)"0x", xl);
                for (j = 0; pad == '0' && k < w && j + k < w; j++)
                    n += scpy(out, param, &pad, 1);
                n += scpy(out, param, tmp, k);
                for (j = 0; pad == ' ' && minus && k < w && j + k < w; j++)
                    n += scpy(out, param, &pad, 1);
            } else if (c == 'm' || c == 'M') {
                mg_pm_t f = va_arg(*ap, mg_pm_t);
                if (c == 'm')
                    out('"', param);
                n += f(out, param, ap);
                if (c == 'm')
                    n += 2, out('"', param);
            } else if (c == 'c') {
                int ch = va_arg(*ap, int);
                out((char)ch, param);
                n++;
            } else if (c == 's') {
                char* p = va_arg(*ap, char*);
                if (pr == ~0U)
                    pr = p == NULL ? 0 : strlen(p);
                for (j = 0; !minus && pr < w && j + pr < w; j++)
                    n += scpy(out, param, &pad, 1);
                n += scpy(out, param, p, pr);
                for (j = 0; minus && pr < w && j + pr < w; j++)
                    n += scpy(out, param, &pad, 1);
            } else if (c == '%') {
                out('%', param);
                n++;
            } else {
                out('%', param);
                out(c, param);
                n += 2;
            }
            i++;
        } else {
            out(fmt[i], param), n++, i++;
        }
    }
    return n;
}

// ---- module: http ----

static int mg_ncasecmp(const char* s1, const char* s2, size_t len)
{
    int diff = 0;
    if (len > 0)
        do {
            int c = *s1++, d = *s2++;
            if (c >= 'A' && c <= 'Z')
                c += 'a' - 'A';
            if (d >= 'A' && d <= 'Z')
                d += 'a' - 'A';
            diff = c - d;
        } while (diff == 0 && s1[-1] != '\0' && --len > 0);
    return diff;
}

bool mg_to_size_t(struct mg_str str, size_t* val);

bool mg_to_size_t(struct mg_str str, size_t* val)
{
    size_t i = 0, max = (size_t)-1, max2 = max / 10, result = 0, ndigits = 0;
    while (i < str.len && (str.buf[i] == ' ' || str.buf[i] == '\t'))
        i++;
    if (i < str.len && str.buf[i] == '-')
        return false;
    while (i < str.len && str.buf[i] >= '0' && str.buf[i] <= '9') {
        size_t digit = (size_t)(str.buf[i] - '0');
        if (result > max2)
            return false; // Overflow
        result *= 10;
        if (result > max - digit)
            return false; // Overflow
        result += digit;
        i++, ndigits++;
    }
    while (i < str.len && (str.buf[i] == ' ' || str.buf[i] == '\t'))
        i++;
    if (ndigits == 0)
        return false; // #2322: Content-Length = 1 * DIGIT
    if (i != str.len)
        return false; // Ditto
    *val = (size_t)result;
    return true;
}

// Chunk deletion marker is the MSB in the "processed" counter
#define MG_DMARK ((size_t)1 << (sizeof(size_t) * 8 - 1))

// Multipart POST example:
// --xyz
// Content-Disposition: form-data; name="val"
//
// abcdef
// --xyz
// Content-Disposition: form-data; name="foo"; filename="a.txt"
// Content-Type: text/plain
//
// hello world
//
// --xyz--
size_t mg_http_next_multipart(struct mg_str body, size_t ofs,
                              struct mg_http_part* part)
{
    struct mg_str cd = mg_str_n("Content-Disposition", 19);
    const char* s = body.buf;
    size_t b = ofs, h1, h2, b1, b2, max = body.len;

    // Init part params
    if (part != NULL)
        part->name = part->filename = part->body = mg_str_n(0, 0);

    // Skip boundary
    while (b + 2 < max && s[b] != '\r' && s[b + 1] != '\n')
        b++;
    if (b <= ofs || b + 2 >= max)
        return 0;
    // MG_INFO(("B: %zu %zu [%.*s]", ofs, b - ofs, (int) (b - ofs), s));

    // Skip headers
    h1 = h2 = b + 2;
    for (;;) {
        while (h2 + 2 < max && s[h2] != '\r' && s[h2 + 1] != '\n')
            h2++;
        if (h2 == h1)
            break;
        if (h2 + 2 >= max)
            return 0;
        // MG_INFO(("Header: [%.*s]", (int) (h2 - h1), &s[h1]));
        if (part != NULL && h1 + cd.len + 2 < h2 && s[h1 + cd.len] == ':'
            && mg_ncasecmp(&s[h1], cd.buf, cd.len) == 0) {
            struct mg_str v = mg_str_n(&s[h1 + cd.len + 2],
                                       h2 - (h1 + cd.len + 2));
            part->name = mg_http_get_header_var(v, mg_str_n("name", 4));
            part->filename = mg_http_get_header_var(v,
                                                    mg_str_n("filename", 8));
        }
        h1 = h2 = h2 + 2;
    }
    b1 = b2 = h2 + 2;
    while (b2 + 2 + (b - ofs) + 2 < max
           && !(s[b2] == '\r' && s[b2 + 1] == '\n'
                && memcmp(&s[b2 + 2], s, b - ofs) == 0))
        b2++;

    if (b2 + 2 >= max)
        return 0;
    if (part != NULL)
        part->body = mg_str_n(&s[b1], b2 - b1);
    // MG_INFO(("Body: [%.*s]", (int) (b2 - b1), &s[b1]));
    return b2 + 2;
}

void mg_http_bauth(struct mg_connection* c, const char* user, const char* pass)
{
    struct mg_str u = mg_str(user), p = mg_str(pass);
    size_t need = c->send.len + 36 + (u.len + p.len) * 2;
    if (c->send.size < need)
        mg_iobuf_resize(&c->send, need);
    if (c->send.size >= need) {
        size_t i, n = 0;
        char* buf = (char*)&c->send.buf[c->send.len];
        memcpy(buf, "Authorization: Basic ", 21); // DON'T use mg_send!
        for (i = 0; i < u.len; i++) {
            n = mg_base64_update(((unsigned char*)u.buf)[i], buf + 21, n);
        }
        if (p.len > 0) {
            n = mg_base64_update(':', buf + 21, n);
            for (i = 0; i < p.len; i++) {
                n = mg_base64_update(((unsigned char*)p.buf)[i], buf + 21, n);
            }
        }
        n = mg_base64_final(buf + 21, n);
        c->send.len += 21 + (size_t)n + 2;
        memcpy(&c->send.buf[c->send.len - 2], "\r\n", 2);
    } else {
        MG_ERROR(("%lu oom %d->%d ", c->id, (int)c->send.size, (int)need));
    }
}

struct mg_str mg_http_var(struct mg_str buf, struct mg_str name)
{
    struct mg_str entry, k, v, result = mg_str_n(NULL, 0);
    while (mg_span(buf, &entry, &buf, '&')) {
        if (mg_span(entry, &k, &v, '=') && name.len == k.len
            && mg_ncasecmp(name.buf, k.buf, k.len) == 0) {
            result = v;
            break;
        }
    }
    return result;
}

int mg_http_get_var(const struct mg_str* buf, const char* name, char* dst,
                    size_t dst_len)
{
    int len;
    if (dst != NULL && dst_len > 0) {
        dst[0] = '\0'; // If destination buffer is valid, always nul-terminate
                       // it
    }
    if (dst == NULL || dst_len == 0) {
        len = -2; // Bad destination
    } else if (buf->buf == NULL || name == NULL || buf->len == 0) {
        len = -1; // Bad source
    } else {
        struct mg_str v = mg_http_var(*buf, mg_str(name));
        if (v.buf == NULL) {
            len = -4; // Name does not exist
        } else {
            len = mg_url_decode(v.buf, v.len, dst, dst_len, 1);
            if (len < 0)
                len = -3; // Failed to decode
        }
    }
    return len;
}

static bool isx(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

int mg_url_decode(const char* src, size_t src_len, char* dst, size_t dst_len,
                  int is_form_url_encoded)
{
    size_t i, j;
    for (i = j = 0; i < src_len && j + 1 < dst_len; i++, j++) {
        if (src[i] == '%') {
            // Use `i + 2 < src_len`, not `i < src_len - 2`, note small src_len
            if (i + 2 < src_len && isx(src[i + 1]) && isx(src[i + 2])) {
                mg_str_to_num(mg_str_n(src + i + 1, 2), 16, &dst[j],
                              sizeof(uint8_t));
                i += 2;
            } else {
                return -1;
            }
        } else if (is_form_url_encoded && src[i] == '+') {
            dst[j] = ' ';
        } else {
            dst[j] = src[i];
        }
    }
    if (j < dst_len)
        dst[j] = '\0'; // Null-terminate the destination
    return i >= src_len && j < dst_len ? (int)j : -1;
}

static bool isok(uint8_t c)
{
    return c == '\n' || c == '\r' || c == '\t' || c >= ' ';
}

int mg_http_get_request_len(const unsigned char* buf, size_t buf_len)
{
    size_t i;
    for (i = 0; i < buf_len; i++) {
        if (!isok(buf[i]))
            return -1;
        if ((i > 0 && buf[i] == '\n' && buf[i - 1] == '\n')
            || (i > 3 && buf[i] == '\n' && buf[i - 1] == '\r'
                && buf[i - 2] == '\n'))
            return (int)i + 1;
    }
    return 0;
}

struct mg_str* mg_http_get_header(struct mg_http_message* h, const char* name)
{
    size_t i, n = strlen(name),
              max = sizeof(h->headers) / sizeof(h->headers[0]);
    for (i = 0; i < max && h->headers[i].name.len > 0; i++) {
        struct mg_str *k = &h->headers[i].name, *v = &h->headers[i].value;
        if (n == k->len && mg_ncasecmp(k->buf, name, n) == 0)
            return v;
    }
    return NULL;
}

// Is it a valid utf-8 continuation byte
static bool vcb(uint8_t c) { return (c & 0xc0) == 0x80; }

// Get character length (valid utf-8). Used to parse method, URI, headers
static size_t clen(const char* s, const char* end)
{
    const unsigned char *u = (unsigned char*)s, c = *u;
    long n = (long)(end - s);
    if (c > ' ' && c <= '~')
        return 1; // Usual ascii printed char
    if ((c & 0xe0) == 0xc0 && n > 1 && vcb(u[1]))
        return 2; // 2-byte UTF8
    if ((c & 0xf0) == 0xe0 && n > 2 && vcb(u[1]) && vcb(u[2]))
        return 3;
    if ((c & 0xf8) == 0xf0 && n > 3 && vcb(u[1]) && vcb(u[2]) && vcb(u[3]))
        return 4;
    return 0;
}

// Skip until the newline. Return advanced `s`, or NULL on error
static const char* skiptorn(const char* s, const char* end, struct mg_str* v)
{
    v->buf = (char*)s;
    while (s < end && s[0] != '\n' && s[0] != '\r')
        s++, v->len++; // To newline
    if (s >= end || (s[0] == '\r' && s[1] != '\n'))
        return NULL; // Stray \r
    if (s < end && s[0] == '\r')
        s++; // Skip \r
    if (s >= end || *s++ != '\n')
        return NULL; // Skip \n
    return s;
}

static bool mg_http_parse_headers(const char* s, const char* end,
                                  struct mg_http_header* h, size_t max_hdrs)
{
    size_t i, n;
    int cl_count = 0, te_count = 0, auth_count = 0;
    int conn_count = 0, cookie_count = 0;
    for (i = 0; i < max_hdrs; i++) {
        struct mg_str k = { NULL, 0 }, v = { NULL, 0 };
        if (s >= end)
            return false;
        if (s[0] == '\n' || (s[0] == '\r' && s[1] == '\n'))
            break;
        k.buf = (char*)s;
        while (s < end && s[0] != ':' && (n = clen(s, end)) > 0)
            s += n, k.len += n;
        if (k.len == 0)
            return false; // Empty name
        if (s >= end || clen(s, end) == 0)
            return false; // Invalid UTF-8
        if (*s++ != ':')
            return false; // Invalid, not followed by :
        // if (clen(s, end) == 0) return false;        // Invalid UTF-8
        while (s < end && (s[0] == ' ' || s[0] == '\t'))
            s++; // Skip spaces
        if ((s = skiptorn(s, end, &v)) == NULL)
            return false;
        while (v.len > 0
               && (v.buf[v.len - 1] == ' ' || v.buf[v.len - 1] == '\t')) {
            v.len--; // Trim spaces
        }
        // detect duplicated headers -> discard
        if (((mg_strcasecmp(k, mg_str("Content-Length")) == 0)
             && (++cl_count > 1))
            || ((mg_strcasecmp(k, mg_str("Transfer-Encoding")) == 0)
                && (++te_count > 1))
            || ((mg_strcasecmp(k, mg_str("Authorization")) == 0)
                && (++auth_count > 1))
            || ((mg_strcasecmp(k, mg_str("Cookie")) == 0)
                && (++cookie_count > 1))
            || ((mg_strcasecmp(k, mg_str("Connection")) == 0)
                && (++conn_count > 1)))
            return false;
        // MG_INFO(("--HH [%.*s] [%.*s]", (int) k.len, k.buf, (int) v.len,
        // v.buf));
        h[i].name = k, h[i].value = v; // Success. Assign values
    }
    return true;
}

int mg_http_parse(const char* s, size_t len, struct mg_http_message* hm)
{
    int is_response, req_len = mg_http_get_request_len((unsigned char*)s, len);
    const char *end = s == NULL ? NULL : s + req_len,
               *qs; // Cannot add to NULL
    const struct mg_str* cl;
    size_t n;
    bool version_prefix_valid;

    memset(hm, 0, sizeof(*hm));
    if (req_len <= 0)
        return req_len;

    hm->message.buf = hm->head.buf = (char*)s;
    hm->body.buf = (char*)end;
    hm->head.len = (size_t)req_len;
    hm->message.len = hm->body.len = (size_t)-1; // Set body length to infinite

    // Parse request line
    hm->method.buf = (char*)s;
    while (s < end && (n = clen(s, end)) > 0)
        s += n, hm->method.len += n;
    while (s < end && s[0] == ' ')
        s++; // Skip spaces
    hm->uri.buf = (char*)s;
    while (s < end && (n = clen(s, end)) > 0)
        s += n, hm->uri.len += n;
    while (s < end && s[0] == ' ')
        s++; // Skip spaces
    is_response = hm->method.len > 5
        && (mg_ncasecmp(hm->method.buf, "HTTP/", 5) == 0);
    if ((s = skiptorn(s, end, &hm->proto)) == NULL)
        return false;
    // If we're given a version, check that it is HTTP/x.x
    version_prefix_valid = hm->proto.len > 5
        && (mg_ncasecmp(hm->proto.buf, "HTTP/", 5) == 0);
    if (!is_response && !version_prefix_valid)
        return -1; // no version detected in request
    if (!is_response && hm->proto.len > 0
        && (!version_prefix_valid || hm->proto.len != 8
            || (hm->proto.buf[5] < '0' || hm->proto.buf[5] > '9')
            || (hm->proto.buf[6] != '.')
            || (hm->proto.buf[7] < '0' || hm->proto.buf[7] > '9'))) {
        return -1;
    }

    // If URI contains '?' character, setup query string
    if ((qs = (const char*)memchr(hm->uri.buf, '?', hm->uri.len)) != NULL) {
        hm->query.buf = (char*)qs + 1;
        hm->query.len = (size_t)(&hm->uri.buf[hm->uri.len] - (qs + 1));
        hm->uri.len = (size_t)(qs - hm->uri.buf);
    }

    // Sanity check. Allow protocol/reason to be empty
    // Do this check after hm->method.len and hm->uri.len are finalised
    if (hm->method.len == 0 || hm->uri.len == 0)
        return -1;

    if (!mg_http_parse_headers(s, end, hm->headers,
                               sizeof(hm->headers) / sizeof(hm->headers[0])))
        return -1; // error when parsing
    if ((cl = mg_http_get_header(hm, "Content-Length")) != NULL) {
        if (mg_to_size_t(*cl, &hm->body.len) == false)
            return -1;
        hm->message.len = (size_t)req_len + hm->body.len;
    }

    // mg_http_parse() is used to parse both HTTP requests and HTTP
    // responses. If HTTP response does not have Content-Length set, then
    // body is read until socket is closed, i.e. body.len is infinite (~0).
    //
    // For HTTP requests though, if Content-Length is not specified
    // set body length to 0.
    if (hm->body.len == (size_t)~0 && !is_response) {
        hm->body.len = 0;
        hm->message.len = (size_t)req_len;
    }

    // The 204 (No content) responses also have 0 body length
    if (hm->body.len == (size_t)~0 && is_response
        && mg_strcasecmp(hm->uri, mg_str("204")) == 0) {
        hm->body.len = 0;
        hm->message.len = (size_t)req_len;
    }
    if (hm->message.len < (size_t)req_len)
        return -1; // Overflow protection

    return req_len;
}

static void mg_http_vprintf_chunk(struct mg_connection* c, const char* fmt,
                                  va_list* ap)
{
    size_t len = c->send.len;
    if (!mg_send(c, "        \r\n", 10))
        mg_error(c, "OOM");
    mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, ap);
    if (c->send.len >= len + 10) {
        mg_snprintf((char*)c->send.buf + len, 9, "%08lx",
                    c->send.len - len - 10);
        c->send.buf[len + 8] = '\r';
        if (c->send.len == len + 10)
            c->is_resp = 0; // Last chunk, reset marker
    }
    if (!mg_send(c, "\r\n", 2))
        mg_error(c, "OOM");
}

void mg_http_printf_chunk(struct mg_connection* c, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_http_vprintf_chunk(c, fmt, &ap);
    va_end(ap);
}

void mg_http_write_chunk(struct mg_connection* c, const char* buf, size_t len)
{
    mg_printf(c, "%lx\r\n", (unsigned long)len);
    if (!mg_send(c, buf, len) || !mg_send(c, "\r\n", 2))
        mg_error(c, "OOM");
    if (len == 0)
        c->is_resp = 0;
}

// clang-format off
static const char *mg_http_status_code_str(int status_code) {
  switch (status_code) {
    case 100: return "Continue";
    case 101: return "Switching Protocols";
    case 102: return "Processing";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 203: return "Non-authoritative Information";
    case 204: return "No Content";
    case 205: return "Reset Content";
    case 206: return "Partial Content";
    case 207: return "Multi-Status";
    case 208: return "Already Reported";
    case 226: return "IM Used";
    case 300: return "Multiple Choices";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 303: return "See Other";
    case 304: return "Not Modified";
    case 305: return "Use Proxy";
    case 307: return "Temporary Redirect";
    case 308: return "Permanent Redirect";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 402: return "Payment Required";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 406: return "Not Acceptable";
    case 407: return "Proxy Authentication Required";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 410: return "Gone";
    case 411: return "Length Required";
    case 412: return "Precondition Failed";
    case 413: return "Payload Too Large";
    case 414: return "Request-URI Too Long";
    case 415: return "Unsupported Media Type";
    case 416: return "Requested Range Not Satisfiable";
    case 417: return "Expectation Failed";
    case 418: return "I'm a teapot";
    case 421: return "Misdirected Request";
    case 422: return "Unprocessable Entity";
    case 423: return "Locked";
    case 424: return "Failed Dependency";
    case 426: return "Upgrade Required";
    case 428: return "Precondition Required";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 444: return "Connection Closed Without Response";
    case 451: return "Unavailable For Legal Reasons";
    case 499: return "Client Closed Request";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    case 505: return "HTTP Version Not Supported";
    case 506: return "Variant Also Negotiates";
    case 507: return "Insufficient Storage";
    case 508: return "Loop Detected";
    case 510: return "Not Extended";
    case 511: return "Network Authentication Required";
    case 599: return "Network Connect Timeout Error";
    default: return "";
  }
}

// clang-format on

void mg_http_reply(struct mg_connection* c, int code, const char* headers,
                   const char* fmt, ...)
{
    va_list ap;
    size_t len;
    mg_printf(c, "HTTP/1.1 %d %s\r\n%sContent-Length:            \r\n\r\n",
              code, mg_http_status_code_str(code),
              headers == NULL ? "" : headers);
    len = c->send.len;
    va_start(ap, fmt);
    mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, &ap);
    va_end(ap);
    if (c->send.len > 16) {
        size_t n = mg_snprintf((char*)&c->send.buf[len - 15], 11, "%-10lu",
                               (unsigned long)(c->send.len - len));
        c->send.buf[len - 15 + n] = ' '; // Change ending 0 to space
    }
    c->is_resp = 0;
}

int mg_http_status(const struct mg_http_message* hm)
{
    return atoi(hm->uri.buf);
}

static bool is_hex_digit(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')
        || (c >= 'A' && c <= 'F');
}

static int skip_chunk(const char* buf, int len, int* pl, int* dl)
{
    int i = 0, n = 0;
    if (len < 3)
        return 0;
    while (i < len && is_hex_digit(buf[i]))
        i++;
    if (i == 0)
        return -1; // Error, no length specified
    if (i > (int)sizeof(int) * 2)
        return -1; // Chunk length is too big
    if (len < i + 1 || buf[i] != '\r' || buf[i + 1] != '\n')
        return -1; // Error
    if (mg_str_to_num(mg_str_n(buf, (size_t)i), 16, &n, sizeof(int)) == false)
        return -1; // Decode chunk length, overflow
    if (n < 0)
        return -1; // Error. TODO(): some checks now redundant
    if (n > len - i - 4)
        return 0; // Chunk not yet fully buffered
    if (buf[i + n + 2] != '\r' || buf[i + n + 3] != '\n')
        return -1; // Error
    *pl = i + 2, *dl = n;
    return i + 2 + n + 2;
}

static void http_cb(struct mg_connection* c, int ev, void* ev_data)
{
    if (ev == MG_EV_READ || ev == MG_EV_CLOSE
        || (ev == MG_EV_POLL && c->is_accepted && !c->is_draining
            && c->recv.len > 0)) { // see #2796
        struct mg_http_message hm;
        size_t ofs = 0; // Parsing offset
        while (c->is_resp == 0 && ofs < c->recv.len) {
            const char* buf = (char*)c->recv.buf + ofs;
            int n = mg_http_parse(buf, c->recv.len - ofs, &hm);
            struct mg_str* te; // Transfer - encoding header
            bool is_chunked = false, is_http_1_0 = false;
            size_t old_len = c->recv.len;
            if (n < 0) {
                // We don't use mg_error() here, to avoid closing pipelined
                // requests prematurely, see #2592
                MG_ERROR(("HTTP parse, %lu bytes", c->recv.len));
                c->is_draining = 1;
                mg_hexdump(buf,
                           c->recv.len - ofs > 16 ? 16 : c->recv.len - ofs);
                c->recv.len = 0;
                return;
            }
            if (n == 0)
                break;                        // Request is not buffered yet
            mg_call(c, MG_EV_HTTP_HDRS, &hm); // Got all HTTP headers
            if (c->recv.len != old_len) {
                // User manipulated received data. Wash our hands
                MG_DEBUG(("%lu detaching HTTP handler", c->id));
                c->pfn = NULL;
                return;
            }
            if (ev == MG_EV_CLOSE) { // If client did not set Content-Length
                hm.message.len = c->recv.len
                    - ofs; // and closes now, deliver MSG
                hm.body.len = hm.message.len
                    - (size_t)(hm.body.buf - hm.message.buf);
            }
            is_http_1_0 = hm.proto.len > 8
                && mg_ncasecmp(hm.proto.buf, "HTTP/1.0", 8) == 0;
            // HTTP/1.0 does not use "Transfer-Encoding: chunked"
            if (!is_http_1_0
                && (te = mg_http_get_header(&hm, "Transfer-Encoding"))
                    != NULL) {
                if (mg_strcasecmp(*te, mg_str("chunked")) == 0) {
                    is_chunked = true;
                } else {
                    mg_error(c, "Invalid Transfer-Encoding"); // See #2460
                    return;
                }
            } else if (mg_http_get_header(&hm, "Content-length") == NULL) {
                // #2593: HTTP packets must contain either Transfer-Encoding or
                // Content-length
                bool is_response = mg_ncasecmp(hm.method.buf, "HTTP/", 5) == 0;
                bool require_content_len = false;
                if (!is_response
                    && (mg_strcasecmp(hm.method, mg_str("POST")) == 0
                        || mg_strcasecmp(hm.method, mg_str("PUT")) == 0)) {
                    // POST and PUT should include an entity body. Therefore,
                    // they should contain a Content-length header (unless the
                    // body length is 0, in which case it can be omitted).
                    // Other requests can also contain a body, but their
                    // content has no defined semantics (RFC 7231)
                    if (hm.body.len != 0)
                        require_content_len = true;
                    ofs += (size_t)n; // this request has been processed
                } else if (is_response) {
                    // HTTP spec 7.2 Entity body: All other responses must
                    // include a body or Content-Length header field defined
                    // with a value of 0.
                    int status = mg_http_status(&hm);
                    require_content_len = status >= 200 && status != 204
                        && status != 304;
                }
                if (require_content_len) {
                    if (!c->is_client)
                        mg_http_reply(c, 411, "", "");
                    MG_ERROR(("Content length missing from %s",
                              is_response ? "response" : "request"));
                }
            }

            if (is_chunked) {
                // For chunked data, strip off prefixes and suffixes from
                // chunks and relocate them right after the headers, then
                // report a message
                char* s = (char*)c->recv.buf + ofs + n;
                int o = 0, pl, dl, cl,
                    len = (int)(c->recv.len - ofs - (size_t)n);

                // Find zero-length chunk (the end of the body)
                while ((cl = skip_chunk(s + o, len - o, &pl, &dl)) > 0 && dl)
                    o += cl;
                if (cl == 0)
                    break; // No zero-len chunk, buffer more data
                if (cl < 0) {
                    mg_error(c, "Invalid chunk");
                    break;
                }

                // Zero chunk found. Second pass: strip + relocate
                o = 0, hm.body.len = 0, hm.message.len = (size_t)n;
                while ((cl = skip_chunk(s + o, len - o, &pl, &dl)) > 0) {
                    memmove(s + hm.body.len, s + o + pl, (size_t)dl);
                    o += cl, hm.body.len += (size_t)dl,
                        hm.message.len += (size_t)dl;
                    if (dl == 0)
                        break;
                }
                ofs += (size_t)(n + o);
            } else { // Normal, non-chunked data
                size_t len = c->recv.len - ofs - (size_t)n;
                if (hm.body.len > len)
                    break; // Buffer more data
                ofs += (size_t)n + hm.body.len;
            }

            if (c->is_accepted)
                c->is_resp = 1;              // Start generating response
            mg_call(c, MG_EV_HTTP_MSG, &hm); // User handler can clear is_resp
            if (c->is_accepted && !c->is_resp) {
                struct mg_str* cc = mg_http_get_header(&hm, "Connection");
                if (cc != NULL && mg_strcasecmp(*cc, mg_str("close")) == 0) {
                    c->is_draining = 1; // honor "Connection: close"
                    break;
                }
            }
        }
        if (ofs > 0)
            mg_iobuf_del(&c->recv, 0, ofs); // Delete processed data
    }
    (void)ev_data;
}

struct mg_connection* mg_http_connect(struct mg_mgr* mgr, const char* url,
                                      mg_event_handler_t fn, void* fn_data)
{
    return mg_connect_svc(mgr, url, fn, fn_data, http_cb, NULL);
}

struct mg_connection* mg_http_listen(struct mg_mgr* mgr, const char* url,
                                     mg_event_handler_t fn, void* fn_data)
{
    struct mg_connection* c = mg_listen(mgr, url, fn, fn_data);
    if (c != NULL)
        c->pfn = http_cb;
    return c;
}

static bool mg_is_url_safe(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'
        || c == '~';
}

size_t mg_url_encode(const char* s, size_t sl, char* buf, size_t len)
{
    size_t i, n = 0;
    for (i = 0; i < sl; i++) {
        int c = *(unsigned char*)&s[i];
        if (n + 4 >= len)
            return 0;
        if (mg_is_url_safe(c)) {
            buf[n++] = s[i];
        } else {
            mg_snprintf(&buf[n], 4, "%%%M", mg_print_hex, 1, &s[i]);
            n += 3;
        }
    }
    if (len > 0 && n < len - 1)
        buf[n] = '\0'; // Null-terminate the destination
    if (len > 0)
        buf[len - 1] = '\0'; // Always.
    return n;
}

void mg_http_creds(struct mg_http_message* hm, char* user, size_t userlen,
                   char* pass, size_t passlen)
{
    struct mg_str* v = mg_http_get_header(hm, "Authorization");
    user[0] = pass[0] = '\0';
    if (v != NULL && v->len > 6 && memcmp(v->buf, "Basic ", 6) == 0) {
        char buf[256];
        size_t n = mg_base64_decode(v->buf + 6, v->len - 6, buf, sizeof(buf));
        const char* p = (const char*)memchr(buf, ':', n > 0 ? n : 0);
        if (p != NULL) {
            mg_snprintf(user, userlen, "%.*s", p - buf, buf);
            mg_snprintf(pass, passlen, "%.*s", n - (size_t)(p - buf) - 1,
                        p + 1);
        }
    } else if (v != NULL && v->len > 7 && memcmp(v->buf, "Bearer ", 7) == 0) {
        mg_snprintf(pass, passlen, "%.*s", (int)v->len - 7, v->buf + 7);
    } else if ((v = mg_http_get_header(hm, "Cookie")) != NULL) {
        struct mg_str t = mg_http_get_header_var(*v,
                                                 mg_str_n("access_token", 12));
        if (t.len > 0)
            mg_snprintf(pass, passlen, "%.*s", (int)t.len, t.buf);
    } else {
        mg_http_get_var(&hm->query, "access_token", pass, passlen);
    }
}

static struct mg_str stripquotes(struct mg_str s)
{
    return s.len > 1 && s.buf[0] == '"' && s.buf[s.len - 1] == '"'
        ? mg_str_n(s.buf + 1, s.len - 2)
        : s;
}

struct mg_str mg_http_get_header_var(struct mg_str s, struct mg_str v)
{
    size_t i;
    for (i = 0; v.len > 0 && i + v.len + 2 < s.len; i++) {
        if (s.buf[i + v.len] == '=' && memcmp(&s.buf[i], v.buf, v.len) == 0) {
            const char *p = &s.buf[i + v.len + 1], *b = p, *x = &s.buf[s.len];
            int q = p < x && *p == '"' ? 1 : 0;
            while (p < x
                   && (q ? p == b || *p != '"'
                         : *p != ';' && *p != ' ' && *p != ','))
                p++;
            // MG_INFO(("[%.*s] [%.*s] [%.*s]", (int) s.len, s.buf, (int)
            // v.len, v.buf, (int) (p - b), b));
            return stripquotes(mg_str_n(b, (size_t)(p - b + q)));
        }
    }
    return mg_str_n(NULL, 0);
}

// ---- module: iobuf ----

static size_t roundup(size_t size, size_t align)
{
    return align == 0 ? size : (size + align - 1) / align * align;
}

bool mg_iobuf_resize(struct mg_iobuf* io, size_t new_size)
{
    bool ok = true;
    new_size = roundup(new_size, io->align);
    if (new_size == 0) {
        mg_bzero(io->buf, io->size);
        mg_free(io->buf);
        io->buf = NULL;
        io->len = io->size = 0;
    } else if (new_size != io->size) {
        // NOTE(lsm): do not use realloc here. Use mg_calloc/mg_free only
        void* p = mg_calloc(1, new_size);
        if (p != NULL) {
            size_t len = new_size < io->len ? new_size : io->len;
            if (len > 0 && io->buf != NULL)
                memmove(p, io->buf, len);
            mg_bzero(io->buf, io->size);
            mg_free(io->buf);
            io->buf = (unsigned char*)p;
            io->size = new_size;
            io->len = len;
        } else {
            ok = false;
            MG_ERROR(("%lld->%lld", (uint64_t)io->size, (uint64_t)new_size));
        }
    }
    return ok;
}

bool mg_iobuf_init(struct mg_iobuf* io, size_t size, size_t align)
{
    io->buf = NULL;
    io->align = align;
    io->size = io->len = 0;
    return mg_iobuf_resize(io, size);
}

size_t mg_iobuf_add(struct mg_iobuf* io, size_t ofs, const void* buf,
                    size_t len)
{
    size_t new_size = roundup(io->len + len, io->align);
    mg_iobuf_resize(io, new_size); // Attempt to resize
    if (new_size != io->size)
        len = 0; // Resize failure, append nothing
    if (ofs < io->len)
        memmove(io->buf + ofs + len, io->buf + ofs, io->len - ofs);
    if (buf != NULL)
        memmove(io->buf + ofs, buf, len);
    if (ofs > io->len)
        io->len += ofs - io->len;
    io->len += len;
    return len;
}

size_t mg_iobuf_del(struct mg_iobuf* io, size_t ofs, size_t len)
{
    if (ofs > io->len)
        ofs = io->len;
    if (ofs + len > io->len)
        len = io->len - ofs;
    if (io->buf)
        memmove(io->buf + ofs, io->buf + ofs + len, io->len - ofs - len);
    if (io->buf)
        mg_bzero(io->buf + io->len - len, len);
    io->len -= len;
    return len;
}

void mg_iobuf_free(struct mg_iobuf* io) { mg_iobuf_resize(io, 0); }

// ---- module: json ----

static const char* escapeseq(int esc)
{
    return esc ? "\b\f\n\r\t\\\"" : "bfnrt\\\"";
}

static char json_esc(int c, int esc)
{
    const char *p, *esc1 = escapeseq(esc), *esc2 = escapeseq(!esc);
    for (p = esc1; *p != '\0'; p++) {
        if (*p == c)
            return esc2[p - esc1];
    }
    return 0;
}

static int mg_pass_string(const char* s, int len)
{
    int i;
    for (i = 0; i < len; i++) {
        if (s[i] == '\\' && i + 1 < len && json_esc(s[i + 1], 1)) {
            i++;
        } else if (s[i] == '\0') {
            return MG_JSON_INVALID;
        } else if (s[i] == '"') {
            return i;
        }
    }
    return MG_JSON_INVALID;
}

static double mg_atod(const char* p, int len, int* numlen)
{
    double d = 0.0;
    int i = 0, sign = 1;

    // Sign
    if (i < len && *p == '-') {
        sign = -1, i++;
    } else if (i < len && *p == '+') {
        i++;
    }

    // Decimal
    for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
        d *= 10.0;
        d += p[i] - '0';
    }
    d *= sign;

    // Fractional
    if (i < len && p[i] == '.') {
        double frac = 0.0, base = 0.1;
        i++;
        for (; i < len && p[i] >= '0' && p[i] <= '9'; i++) {
            frac += base * (p[i] - '0');
            base /= 10.0;
        }
        d += frac * sign;
    }

    // Exponential
    if (i < len && (p[i] == 'e' || p[i] == 'E')) {
        int exp = 0, minus = 0;
        i++;
        if (i < len && p[i] == '-')
            minus = 1, i++;
        if (i < len && p[i] == '+')
            i++;
        while (i < len && p[i] >= '0' && p[i] <= '9' && exp < 308)
            exp = exp * 10 + (p[i++] - '0');
        // use fast exponentiation
        // https://en.wikipedia.org/wiki/Exponentiation_by_squaring
        if (exp != 0) {
            double x = 10, y = 1;
            if (exp > 308)
                exp = 308;
            if (minus)
                x = 0.1;
            while (exp > 1) {
                if (exp & 1) {
                    y *= x;
                    --exp;
                }
                x *= x;
                exp >>= 1;
            }
            d *= x * y;
        }
    }

    if (numlen != NULL)
        *numlen = i;
    return d;
}

// Iterate over object or array elements
size_t mg_json_next(struct mg_str obj, size_t ofs, struct mg_str* key,
                    struct mg_str* val)
{
    if (ofs >= obj.len) {
        ofs = 0; // Out of boundaries, stop scanning
    } else if (obj.len < 2 || (*obj.buf != '{' && *obj.buf != '[')) {
        ofs = 0; // Not an array or object, stop
    } else {
        struct mg_str sub = mg_str_n(obj.buf + ofs, obj.len - ofs);
        if (ofs == 0)
            ofs++, sub.buf++, sub.len--;
        if (*obj.buf == '[') { // Iterate over an array
            int n = 0, o = mg_json_get(sub, "$", &n);
            if (n < 0 || o < 0 || (size_t)(o + n) > sub.len) {
                ofs = 0; // Error parsing key, stop scanning
            } else {
                if (key)
                    *key = mg_str_n(NULL, 0);
                if (val)
                    *val = mg_str_n(sub.buf + o, (size_t)n);
                ofs = (size_t)(&sub.buf[o + n] - obj.buf);
            }
        } else { // Iterate over an object
            int n = 0, o = mg_json_get(sub, "$", &n);
            if (n < 0 || o < 0 || (size_t)(o + n) > sub.len) {
                ofs = 0; // Error parsing key, stop scanning
            } else {
                if (key)
                    *key = mg_str_n(sub.buf + o, (size_t)n);
                sub.buf += o + n, sub.len -= (size_t)(o + n);
                while (sub.len > 0 && *sub.buf != ':')
                    sub.len--, sub.buf++;
                if (sub.len > 0 && *sub.buf == ':')
                    sub.len--, sub.buf++;
                n = 0, o = mg_json_get(sub, "$", &n);
                if (n < 0 || o < 0 || (size_t)(o + n) > sub.len) {
                    ofs = 0; // Error parsing value, stop scanning
                } else {
                    if (val)
                        *val = mg_str_n(sub.buf + o, (size_t)n);
                    ofs = (size_t)(&sub.buf[o + n] - obj.buf);
                }
            }
        }
        // MG_INFO(("SUB ofs %u %.*s", ofs, sub.len, sub.buf));
        while (ofs && ofs < obj.len
               && (obj.buf[ofs] == ' ' || obj.buf[ofs] == '\t'
                   || obj.buf[ofs] == '\n' || obj.buf[ofs] == '\r')) {
            ofs++;
        }
        if (ofs && ofs < obj.len && obj.buf[ofs] == ',')
            ofs++;
        if (ofs > obj.len)
            ofs = 0;
    }
    return ofs;
}

int mg_json_get(struct mg_str json, const char* path, int* toklen)
{
    const char* s = json.buf;
    int len = (int)json.len;

    enum { S_VALUE, S_KEY, S_COLON, S_COMMA_OR_EOO } expecting = S_VALUE;

    unsigned char nesting[MG_JSON_MAX_DEPTH];
    int i = 0;            // Current offset in `s`
    int j = 0;            // Offset in `s` we're looking for (return value)
    int depth = 0;        // Current depth (nesting level)
    int ed = 0;           // Expected depth
    int pos = 1;          // Current position in `path`
    int ci = -1, ei = -1; // Current and expected index in array

    if (toklen)
        *toklen = 0;
    if (path[0] != '$')
        return MG_JSON_INVALID;

#define MG_CHECKRET(x)                                                        \
    do {                                                                      \
        if (depth == ed && path[pos] == '\0' && ci == ei) {                   \
            if (toklen)                                                       \
                *toklen = i - j + 1;                                          \
            return j;                                                         \
        }                                                                     \
    } while (0)

// In the ascii table, the distance between `[` and `]` is 2.
// Ditto for `{` and `}`. Hence +2 in the code below.
#define MG_EOO(x)                                                             \
    do {                                                                      \
        if (depth == ed && ci != ei)                                          \
            return MG_JSON_NOT_FOUND;                                         \
        if (c != nesting[depth - 1] + 2)                                      \
            return MG_JSON_INVALID;                                           \
        depth--;                                                              \
        MG_CHECKRET(x);                                                       \
    } while (0)

    for (i = 0; i < len; i++) {
        unsigned char c = ((unsigned char*)s)[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
            continue;
        switch (expecting) {
        case S_VALUE:
            // p("V %s [%.*s] %d %d %d %d\n", path, pos, path, depth, ed, ci,
            // ei);
            if (depth == ed)
                j = i;
            if (c == '{') {
                if (depth >= (int)sizeof(nesting))
                    return MG_JSON_TOO_DEEP;
                if (depth == ed && path[pos] == '.' && ci == ei) {
                    // If we start the object, reset array indices
                    ed++, pos++, ci = ei = -1;
                }
                nesting[depth++] = c;
                expecting = S_KEY;
                break;
            } else if (c == '[') {
                if (depth >= (int)sizeof(nesting))
                    return MG_JSON_TOO_DEEP;
                if (depth == ed && path[pos] == '[' && ei == ci) {
                    ed++, pos++, ci = 0;
                    for (ei = 0; path[pos] != ']' && path[pos] != '\0';
                         pos++) {
                        ei *= 10;
                        ei += path[pos] - '0';
                    }
                    if (path[pos] != 0)
                        pos++;
                }
                nesting[depth++] = c;
                break;
            } else if (c == ']' && depth > 0) { // Empty array
                MG_EOO(']');
            } else if (c == 't' && i + 3 < len
                       && memcmp(&s[i], "true", 4) == 0) {
                i += 3;
            } else if (c == 'n' && i + 3 < len
                       && memcmp(&s[i], "null", 4) == 0) {
                i += 3;
            } else if (c == 'f' && i + 4 < len
                       && memcmp(&s[i], "false", 5) == 0) {
                i += 4;
            } else if (c == '-' || ((c >= '0' && c <= '9'))) {
                int numlen = 0;
                mg_atod(&s[i], len - i, &numlen);
                i += numlen - 1;
            } else if (c == '"') {
                int n = mg_pass_string(&s[i + 1], len - i - 1);
                if (n < 0)
                    return n;
                i += n + 1;
            } else {
                return MG_JSON_INVALID;
            }
            MG_CHECKRET('V');
            if (depth == ed && ei >= 0)
                ci++;
            expecting = S_COMMA_OR_EOO;
            break;

        case S_KEY:
            if (c == '"') {
                int n = mg_pass_string(&s[i + 1], len - i - 1);
                if (n < 0)
                    return n;
                if (i + 1 + n >= len)
                    return MG_JSON_NOT_FOUND;
                if (depth < ed)
                    return MG_JSON_NOT_FOUND;
                if (depth == ed && path[pos - 1] != '.')
                    return MG_JSON_NOT_FOUND;
                // printf("K %s [%.*s] [%.*s] %d %d %d %d %d\n", path, pos,
                // path, n,
                //        &s[i + 1], n, depth, ed, ci, ei);
                //  NOTE(cpq): in the check sequence below is important.
                //  strncmp() must go first: it fails fast if the remaining
                //  length of the path is smaller than `n`.
                if (depth == ed && path[pos - 1] == '.'
                    && strncmp(&s[i + 1], &path[pos], (size_t)n) == 0
                    && (path[pos + n] == '\0' || path[pos + n] == '.'
                        || path[pos + n] == '[')) {
                    pos += n;
                }
                i += n + 1;
                expecting = S_COLON;
            } else if (c == '}') { // Empty object
                MG_EOO('}');
                expecting = S_COMMA_OR_EOO;
                if (depth == ed && ei >= 0)
                    ci++;
            } else {
                return MG_JSON_INVALID;
            }
            break;

        case S_COLON:
            if (c == ':') {
                expecting = S_VALUE;
            } else {
                return MG_JSON_INVALID;
            }
            break;

        case S_COMMA_OR_EOO:
            if (depth <= 0) {
                return MG_JSON_INVALID;
            } else if (c == ',') {
                expecting = (nesting[depth - 1] == '{') ? S_KEY : S_VALUE;
            } else if (c == ']' || c == '}') {
                if (depth == ed && c == '}' && path[pos - 1] == '.')
                    return MG_JSON_NOT_FOUND;
                if (depth == ed && c == ']' && path[pos - 1] == ',')
                    return MG_JSON_NOT_FOUND;
                MG_EOO('O');
                if (depth == ed && ei >= 0)
                    ci++;
            } else {
                return MG_JSON_INVALID;
            }
            break;
        }
    }
    return MG_JSON_NOT_FOUND;
}

struct mg_str mg_json_get_tok(struct mg_str json, const char* path)
{
    int len = 0, ofs = mg_json_get(json, path, &len);
    return mg_str_n(ofs < 0 ? NULL : json.buf + ofs,
                    (size_t)(len < 0 ? 0 : len));
}

bool mg_json_get_num(struct mg_str json, const char* path, double* v)
{
    int n, toklen, found = 0;
    if ((n = mg_json_get(json, path, &toklen)) >= 0
        && (json.buf[n] == '-'
            || (json.buf[n] >= '0' && json.buf[n] <= '9'))) {
        if (v != NULL)
            *v = mg_atod(json.buf + n, toklen, NULL);
        found = 1;
    }
    return found;
}

bool mg_json_get_bool(struct mg_str json, const char* path, bool* v)
{
    int found = 0, off = mg_json_get(json, path, NULL);
    if (off >= 0 && (json.buf[off] == 't' || json.buf[off] == 'f')) {
        if (v != NULL)
            *v = json.buf[off] == 't';
        found = 1;
    }
    return found;
}

bool mg_json_unescape(struct mg_str s, char* to, size_t n)
{
    size_t i, j;
    for (i = 0, j = 0; i < s.len && j < n; i++, j++) {
        if (s.buf[i] == '\\' && i + 5 < s.len && s.buf[i + 1] == 'u') {
            //  \uXXXX escape. We process simple one-byte chars \u00xx within
            //  ASCII range. More complex chars would require dragging in a
            //  UTF8 library, which is too much for us
            if (mg_str_to_num(mg_str_n(s.buf + i + 2, 4), 16, &to[j],
                              sizeof(uint8_t))
                == false)
                return false;
            i += 5;
        } else if (s.buf[i] == '\\' && i + 1 < s.len) {
            char c = json_esc(s.buf[i + 1], 0);
            if (c == 0)
                return false;
            to[j] = c;
            i++;
        } else {
            to[j] = s.buf[i];
        }
    }
    if (j >= n)
        return false;
    if (n > 0)
        to[j] = '\0';
    return true;
}

char* mg_json_get_str(struct mg_str json, const char* path)
{
    char* result = NULL;
    int len = 0, off = mg_json_get(json, path, &len);
    if (off >= 0 && len > 1 && json.buf[off] == '"') {
        if ((result = (char*)mg_calloc(1, (size_t)len)) != NULL
            && !mg_json_unescape(
                mg_str_n(json.buf + off + 1, (size_t)(len - 2)), result,
                (size_t)len)) {
            mg_free(result);
            result = NULL;
        }
    }
    return result;
}

char* mg_json_get_b64(struct mg_str json, const char* path, int* slen)
{
    char* result = NULL;
    int len = 0, off = mg_json_get(json, path, &len);
    if (off >= 0 && json.buf[off] == '"' && len > 1
        && (result = (char*)mg_calloc(1, (size_t)len)) != NULL) {
        size_t k = mg_base64_decode(json.buf + off + 1, (size_t)(len - 2),
                                    result, (size_t)len);
        if (slen != NULL)
            *slen = (int)k;
    }
    return result;
}

char* mg_json_get_hex(struct mg_str json, const char* path, int* slen)
{
    char* result = NULL;
    int len = 0, off = mg_json_get(json, path, &len);
    if (off >= 0 && json.buf[off] == '"' && len > 1
        && (result = (char*)mg_calloc(1, (size_t)len / 2)) != NULL) {
        int i;
        for (i = 0; i < len - 2; i += 2) {
            mg_str_to_num(mg_str_n(json.buf + off + 1 + i, 2), 16,
                          &result[i >> 1], sizeof(uint8_t));
        }
        result[len / 2 - 1] = '\0';
        if (slen != NULL)
            *slen = len / 2 - 1;
    }
    return result;
}

long mg_json_get_long(struct mg_str json, const char* path, long dflt)
{
    double dv;
    long result = dflt;
    if (mg_json_get_num(json, path, &dv))
        result = (long)dv;
    return result;
}

// ---- module: log ----

int mg_log_level = MG_LL_DEBUG;
static mg_pfn_t s_log_func = mg_pfn_stdout;
static void* s_log_func_param = NULL;

void mg_log_set_fn(mg_pfn_t fn, void* param)
{
    s_log_func = fn;
    s_log_func_param = param;
}

static void logc(unsigned char c) { s_log_func((char)c, s_log_func_param); }

static void logs(const char* buf, size_t len)
{
    size_t i;
    for (i = 0; i < len; i++)
        logc(((unsigned char*)buf)[i]);
}

#if MG_ENABLE_CUSTOM_LOG
// Let user define their own mg_log_prefix() and mg_log()
#else
void mg_log_prefix(int level, const char* file, int line, const char* fname)
{
    const char* p = strrchr(file, '/');
    char buf[41];
    size_t n;
    if (p == NULL)
        p = strrchr(file, '\\');
    n = mg_snprintf(buf, sizeof(buf), "%-6llx %d %s:%d:%s", mg_millis(), level,
                    p == NULL ? file : p + 1, line, fname);
    if (n > sizeof(buf) - 2)
        n = sizeof(buf) - 2;
    while (n < sizeof(buf))
        buf[n++] = ' ';
    logs(buf, n - 1);
}

void mg_log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    mg_vxprintf(s_log_func, s_log_func_param, fmt, &ap);
    va_end(ap);
    logs("\r\n", 2);
}
#endif

static unsigned char nibble(unsigned c)
{
    return (unsigned char)(c < 10 ? c + '0' : c + 'W');
}

#define ISPRINT(x) ((x) >= ' ' && (x) <= '~')

void mg_hexdump(const void* buf, size_t len)
{
    const unsigned char* p = (const unsigned char*)buf;
    unsigned char ascii[16], alen = 0;
    size_t i;
    for (i = 0; i < len; i++) {
        if ((i % 16) == 0) {
            // Print buffered ascii chars
            if (i > 0)
                logs("  ", 2), logs((char*)ascii, 16), logs("\r\n", 2),
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

// ---- module: net ----

size_t mg_vprintf(struct mg_connection* c, const char* fmt, va_list* ap)
{
    size_t old = c->send.len;
    size_t expected = mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, ap);
    size_t actual = c->send.len - old;
    if (actual != expected) {
        mg_error(c, "OOM");
        c->send.len = old;
        actual = 0;
    }
    return actual;
}

size_t mg_printf(struct mg_connection* c, const char* fmt, ...)
{
    size_t len = 0;
    va_list ap;
    va_start(ap, fmt);
    len = mg_vprintf(c, fmt, &ap);
    va_end(ap);
    return len;
}

static bool mg_atonl(struct mg_str str, struct mg_addr* addr)
{
    uint32_t localhost = mg_htonl(0x7f000001);
    if (mg_strcasecmp(str, mg_str("localhost")) != 0)
        return false;
    memcpy(addr->addr.ip, &localhost, sizeof(uint32_t));
    addr->is_ip6 = false;
    return true;
}

static bool mg_atone(struct mg_str str, struct mg_addr* addr)
{
    if (str.len > 0)
        return false;
    memset(addr->addr.ip, 0, sizeof(addr->addr.ip));
    addr->is_ip6 = false;
    return true;
}

static bool mg_aton4(struct mg_str str, struct mg_addr* addr)
{
    uint8_t data[4] = { 0, 0, 0, 0 };
    size_t i, num_dots = 0;
    for (i = 0; i < str.len; i++) {
        if (str.buf[i] >= '0' && str.buf[i] <= '9') {
            int octet = data[num_dots] * 10 + (str.buf[i] - '0');
            if (octet > 255)
                return false;
            data[num_dots] = (uint8_t)octet;
        } else if (str.buf[i] == '.') {
            if (num_dots >= 3 || i == 0 || str.buf[i - 1] == '.')
                return false;
            num_dots++;
        } else {
            return false;
        }
    }
    if (num_dots != 3 || str.buf[i - 1] == '.')
        return false;
    memcpy(&addr->addr.ip, data, sizeof(data));
    addr->is_ip6 = false;
    return true;
}

static bool mg_v4mapped(struct mg_str str, struct mg_addr* addr)
{
    int i;
    uint32_t ipv4;
    if (str.len < 14)
        return false;
    if (str.buf[0] != ':' || str.buf[1] != ':' || str.buf[6] != ':')
        return false;
    for (i = 2; i < 6; i++) {
        if (str.buf[i] != 'f' && str.buf[i] != 'F')
            return false;
    }
    // struct mg_str s = mg_str_n(&str.buf[7], str.len - 7);
    if (!mg_aton4(mg_str_n(&str.buf[7], str.len - 7), addr))
        return false;
    memcpy(&ipv4, addr->addr.ip, sizeof(ipv4));
    memset(addr->addr.ip, 0, sizeof(addr->addr.ip));
    addr->addr.ip[10] = addr->addr.ip[11] = 255;
    memcpy(&addr->addr.ip[12], &ipv4, 4);
    addr->is_ip6 = true;
    return true;
}

static bool mg_aton6(struct mg_str str, struct mg_addr* addr)
{
    size_t i, j = 0, n = 0, dc = 42;
    addr->scope_id = 0;
    if (str.len > 2 && str.buf[0] == '[')
        str.buf++, str.len -= 2;
    if (mg_v4mapped(str, addr))
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
            mg_str_to_num(mg_str_n(&str.buf[j], i - j + 1), 16, &val,
                          sizeof(val));
            addr->addr.ip[n] = (uint8_t)((val >> 8) & 255);
            addr->addr.ip[n + 1] = (uint8_t)(val & 255);
        } else if (str.buf[i] == ':') {
            j = i + 1;
            if (i > 0 && str.buf[i - 1] == ':') {
                dc = n; // Double colon
                if (i > 1 && str.buf[i - 2] == ':')
                    return false;
            } else if (i > 0) {
                n += 2;
            }
            if (n > 14)
                return false;
            addr->addr.ip[n] = addr->addr.ip[n + 1] = 0; // For trailing ::
        } else if (str.buf[i] == '%') { // Scope ID, last in string
            if (mg_str_to_num(mg_str_n(&str.buf[i + 1], str.len - i - 1), 10,
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
    if (n < 14 && dc == 42)
        return false;
    if (n < 14) {
        memmove(&addr->addr.ip[dc + (14 - n)], &addr->addr.ip[dc], n - dc + 2);
        memset(&addr->addr.ip[dc], 0, 14 - n);
    }

    addr->is_ip6 = true;
    return true;
}

bool mg_aton(struct mg_str str, struct mg_addr* addr)
{
    // MG_INFO(("[%.*s]", (int) str.len, str.buf));
    return mg_atone(str, addr) || mg_atonl(str, addr) || mg_aton4(str, addr)
        || mg_aton6(str, addr);
}

struct mg_connection* mg_alloc_conn(struct mg_mgr* mgr)
{
    struct mg_connection* c = (struct mg_connection*)mg_calloc(
        1, sizeof(*c) + mgr->extraconnsize);
    if (c != NULL) {
        c->mgr = mgr;
        c->send.align = c->recv.align = c->rtls.align = MG_IO_SIZE;
        c->id = ++mgr->nextid;
        MG_PROF_INIT(c);
    }
    return c;
}

void mg_close_conn(struct mg_connection* c)
{
    mg_resolve_cancel(c); // Close any pending DNS query
    LIST_DELETE(struct mg_connection, &c->mgr->conns, c);
    if (c == c->mgr->dns4.c)
        c->mgr->dns4.c = NULL;
    if (c == c->mgr->dns6.c)
        c->mgr->dns6.c = NULL;
    // Order of operations is important. `MG_EV_CLOSE` event must be fired
    // before we deallocate received data, see #1331
    mg_call(c, MG_EV_CLOSE, NULL);
    MG_DEBUG(("%lu %ld closed", c->id, c->fd));
    MG_PROF_DUMP(c);
    MG_PROF_FREE(c);

    mg_tls_free(c);
    mg_iobuf_free(&c->recv);
    mg_iobuf_free(&c->send);
    mg_iobuf_free(&c->rtls);
    mg_bzero((unsigned char*)c, sizeof(*c));
    mg_free(c);
}

struct mg_connection* mg_connect_svc(struct mg_mgr* mgr, const char* url,
                                     mg_event_handler_t fn, void* fn_data,
                                     mg_event_handler_t pfn, void* pfn_data)
{
    struct mg_connection* c = NULL;
    if (url == NULL || url[0] == '\0') {
        MG_ERROR(("null url"));
    } else if ((c = mg_alloc_conn(mgr)) == NULL) {
        MG_ERROR(("OOM"));
    } else {
        LIST_ADD_HEAD(struct mg_connection, &mgr->conns, c);
        c->is_udp = (strncmp(url, "udp:", 4) == 0);
        c->fd = (void*)(size_t)MG_INVALID_SOCKET;
        c->fn = fn;
        c->is_client = true;
        c->fn_data = fn_data;
        c->is_tls = (mg_url_is_ssl(url) != 0);
        c->pfn = pfn;
        c->pfn_data = pfn_data;
        mg_call(c, MG_EV_OPEN, (void*)url);
        MG_DEBUG(("%lu %ld %s", c->id, c->fd, url));
        mg_resolve(c, url);
    }
    return c;
}

struct mg_connection* mg_connect(struct mg_mgr* mgr, const char* url,
                                 mg_event_handler_t fn, void* fn_data)
{
    return mg_connect_svc(mgr, url, fn, fn_data, NULL, NULL);
}

struct mg_connection* mg_listen(struct mg_mgr* mgr, const char* url,
                                mg_event_handler_t fn, void* fn_data)
{
    struct mg_connection* c = NULL;
    if ((c = mg_alloc_conn(mgr)) == NULL) {
        MG_ERROR(("OOM %s", url));
    } else if (!mg_open_listener(c, url)) {
        MG_ERROR(("Failed: %s", url));
        MG_PROF_FREE(c);
        mg_free(c);
        c = NULL;
    } else {
        c->is_listening = 1;
        c->is_udp = strncmp(url, "udp:", 4) == 0;
        LIST_ADD_HEAD(struct mg_connection, &mgr->conns, c);
        c->fn = fn;
        c->fn_data = fn_data;
        c->is_tls = (mg_url_is_ssl(url) != 0);
        mg_call(c, MG_EV_OPEN, NULL);
        MG_DEBUG(("%lu %ld %s", c->id, c->fd, url));
    }
    return c;
}

struct mg_connection* mg_wrapfd(struct mg_mgr* mgr, int fd,
                                mg_event_handler_t fn, void* fn_data)
{
    struct mg_connection* c = mg_alloc_conn(mgr);
    if (c != NULL) {
        c->fd = (void*)(size_t)fd;
        c->fn = fn;
        c->fn_data = fn_data;
        MG_EPOLL_ADD(c);
        mg_call(c, MG_EV_OPEN, NULL);
        LIST_ADD_HEAD(struct mg_connection, &mgr->conns, c);
    }
    return c;
}

struct mg_timer* mg_timer_add(struct mg_mgr* mgr, uint64_t milliseconds,
                              unsigned flags, void (*fn)(void*), void* arg)
{
    struct mg_timer* t = (struct mg_timer*)mg_calloc(1, sizeof(*t));
    if (t != NULL) {
        flags |= MG_TIMER_AUTODELETE; // We have alloc'ed it, so autodelete
        mg_timer_init(&mgr->timers, t, milliseconds, flags, fn, arg);
    }
    return t;
}

long mg_io_recv(struct mg_connection* c, void* buf, size_t len)
{
    if (c->rtls.len == 0)
        return MG_IO_WAIT;
    if (len > c->rtls.len)
        len = c->rtls.len;
    memcpy(buf, c->rtls.buf, len);
    mg_iobuf_del(&c->rtls, 0, len);
    return (long)len;
}

void mg_mgr_free(struct mg_mgr* mgr)
{
    struct mg_connection* c;
    struct mg_timer *tmp, *t = mgr->timers;
    while (t != NULL)
        tmp = t->next, mg_free(t), t = tmp;
    mgr->timers = NULL; // Important. Next call to poll won't touch timers
    for (c = mgr->conns; c != NULL; c = c->next)
        c->is_closing = 1;
    mg_mgr_poll(mgr, 0);
    MG_DEBUG(("All connections closed"));
#if MG_ENABLE_EPOLL
    if (mgr->epoll_fd >= 0)
        close(mgr->epoll_fd), mgr->epoll_fd = -1;
#endif
    mg_tls_ctx_free(mgr);
}

void mg_mgr_init(struct mg_mgr* mgr)
{
    memset(mgr, 0, sizeof(*mgr));
#if MG_ENABLE_EPOLL
    if ((mgr->epoll_fd = epoll_create1(EPOLL_CLOEXEC)) < 0)
        MG_ERROR(("epoll_create1 errno %d", errno));
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
    mgr->dnstimeout = 3000;
    mgr->dns4.url = "udp://8.8.8.8:53";
    mgr->dns6.url = "udp://[2001:4860:4860::8888]:53";
    mg_tls_ctx_init(mgr);
    MG_DEBUG(("MG_IO_SIZE: %lu, TLS: %s", MG_IO_SIZE,
              MG_TLS == MG_TLS_NONE          ? "none"
                  : MG_TLS == MG_TLS_MBED    ? "MbedTLS"
                  : MG_TLS == MG_TLS_OPENSSL ? "OpenSSL"
                  : MG_TLS == MG_TLS_BUILTIN ? "builtin"
                  : MG_TLS == MG_TLS_WOLFSSL ? "WolfSSL"
                                             : "custom"));
}

// ---- module: printf ----

static void mg_pfn_iobuf_private(char ch, void* param, bool expand)
{
    struct mg_iobuf* io = (struct mg_iobuf*)param;
    if (expand && io->len + 2 > io->size)
        mg_iobuf_resize(io, io->len + 2);
    if (io->len + 2 <= io->size) {
        io->buf[io->len++] = (uint8_t)ch;
        io->buf[io->len] = 0;
    } else if (io->len < io->size) {
        io->buf[io->len++] = 0; // Guarantee to 0-terminate
    }
}

void mg_pfn_iobuf_noresize(char ch, void* param)
{
    mg_pfn_iobuf_private(ch, param, false);
}

void mg_pfn_iobuf(char ch, void* param)
{
    mg_pfn_iobuf_private(ch, param, true);
}

size_t mg_vsnprintf(char* buf, size_t len, const char* fmt, va_list* ap)
{
    struct mg_iobuf io = { 0, 0, 0, 0 };
    size_t n;
    io.buf = (uint8_t*)buf, io.size = len;
    n = mg_vxprintf(mg_pfn_iobuf_noresize, &io, fmt, ap);
    if (n < len)
        buf[n] = '\0';
    return n;
}

size_t mg_snprintf(char* buf, size_t len, const char* fmt, ...)
{
    va_list ap;
    size_t n;
    va_start(ap, fmt);
    n = mg_vsnprintf(buf, len, fmt, &ap);
    va_end(ap);
    return n;
}

char* mg_vmprintf(const char* fmt, va_list* ap)
{
    struct mg_iobuf io = { 0, 0, 0, 256 };
    mg_vxprintf(mg_pfn_iobuf, &io, fmt, ap);
    return (char*)io.buf;
}

char* mg_mprintf(const char* fmt, ...)
{
    char* s;
    va_list ap;
    va_start(ap, fmt);
    s = mg_vmprintf(fmt, &ap);
    va_end(ap);
    return s;
}

void mg_pfn_stdout(char c, void* param)
{
    putchar(c);
    (void)param;
}

static size_t print_ip4(void (*out)(char, void*), void* arg, uint8_t* p)
{
    return mg_xprintf(out, arg, "%d.%d.%d.%d", p[0], p[1], p[2], p[3]);
}

static size_t print_ip6(void (*out)(char, void*), void* arg, uint16_t* p)
{
    return mg_xprintf(out, arg, "[%x:%x:%x:%x:%x:%x:%x:%x]", mg_ntohs(p[0]),
                      mg_ntohs(p[1]), mg_ntohs(p[2]), mg_ntohs(p[3]),
                      mg_ntohs(p[4]), mg_ntohs(p[5]), mg_ntohs(p[6]),
                      mg_ntohs(p[7]));
}

size_t mg_print_ip4(void (*out)(char, void*), void* arg, va_list* ap)
{
    uint8_t* p = va_arg(*ap, uint8_t*);
    return print_ip4(out, arg, p);
}

size_t mg_print_ip6(void (*out)(char, void*), void* arg, va_list* ap)
{
    uint16_t* p = va_arg(*ap, uint16_t*);
    return print_ip6(out, arg, p);
}

size_t mg_print_ip(void (*out)(char, void*), void* arg, va_list* ap)
{
    struct mg_addr* addr = va_arg(*ap, struct mg_addr*);
    if (addr->is_ip6)
        return print_ip6(out, arg, (uint16_t*)addr->addr.ip);
    return print_ip4(out, arg, (uint8_t*)&addr->addr.ip);
}

size_t mg_print_ip_port(void (*out)(char, void*), void* arg, va_list* ap)
{
    struct mg_addr* a = va_arg(*ap, struct mg_addr*);
    return mg_xprintf(out, arg, "%M:%hu", mg_print_ip, a, mg_ntohs(a->port));
}

static char mg_esc(int c, bool esc)
{
    const char *p, *esc1 = "\b\f\n\r\t\\\"", *esc2 = "bfnrt\\\"";
    for (p = esc ? esc1 : esc2; *p != '\0'; p++) {
        if (*p == c)
            return esc ? esc2[p - esc1] : esc1[p - esc2];
    }
    return 0;
}

static char mg_escape(int c) { return mg_esc(c, true); }

static size_t qcpy(void (*out)(char, void*), void* ptr, char* buf, size_t len)
{
    size_t i = 0, extra = 0;
    for (i = 0; i < len && buf[i] != '\0'; i++) {
        char c = mg_escape(buf[i]);
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

size_t mg_print_hex(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t bl = (size_t)va_arg(*ap, int);
    uint8_t* p = va_arg(*ap, uint8_t*);
    const char* hex = "0123456789abcdef";
    size_t j;
    for (j = 0; j < bl; j++) {
        out(hex[(p[j] >> 4) & 0x0F], arg);
        out(hex[p[j] & 0x0F], arg);
    }
    return 2 * bl;
}

size_t mg_print_base64(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t len = (size_t)va_arg(*ap, int);
    uint8_t* buf = va_arg(*ap, uint8_t*);
    return bcpy(out, arg, buf, len);
}

size_t mg_print_esc(void (*out)(char, void*), void* arg, va_list* ap)
{
    size_t len = (size_t)va_arg(*ap, int);
    char* p = va_arg(*ap, char*);
    if (len == 0)
        len = p == NULL ? 0 : strlen(p);
    return qcpy(out, arg, p, len);
}

// ---- module: sock ----

#if MG_ENABLE_SOCKET

#ifndef closesocket
#define closesocket(x) close(x)
#endif

#define FD(c_) ((MG_SOCKET_TYPE)(size_t)(c_)->fd)
#define S2PTR(s_) ((void*)(size_t)(s_))

#ifndef MSG_NONBLOCKING
#define MSG_NONBLOCKING 0
#endif

#ifndef AF_INET6
#define AF_INET6 10
#endif

#ifndef MG_SOCK_ERR
#define MG_SOCK_ERR(errcode) ((errcode) < 0 ? errno : 0)
#endif

#ifndef MG_SOCK_INTR
#define MG_SOCK_INTR(fd) (fd == MG_INVALID_SOCKET && MG_SOCK_ERR(-1) == EINTR)
#endif

#ifndef MG_SOCK_PENDING
#define MG_SOCK_PENDING(errcode)                                              \
    (((errcode) < 0) && (errno == EINPROGRESS || errno == EWOULDBLOCK))
#endif

#ifndef MG_SOCK_RESET
#define MG_SOCK_RESET(errcode)                                                \
    (((errcode) < 0) && (errno == EPIPE || errno == ECONNRESET))
#endif

union usa {
    struct sockaddr sa;
    struct sockaddr_in sin;
#if MG_ENABLE_IPV6
    struct sockaddr_in6 sin6;
#endif
};

static socklen_t tousa(struct mg_addr* a, union usa* usa)
{
    socklen_t len = sizeof(usa->sin);
    memset(usa, 0, sizeof(*usa));
    usa->sin.sin_family = AF_INET;
    usa->sin.sin_port = a->port;
    memcpy(&usa->sin.sin_addr, a->addr.ip, sizeof(uint32_t));
#if MG_ENABLE_IPV6
    if (a->is_ip6) {
        usa->sin.sin_family = AF_INET6;
        usa->sin6.sin6_port = a->port;
        usa->sin6.sin6_scope_id = a->scope_id;
        memcpy(&usa->sin6.sin6_addr, a->addr.ip, sizeof(a->addr.ip));
        len = sizeof(usa->sin6);
    }
#endif
    return len;
}

static void tomgaddr(union usa* usa, struct mg_addr* a, bool is_ip6)
{
    a->is_ip6 = is_ip6;
#if MG_ENABLE_IPV6
    if (is_ip6) {
        memcpy(a->addr.ip, &usa->sin6.sin6_addr, sizeof(a->addr.ip));
        a->port = usa->sin6.sin6_port;
        a->scope_id = (uint8_t)usa->sin6.sin6_scope_id;
    } else
#endif
    {
        a->port = usa->sin.sin_port;
        memcpy(&a->addr.ip, &usa->sin.sin_addr, sizeof(uint32_t));
    }
}

static void setlocaddr(MG_SOCKET_TYPE fd, struct mg_addr* addr)
{
    union usa usa;
    socklen_t n = sizeof(usa);
    if (getsockname(fd, &usa.sa, &n) == 0) {
        tomgaddr(&usa, addr, n != sizeof(usa.sin));
    }
}

// Get the local 'addr' the stack will use to connect to 'to'
void mg_getlocaddr(struct mg_connection* c, struct mg_addr* to,
                   struct mg_addr* addr);

void mg_getlocaddr(struct mg_connection* c, struct mg_addr* to,
                   struct mg_addr* addr)
{
    union usa usa;
    socklen_t slen;
    MG_SOCKET_TYPE fd;
    int rc, af = to->is_ip6 ? AF_INET6 : AF_INET;
    fd = socket(af, SOCK_DGRAM, IPPROTO_UDP);
    if (fd == MG_INVALID_SOCKET) {
        mg_error(c, "socket(): %d", MG_SOCK_ERR(-1));
        return;
    }
    // NOTE(): TI-RTOS NDK may require binding
    slen = tousa(to, &usa);
    if ((rc = connect(fd, &usa.sa, slen)) != 0) {
        mg_error(c, "connect: %d", MG_SOCK_ERR(rc));
        return;
    }
    setlocaddr(fd, addr);
    closesocket(fd);
}

static void iolog(struct mg_connection* c, char* buf, long n, bool r)
{
    if (n == MG_IO_WAIT) {
        // Do nothing
    } else if (n <= 0) {
        c->is_closing = 1; // Termination. Don't call mg_error(): #1529
    } else if (n > 0) {
        if (c->is_hexdumping) {
            MG_INFO(("\n-- %lu %M %s %M %ld", c->id, mg_print_ip_port, &c->loc,
                     r ? "<-" : "->", mg_print_ip_port, &c->rem, n));
            mg_hexdump(buf, (size_t)n);
        }
        if (r) {
            c->recv.len += (size_t)n;
            mg_call(c, MG_EV_READ, &n);
        } else {
            mg_iobuf_del(&c->send, 0, (size_t)n);
            // if (c->send.len == 0) mg_iobuf_resize(&c->send, 0);
            if (c->send.len == 0) {
                MG_EPOLL_MOD(c, 0);
            }
            mg_call(c, MG_EV_WRITE, &n);
        }
    }
}

long mg_io_send(struct mg_connection* c, const void* buf, size_t len)
{
    long n;
    if (c->is_udp) {
        union usa usa;
        socklen_t slen = tousa(&c->rem, &usa);
        n = sendto(FD(c), (char*)buf, len, 0, &usa.sa, slen);
        if (n > 0)
            setlocaddr(FD(c), &c->loc);
    } else {
        n = send(FD(c), (char*)buf, len, MSG_NONBLOCKING);
    }
    MG_VERBOSE(("%lu %ld %d", c->id, n, MG_SOCK_ERR(n)));
    if (MG_SOCK_PENDING(n))
        return MG_IO_WAIT;
    if (MG_SOCK_RESET(n))
        return MG_IO_RESET; // MbedTLS, see #1507
    if (n <= 0)
        return MG_IO_ERR;
    return n;
}

bool mg_send(struct mg_connection* c, const void* buf, size_t len)
{
    if (c->is_udp) {
        long n = mg_io_send(c, buf, len);
        MG_DEBUG(("%lu %ld %lu:%lu:%lu %ld err %d", c->id, c->fd, c->send.len,
                  c->recv.len, c->rtls.len, n, MG_SOCK_ERR(n)));
        iolog(c, (char*)buf, n, false);
        return n > 0;
    } else {
        return len == 0 || mg_iobuf_add(&c->send, c->send.len, buf, len) > 0;
        // returning 0 means an OOM condition (iobuf couldn't resize), yet this
        // is so far recoverable, let the caller decide
    }
}

static void mg_set_non_blocking_mode(MG_SOCKET_TYPE fd)
{
#if defined(MG_CUSTOM_NONBLOCK)
    MG_CUSTOM_NONBLOCK(fd);
#elif MG_ARCH == MG_ARCH_WIN32 && MG_ENABLE_WINSOCK
    unsigned long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#elif MG_ENABLE_RL
    unsigned long on = 1;
    ioctlsocket(fd, FIONBIO, &on);
#elif MG_ENABLE_FREERTOS_TCP
    const BaseType_t off = 0;
    if (setsockopt(fd, 0, FREERTOS_SO_RCVTIMEO, &off, sizeof(off)) != 0)
        (void)0;
    if (setsockopt(fd, 0, FREERTOS_SO_SNDTIMEO, &off, sizeof(off)) != 0)
        (void)0;
#elif MG_ENABLE_LWIP
    lwip_fcntl(fd, F_SETFL, O_NONBLOCK);
#elif MG_ARCH == MG_ARCH_THREADX
    // NetxDuo fails to send large blocks of data to the non-blocking sockets
    (void)fd;
    // fcntl(fd, F_SETFL, O_NONBLOCK);
#elif MG_ARCH == MG_ARCH_TIRTOS
    int val = 0;
    setsockopt(fd, SOL_SOCKET, SO_BLOCKING, &val, sizeof(val));
    // SPRU524J section 3.3.3 page 63, SO_SNDLOWAT
    int sz = sizeof(val);
    getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &val, &sz);
    val /= 2; // set send low-water mark at half send buffer size
    setsockopt(fd, SOL_SOCKET, SO_SNDLOWAT, &val, sizeof(val));
#else
    fcntl(fd, F_SETFL,
          fcntl(fd, F_GETFL, 0) | O_NONBLOCK); // Non-blocking mode
    fcntl(fd, F_SETFD, FD_CLOEXEC);            // Set close-on-exec
#endif
}

void mg_multicast_add(struct mg_connection* c, char* ip);

void mg_multicast_add(struct mg_connection* c, char* ip)
{
#if MG_ENABLE_RL
    MG_ERROR(("unsupported"));
#elif MG_ENABLE_FREERTOS_TCP
    // TODO(): prvAllowIPPacketIPv4()
#else
    // lwIP, Unix, Windows, Zephyr 4+(, AzureRTOS ?)
#if MG_ENABLE_LWIP && !LWIP_IGMP
    MG_ERROR(("LWIP_IGMP not defined, no multicast support"));
#else
#if defined(__ZEPHYR__) && ZEPHYR_VERSION_CODE < 0x40000
    MG_ERROR(("struct ip_mreq not defined"));
#else
    struct ip_mreq mreq;
    mreq.imr_multiaddr.s_addr = inet_addr(ip);
    mreq.imr_interface.s_addr = mg_htonl(INADDR_ANY);
    setsockopt(FD(c), IPPROTO_IP, IP_ADD_MEMBERSHIP, (char*)&mreq,
               sizeof(mreq));
#endif // !Zephyr
#endif // !lwIP
#endif
}

bool mg_open_listener(struct mg_connection* c, const char* url)
{
    MG_SOCKET_TYPE fd = MG_INVALID_SOCKET;
    bool success = false;
    c->loc.port = mg_htons(mg_url_port(url));
    if (!mg_aton(mg_url_host(url), &c->loc)) {
        MG_ERROR(("invalid listening URL: %s", url));
    } else {
        union usa usa;
        socklen_t slen = tousa(&c->loc, &usa);
        int rc, on = 1, af = c->loc.is_ip6 ? AF_INET6 : AF_INET;
        int type = strncmp(url, "udp:", 4) == 0 ? SOCK_DGRAM : SOCK_STREAM;
        int proto = type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
        (void)on;

        if ((fd = socket(af, type, proto)) == MG_INVALID_SOCKET) {
            MG_ERROR(("socket: %d", MG_SOCK_ERR(-1)));
#if defined(SO_EXCLUSIVEADDRUSE)
        } else if ((rc = setsockopt(fd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                                    (char*)&on, sizeof(on)))
                   != 0) {
            // "Using SO_REUSEADDR and SO_EXCLUSIVEADDRUSE"
            MG_ERROR(("setsockopt(SO_EXCLUSIVEADDRUSE): %d %d", on,
                      MG_SOCK_ERR(rc)));
#elif defined(SO_REUSEADDR) && (!defined(LWIP_SOCKET) || SO_REUSE)
        } else if ((rc = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char*)&on,
                                    sizeof(on)))
                   != 0) {
            // 1. SO_REUSEADDR semantics on UNIX and Windows is different.  On
            // Windows, SO_REUSEADDR allows to bind a socket to a port without
            // error even if the port is already open by another program. This
            // is not the behavior SO_REUSEADDR was designed for, and leads to
            // hard-to-track failure scenarios.
            //
            // 2. For LWIP, SO_REUSEADDR should be explicitly enabled by
            // defining SO_REUSE = 1 in lwipopts.h, otherwise the code below
            // will compile but won't work! (setsockopt will return EINVAL)
            MG_ERROR(("setsockopt(SO_REUSEADDR): %d", MG_SOCK_ERR(rc)));
#endif
#if MG_IPV6_V6ONLY
            // Bind only to the V6 address, not V4 address on this port
        } else if (c->loc.is_ip6
                   && (rc = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
                                       (char*)&on, sizeof(on)))
                       != 0) {
            // See #2089. Allow to bind v4 and v6 sockets on the same port
            MG_ERROR(("setsockopt(IPV6_V6ONLY): %d", MG_SOCK_ERR(rc)));
#endif
        } else if ((rc = bind(fd, &usa.sa, slen)) != 0) {
            MG_ERROR(("bind: %d", MG_SOCK_ERR(rc)));
        } else if ((type == SOCK_STREAM
                    && (rc = listen(fd, MG_SOCK_LISTEN_BACKLOG_SIZE)) != 0)) {
            // NOTE(lsm): FreeRTOS uses backlog value as a connection limit
            // In case port was set to 0, get the real port number
            MG_ERROR(("listen: %d", MG_SOCK_ERR(rc)));
        } else {
            setlocaddr(fd, &c->loc);
            mg_set_non_blocking_mode(fd);
            c->fd = S2PTR(fd);
            MG_EPOLL_ADD(c);
            success = true;
        }
    }
    if (success == false && fd != MG_INVALID_SOCKET)
        closesocket(fd);
    return success;
}

static long recv_raw(struct mg_connection* c, void* buf, size_t len)
{
    long n = 0;
    if (c->is_udp) {
        union usa usa;
        socklen_t slen = tousa(&c->rem, &usa);
        n = recvfrom(FD(c), (char*)buf, len, 0, &usa.sa, &slen);
        if (n > 0)
            tomgaddr(&usa, &c->rem, slen != sizeof(usa.sin));
    } else {
        n = recv(FD(c), (char*)buf, len, MSG_NONBLOCKING);
    }
    MG_VERBOSE(("%lu %ld %d", c->id, n, MG_SOCK_ERR(n)));
    if (MG_SOCK_PENDING(n))
        return MG_IO_WAIT;
    if (MG_SOCK_RESET(n))
        return MG_IO_RESET; // MbedTLS, see #1507
    if (n <= 0)
        return MG_IO_ERR;
    return n;
}

static bool ioalloc(struct mg_connection* c, struct mg_iobuf* io)
{
    bool res = false;
    if (io->len >= MG_MAX_RECV_SIZE) {
        mg_error(c, "MG_MAX_RECV_SIZE");
    } else if (io->size <= io->len
               && !mg_iobuf_resize(io, io->size + MG_IO_SIZE)) {
        mg_error(c, "OOM");
    } else {
        res = true;
    }
    return res;
}

// NOTE(lsm): do only one iteration of reads, cause some systems
// (e.g. FreeRTOS stack) return 0 instead of -1/EWOULDBLOCK when no data
static void read_conn(struct mg_connection* c)
{
    if (ioalloc(c, &c->recv)) {
        char* buf = (char*)&c->recv.buf[c->recv.len];
        size_t len = c->recv.size - c->recv.len;
        long n = -1;
        if (c->is_tls) {
            // Do not read to the raw TLS buffer if it already has enough.
            // This is to prevent overflowing c->rtls if our reads are slow
            long m;
            if (c->rtls.len
                < 16 * 1024 + 40) { // TLS record, header, MAC, padding
                if (!ioalloc(c, &c->rtls))
                    return;
                n = recv_raw(c, (char*)&c->rtls.buf[c->rtls.len],
                             c->rtls.size - c->rtls.len);
                if (n > 0)
                    c->rtls.len += (size_t)n;
            }
            // there can still be > 16K from last iteration, always
            // mg_tls_recv()
            m = c->is_tls_hs ? (long)MG_IO_WAIT : mg_tls_recv(c, buf, len);
            if (n == MG_IO_ERR || n == MG_IO_RESET) { // Windows, see #3031
                if (c->rtls.len == 0 || m < 0) {
                    // Close only when we have fully drained both rtls and TLS
                    // buffers
                    c->is_closing = 1; // or there's nothing we can do about
                                       // it.
                    if (m < 0)
                        m = MG_IO_ERR; // but return last record data, see
                                       // #3104
                } else {               // see #2885
                    // TLS buffer is capped to max record size, even though,
                    // there can be more than one record, give TLS a chance to
                    // process them.
                }
            } else if (c->is_tls_hs) {
                mg_tls_handshake(c);
            }
            n = m;
        } else {
            n = recv_raw(c, buf, len);
        }
        MG_DEBUG(("%lu %ld %lu:%lu:%lu %ld err %d", c->id, c->fd, c->send.len,
                  c->recv.len, c->rtls.len, n, MG_SOCK_ERR(n)));
        iolog(c, buf, n, true);
    }
}

static void write_conn(struct mg_connection* c)
{
    char* buf = (char*)c->send.buf;
    size_t len = c->send.len;
    long n = c->is_tls ? mg_tls_send(c, buf, len) : mg_io_send(c, buf, len);
    // TODO(): mg_tls_send() may return 0 forever on steady OOM
    MG_DEBUG(("%lu %ld snd %ld/%ld rcv %ld/%ld n=%ld err=%d", c->id, c->fd,
              (long)c->send.len, (long)c->send.size, (long)c->recv.len,
              (long)c->recv.size, n, MG_SOCK_ERR(n)));
    iolog(c, buf, n, false);
}

static void close_conn(struct mg_connection* c)
{
    if (FD(c) != MG_INVALID_SOCKET) {
#if MG_ENABLE_EPOLL
        epoll_ctl(c->mgr->epoll_fd, EPOLL_CTL_DEL, FD(c), NULL);
#endif
        closesocket(FD(c));
#if MG_ENABLE_FREERTOS_TCP
        FreeRTOS_FD_CLR(c->fd, c->mgr->ss, eSELECT_ALL);
#endif
    }
    mg_close_conn(c);
}

static void connect_conn(struct mg_connection* c)
{
    union usa usa;
    socklen_t n = sizeof(usa);
    // Use getpeername() to test whether we have connected
    if (getpeername(FD(c), &usa.sa, &n) == 0) {
        c->is_connecting = 0;
        setlocaddr(FD(c), &c->loc);
        mg_call(c, MG_EV_CONNECT, NULL);
        MG_EPOLL_MOD(c, 0);
        if (c->is_tls_hs)
            mg_tls_handshake(c);
        if (!c->is_tls_hs)
            c->is_tls = 0; // user did not call mg_tls_init()
    } else {
        mg_error(c, "socket error");
    }
}

static void setsockopts(struct mg_connection* c)
{
#if MG_ENABLE_FREERTOS_TCP || MG_ARCH == MG_ARCH_THREADX                      \
    || MG_ARCH == MG_ARCH_TIRTOS
    (void)c;
#else
    int on = 1;
#if !defined(SOL_TCP)
#define SOL_TCP IPPROTO_TCP
#endif
    if (setsockopt(FD(c), SOL_TCP, TCP_NODELAY, (char*)&on, sizeof(on)) != 0)
        (void)0;
    if (setsockopt(FD(c), SOL_SOCKET, SO_KEEPALIVE, (char*)&on, sizeof(on))
        != 0)
        (void)0;
#endif
}

void mg_connect_resolved(struct mg_connection* c)
{
    int type = c->is_udp ? SOCK_DGRAM : SOCK_STREAM;
    int proto = type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP;
    int rc, af = c->rem.is_ip6 ? AF_INET6 : AF_INET; // c->rem has resolved IP
    c->fd = S2PTR(socket(af, type, proto));          // Create outbound socket
    c->is_resolving = 0;                             // Clear resolving flag
    if (FD(c) == MG_INVALID_SOCKET) {
        mg_error(c, "socket(): %d", MG_SOCK_ERR(-1));
    } else if (c->is_udp) {
        MG_EPOLL_ADD(c);
#if MG_ARCH == MG_ARCH_TIRTOS
        union usa
            usa; // TI-RTOS NDK requires binding to receive on UDP sockets
        socklen_t slen = tousa(&c->loc, &usa);
        if ((rc = bind(c->fd, &usa.sa, slen)) != 0)
            MG_ERROR(("bind: %d", MG_SOCK_ERR(rc)));
#endif
        setlocaddr(FD(c), &c->loc);
        mg_call(c, MG_EV_RESOLVE, NULL);
        mg_call(c, MG_EV_CONNECT, NULL);
    } else {
        union usa usa;
        socklen_t slen = tousa(&c->rem, &usa);
        mg_set_non_blocking_mode(FD(c));
        setsockopts(c);
        MG_EPOLL_ADD(c);
        mg_call(c, MG_EV_RESOLVE, NULL);
        rc = connect(FD(c), &usa.sa, slen); // Attempt to connect
        if (rc == 0) {                      // Success
            setlocaddr(FD(c), &c->loc);
            mg_call(c, MG_EV_CONNECT, NULL); // Send MG_EV_CONNECT to the user
            if (!c->is_tls_hs)
                c->is_tls = 0;            // user did not call mg_tls_init()
        } else if (MG_SOCK_PENDING(rc)) { // Need to wait for TCP handshake
            MG_DEBUG(("%lu %ld -> %M pend", c->id, c->fd, mg_print_ip_port,
                      &c->rem));
            c->is_connecting = 1;
        } else {
            mg_error(c, "connect: %d", MG_SOCK_ERR(rc));
        }
    }
}

static MG_SOCKET_TYPE raccept(MG_SOCKET_TYPE sock, union usa* usa,
                              socklen_t* len)
{
    MG_SOCKET_TYPE fd = MG_INVALID_SOCKET;
    do {
        memset(usa, 0, sizeof(*usa));
        fd = accept(sock, &usa->sa, len);
    } while (MG_SOCK_INTR(fd));
    return fd;
}

static void accept_conn(struct mg_mgr* mgr, struct mg_connection* lsn)
{
    struct mg_connection* c = NULL;
    union usa usa;
    socklen_t sa_len = sizeof(usa);
    MG_SOCKET_TYPE fd = raccept(FD(lsn), &usa, &sa_len);
    if (fd == MG_INVALID_SOCKET) {
#if MG_ARCH == MG_ARCH_THREADX || defined(__ECOS)
        // NetxDuo, in non-block socket mode can mark listening socket readable
        // even it is not. See comment for 'select' func implementation in
        // nx_bsd.c That's not an error, just should try later
        if (errno != EAGAIN)
#endif
            MG_ERROR(
                ("%lu accept failed, errno %d", lsn->id, MG_SOCK_ERR(-1)));
#if (MG_ARCH != MG_ARCH_WIN32) && !MG_ENABLE_FREERTOS_TCP                     \
    && (MG_ARCH != MG_ARCH_TIRTOS) && !MG_ENABLE_POLL && !MG_ENABLE_EPOLL
    } else if ((long)fd >= FD_SETSIZE) {
        MG_ERROR(("%ld > %ld", (long)fd, (long)FD_SETSIZE));
        closesocket(fd);
#endif
    } else if ((c = mg_alloc_conn(mgr)) == NULL) {
        MG_ERROR(("%lu OOM", lsn->id));
        closesocket(fd);
    } else {
        tomgaddr(&usa, &c->rem, sa_len != sizeof(usa.sin));
        LIST_ADD_HEAD(struct mg_connection, &mgr->conns, c);
        c->fd = S2PTR(fd);
        MG_EPOLL_ADD(c);
        mg_set_non_blocking_mode(FD(c));
        setsockopts(c);
        c->is_accepted = 1;
        c->is_hexdumping = lsn->is_hexdumping;
        setlocaddr(fd,
                   &c->loc); // set local addr to where the client connected to
        c->pfn = lsn->pfn;
        c->pfn_data = lsn->pfn_data;
        c->fn = lsn->fn;
        c->fn_data = lsn->fn_data;
        c->is_tls = lsn->is_tls;
        MG_DEBUG(("%lu %ld accepted %M -> %M", c->id, c->fd, mg_print_ip_port,
                  &c->rem, mg_print_ip_port, &c->loc));
        mg_call(c, MG_EV_OPEN, NULL);
        mg_call(c, MG_EV_ACCEPT, NULL);
        if (!c->is_tls_hs)
            c->is_tls = 0; // user did not call mg_tls_init()
    }
}

static bool can_read(const struct mg_connection* c)
{
    return c->is_full == false;
}

static bool can_write(const struct mg_connection* c)
{
    return c->is_connecting || (c->send.len > 0 && c->is_tls_hs == 0);
}

static bool skip_iotest(const struct mg_connection* c)
{
    return (c->is_closing || c->is_resolving || FD(c) == MG_INVALID_SOCKET)
        || (can_read(c) == false && can_write(c) == false);
}

static void mg_iotest(struct mg_mgr* mgr, int ms)
{
#if MG_ENABLE_FREERTOS_TCP
    struct mg_connection* c;
    for (c = mgr->conns; c != NULL; c = c->next) {
        c->is_readable = c->is_writable = 0;
        if (skip_iotest(c))
            continue;
        if (can_read(c))
            FreeRTOS_FD_SET(c->fd, mgr->ss, eSELECT_READ | eSELECT_EXCEPT);
        if (can_write(c))
            FreeRTOS_FD_SET(c->fd, mgr->ss, eSELECT_WRITE);
        if (c->is_closing)
            ms = 1;
    }
    FreeRTOS_select(mgr->ss, pdMS_TO_TICKS(ms));
    for (c = mgr->conns; c != NULL; c = c->next) {
        EventBits_t bits = FreeRTOS_FD_ISSET(c->fd, mgr->ss);
        c->is_readable = bits & (eSELECT_READ | eSELECT_EXCEPT) ? 1U : 0;
        c->is_writable = bits & eSELECT_WRITE ? 1U : 0;
        if (c->fd != MG_INVALID_SOCKET)
            FreeRTOS_FD_CLR(c->fd, mgr->ss,
                            eSELECT_READ | eSELECT_EXCEPT | eSELECT_WRITE);
    }
#elif MG_ENABLE_EPOLL
    size_t max = 1;
    for (struct mg_connection* c = mgr->conns; c != NULL; c = c->next) {
        c->is_readable = c->is_writable = 0;
        if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
            ms = 1, c->is_readable = 1;
        if (can_write(c))
            MG_EPOLL_MOD(c, 1);
        if (c->is_closing)
            ms = 1;
        max++;
    }
    struct epoll_event* evs = (struct epoll_event*)alloca(max
                                                          * sizeof(evs[0]));
    int n = epoll_wait(mgr->epoll_fd, evs, (int)max, ms);
    for (int i = 0; i < n; i++) {
        struct mg_connection* c = (struct mg_connection*)evs[i].data.ptr;
        if (evs[i].events & EPOLLERR) {
            mg_error(c, "socket error");
        } else if (c->is_readable == 0) {
            bool rd = evs[i].events & (EPOLLIN | EPOLLHUP);
            bool wr = evs[i].events & EPOLLOUT;
            c->is_readable = can_read(c) && rd ? 1U : 0;
            c->is_writable = can_write(c) && wr ? 1U : 0;
            if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
                c->is_readable = 1;
        }
    }
    (void)skip_iotest;
#elif MG_ENABLE_POLL
    nfds_t n = 0;
    for (struct mg_connection* c = mgr->conns; c != NULL; c = c->next)
        n++;
    struct pollfd* fds = (struct pollfd*)alloca(n * sizeof(fds[0]));
    memset(fds, 0, n * sizeof(fds[0]));
    n = 0;
    for (struct mg_connection* c = mgr->conns; c != NULL; c = c->next) {
        c->is_readable = c->is_writable = 0;
        if (c->is_closing)
            ms = 1;
        if (skip_iotest(c)) {
            // Socket not valid, ignore
        } else {
            // Don't wait if TLS is ready
            if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
                ms = 1;
            fds[n].fd = FD(c);
            if (can_read(c))
                fds[n].events |= POLLIN;
            if (can_write(c))
                fds[n].events |= POLLOUT;
            n++;
        }
    }

    // MG_INFO(("poll n=%d ms=%d", (int) n, ms));
    if (poll(fds, n, ms) < 0) {
#if MG_ARCH == MG_ARCH_WIN32
        if (n == 0)
            Sleep(ms); // On Windows, poll fails if no sockets
#endif
        memset(fds, 0, n * sizeof(fds[0]));
    }
    n = 0;
    for (struct mg_connection* c = mgr->conns; c != NULL; c = c->next) {
        if (skip_iotest(c)) {
            // Socket not valid, ignore
        } else {
            if (fds[n].revents & POLLERR) {
                mg_error(c, "socket error");
            } else {
                c->is_readable = (unsigned)(fds[n].revents & (POLLIN | POLLHUP)
                                                ? 1
                                                : 0);
                c->is_writable = (unsigned)(fds[n].revents & POLLOUT ? 1 : 0);
                if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
                    c->is_readable = 1;
            }
            n++;
        }
    }
#else
    struct timeval tv = { ms / 1000, (ms % 1000) * 1000 },
                   tv_1ms = { 0, 1000 }, *tvp;
    struct mg_connection* c;
    fd_set rset, wset, eset;
    MG_SOCKET_TYPE maxfd = 0;
    int rc;

    FD_ZERO(&rset);
    FD_ZERO(&wset);
    FD_ZERO(&eset);
    tvp = ms < 0 ? NULL : &tv;
    for (c = mgr->conns; c != NULL; c = c->next) {
        c->is_readable = c->is_writable = 0;
        if (skip_iotest(c))
            continue;
        FD_SET(FD(c), &eset);
        if (can_read(c))
            FD_SET(FD(c), &rset);
        if (can_write(c))
            FD_SET(FD(c), &wset);
        if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
            tvp = &tv_1ms;
        if (FD(c) > maxfd)
            maxfd = FD(c);
        if (c->is_closing)
            tvp = &tv_1ms;
    }

    if ((rc = select((int)maxfd + 1, &rset, &wset, &eset, tvp)) <= 0) {
#if MG_ARCH == MG_ARCH_WIN32
        if (maxfd == 0)
            Sleep(ms); // On Windows, select fails if no sockets
#else
        if (rc < 0)
            MG_ERROR(("select: %d %d", rc, MG_SOCK_ERR(rc)));
#endif
        FD_ZERO(&rset);
        FD_ZERO(&wset);
        FD_ZERO(&eset);
    }

    for (c = mgr->conns; c != NULL; c = c->next) {
        if (FD(c) != MG_INVALID_SOCKET && FD_ISSET(FD(c), &eset)) {
#if MG_ARCH == MG_ARCH_THREADX
            // NetxDuo stack returns exceptions for listening connection after
            // accept
            if (c->is_listening == 0)
                mg_error(c, "socket error");
#else
            mg_error(c, "socket error");
#endif
        } else {
            c->is_readable = FD(c) != MG_INVALID_SOCKET
                && FD_ISSET(FD(c), &rset);
            c->is_writable = FD(c) != MG_INVALID_SOCKET
                && FD_ISSET(FD(c), &wset);
            if (c->rtls.len > 0 || mg_tls_pending(c) > 0)
                c->is_readable = 1;
        }
    }
#endif
}

static bool mg_socketpair(MG_SOCKET_TYPE sp[2], union usa usa[2])
{
    socklen_t n = sizeof(usa[0].sin);
    bool success = false;

    sp[0] = sp[1] = MG_INVALID_SOCKET;
    (void)memset(&usa[0], 0, sizeof(usa[0]));
    usa[0].sin.sin_family = AF_INET;
    *(uint32_t*)&usa->sin.sin_addr = mg_htonl(0x7f000001U); // 127.0.0.1
    usa[1] = usa[0];

    if ((sp[0] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) != MG_INVALID_SOCKET
        && (sp[1] = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP))
            != MG_INVALID_SOCKET
        && bind(sp[0], &usa[0].sa, n) == 0 &&      //
        bind(sp[1], &usa[1].sa, n) == 0 &&         //
        getsockname(sp[0], &usa[0].sa, &n) == 0 && //
        getsockname(sp[1], &usa[1].sa, &n) == 0 && //
        connect(sp[0], &usa[1].sa, n) == 0 &&      //
        connect(sp[1], &usa[0].sa, n) == 0) {      //
        success = true;
    }
    if (!success) {
        if (sp[0] != MG_INVALID_SOCKET)
            closesocket(sp[0]);
        if (sp[1] != MG_INVALID_SOCKET)
            closesocket(sp[1]);
        sp[0] = sp[1] = MG_INVALID_SOCKET;
    }
    return success;
}

// mg_wakeup() event handler
static void wufn(struct mg_connection* c, int ev, void* ev_data)
{
    if (ev == MG_EV_READ) {
        unsigned long* id = (unsigned long*)c->recv.buf;
        // MG_INFO(("Got data"));
        // mg_hexdump(c->recv.buf, c->recv.len);
        if (c->recv.len >= sizeof(*id)) {
            struct mg_connection* t;
            for (t = c->mgr->conns; t != NULL; t = t->next) {
                if (t->id == *id) {
                    struct mg_str data = mg_str_n((char*)c->recv.buf
                                                      + sizeof(*id),
                                                  c->recv.len - sizeof(*id));
                    mg_call(t, MG_EV_WAKEUP, &data);
                }
            }
        }
        c->recv.len = 0; // Consume received data
    } else if (ev == MG_EV_CLOSE) {
        closesocket(c->mgr->pipe); // When we're closing, close the other
        c->mgr->pipe = MG_INVALID_SOCKET; // side of the socketpair, too
    }
    (void)ev_data;
}

bool mg_wakeup_init(struct mg_mgr* mgr)
{
    bool ok = false;
    if (mgr->pipe == MG_INVALID_SOCKET) {
        union usa usa[2];
        MG_SOCKET_TYPE sp[2] = { MG_INVALID_SOCKET, MG_INVALID_SOCKET };
        struct mg_connection* c = NULL;
        if (!mg_socketpair(sp, usa)) {
            MG_ERROR(("Cannot create socket pair"));
        } else if ((c = mg_wrapfd(mgr, (int)sp[1], wufn, NULL)) == NULL) {
            closesocket(sp[0]);
            closesocket(sp[1]);
            sp[0] = sp[1] = MG_INVALID_SOCKET;
        } else {
            tomgaddr(&usa[0], &c->rem, false);
            MG_DEBUG(("%lu %p pipe %lu", c->id, c->fd, (unsigned long)sp[0]));
            mgr->pipe = sp[0];
            ok = true;
        }
    }
    return ok;
}

bool mg_wakeup(struct mg_mgr* mgr, unsigned long conn_id, const void* buf,
               size_t len)
{
    if (mgr->pipe != MG_INVALID_SOCKET && conn_id > 0) {
        char* extended_buf = (char*)alloca(len + sizeof(conn_id));
        memcpy(extended_buf, &conn_id, sizeof(conn_id));
        memcpy(extended_buf + sizeof(conn_id), buf, len);
        send(mgr->pipe, extended_buf, len + sizeof(conn_id), MSG_NONBLOCKING);
        return true;
    }
    return false;
}

void mg_mgr_poll(struct mg_mgr* mgr, int ms)
{
    struct mg_connection *c, *tmp;
    uint64_t now;

    mg_iotest(mgr, ms);
    now = mg_millis();
    mg_timer_poll(&mgr->timers, now);

    for (c = mgr->conns; c != NULL; c = tmp) {
        bool is_resp = c->is_resp;
        tmp = c->next;
        mg_call(c, MG_EV_POLL, &now);
        if (is_resp && !c->is_resp) {
            long n = 0;
            mg_call(c, MG_EV_READ, &n);
        }
        MG_VERBOSE(("%lu %c%c %c%c%c%c%c %lu %lu", c->id,
                    c->is_readable ? 'r' : '-', c->is_writable ? 'w' : '-',
                    c->is_tls ? 'T' : 't', c->is_connecting ? 'C' : 'c',
                    c->is_tls_hs ? 'H' : 'h', c->is_resolving ? 'R' : 'r',
                    c->is_closing ? 'C' : 'c', mg_tls_pending(c),
                    c->rtls.len));
        if (c->is_resolving || c->is_closing) {
            // Do nothing
        } else if (c->is_listening && c->is_udp == 0) {
            if (c->is_readable)
                accept_conn(mgr, c);
        } else if (c->is_connecting) {
            if (c->is_readable || c->is_writable)
                connect_conn(c);
        } else {
            if (c->is_readable)
                read_conn(c);
            if (c->is_writable)
                write_conn(c);
            if (c->is_tls && !c->is_tls_hs && c->send.len == 0)
                mg_tls_flush(c);
        }

        if (c->is_draining && c->send.len == 0)
            c->is_closing = 1;
        if (c->is_closing)
            close_conn(c);
    }
}

#endif // MG_ENABLE_SOCKET

// ---- module: str ----

struct mg_str mg_str_s(const char* s)
{
    struct mg_str str;
    str.buf = (char*)s, str.len = (s == NULL) ? 0 : strlen(s);
    return str;
}

struct mg_str mg_str_n(const char* s, size_t n)
{
    struct mg_str str;
    str.buf = (char*)s, str.len = n;
    return str;
}

static int mg_tolc(char c)
{
    return (c >= 'A' && c <= 'Z') ? c + 'a' - 'A' : c;
}

int mg_casecmp(const char* s1, const char* s2)
{
    int diff = 0;
    do {
        int c = mg_tolc(*s1++), d = mg_tolc(*s2++);
        diff = c - d;
    } while (diff == 0 && s1[-1] != '\0');
    return diff;
}

struct mg_str mg_strdup(const struct mg_str s)
{
    struct mg_str r = { NULL, 0 };
    if (s.len > 0 && s.buf != NULL) {
        char* sc = (char*)mg_calloc(1, s.len + 1);
        if (sc != NULL) {
            memcpy(sc, s.buf, s.len);
            sc[s.len] = '\0';
            r.buf = sc;
            r.len = s.len;
        }
    }
    return r;
}

int mg_strcmp(const struct mg_str str1, const struct mg_str str2)
{
    size_t i = 0;
    while (i < str1.len && i < str2.len) {
        int c1 = str1.buf[i];
        int c2 = str2.buf[i];
        if (c1 < c2)
            return -1;
        if (c1 > c2)
            return 1;
        i++;
    }
    if (i < str1.len)
        return 1;
    if (i < str2.len)
        return -1;
    return 0;
}

int mg_strcasecmp(const struct mg_str str1, const struct mg_str str2)
{
    size_t i = 0;
    while (i < str1.len && i < str2.len) {
        int c1 = mg_tolc(str1.buf[i]);
        int c2 = mg_tolc(str2.buf[i]);
        if (c1 < c2)
            return -1;
        if (c1 > c2)
            return 1;
        i++;
    }
    if (i < str1.len)
        return 1;
    if (i < str2.len)
        return -1;
    return 0;
}

bool mg_match(struct mg_str s, struct mg_str p, struct mg_str* caps)
{
    size_t i = 0, j = 0, ni = 0, nj = 0;
    if (caps)
        caps->buf = NULL, caps->len = 0;
    while (i < p.len || j < s.len) {
        if (i < p.len && j < s.len
            && (p.buf[i] == '?'
                || (p.buf[i] != '*' && p.buf[i] != '#'
                    && s.buf[j] == p.buf[i]))) {
            if (caps == NULL) {
            } else if (p.buf[i] == '?') {
                caps->buf = &s.buf[j], caps->len = 1;    // Finalize `?` cap
                caps++, caps->buf = NULL, caps->len = 0; // Init next cap
            } else if (caps->buf != NULL && caps->len == 0) {
                caps->len = (size_t)(&s.buf[j]
                                     - caps->buf); // Finalize current cap
                caps++, caps->len = 0, caps->buf = NULL; // Init next cap
            }
            i++, j++;
        } else if (i < p.len && (p.buf[i] == '*' || p.buf[i] == '#')) {
            if (caps && !caps->buf)
                caps->len = 0, caps->buf = &s.buf[j]; // Init cap
            ni = i++, nj = j + 1;
        } else if (nj > 0 && nj <= s.len
                   && ((ni < p.len && p.buf[ni] == '#')
                       || (j < s.len && s.buf[j] != '/'))) {
            i = ni, j = nj;
            if (caps && caps->buf == NULL && caps->len == 0) {
                caps--, caps->len = 0; // Restart previous cap
            }
        } else {
            return false;
        }
    }
    if (caps && caps->buf && caps->len == 0) {
        caps->len = (size_t)(&s.buf[j] - caps->buf);
    }
    return true;
}

bool mg_span(struct mg_str s, struct mg_str* a, struct mg_str* b, char sep)
{
    if (s.len == 0 || s.buf == NULL) {
        return false; // Empty string, nothing to span - fail
    } else {
        size_t len = 0;
        while (len < s.len && s.buf[len] != sep)
            len++; // Find separator
        if (a)
            *a = mg_str_n(s.buf, len); // Init a
        if (b)
            *b = mg_str_n(s.buf + len, s.len - len); // Init b
        if (b && len < s.len)
            b->buf++, b->len--; // Skip separator
        return true;
    }
}

bool mg_str_to_num(struct mg_str str, int base, void* val, size_t val_len)
{
    size_t i = 0, ndigits = 0;
    uint64_t max = val_len == sizeof(uint8_t) ? 0xFF
        : val_len == sizeof(uint16_t)         ? 0xFFFF
        : val_len == sizeof(uint32_t)         ? 0xFFFFFFFF
                                              : (uint64_t)~0;
    uint64_t result = 0;
    if (max == (uint64_t)~0 && val_len != sizeof(uint64_t))
        return false;
    if (base == 0 && str.len >= 2) {
        if (str.buf[i] == '0') {
            i++;
            base = str.buf[i] == 'b' ? 2 : str.buf[i] == 'x' ? 16 : 10;
            if (base != 10)
                ++i;
        } else {
            base = 10;
        }
    }
    switch (base) {
    case 2:
        while (i < str.len && (str.buf[i] == '0' || str.buf[i] == '1')) {
            uint64_t digit = (uint64_t)(str.buf[i] - '0');
            if (result > max / 2)
                return false; // Overflow
            result *= 2;
            if (result > max - digit)
                return false; // Overflow
            result += digit;
            i++, ndigits++;
        }
        break;
    case 10:
        while (i < str.len && str.buf[i] >= '0' && str.buf[i] <= '9') {
            uint64_t digit = (uint64_t)(str.buf[i] - '0');
            if (result > max / 10)
                return false; // Overflow
            result *= 10;
            if (result > max - digit)
                return false; // Overflow
            result += digit;
            i++, ndigits++;
        }
        break;
    case 16:
        while (i < str.len) {
            char c = str.buf[i];
            uint64_t digit = (c >= '0' && c <= '9') ? (uint64_t)(c - '0')
                : (c >= 'A' && c <= 'F')            ? (uint64_t)(c - '7')
                : (c >= 'a' && c <= 'f')            ? (uint64_t)(c - 'W')
                                                    : (uint64_t)~0;
            if (digit == (uint64_t)~0)
                break;
            if (result > max / 16)
                return false; // Overflow
            result *= 16;
            if (result > max - digit)
                return false; // Overflow
            result += digit;
            i++, ndigits++;
        }
        break;
    default:
        return false;
    }
    if (ndigits == 0)
        return false;
    if (i != str.len)
        return false;
    if (val_len == 1) {
        *((uint8_t*)val) = (uint8_t)result;
    } else if (val_len == 2) {
        *((uint16_t*)val) = (uint16_t)result;
    } else if (val_len == 4) {
        *((uint32_t*)val) = (uint32_t)result;
    } else {
        *((uint64_t*)val) = (uint64_t)result;
    }
    return true;
}

// ---- module: timer ----

void mg_timer_init(struct mg_timer** head, struct mg_timer* t, uint64_t ms,
                   unsigned flags, void (*fn)(void*), void* arg)
{
    t->period_ms = ms, t->expire = 0;
    t->flags = flags, t->fn = fn, t->arg = arg, t->next = *head;
    *head = t;
}

void mg_timer_free(struct mg_timer** head, struct mg_timer* t)
{
    while (*head && *head != t)
        head = &(*head)->next;
    if (*head)
        *head = t->next;
}

// t: expiration time, prd: period, now: current time. Return true if expired
bool mg_timer_expired(uint64_t* t, uint64_t prd, uint64_t now)
{
    if (now + prd < *t)
        *t = 0; // Time wrapped? Reset timer
    if (*t == 0)
        *t = now + prd; // Firt poll? Set expiration
    if (*t > now)
        return false;                             // Not expired yet, return
    *t = (now - *t) > prd ? now + prd : *t + prd; // Next expiration time
    return true;                                  // Expired, return true
}

void mg_timer_poll(struct mg_timer** head, uint64_t now_ms)
{
    struct mg_timer *t, *tmp;
    for (t = *head; t != NULL; t = tmp) {
        bool once = t->expire == 0 && (t->flags & MG_TIMER_RUN_NOW)
            && !(t->flags & MG_TIMER_CALLED); // Handle MG_TIMER_NOW only once
        bool expired = mg_timer_expired(&t->expire, t->period_ms, now_ms);
        tmp = t->next;
        if (!once && !expired)
            continue;
        if ((t->flags & MG_TIMER_REPEAT) || !(t->flags & MG_TIMER_CALLED)) {
            t->fn(t->arg);
        }
        t->flags |= MG_TIMER_CALLED;

        // If this timer is not repeating and marked AUTODELETE, remove it
        if (!(t->flags & MG_TIMER_REPEAT)
            && (t->flags & MG_TIMER_AUTODELETE)) {
            mg_timer_free(head, t);
            mg_free(t);
        }
    }
}

// ---- module: tls_dummy ----
// TLS stubs (all no-ops) -- included unconditionally since nanosrv has no TLS

void mg_tls_init(struct mg_connection* c, const struct mg_tls_opts* opts)
{
    (void)opts;
    mg_error(c, "TLS is not enabled");
}

void mg_tls_handshake(struct mg_connection* c) { (void)c; }

void mg_tls_free(struct mg_connection* c) { (void)c; }

long mg_tls_recv(struct mg_connection* c, void* buf, size_t len)
{
    return c == NULL || buf == NULL || len == 0 ? 0 : -1;
}

long mg_tls_send(struct mg_connection* c, const void* buf, size_t len)
{
    return c == NULL || buf == NULL || len == 0 ? 0 : -1;
}

size_t mg_tls_pending(struct mg_connection* c)
{
    (void)c;
    return 0;
}

void mg_tls_flush(struct mg_connection* c) { (void)c; }

void mg_tls_ctx_init(struct mg_mgr* mgr) { (void)mgr; }

void mg_tls_ctx_free(struct mg_mgr* mgr) { (void)mgr; }

// ---- module: sha1 ----
/* Copyright(c) By Steve Reid <steve@edmweb.com> */
/* 100% Public Domain */

union char64long16 {
    unsigned char c[64];
    uint32_t l[16];
};

#define rol(value, bits) (((value) << (bits)) | ((value) >> (32 - (bits))))

static uint32_t blk0(union char64long16* block, int i)
{
    if (MG_BIG_ENDIAN) {
    } else {
        block->l[i] = (rol(block->l[i], 24) & 0xFF00FF00)
            | (rol(block->l[i], 8) & 0x00FF00FF);
    }
    return block->l[i];
}

#undef blk
#undef R0
#undef R1
#undef R2
#undef R3
#undef R4

#define blk(i)                                                                \
    (block->l[i & 15] = rol(block->l[(i + 13) & 15] ^ block->l[(i + 8) & 15]  \
                                ^ block->l[(i + 2) & 15] ^ block->l[i & 15],  \
                            1))
#define R0(v, w, x, y, z, i)                                                  \
    z += ((w & (x ^ y)) ^ y) + blk0(block, i) + 0x5A827999 + rol(v, 5);       \
    w = rol(w, 30);
#define R1(v, w, x, y, z, i)                                                  \
    z += ((w & (x ^ y)) ^ y) + blk(i) + 0x5A827999 + rol(v, 5);               \
    w = rol(w, 30);
#define R2(v, w, x, y, z, i)                                                  \
    z += (w ^ x ^ y) + blk(i) + 0x6ED9EBA1 + rol(v, 5);                       \
    w = rol(w, 30);
#define R3(v, w, x, y, z, i)                                                  \
    z += (((w | x) & y) | (w & x)) + blk(i) + 0x8F1BBCDC + rol(v, 5);         \
    w = rol(w, 30);
#define R4(v, w, x, y, z, i)                                                  \
    z += (w ^ x ^ y) + blk(i) + 0xCA62C1D6 + rol(v, 5);                       \
    w = rol(w, 30);

static void mg_sha1_transform(uint32_t state[5], const unsigned char* buffer)
{
    uint32_t a, b, c, d, e;
    union char64long16 block[1];

    memcpy(block, buffer, 64);
    a = state[0];
    b = state[1];
    c = state[2];
    d = state[3];
    e = state[4];
    R0(a, b, c, d, e, 0);
    R0(e, a, b, c, d, 1);
    R0(d, e, a, b, c, 2);
    R0(c, d, e, a, b, 3);
    R0(b, c, d, e, a, 4);
    R0(a, b, c, d, e, 5);
    R0(e, a, b, c, d, 6);
    R0(d, e, a, b, c, 7);
    R0(c, d, e, a, b, 8);
    R0(b, c, d, e, a, 9);
    R0(a, b, c, d, e, 10);
    R0(e, a, b, c, d, 11);
    R0(d, e, a, b, c, 12);
    R0(c, d, e, a, b, 13);
    R0(b, c, d, e, a, 14);
    R0(a, b, c, d, e, 15);
    R1(e, a, b, c, d, 16);
    R1(d, e, a, b, c, 17);
    R1(c, d, e, a, b, 18);
    R1(b, c, d, e, a, 19);
    R2(a, b, c, d, e, 20);
    R2(e, a, b, c, d, 21);
    R2(d, e, a, b, c, 22);
    R2(c, d, e, a, b, 23);
    R2(b, c, d, e, a, 24);
    R2(a, b, c, d, e, 25);
    R2(e, a, b, c, d, 26);
    R2(d, e, a, b, c, 27);
    R2(c, d, e, a, b, 28);
    R2(b, c, d, e, a, 29);
    R2(a, b, c, d, e, 30);
    R2(e, a, b, c, d, 31);
    R2(d, e, a, b, c, 32);
    R2(c, d, e, a, b, 33);
    R2(b, c, d, e, a, 34);
    R2(a, b, c, d, e, 35);
    R2(e, a, b, c, d, 36);
    R2(d, e, a, b, c, 37);
    R2(c, d, e, a, b, 38);
    R2(b, c, d, e, a, 39);
    R3(a, b, c, d, e, 40);
    R3(e, a, b, c, d, 41);
    R3(d, e, a, b, c, 42);
    R3(c, d, e, a, b, 43);
    R3(b, c, d, e, a, 44);
    R3(a, b, c, d, e, 45);
    R3(e, a, b, c, d, 46);
    R3(d, e, a, b, c, 47);
    R3(c, d, e, a, b, 48);
    R3(b, c, d, e, a, 49);
    R3(a, b, c, d, e, 50);
    R3(e, a, b, c, d, 51);
    R3(d, e, a, b, c, 52);
    R3(c, d, e, a, b, 53);
    R3(b, c, d, e, a, 54);
    R3(a, b, c, d, e, 55);
    R3(e, a, b, c, d, 56);
    R3(d, e, a, b, c, 57);
    R3(c, d, e, a, b, 58);
    R3(b, c, d, e, a, 59);
    R4(a, b, c, d, e, 60);
    R4(e, a, b, c, d, 61);
    R4(d, e, a, b, c, 62);
    R4(c, d, e, a, b, 63);
    R4(b, c, d, e, a, 64);
    R4(a, b, c, d, e, 65);
    R4(e, a, b, c, d, 66);
    R4(d, e, a, b, c, 67);
    R4(c, d, e, a, b, 68);
    R4(b, c, d, e, a, 69);
    R4(a, b, c, d, e, 70);
    R4(e, a, b, c, d, 71);
    R4(d, e, a, b, c, 72);
    R4(c, d, e, a, b, 73);
    R4(b, c, d, e, a, 74);
    R4(a, b, c, d, e, 75);
    R4(e, a, b, c, d, 76);
    R4(d, e, a, b, c, 77);
    R4(c, d, e, a, b, 78);
    R4(b, c, d, e, a, 79);
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    memset(block, 0, sizeof(block));
    a = b = c = d = e = 0;
    (void)a;
    (void)b;
    (void)c;
    (void)d;
    (void)e;
}

void mg_sha1_init(mg_sha1_ctx* context)
{
    context->state[0] = 0x67452301;
    context->state[1] = 0xEFCDAB89;
    context->state[2] = 0x98BADCFE;
    context->state[3] = 0x10325476;
    context->state[4] = 0xC3D2E1F0;
    context->count[0] = context->count[1] = 0;
}

void mg_sha1_update(mg_sha1_ctx* context, const unsigned char* data,
                    size_t len)
{
    size_t i, j;

    j = context->count[0];
    if ((context->count[0] += (uint32_t)len << 3) < j)
        context->count[1]++;
    context->count[1] += (uint32_t)(len >> 29);
    j = (j >> 3) & 63;
    if ((j + len) > 63) {
        memcpy(&context->buffer[j], data, (i = 64 - j));
        mg_sha1_transform(context->state, context->buffer);
        for (; i + 63 < len; i += 64) {
            mg_sha1_transform(context->state, &data[i]);
        }
        j = 0;
    } else
        i = 0;
    memcpy(&context->buffer[j], &data[i], len - i);
}

void mg_sha1_final(unsigned char digest[20], mg_sha1_ctx* context)
{
    unsigned i;
    unsigned char finalcount[8], c;

    for (i = 0; i < 8; i++) {
        finalcount[i] = (unsigned char)((context->count[(i >= 4 ? 0 : 1)]
                                         >> ((3 - (i & 3)) * 8))
                                        & 255);
    }
    c = 0200;
    mg_sha1_update(context, &c, 1);
    while ((context->count[0] & 504) != 448) {
        c = 0000;
        mg_sha1_update(context, &c, 1);
    }
    mg_sha1_update(context, finalcount, 8);
    for (i = 0; i < 20; i++) {
        digest[i] = (unsigned char)((context->state[i >> 2]
                                     >> ((3 - (i & 3)) * 8))
                                    & 255);
    }
    memset(context, '\0', sizeof(*context));
    memset(&finalcount, '\0', sizeof(finalcount));
}

// ---- module: ws ----

struct ws_msg {
    uint8_t flags;
    size_t header_len;
    size_t data_len;
};

size_t mg_ws_vprintf(struct mg_connection* c, int op, const char* fmt,
                     va_list* ap)
{
    size_t len = c->send.len;
    size_t n = mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, ap);
    mg_ws_wrap(c, c->send.len - len, op);
    return n;
}

size_t mg_ws_printf(struct mg_connection* c, int op, const char* fmt, ...)
{
    size_t len = 0;
    va_list ap;
    va_start(ap, fmt);
    len = mg_ws_vprintf(c, op, fmt, &ap);
    va_end(ap);
    return len;
}

static void ws_handshake(struct mg_connection* c, const struct mg_str* wskey,
                         const struct mg_str* wsproto, const char* fmt,
                         va_list* ap)
{
    const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20], b64_sha[30];

    mg_sha1_ctx sha_ctx;
    mg_sha1_init(&sha_ctx);
    mg_sha1_update(&sha_ctx, (unsigned char*)wskey->buf, wskey->len);
    mg_sha1_update(&sha_ctx, (unsigned char*)magic, 36);
    mg_sha1_final(sha, &sha_ctx);
    mg_base64_encode(sha, sizeof(sha), (char*)b64_sha, sizeof(b64_sha));
    mg_xprintf(mg_pfn_iobuf, &c->send,
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n",
               b64_sha);
    if (fmt != NULL)
        mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, ap);
    if (wsproto != NULL) {
        mg_printf(c, "Sec-WebSocket-Protocol: %.*s\r\n", (int)wsproto->len,
                  wsproto->buf);
    }
    if (!mg_send(c, "\r\n", 2))
        mg_error(c, "OOM");
}

static uint32_t be32(const uint8_t* p)
{
    return (((uint32_t)p[3]) << 0) | (((uint32_t)p[2]) << 8)
        | (((uint32_t)p[1]) << 16) | (((uint32_t)p[0]) << 24);
}

static size_t ws_process(uint8_t* buf, size_t len, struct ws_msg* msg)
{
    size_t i, n = 0, mask_len = 0;
    memset(msg, 0, sizeof(*msg));
    if (len >= 2) {
        n = buf[1] & 0x7f;
        mask_len = buf[1] & 128 ? 4 : 0;
        msg->flags = buf[0];
        if (n < 126 && len >= mask_len) {
            msg->data_len = n;
            msg->header_len = 2 + mask_len;
        } else if (n == 126 && len >= 4 + mask_len) {
            msg->header_len = 4 + mask_len;
            msg->data_len = (((size_t)buf[2]) << 8) | buf[3];
        } else if (len >= 10 + mask_len) {
            msg->header_len = 10 + mask_len;
            msg->data_len = (size_t)(((uint64_t)be32(buf + 2) << 32)
                                     + be32(buf + 6));
        }
    }
    if (msg->data_len > 1024 * 1024 * 1024)
        return 0;
    if (msg->header_len + msg->data_len > len)
        return 0;
    if (mask_len > 0) {
        uint8_t *p = buf + msg->header_len, *m = p - mask_len;
        for (i = 0; i < msg->data_len; i++)
            p[i] ^= m[i & 3];
    }
    return msg->header_len + msg->data_len;
}

static size_t mkhdr(size_t len, int op, bool is_client, uint8_t* buf)
{
    size_t n = 0;
    buf[0] = (uint8_t)(op | 128);
    if (len < 126) {
        buf[1] = (unsigned char)len;
        n = 2;
    } else if (len < 65536) {
        uint16_t tmp = mg_htons((uint16_t)len);
        buf[1] = 126;
        memcpy(&buf[2], &tmp, sizeof(tmp));
        n = 4;
    } else {
        uint32_t tmp;
        buf[1] = 127;
        tmp = mg_htonl((uint32_t)(((uint64_t)len) >> 32));
        memcpy(&buf[2], &tmp, sizeof(tmp));
        tmp = mg_htonl((uint32_t)(len & 0xffffffffU));
        memcpy(&buf[6], &tmp, sizeof(tmp));
        n = 10;
    }
    if (is_client) {
        buf[1] |= 1 << 7;
        mg_random(&buf[n], 4);
        n += 4;
    }
    return n;
}

static void mg_ws_mask(struct mg_connection* c, size_t len)
{
    if (c->is_client && c->send.buf != NULL) {
        size_t i;
        uint8_t *p = c->send.buf + c->send.len - len, *mask = p - 4;
        for (i = 0; i < len; i++)
            p[i] ^= mask[i & 3];
    }
}

size_t mg_ws_send(struct mg_connection* c, const void* buf, size_t len, int op)
{
    uint8_t header[14];
    size_t header_len = mkhdr(len, op, c->is_client, header);
    if (!mg_send(c, header, header_len))
        return 0;
    if (!mg_send(c, buf, len))
        return header_len;
    MG_VERBOSE(("WS out: %d [%.*s]", (int)len, (int)len, buf));
    mg_ws_mask(c, len);
    return header_len + len;
}

static bool mg_ws_client_handshake(struct mg_connection* c)
{
    int n = mg_http_get_request_len(c->recv.buf, c->recv.len);
    if (n < 0) {
        mg_error(c, "not http");
    } else if (n > 0) {
        if (n < 15 || memcmp(c->recv.buf + 9, "101", 3) != 0) {
            mg_error(c, "ws handshake error");
        } else {
            struct mg_http_message hm;
            if (mg_http_parse((char*)c->recv.buf, c->recv.len, &hm)) {
                c->is_websocket = 1;
                mg_call(c, MG_EV_WS_OPEN, &hm);
            } else {
                mg_error(c, "ws handshake error");
            }
        }
        mg_iobuf_del(&c->recv, 0, (size_t)n);
    } else {
        return true;
    }
    return false;
}

static void mg_ws_cb(struct mg_connection* c, int ev, void* ev_data)
{
    struct ws_msg msg;
    size_t ofs = (size_t)c->pfn_data;

    if (ev == MG_EV_READ) {
        if (c->is_client && !c->is_websocket && mg_ws_client_handshake(c))
            return;

        while (ws_process(c->recv.buf + ofs, c->recv.len - ofs, &msg) > 0) {
            char* s = (char*)c->recv.buf + ofs + msg.header_len;
            struct mg_ws_message m;
            size_t len;
            uint8_t final, op;
            m.data.buf = s, m.data.len = msg.data_len, m.flags = msg.flags;
            len = msg.header_len + msg.data_len;
            final = msg.flags & 128;
            op = msg.flags & 15;
            switch (op) {
            case WEBSOCKET_OP_CONTINUE:
                mg_call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_PING:
                MG_DEBUG(("%s", "WS PONG"));
                mg_ws_send(c, s, msg.data_len, WEBSOCKET_OP_PONG);
                mg_call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_PONG:
                mg_call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_TEXT:
            case WEBSOCKET_OP_BINARY:
                if (final)
                    mg_call(c, MG_EV_WS_MSG, &m);
                break;
            case WEBSOCKET_OP_CLOSE:
                MG_DEBUG(("%lu WS CLOSE", c->id));
                mg_call(c, MG_EV_WS_CTL, &m);
                mg_ws_send(c, m.data.buf, m.data.len, WEBSOCKET_OP_CLOSE);
                c->is_draining = 1;
                break;
            default:
                mg_error(c, "unknown WS op %d", op);
                break;
            }

            if (final == 0 || op == 0) {
                if (op)
                    ofs++, len--, msg.header_len--;
                mg_iobuf_del(&c->recv, ofs, msg.header_len);
                len -= msg.header_len;
                ofs += len;
                c->pfn_data = (void*)ofs;
            }
            if (final && op)
                mg_iobuf_del(&c->recv, ofs, len);
            if (final && !op && (ofs > 0)) {
                m.flags = c->recv.buf[0];
                m.data = mg_str_n((char*)&c->recv.buf[1], (size_t)(ofs - 1));
                mg_call(c, MG_EV_WS_MSG, &m);
                mg_iobuf_del(&c->recv, 0, ofs);
                ofs = 0;
                c->pfn_data = NULL;
            }
        }
    }
    (void)ev_data;
}

struct mg_connection* mg_ws_connect(struct mg_mgr* mgr, const char* url,
                                    mg_event_handler_t fn, void* fn_data,
                                    const char* fmt, ...)
{
    struct mg_connection* c = mg_connect(mgr, url, fn, fn_data);
    if (c != NULL) {
        char nonce[16], key[30];
        struct mg_str host = mg_url_host(url);
        mg_random(nonce, sizeof(nonce));
        mg_base64_encode((unsigned char*)nonce, sizeof(nonce), key,
                         sizeof(key));
        mg_xprintf(mg_pfn_iobuf, &c->send,
                   "GET %s HTTP/1.1\r\n"
                   "Upgrade: websocket\r\n"
                   "Host: %.*s\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Version: 13\r\n"
                   "Sec-WebSocket-Key: %s\r\n",
                   mg_url_uri(url), (int)host.len, host.buf, key);
        if (fmt != NULL) {
            va_list ap;
            va_start(ap, fmt);
            mg_vxprintf(mg_pfn_iobuf, &c->send, fmt, &ap);
            va_end(ap);
        }
        mg_xprintf(mg_pfn_iobuf, &c->send, "\r\n");
        c->pfn = mg_ws_cb;
        c->pfn_data = NULL;
    }
    return c;
}

void mg_ws_upgrade(struct mg_connection* c, struct mg_http_message* hm,
                   const char* fmt, ...)
{
    struct mg_str* wskey = mg_http_get_header(hm, "Sec-WebSocket-Key");
    c->pfn = mg_ws_cb;
    c->pfn_data = NULL;
    if (wskey == NULL) {
        mg_http_reply(c, 426, "", "WS upgrade expected\n");
        c->is_draining = 1;
    } else {
        struct mg_str* wsproto = mg_http_get_header(hm,
                                                    "Sec-WebSocket-Protocol");
        va_list ap;
        va_start(ap, fmt);
        ws_handshake(c, wskey, wsproto, fmt, &ap);
        va_end(ap);
        c->is_websocket = 1;
        c->is_resp = 0;
        mg_call(c, MG_EV_WS_OPEN, hm);
    }
}

size_t mg_ws_wrap(struct mg_connection* c, size_t len, int op)
{
    uint8_t header[14], *p;
    size_t header_len = mkhdr(len, op, c->is_client, header);

    if (mg_iobuf_add(&c->send, c->send.len, NULL, header_len) != 0) {
        p = &c->send.buf[c->send.len - len];
        memmove(p, p - header_len, len);
        memcpy(p - header_len, header, header_len);
        mg_ws_mask(c, len);
    }
    return c->send.len;
}

// ---- module: url ----

struct url {
    size_t key, user, pass, host, port, uri, end;
};

int mg_url_is_ssl(const char* url)
{
    return strncmp(url, "wss:", 4) == 0 || strncmp(url, "https:", 6) == 0
        || strncmp(url, "mqtts:", 6) == 0 || strncmp(url, "ssl:", 4) == 0
        || strncmp(url, "tls:", 4) == 0 || strncmp(url, "tcps:", 5) == 0;
}

static struct url urlparse(const char* url)
{
    size_t i;
    struct url u;
    memset(&u, 0, sizeof(u));
    for (i = 0; url[i] != '\0'; i++) {
        if (url[i] == '/' && i > 0 && u.host == 0 && url[i - 1] == '/') {
            u.host = i + 1;
            u.port = 0;
        } else if (url[i] == ']') {
            u.port = 0; // IPv6 URLs, like http://[::1]/bar
        } else if (url[i] == ':' && u.port == 0 && u.uri == 0) {
            u.port = i + 1;
        } else if (url[i] == '@' && u.user == 0 && u.pass == 0 && u.uri == 0) {
            u.user = u.host;
            u.pass = u.port;
            u.host = i + 1;
            u.port = 0;
        } else if (url[i] == '/' && u.host && u.uri == 0) {
            u.uri = i;
        }
    }
    u.end = i;
#if 0
  printf("[%s] %d %d %d %d %d\n", url, u.user, u.pass, u.host, u.port, u.uri);
#endif
    return u;
}

struct mg_str mg_url_host(const char* url)
{
    struct url u = urlparse(url);
    size_t n = u.port ? u.port - u.host - 1
        : u.uri       ? u.uri - u.host
                      : u.end - u.host;
    struct mg_str s = mg_str_n(url + u.host, n);
    return s;
}

const char* mg_url_uri(const char* url)
{
    struct url u = urlparse(url);
    return u.uri ? url + u.uri : "/";
}

unsigned short mg_url_port(const char* url)
{
    struct url u = urlparse(url);
    unsigned short port = 0;
    if (strncmp(url, "http:", 5) == 0 || strncmp(url, "ws:", 3) == 0)
        port = 80;
    if (strncmp(url, "wss:", 4) == 0 || strncmp(url, "https:", 6) == 0)
        port = 443;
    if (strncmp(url, "mqtt:", 5) == 0)
        port = 1883;
    if (strncmp(url, "mqtts:", 6) == 0)
        port = 8883;
    if (u.port)
        port = (unsigned short)atoi(url + u.port);
    return port;
}

struct mg_str mg_url_user(const char* url)
{
    struct url u = urlparse(url);
    struct mg_str s = mg_str("");
    if (u.user && (u.pass || u.host)) {
        size_t n = u.pass ? u.pass - u.user - 1 : u.host - u.user - 1;
        s = mg_str_n(url + u.user, n);
    }
    return s;
}

struct mg_str mg_url_pass(const char* url)
{
    struct url u = urlparse(url);
    struct mg_str s = mg_str_n("", 0UL);
    if (u.pass && u.host) {
        size_t n = u.host - u.pass - 1;
        s = mg_str_n(url + u.pass, n);
    }
    return s;
}

// ---- module: util ----

// See https://github.com/cesanta/mongoose/pull/1265
void mg_bzero(volatile unsigned char* buf, size_t len)
{
    if (buf != NULL) {
        while (len--)
            *buf++ = 0;
    }
}

#if MG_ENABLE_CUSTOM_RANDOM
#else
bool mg_random(void* buf, size_t len)
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

char* mg_random_str(char* buf, size_t len)
{
    size_t i;
    mg_random(buf, len);
    for (i = 0; i < len; i++) {
        uint8_t c = ((uint8_t*)buf)[i] % 62U;
        buf[i] = i == len - 1 ? (char)'\0'            // 0-terminate last byte
            : c < 26          ? (char)('a' + c)       // lowercase
            : c < 52          ? (char)('A' + c - 26)  // uppercase
                              : (char)('0' + c - 52); // numeric
    }
    return buf;
}

uint32_t mg_crc32(uint32_t crc, const char* buf, size_t len)
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

static int isbyte(int n) { return n >= 0 && n <= 255; }

static int parse_net(const char* spec, uint32_t* net, uint32_t* mask)
{
    int n, a, b, c, d, slash = 32, len = 0;
    if ((sscanf(spec, "%d.%d.%d.%d/%d%n", &a, &b, &c, &d, &slash, &n) == 5
         || sscanf(spec, "%d.%d.%d.%d%n", &a, &b, &c, &d, &n) == 4)
        && isbyte(a) && isbyte(b) && isbyte(c) && isbyte(d) && slash >= 0
        && slash < 33) {
        len = n;
        *net = ((uint32_t)a << 24) | ((uint32_t)b << 16) | ((uint32_t)c << 8)
            | (uint32_t)d;
        *mask = slash ? (uint32_t)(0xffffffffU << (32 - slash)) : (uint32_t)0;
    }
    return len;
}

int mg_check_ip_acl(struct mg_str acl, struct mg_addr* remote_ip)
{
    struct mg_str entry;
    int allowed = acl.len == 0 ? '+'
                               : '-'; // If any ACL is set, deny by default
    uint32_t remote_ip4;
    if (remote_ip->is_ip6) {
        return -1; // TODO(): handle IPv6 ACL and addresses
    } else {       // IPv4
        memcpy((void*)&remote_ip4, remote_ip->addr.ip, sizeof(remote_ip4));
        while (mg_span(acl, &entry, &acl, ',')) {
            uint32_t net, mask;
            if (entry.buf[0] != '+' && entry.buf[0] != '-')
                return -1;
            if (parse_net(&entry.buf[1], &net, &mask) == 0)
                return -2;
            if ((mg_ntohl(remote_ip4) & mask) == net)
                allowed = entry.buf[0];
        }
    }
    return allowed == '+';
}

bool mg_path_is_sane(const struct mg_str path)
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
uint64_t mg_millis(void)
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
uint16_t mg_ntohs(uint16_t net) { return MG_LOAD_BE16(&net); }

uint32_t mg_ntohl(uint32_t net) { return MG_LOAD_BE32(&net); }

uint64_t mg_ntohll(uint64_t net) { return MG_LOAD_BE64(&net); }

void mg_delayms(unsigned int ms)
{
    uint64_t to = mg_millis() + ms + 1;
    while (mg_millis() < to)
        (void)0;
}

#if MG_ENABLE_CUSTOM_CALLOC
#else
void* mg_calloc(size_t count, size_t size) { return calloc(count, size); }

void mg_free(void* ptr) { free(ptr); }
#endif
