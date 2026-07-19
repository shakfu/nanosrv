#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: dns ----

struct dns_data {
    struct dns_data* next;
    struct Connection* c;
    uint64_t expire;
    uint16_t txnid;
};

static void sendnsreq(struct Connection*, struct Str*, int,
                         struct DnsConfig*, bool);

static void dns_free(struct dns_data** head, struct dns_data* d)
{
    LIST_DELETE(struct dns_data, head, d);
    mem_free(d);
}

void resolve_cancel(struct Connection* c)
{
    struct dns_data *tmp, *d;
    struct dns_data** head = reinterpret_cast<struct dns_data**>(&c->mgr->active_dns_requests);
    for (d = *head; d != NULL; d = tmp) {
        tmp = d->next;
        if (d->c == c)
            dns_free(head, d);
    }
}

static size_t dns_parse_name_depth(const uint8_t* s, size_t len, size_t ofs,
                                      char* to, size_t tolen, size_t j,
                                      int depth)
{
    size_t i = 0;
    if (tolen > 0 && depth == 0)
        to[0] = '\0';
    if (depth > MG_DNS_MAX_RECURSION_DEPTH)
        return 0;
    // MG_INFO(("ofs %lx %x %x", (unsigned long) ofs, s[ofs], s[ofs + 1]));
    while (ofs + i + 1 < len) {
        size_t n = s[ofs + i];
        if (n == 0) {
            i++;
            break;
        }
        if (n & MG_DNS_COMPRESS_MASK) {
            size_t ptr = (((n & MG_DNS_COMPRESS_PTR_MASK) << 8) | s[ofs + i + 1]); // 12 is hdr len
            // MG_INFO(("PTR %lx", (unsigned long) ptr));
            if (ptr + 1 < len && (s[ptr] & MG_DNS_COMPRESS_MASK) == 0
                && dns_parse_name_depth(s, len, ptr, to, tolen, j,
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

static size_t dns_parse_name(const uint8_t* s, size_t n, size_t ofs,
                                char* dst, size_t dstlen)
{
    return dns_parse_name_depth(s, n, ofs, dst, dstlen, 0, 0);
}

size_t dns_parse_rr(const uint8_t* buf, size_t len, size_t ofs,
                       bool is_question, struct DnsRR* rr)
{
    const uint8_t *s = buf + ofs, *e = &buf[len];

    memset(rr, 0, sizeof(*rr));
    if (len < sizeof(struct DnsHeader))
        return 0; // Too small
    if (len > MG_DNS_PACKET_MAX_SIZE)
        return 0; //  Too large, we don't expect that
    if (s >= e)
        return 0; //  Overflow

    if ((rr->nlen = static_cast<uint16_t>(dns_parse_name(buf, len, ofs, NULL, 0))) == 0)
        return 0;
    s += rr->nlen + 4;
    if (s > e)
        return 0;
    rr->atype = static_cast<uint16_t>((static_cast<uint16_t>(s[-4]) << 8) | s[-3]);
    rr->aclass = static_cast<uint16_t>((static_cast<uint16_t>(s[-2]) << 8) | s[-1]);
    if (is_question)
        return static_cast<size_t>(rr->nlen + 4);

    s += 6;
    if (s > e)
        return 0;
    rr->alen = static_cast<uint16_t>((static_cast<uint16_t>(s[-2]) << 8) | s[-1]);
    if (s + rr->alen > e)
        return 0;
    return static_cast<size_t>(rr->nlen + rr->alen + 10);
}

bool dns_parse(const uint8_t* buf, size_t len, struct DnsMessage* dm)
{
    const struct DnsHeader* h = reinterpret_cast<const struct DnsHeader*>(buf);
    struct DnsRR rr;
    size_t i, n, num_answers, ofs = sizeof(*h);
    bool is_response;
    memset(dm, 0, sizeof(*dm));

    if (len < sizeof(*h))
        return 0; // Too small, headers dont fit
    if (ntohs_(h->num_questions) > 1)
        return 0; // Sanity
    num_answers = ntohs_(h->num_answers);
    if (num_answers > MG_DNS_MAX_ANSWERS) {
        MG_DEBUG(("Got %u answers, ignoring beyond 10th one", num_answers));
        num_answers = MG_DNS_MAX_ANSWERS; // Sanity cap
    }
    dm->txnid = ntohs_(h->txnid);
    is_response = ntohs_(h->flags) & 0x8000;

    for (i = 0; i < ntohs_(h->num_questions); i++) {
        if ((n = dns_parse_rr(buf, len, ofs, true, &rr)) == 0)
            return false;
        // MG_INFO(("Q %lu %lu %hu/%hu", ofs, n, rr.atype, rr.aclass));
        dns_parse_name(buf, len, ofs, dm->name, sizeof(dm->name));
        ofs += n;
    }

    if (!is_response) {
        // For queries, there is no need to parse the answers. In this way,
        // we also ensure the domain name (dm->name) is parsed from
        // the question field.
        return true;
    }

    for (i = 0; i < num_answers; i++) {
        if ((n = dns_parse_rr(buf, len, ofs, false, &rr)) == 0)
            return false;
        // MG_INFO(("A -- %lu %lu %hu/%hu %s", ofs, n, rr.atype, rr.aclass,
        // dm->name));
        dns_parse_name(buf, len, ofs, dm->name, sizeof(dm->name));
        ofs += n;

        if (rr.alen == MG_DNS_IPV4_ADDR_LEN && rr.atype == MG_DNS_RTYPE_A && rr.aclass == MG_DNS_CLASS_IN) {
            dm->addr.is_ip6 = false;
            memcpy(&dm->addr.addr.ip, &buf[ofs - MG_DNS_IPV4_ADDR_LEN], MG_DNS_IPV4_ADDR_LEN);
            dm->resolved = true;
            break; // Return success
        } else if (rr.alen == MG_DNS_IPV6_ADDR_LEN && rr.atype == MG_DNS_RTYPE_AAAA
                   && rr.aclass == MG_DNS_CLASS_IN) {
            dm->addr.is_ip6 = true;
            memcpy(&dm->addr.addr.ip, &buf[ofs - MG_DNS_IPV6_ADDR_LEN], MG_DNS_IPV6_ADDR_LEN);
            dm->resolved = true;
            break; // Return success
        }
    }
    return true;
}

static void dns_cb(struct Connection* c, int ev, void* ev_data)
{
    struct dns_data *d, *tmp;
    struct dns_data** head = reinterpret_cast<struct dns_data**>(&c->mgr->active_dns_requests);
    if (ev == MG_EV_POLL) {
        uint64_t now = *static_cast<uint64_t*>(ev_data);
        for (d = *head; d != NULL; d = tmp) {
            tmp = d->next;
            // MG_DEBUG ("%lu %lu dns poll", d->expire, now));
            if (now > d->expire)
                error(d->c, "DNS timeout");
        }
    } else if (ev == MG_EV_READ) {
        struct DnsMessage dm;
        int resolved = 0;
        if (dns_parse(c->recv.buf, c->recv.len, &dm) == false) {
            MG_ERROR(("Unexpected DNS response:"));
            hexdump(c->recv.buf, c->recv.len);
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
                                  print_ip, &d->c->rem));
                        connect_resolved(d->c);
#if MG_ENABLE_IPV6
                    } else if (dm.addr.is_ip6 == false && dm.name[0] != '\0'
                               && c->mgr->use_dns6 == false) {
                        struct Str x = Str(dm.name);
                        sendnsreq(d->c, &x, c->mgr->dnstimeout,
                                     &c->mgr->dns6, true);
#endif
                    } else {
                        error(d->c, "%s DNS lookup failed", dm.name);
                    }
                } else {
                    MG_ERROR(("%lu already resolved", d->c->id));
                }
                dns_free(head, d);
                resolved = 1;
            }
        }
        if (!resolved)
            MG_ERROR(("stray DNS reply"));
        c->recv.len = 0;
    } else if (ev == MG_EV_CLOSE) {
        for (d = *head; d != NULL; d = tmp) {
            tmp = d->next;
            error(d->c, "DNS error");
            dns_free(head, d);
        }
    }
}

