#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: tls_dummy ----
// TLS stubs (all no-ops) -- included unconditionally since nanosrv has no TLS

void tls_init(struct Connection* c, const struct TlsOpts* opts)
{
    (void)opts;
    error(c, "TLS is not enabled");
}

void tls_handshake(struct Connection* c) { (void)c; }

void tls_free(struct Connection* c) { (void)c; }

long tls_recv(struct Connection* c, void* buf, size_t len)
{
    return c == NULL || buf == NULL || len == 0 ? 0 : -1;
}

long tls_send(struct Connection* c, const void* buf, size_t len)
{
    return c == NULL || buf == NULL || len == 0 ? 0 : -1;
}

size_t tls_pending(struct Connection* c)
{
    (void)c;
    return 0;
}

void tls_flush(struct Connection* c) { (void)c; }

void tls_ctx_init(struct Mgr* mgr) { (void)mgr; }

void tls_ctx_free(struct Mgr* mgr) { (void)mgr; }

} // namespace nanosrv
