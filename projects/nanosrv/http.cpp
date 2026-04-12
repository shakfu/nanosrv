#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: http ----

static int ncasecmp(const char* s1, const char* s2, size_t len)
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

bool to_size_t(struct Str str, size_t* val);

bool to_size_t(struct Str str, size_t* val)
{
    size_t i = 0, max = static_cast<size_t>(-1), max2 = max / 10, result = 0, ndigits = 0;
    while (i < str.len && (str.buf[i] == ' ' || str.buf[i] == '\t'))
        i++;
    if (i < str.len && str.buf[i] == '-')
        return false;
    while (i < str.len && str.buf[i] >= '0' && str.buf[i] <= '9') {
        size_t digit = static_cast<size_t>(str.buf[i] - '0');
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
    *val = static_cast<size_t>(result);
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
size_t http_next_multipart(struct Str body, size_t ofs,
                              struct HttpPart* part)
{
    struct Str cd = str_n(MG_HTTP_CONTENT_DISPOSITION, MG_HTTP_CONTENT_DISPOSITION_LEN);
    const char* s = body.buf;
    size_t b = ofs, h1, h2, b1, b2, max = body.len;

    // Init part params
    if (part != NULL)
        part->name = part->filename = part->body = str_n(0, 0);

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
            && ncasecmp(&s[h1], cd.buf, cd.len) == 0) {
            struct Str v = str_n(&s[h1 + cd.len + 2],
                                       h2 - (h1 + cd.len + 2));
            part->name = http_get_header_var(v, str_n(MG_HTTP_FORM_NAME, MG_HTTP_FORM_NAME_LEN));
            part->filename = http_get_header_var(v,
                                                    str_n(MG_HTTP_FORM_FILENAME, MG_HTTP_FORM_FILENAME_LEN));
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
        part->body = str_n(&s[b1], b2 - b1);
    // MG_INFO(("Body: [%.*s]", (int) (b2 - b1), &s[b1]));
    return b2 + 2;
}

void http_bauth(struct Connection* c, const char* user, const char* pass)
{
    struct Str u = Str(user), p = Str(pass);
    size_t need = c->send.len + MG_HTTP_BAUTH_OVERHEAD + (u.len + p.len) * 2;
    if (c->send.size < need)
        (void)iobuf_resize(&c->send, need);
    if (c->send.size >= need) {
        size_t i, n = 0;
        char* buf = reinterpret_cast<char*>(&c->send.buf[c->send.len]);
        memcpy(buf, MG_HTTP_AUTH_HEADER, MG_HTTP_AUTH_HEADER_LEN); // DON'T use send_data!
        for (i = 0; i < u.len; i++) {
            n = base64_update((reinterpret_cast<unsigned char*>(u.buf))[i], buf + MG_HTTP_AUTH_HEADER_LEN, n);
        }
        if (p.len > 0) {
            n = base64_update(':', buf + MG_HTTP_AUTH_HEADER_LEN, n);
            for (i = 0; i < p.len; i++) {
                n = base64_update((reinterpret_cast<unsigned char*>(p.buf))[i], buf + MG_HTTP_AUTH_HEADER_LEN, n);
            }
        }
        n = base64_final(buf + MG_HTTP_AUTH_HEADER_LEN, n);
        c->send.len += MG_HTTP_AUTH_HEADER_LEN + static_cast<size_t>(n) + 2;
        memcpy(&c->send.buf[c->send.len - 2], "\r\n", 2);
    } else {
        MG_ERROR(("%lu oom %d->%d ", c->id, static_cast<int>(c->send.size), static_cast<int>(need)));
    }
}

struct Str http_var(struct Str buf, struct Str name)
{
    struct Str entry, k, v, result = str_n(NULL, 0);
    while (span(buf, &entry, &buf, '&')) {
        if (span(entry, &k, &v, '=') && name.len == k.len
            && ncasecmp(name.buf, k.buf, k.len) == 0) {
            result = v;
            break;
        }
    }
    return result;
}

int http_get_var(const struct Str* buf, const char* name, char* dst,
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
        struct Str v = http_var(*buf, Str(name));
        if (v.buf == NULL) {
            len = -4; // Name does not exist
        } else {
            len = url_decode(v.buf, v.len, dst, dst_len, 1);
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

int url_decode(const char* src, size_t src_len, char* dst, size_t dst_len,
                  int is_form_url_encoded)
{
    size_t i, j;
    for (i = j = 0; i < src_len && j + 1 < dst_len; i++, j++) {
        if (src[i] == '%') {
            // Use `i + 2 < src_len`, not `i < src_len - 2`, note small src_len
            if (i + 2 < src_len && isx(src[i + 1]) && isx(src[i + 2])) {
                str_to_num(str_n(src + i + 1, 2), 16, &dst[j],
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
    return i >= src_len && j < dst_len ? static_cast<int>(j) : -1;
}

static bool isok(uint8_t c)
{
    return c == '\n' || c == '\r' || c == '\t' || c >= ' ';
}

int http_get_request_len(const unsigned char* buf, size_t buf_len)
{
    size_t i;
    for (i = 0; i < buf_len; i++) {
        if (!isok(buf[i]))
            return -1;
        if ((i > 0 && buf[i] == '\n' && buf[i - 1] == '\n')
            || (i > 3 && buf[i] == '\n' && buf[i - 1] == '\r'
                && buf[i - 2] == '\n'))
            return static_cast<int>(i) + 1;
    }
    return 0;
}

struct Str* http_get_header(struct HttpMessage* h, const char* name)
{
    size_t i, n = strlen(name),
              max = sizeof(h->headers) / sizeof(h->headers[0]);
    for (i = 0; i < max && h->headers[i].name.len > 0; i++) {
        struct Str *k = &h->headers[i].name, *v = &h->headers[i].value;
        if (n == k->len && ncasecmp(k->buf, name, n) == 0)
            return v;
    }
    return NULL;
}

// Is it a valid utf-8 continuation byte
static bool vcb(uint8_t c) { return (c & 0xc0) == 0x80; }

// Get character length (valid utf-8). Used to parse method, URI, headers
static size_t clen(const char* s, const char* end)
{
    const unsigned char *u = reinterpret_cast<const unsigned char*>(s), c = *u;
    long n = static_cast<long>(end - s);
    if (c > ' ' && c <= '~')
        return 1; // Usual ascii printed char
    if ((c & MG_UTF8_2BYTE_MASK) == MG_UTF8_2BYTE_VALUE && n > 1 && vcb(u[1]))
        return 2; // 2-byte UTF8
    if ((c & MG_UTF8_3BYTE_MASK) == MG_UTF8_3BYTE_VALUE && n > 2 && vcb(u[1]) && vcb(u[2]))
        return 3;
    if ((c & MG_UTF8_4BYTE_MASK) == MG_UTF8_4BYTE_VALUE && n > 3 && vcb(u[1]) && vcb(u[2]) && vcb(u[3]))
        return 4;
    return 0;
}

// Skip until the newline. Return advanced `s`, or NULL on error
static const char* skiptorn(const char* s, const char* end, struct Str* v)
{
    v->buf = const_cast<char*>(s);
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

static bool http_parse_headers(const char* s, const char* end,
                                  struct HttpHeader* h, size_t max_hdrs)
{
    size_t i, n;
    int cl_count = 0, te_count = 0, auth_count = 0;
    int conn_count = 0, cookie_count = 0;
    for (i = 0; i < max_hdrs; i++) {
        struct Str k = {}, v = {};
        if (s >= end)
            return false;
        if (s[0] == '\n' || (s[0] == '\r' && s[1] == '\n'))
            break;
        k.buf = const_cast<char*>(s);
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
        if (((str_casecmp(k, Str("Content-Length")) == 0)
             && (++cl_count > 1))
            || ((str_casecmp(k, Str("Transfer-Encoding")) == 0)
                && (++te_count > 1))
            || ((str_casecmp(k, Str("Authorization")) == 0)
                && (++auth_count > 1))
            || ((str_casecmp(k, Str("Cookie")) == 0)
                && (++cookie_count > 1))
            || ((str_casecmp(k, Str("Connection")) == 0)
                && (++conn_count > 1)))
            return false;
        // MG_INFO(("--HH [%.*s] [%.*s]", (int) k.len, k.buf, (int) v.len,
        // v.buf));
        h[i].name = k, h[i].value = v; // Success. Assign values
    }
    return true;
}

int http_parse(const char* s, size_t len, struct HttpMessage* hm)
{
    int is_response, req_len = http_get_request_len(reinterpret_cast<const unsigned char*>(s), len);
    const char *end = s == NULL ? NULL : s + req_len,
               *qs; // Cannot add to NULL
    const struct Str* cl;
    size_t n;
    bool version_prefix_valid;

    memset(hm, 0, sizeof(*hm));
    if (req_len <= 0)
        return req_len;

    hm->message.buf = hm->head.buf = const_cast<char*>(s);
    hm->body.buf = const_cast<char*>(end);
    hm->head.len = static_cast<size_t>(req_len);
    hm->message.len = hm->body.len = static_cast<size_t>(-1); // Set body length to infinite

    // Parse request line
    hm->method.buf = const_cast<char*>(s);
    while (s < end && (n = clen(s, end)) > 0)
        s += n, hm->method.len += n;
    while (s < end && s[0] == ' ')
        s++; // Skip spaces
    hm->uri.buf = const_cast<char*>(s);
    while (s < end && (n = clen(s, end)) > 0)
        s += n, hm->uri.len += n;
    while (s < end && s[0] == ' ')
        s++; // Skip spaces
    is_response = hm->method.len > MG_HTTP_VERSION_PREFIX_LEN
        && (ncasecmp(hm->method.buf, MG_HTTP_VERSION_PREFIX, MG_HTTP_VERSION_PREFIX_LEN) == 0);
    if ((s = skiptorn(s, end, &hm->proto)) == NULL)
        return false;
    // If we're given a version, check that it is HTTP/x.x
    version_prefix_valid = hm->proto.len > MG_HTTP_VERSION_PREFIX_LEN
        && (ncasecmp(hm->proto.buf, MG_HTTP_VERSION_PREFIX, MG_HTTP_VERSION_PREFIX_LEN) == 0);
    if (!is_response && !version_prefix_valid)
        return -1; // no version detected in request
    if (!is_response && hm->proto.len > 0
        && (!version_prefix_valid || hm->proto.len != MG_HTTP_VERSION_FULL_LEN
            || (hm->proto.buf[5] < '0' || hm->proto.buf[5] > '9')
            || (hm->proto.buf[6] != '.')
            || (hm->proto.buf[7] < '0' || hm->proto.buf[7] > '9'))) {
        return -1;
    }

    // If URI contains '?' character, setup query string
    if ((qs = static_cast<const char*>(memchr(hm->uri.buf, '?', hm->uri.len))) != NULL) {
        hm->query.buf = const_cast<char*>(qs) + 1;
        hm->query.len = static_cast<size_t>(&hm->uri.buf[hm->uri.len] - (qs + 1));
        hm->uri.len = static_cast<size_t>(qs - hm->uri.buf);
    }

    // Sanity check. Allow protocol/reason to be empty
    // Do this check after hm->method.len and hm->uri.len are finalised
    if (hm->method.len == 0 || hm->uri.len == 0)
        return -1;

    if (!http_parse_headers(s, end, hm->headers,
                               sizeof(hm->headers) / sizeof(hm->headers[0])))
        return -1; // error when parsing
    if ((cl = http_get_header(hm, "Content-Length")) != NULL) {
        if (to_size_t(*cl, &hm->body.len) == false)
            return -1;
        hm->message.len = static_cast<size_t>(req_len) + hm->body.len;
    }

    // http_parse() is used to parse both HTTP requests and HTTP
    // responses. If HTTP response does not have Content-Length set, then
    // body is read until socket is closed, i.e. body.len is infinite (~0).
    //
    // For HTTP requests though, if Content-Length is not specified
    // set body length to 0.
    if (hm->body.len == static_cast<size_t>(~0) && !is_response) {
        hm->body.len = 0;
        hm->message.len = static_cast<size_t>(req_len);
    }

    // The 204 (No content) responses also have 0 body length
    if (hm->body.len == static_cast<size_t>(~0) && is_response
        && str_casecmp(hm->uri, Str("204")) == 0) { // MG_HTTP_STATUS_NO_CONTENT
        hm->body.len = 0;
        hm->message.len = static_cast<size_t>(req_len);
    }
    if (hm->message.len < static_cast<size_t>(req_len))
        return -1; // Overflow protection

    return req_len;
}

static void http_vprintf_chunk(struct Connection* c, const char* fmt,
                                  va_list* ap)
{
    size_t len = c->send.len;
    if (!send_data(c, MG_HTTP_CHUNK_PLACEHOLDER, MG_HTTP_CHUNK_PLACEHOLDER_LEN))
        error(c, "OOM");
    vxprintf(pfn_iobuf, &c->send, fmt, ap);
    if (c->send.len >= len + MG_HTTP_CHUNK_PLACEHOLDER_LEN) {
        snprintf_(reinterpret_cast<char*>(c->send.buf) + len, MG_HTTP_CHUNK_SIZE_HEX_WIDTH + 1, "%08lx",
                    c->send.len - len - MG_HTTP_CHUNK_PLACEHOLDER_LEN);
        c->send.buf[len + MG_HTTP_CHUNK_SIZE_HEX_WIDTH] = '\r';
        if (c->send.len == len + MG_HTTP_CHUNK_PLACEHOLDER_LEN)
            c->is_resp = 0; // Last chunk, reset marker
    }
    if (!send_data(c, "\r\n", 2))
        error(c, "OOM");
}

void http_printf_chunk(struct Connection* c, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    http_vprintf_chunk(c, fmt, &ap);
    va_end(ap);
}

void http_write_chunk(struct Connection* c, const char* buf, size_t len)
{
    conn_printf(c, "%lx\r\n", static_cast<unsigned long>(len));
    if (!send_data(c, buf, len) || !send_data(c, "\r\n", 2))
        error(c, "OOM");
    if (len == 0)
        c->is_resp = 0;
}

// clang-format off
static const char *http_status_code_str(int status_code) {
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

void http_reply(struct Connection* c, int code, const char* headers,
                   const char* fmt, ...)
{
    va_list ap;
    size_t len;
    conn_printf(c, "HTTP/1.1 %d %s\r\n%sContent-Length:            \r\n\r\n",
              code, http_status_code_str(code),
              headers == NULL ? "" : headers);
    len = c->send.len;
    va_start(ap, fmt);
    vxprintf(pfn_iobuf, &c->send, fmt, &ap);
    va_end(ap);
    if (c->send.len > MG_HTTP_CONTENT_LEN_MIN_SIZE) {
        size_t n = snprintf_(reinterpret_cast<char*>(&c->send.buf[len - MG_HTTP_CONTENT_LEN_OFFSET]), MG_HTTP_CONTENT_LEN_MAX_WIDTH, "%-10lu",
                               static_cast<unsigned long>(c->send.len - len));
        c->send.buf[len - MG_HTTP_CONTENT_LEN_OFFSET + n] = ' '; // Change ending 0 to space
    }
    c->is_resp = 0;
}

int http_status(const struct HttpMessage* hm)
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
    if (i > static_cast<int>(sizeof(int)) * 2)
        return -1; // Chunk length is too big
    if (len < i + 1 || buf[i] != '\r' || buf[i + 1] != '\n')
        return -1; // Error
    if (str_to_num(str_n(buf, static_cast<size_t>(i)), 16, &n, sizeof(int)) == false)
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

void http_cb(struct Connection* c, int ev, void* ev_data)
{
    if (ev == MG_EV_READ || ev == MG_EV_CLOSE
        || (ev == MG_EV_POLL && c->is_accepted && !c->is_draining
            && c->recv.len > 0)) { // see #2796
        struct HttpMessage hm;
        size_t ofs = 0; // Parsing offset
        while (c->is_resp == 0 && ofs < c->recv.len) {
            const char* buf = reinterpret_cast<char*>(c->recv.buf) + ofs;
            int n = http_parse(buf, c->recv.len - ofs, &hm);
            struct Str* te; // Transfer - encoding header
            bool is_chunked = false, is_http_1_0 = false;
            size_t old_len = c->recv.len;
            if (n < 0) {
                // We don't use error() here, to avoid closing pipelined
                // requests prematurely, see #2592
                MG_ERROR(("HTTP parse, %lu bytes", c->recv.len));
                c->is_draining = 1;
                hexdump(buf,
                           c->recv.len - ofs > 16 ? 16 : c->recv.len - ofs);
                c->recv.len = 0;
                return;
            }
            if (n == 0)
                break;                        // Request is not buffered yet
            call(c, MG_EV_HTTP_HDRS, &hm); // Got all HTTP headers
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
                    - static_cast<size_t>(hm.body.buf - hm.message.buf);
            }
            is_http_1_0 = hm.proto.len > 8
                && ncasecmp(hm.proto.buf, "HTTP/1.0", 8) == 0;
            // HTTP/1.0 does not use "Transfer-Encoding: chunked"
            if (!is_http_1_0
                && (te = http_get_header(&hm, "Transfer-Encoding"))
                    != NULL) {
                if (str_casecmp(*te, Str("chunked")) == 0) {
                    is_chunked = true;
                } else {
                    error(c, "Invalid Transfer-Encoding"); // See #2460
                    return;
                }
            } else if (http_get_header(&hm, "Content-length") == NULL) {
                // #2593: HTTP packets must contain either Transfer-Encoding or
                // Content-length
                bool is_response = ncasecmp(hm.method.buf, MG_HTTP_VERSION_PREFIX, MG_HTTP_VERSION_PREFIX_LEN) == 0;
                bool require_content_len = false;
                if (!is_response
                    && (str_casecmp(hm.method, Str("POST")) == 0
                        || str_casecmp(hm.method, Str("PUT")) == 0)) {
                    // POST and PUT should include an entity body. Therefore,
                    // they should contain a Content-length header (unless the
                    // body length is 0, in which case it can be omitted).
                    // Other requests can also contain a body, but their
                    // content has no defined semantics (RFC 7231)
                    if (hm.body.len != 0)
                        require_content_len = true;
                    ofs += static_cast<size_t>(n); // this request has been processed
                } else if (is_response) {
                    // HTTP spec 7.2 Entity body: All other responses must
                    // include a body or Content-Length header field defined
                    // with a value of 0.
                    int status = http_status(&hm);
                    require_content_len = status >= 200 && status != 204
                        && status != 304;
                }
                if (require_content_len) {
                    if (!c->is_client)
                        http_reply(c, 411, "", "");
                    MG_ERROR(("Content length missing from %s",
                              is_response ? "response" : "request"));
                }
            }

            if (is_chunked) {
                // For chunked data, strip off prefixes and suffixes from
                // chunks and relocate them right after the headers, then
                // report a message
                char* s = reinterpret_cast<char*>(c->recv.buf) + ofs + n;
                int o = 0, pl, dl, cl,
                    len = static_cast<int>(c->recv.len - ofs - static_cast<size_t>(n));

                // Find zero-length chunk (the end of the body)
                while ((cl = skip_chunk(s + o, len - o, &pl, &dl)) > 0 && dl)
                    o += cl;
                if (cl == 0)
                    break; // No zero-len chunk, buffer more data
                if (cl < 0) {
                    error(c, "Invalid chunk");
                    break;
                }

                // Zero chunk found. Second pass: strip + relocate
                o = 0, hm.body.len = 0, hm.message.len = static_cast<size_t>(n);
                while ((cl = skip_chunk(s + o, len - o, &pl, &dl)) > 0) {
                    memmove(s + hm.body.len, s + o + pl, static_cast<size_t>(dl));
                    o += cl, hm.body.len += static_cast<size_t>(dl),
                        hm.message.len += static_cast<size_t>(dl);
                    if (dl == 0)
                        break;
                }
                ofs += static_cast<size_t>(n + o);
            } else { // Normal, non-chunked data
                size_t len = c->recv.len - ofs - static_cast<size_t>(n);
                if (hm.body.len > len)
                    break; // Buffer more data
                ofs += static_cast<size_t>(n) + hm.body.len;
            }

            if (c->is_accepted)
                c->is_resp = 1;              // Start generating response
            call(c, MG_EV_HTTP_MSG, &hm); // User handler can clear is_resp
            if (c->is_accepted && !c->is_resp) {
                struct Str* cc = http_get_header(&hm, "Connection");
                if (cc != NULL && str_casecmp(*cc, Str("close")) == 0) {
                    c->is_draining = 1; // honor "Connection: close"
                    break;
                }
            }
        }
        if (ofs > 0)
            iobuf_del(&c->recv, 0, ofs); // Delete processed data
    }
    (void)ev_data;
}

struct Connection* http_connect(struct Mgr* mgr, const char* url,
                                      EventHandler fn, void* fn_data)
{
    return connect_svc(mgr, url, fn, fn_data, http_cb, NULL);
}

struct Connection* http_listen(struct Mgr* mgr, const char* url,
                                     EventHandler fn, void* fn_data)
{
    struct Connection* c = listen_(mgr, url, fn, fn_data);
    if (c != NULL)
        c->pfn = http_cb;
    return c;
}

static bool is_url_safe(int c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
        || (c >= 'A' && c <= 'Z') || c == '.' || c == '_' || c == '-'
        || c == '~';
}

size_t url_encode(const char* s, size_t sl, char* buf, size_t len)
{
    size_t i, n = 0;
    for (i = 0; i < sl; i++) {
        int c = *reinterpret_cast<const unsigned char*>(&s[i]);
        if (n + 4 >= len)
            return 0;
        if (is_url_safe(c)) {
            buf[n++] = s[i];
        } else {
            snprintf_(&buf[n], 4, "%%%M", print_hex, 1, &s[i]);
            n += 3;
        }
    }
    if (len > 0 && n < len - 1)
        buf[n] = '\0'; // Null-terminate the destination
    if (len > 0)
        buf[len - 1] = '\0'; // Always.
    return n;
}

void http_creds(struct HttpMessage* hm, char* user, size_t userlen,
                   char* pass, size_t passlen)
{
    struct Str* v = http_get_header(hm, "Authorization");
    user[0] = pass[0] = '\0';
    if (v != NULL && v->len > MG_HTTP_AUTH_BASIC_PREFIX_LEN && memcmp(v->buf, MG_HTTP_AUTH_BASIC_PREFIX, MG_HTTP_AUTH_BASIC_PREFIX_LEN) == 0) {
        char buf[256];
        size_t n = base64_decode(v->buf + MG_HTTP_AUTH_BASIC_PREFIX_LEN, v->len - MG_HTTP_AUTH_BASIC_PREFIX_LEN, buf, sizeof(buf));
        const char* p = static_cast<const char*>(memchr(buf, ':', n > 0 ? n : 0));
        if (p != NULL) {
            snprintf_(user, userlen, "%.*s", p - buf, buf);
            snprintf_(pass, passlen, "%.*s", n - static_cast<size_t>(p - buf) - 1,
                        p + 1);
        }
    } else if (v != NULL && v->len > MG_HTTP_AUTH_BEARER_PREFIX_LEN && memcmp(v->buf, MG_HTTP_AUTH_BEARER_PREFIX, MG_HTTP_AUTH_BEARER_PREFIX_LEN) == 0) {
        snprintf_(pass, passlen, "%.*s", static_cast<int>(v->len) - MG_HTTP_AUTH_BEARER_PREFIX_LEN, v->buf + MG_HTTP_AUTH_BEARER_PREFIX_LEN);
    } else if ((v = http_get_header(hm, "Cookie")) != NULL) {
        struct Str t = http_get_header_var(*v,
                                                 str_n(MG_HTTP_ACCESS_TOKEN_COOKIE, MG_HTTP_ACCESS_TOKEN_COOKIE_LEN));
        if (t.len > 0)
            snprintf_(pass, passlen, "%.*s", static_cast<int>(t.len), t.buf);
    } else {
        http_get_var(&hm->query, "access_token", pass, passlen);
    }
}

static struct Str stripquotes(struct Str s)
{
    return s.len > 1 && s.buf[0] == '"' && s.buf[s.len - 1] == '"'
        ? str_n(s.buf + 1, s.len - 2)
        : s;
}

struct Str http_get_header_var(struct Str s, struct Str v)
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
            return stripquotes(str_n(b, static_cast<size_t>(p - b + q)));
        }
    }
    return str_n(NULL, 0);
}

// General trampoline: forwards events to HandlerFn with typed Event enum.
static void handler_trampoline(struct Connection* c, int ev, void* ev_data)
{
    auto* fn = static_cast<HandlerFn*>(c->fn_data);
    (*fn)(*c, static_cast<Event>(ev), ev_data);
    if (ev == MG_EV_CLOSE && c->is_listening) {
        delete fn;
        c->fn_data = nullptr;
    }
}

Connection* http_listen(Manager& mgr, const char* url, HandlerFn handler)
{
    auto* fn = new HandlerFn(std::move(handler));
    auto* c = http_listen(mgr.raw(), url, handler_trampoline, fn);
    if (c == nullptr)
        delete fn;
    return c;
}

Connection* http_connect(Manager& mgr, const char* url, HandlerFn handler)
{
    auto* fn = new HandlerFn(std::move(handler));
    auto* c = http_connect(mgr.raw(), url, handler_trampoline, fn);
    if (c == nullptr)
        delete fn;
    return c;
}

// Typed HTTP trampoline: only fires the user callback on HttpMessage events.
// Other events (Poll, Close, etc.) are silently handled.
static void http_handler_trampoline(struct Connection* c, int ev, void* ev_data)
{
    auto* fn = static_cast<Manager::HttpHandler*>(c->fn_data);
    if (ev == MG_EV_HTTP_MSG) {
        (*fn)(*c, *static_cast<HttpMessage*>(ev_data));
    }
    if (ev == MG_EV_CLOSE && c->is_listening) {
        delete fn;
        c->fn_data = nullptr;
    }
}

ConnectionRef Manager::http_listen(std::string_view url, HttpHandler handler)
{
    std::string url_str(url);
    auto* fn = new HttpHandler(std::move(handler));
    auto* c = nanosrv::http_listen(raw(), url_str.c_str(), http_handler_trampoline, fn);
    if (c == nullptr)
        delete fn;
    return ConnectionRef(c);
}

// -- Modern C++ API: HttpMessage methods --

std::optional<std::string_view> HttpMessage::header(const char* name) const
{
    struct Str* s = http_get_header(const_cast<HttpMessage*>(this), name);
    if (s == nullptr)
        return std::nullopt;
    return std::string_view(s->buf, s->len);
}

int HttpMessage::status_code() const
{
    return http_status(this);
}

std::pair<std::string, std::string> HttpMessage::credentials() const
{
    char user[256];
    char pass[256];
    http_creds(const_cast<HttpMessage*>(this), user, sizeof(user), pass, sizeof(pass));
    return {std::string(user), std::string(pass)};
}

// -- Modern C++ API: std::string returning url encode/decode --

std::string url_encode(std::string_view input)
{
    size_t buflen = input.size() * 3 + 1;
    std::string buf(buflen, '\0');
    size_t n = url_encode(input.data(), input.size(), buf.data(), buflen);
    buf.resize(n);
    return buf;
}

std::string url_decode(std::string_view input)
{
    size_t buflen = input.size() + 1;
    std::string buf(buflen, '\0');
    int n = url_decode(input.data(), input.size(), buf.data(), buflen, 0);
    if (n < 0)
        return {};
    buf.resize(static_cast<size_t>(n));
    return buf;
}

} // namespace nanosrv