static bool dns_send(struct Connection* c, const struct Str* name,
                        uint16_t txnid, bool ipv6)
{
    struct {
        struct DnsHeader header;
        uint8_t data[256];
    } pkt;

    size_t i, n;
    memset(&pkt, 0, sizeof(pkt));
    pkt.header.txnid = mg_htons(txnid);
    pkt.header.flags = mg_htons(MG_DNS_QUERY_FLAG);
    pkt.header.num_questions = mg_htons(1);
    for (i = n = 0; i < sizeof(pkt.data) - 5; i++) {
        if (name->buf[i] == '.' || i >= name->len) {
            pkt.data[n] = static_cast<uint8_t>(i - n);
            memcpy(&pkt.data[n + 1], name->buf + n, i - n);
            n = i + 1;
        }
        if (i >= name->len)
            break;
    }
    memcpy(&pkt.data[n], "\x00\x00\x01\x00\x01", 5); // A query
    n += 5;
    if (ipv6)
        pkt.data[n - 3] = MG_DNS_RTYPE_AAAA; // AAAA query
    // memcpy(&pkt.data[n], "\xc0\x0c\x00\x1c\x00\x01", 6);  // AAAA query
    // n += 6;
    return send_data(c, &pkt, sizeof(pkt.header) + n);
}

