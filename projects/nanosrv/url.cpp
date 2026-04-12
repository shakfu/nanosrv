#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: url ----

struct url {
    size_t key, user, pass, host, port, uri, end;
};

int url_is_ssl(const char* url)
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

struct Str url_host(const char* url)
{
    struct url u = urlparse(url);
    size_t n = u.port ? u.port - u.host - 1
        : u.uri       ? u.uri - u.host
                      : u.end - u.host;
    struct Str s = str_n(url + u.host, n);
    return s;
}

const char* url_uri(const char* url)
{
    struct url u = urlparse(url);
    return u.uri ? url + u.uri : "/";
}

unsigned short url_port(const char* url)
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

struct Str url_user(const char* url)
{
    struct url u = urlparse(url);
    struct Str s = Str("");
    if (u.user && (u.pass || u.host)) {
        size_t n = u.pass ? u.pass - u.user - 1 : u.host - u.user - 1;
        s = str_n(url + u.user, n);
    }
    return s;
}

struct Str url_pass(const char* url)
{
    struct url u = urlparse(url);
    struct Str s = str_n("", 0UL);
    if (u.pass && u.host) {
        size_t n = u.host - u.pass - 1;
        s = str_n(url + u.pass, n);
    }
    return s;
}

// -- Modern C++ API: Url::parse --

Url Url::parse(std::string_view url_sv)
{
    // The underlying C functions require null-terminated strings, and the
    // returned Str values point into the *original* string. We create a
    // temporary null-terminated copy for parsing, but since url_host returns
    // a Str pointing into the passed-in const char*, and url_uri returns a
    // const char* into the passed-in string, we need the source to remain
    // alive. We copy the data we need into the Url struct fields that are
    // string_views -- the caller must keep the original URL string alive.
    std::string url_z(url_sv);
    const char* url = url_z.c_str();

    Url result;
    result.is_ssl = (url_is_ssl(url) != 0);
    result.port = url_port(url);

    // url_host returns a Str pointing into url -- we need to convert to
    // a string_view into the *caller's* original data (url_sv).
    struct Str host_str = url_host(url);
    if (host_str.buf != nullptr) {
        size_t offset = static_cast<size_t>(host_str.buf - url);
        result.host = url_sv.substr(offset, host_str.len);
    }

    // url_uri returns a pointer into url or a static "/".
    const char* uri = url_uri(url);
    if (uri != nullptr && uri[0] == '/' && uri != url + 0) {
        // Check if it points into our copy
        if (uri >= url && uri < url + url_z.size()) {
            size_t offset = static_cast<size_t>(uri - url);
            result.path = url_sv.substr(offset);
        } else {
            // Static "/" returned for URLs without a path
            result.path = "/";
        }
    } else {
        result.path = "/";
    }

    return result;
}

} // namespace nanosrv
