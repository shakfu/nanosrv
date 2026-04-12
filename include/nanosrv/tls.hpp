#pragma once
#include "net.hpp"

namespace nanosrv {

struct TlsOpts {
    struct Str ca;
    struct Str cert;
    struct Str key;
    struct Str name;
    int skip_verification;
};

void tls_init(struct Connection*, const struct TlsOpts*);
void tls_handshake(struct Connection*);
void tls_free(struct Connection*);
long tls_recv(struct Connection*, void* buf, size_t len);
long tls_send(struct Connection*, const void* buf, size_t len);
size_t tls_pending(struct Connection*);
void tls_flush(struct Connection*);
void tls_ctx_init(struct Mgr*);
void tls_ctx_free(struct Mgr*);

} // namespace nanosrv