bool dnsc_init(struct Mgr* mgr, struct DnsConfig* dnsc);

bool dnsc_init(struct Mgr* mgr, struct DnsConfig* dnsc)
{
    if (dnsc->url == NULL) {
        error(0, "DNS server URL is NULL. Call mgr_init()");
        return false;
    }
    if (dnsc->c == NULL) {
        dnsc->c = connect(mgr, dnsc->url, NULL, NULL);
        if (dnsc->c == NULL)
            return false;
        dnsc->c->pfn = dns_cb;
    }
    return true;
}

static void sendnsreq(struct Connection* c, struct Str* name, int ms,
                         struct DnsConfig* dnsc, bool ipv6)
{
    struct dns_data* d = NULL;
    if (!dnsc_init(c->mgr, dnsc)) {
        error(c, "resolver");
    } else if ((d = static_cast<struct dns_data*>(mem_calloc(1, sizeof(*d)))) == NULL) {
        error(c, "resolve OOM");
    } else {
        struct dns_data* reqs = static_cast<struct dns_data*>(c->mgr->active_dns_requests);
        uint16_t id;
        // Pick a random txnid that does not collide with any in-flight
        // request. Rescan the whole outstanding list on every collision so
        // that concurrent lookups never share an id (a collision would let a
        // response for one query satisfy another). 65536 attempts bounds the
        // loop even if the list were pathologically full.
        for (unsigned attempt = 0; attempt < 0x10000; attempt++) {
            random_(&id, sizeof(uint16_t));
            bool taken = false;
            for (struct dns_data* r = reqs; r != NULL; r = r->next) {
                if (r->txnid == id) {
                    taken = true;
                    break;
                }
            }
            if (!taken)
                break;
        }
        d->txnid = id;
        d->next = reqs;
        c->mgr->active_dns_requests = d;
        d->expire = millis() + static_cast<uint64_t>(ms);
        d->c = c;
        c->is_resolving = 1;
        MG_VERBOSE(("%lu resolving %.*s @ %s, txnid %hu", c->id,
                    static_cast<int>(name->len), name->buf, dnsc->url, d->txnid));
        if (!dns_send(dnsc->c, name, d->txnid, ipv6)) {
            error(dnsc->c, "DNS send");
        }
    }
}

void resolve(struct Connection* c, const char* url)
{
    struct Str host = url_host(url);
    c->rem.port = mg_htons(url_port(url));
    if (aton(host, &c->rem)) {
        // host is an IP address, do not fire name resolution
        connect_resolved(c);
    } else {
        // host is not an IP, send DNS resolution request
        struct DnsConfig* dns = c->mgr->use_dns6 ? &c->mgr->dns6 : &c->mgr->dns4;
        sendnsreq(c, &host, c->mgr->dnstimeout, dns, c->mgr->use_dns6);
    }
}

} // namespace nanosrv
