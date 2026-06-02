#include "nanosrv/net.hpp"
#include "nanosrv/tls.hpp"
#include "nanosrv/util.hpp"

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/debug.h>
#include <mbedtls/entropy.h>
#include <mbedtls/error.h>
#include <mbedtls/net_sockets.h> // for the MBEDTLS_ERR_NET_* BIO error codes
#include <mbedtls/pk.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509_crt.h>
#if defined(MBEDTLS_PSA_CRYPTO_C)
#include <psa/crypto.h>
#endif

// ---- module: tls_mbed ----
// mbedTLS-backed implementation of the nanosrv TLS hooks. Written against the
// public mbedTLS 3.x API. The integration model is the same one the rest of the
// event loop already assumes: raw inbound bytes are buffered in c->rtls and fed
// to mbedTLS through a BIO recv callback wrapping io_recv(); outbound ciphertext
// is handed to io_send() through a BIO send callback. Both callbacks translate
// nanosrv's MG_IO_* sentinels to mbedTLS WANT/error codes and back.

namespace nanosrv {

namespace {

// Per-manager TLS state. The RNG (entropy source + CTR_DRBG) is shared across
// every connection on this manager. nanosrv's ShardedManager gives each worker
// its own Mgr, hence its own context, so this state is never touched by more
// than one thread.
struct TlsCtx {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context drbg;
};

// Per-connection TLS state, hung off Connection::tls as a void*.
struct TlsConn {
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    mbedtls_x509_crt cert;
    mbedtls_pk_context pk;
    // mbedTLS requires that an mbedtls_ssl_write() returning WANT_WRITE be
    // retried with the identical (buf,len). When that happens we pin the
    // original arguments here and replay them until the buffered record drains
    // (see tls_send/tls_flush).
    const unsigned char* pending_buf;
    size_t pending_len;
};

void log_mbed_err(const Connection* c, const char* what, int rc)
{
    char msg[128];
    mbedtls_strerror(rc, msg, sizeof(msg));
    MG_ERROR(("%lu %s: -0x%x (%s)", c->id, what, -rc, msg));
}

// Parse a PEM or DER certificate chain into `crt`. An empty Str loads nothing
// (a server may have a CA but no own-cert, or vice versa). PEM input must be
// NUL-terminated and the terminating NUL must be counted in the length passed
// to mbedTLS, so we add one for PEM; DER is length-exact. A leading 0x30 byte
// is the ASN.1 SEQUENCE tag that begins DER; anything else is treated as PEM.
bool load_cert(Str s, mbedtls_x509_crt* crt)
{
    if (s.buf == nullptr || s.len == 0)
        return true;
    size_t len = s.len + (static_cast<unsigned char>(s.buf[0]) == 0x30 ? 0 : 1);
    int rc = mbedtls_x509_crt_parse(
        crt, reinterpret_cast<const unsigned char*>(s.buf), len);
    return rc == 0;
}

bool load_key(Str s, mbedtls_pk_context* pk, mbedtls_ctr_drbg_context* drbg)
{
    if (s.buf == nullptr || s.len == 0)
        return true;
    size_t len = s.len + (static_cast<unsigned char>(s.buf[0]) == 0x30 ? 0 : 1);
    int rc = mbedtls_pk_parse_key(
        pk, reinterpret_cast<const unsigned char*>(s.buf), len, nullptr, 0,
        mbedtls_ctr_drbg_random, drbg);
    return rc == 0;
}

// BIO callbacks: bridge mbedTLS record I/O to nanosrv's buffered socket I/O.
int bio_send(void* ctx, const unsigned char* buf, size_t len)
{
    long n = io_send(static_cast<Connection*>(ctx), buf, len);
    if (n == MG_IO_WAIT)
        return MBEDTLS_ERR_SSL_WANT_WRITE;
    if (n == MG_IO_RESET)
        return MBEDTLS_ERR_NET_CONN_RESET;
    if (n == MG_IO_ERR)
        return MBEDTLS_ERR_NET_SEND_FAILED;
    return static_cast<int>(n);
}

int bio_recv(void* ctx, unsigned char* buf, size_t len)
{
    long n = io_recv(static_cast<Connection*>(ctx), buf, len);
    if (n == MG_IO_WAIT)
        return MBEDTLS_ERR_SSL_WANT_READ;
    if (n == MG_IO_RESET)
        return MBEDTLS_ERR_NET_CONN_RESET;
    if (n == MG_IO_ERR)
        return MBEDTLS_ERR_NET_RECV_FAILED;
    return static_cast<int>(n);
}

} // namespace

bool tls_available() { return true; }

void tls_ctx_init(struct Mgr* mgr)
{
    auto* ctx = static_cast<TlsCtx*>(mem_calloc(1, sizeof(TlsCtx)));
    if (ctx == nullptr) {
        MG_ERROR(("TLS context OOM"));
        return;
    }
#if defined(MBEDTLS_PSA_CRYPTO_C)
    // TLS 1.3 in mbedTLS 3.x routes crypto through PSA. Initializing global PSA
    // state is idempotent, so calling it once per manager is safe.
    psa_crypto_init();
#endif
    mbedtls_entropy_init(&ctx->entropy);
    mbedtls_ctr_drbg_init(&ctx->drbg);
    int rc = mbedtls_ctr_drbg_seed(&ctx->drbg, mbedtls_entropy_func,
                                   &ctx->entropy, nullptr, 0);
    if (rc != 0) {
        char msg[128];
        mbedtls_strerror(rc, msg, sizeof(msg));
        MG_ERROR(("TLS RNG seed failed: -0x%x (%s)", -rc, msg));
        mbedtls_ctr_drbg_free(&ctx->drbg);
        mbedtls_entropy_free(&ctx->entropy);
        mem_free(ctx);
        return;
    }
    mgr->tls_ctx = ctx;
}

void tls_ctx_free(struct Mgr* mgr)
{
    auto* ctx = static_cast<TlsCtx*>(mgr->tls_ctx);
    if (ctx != nullptr) {
        mbedtls_ctr_drbg_free(&ctx->drbg);
        mbedtls_entropy_free(&ctx->entropy);
        mem_free(ctx);
        mgr->tls_ctx = nullptr;
    }
}

void tls_free(struct Connection* c)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    if (tls != nullptr) {
        mbedtls_ssl_free(&tls->ssl);
        mbedtls_pk_free(&tls->pk);
        mbedtls_x509_crt_free(&tls->cert);
        mbedtls_x509_crt_free(&tls->ca);
        mbedtls_ssl_config_free(&tls->conf);
        mem_free(tls);
        c->tls = nullptr;
    }
}

void tls_init(struct Connection* c, const struct TlsOpts* opts)
{
    auto* ctx = static_cast<TlsCtx*>(c->mgr->tls_ctx);
    if (ctx == nullptr) {
        error(c, "TLS context not initialized");
        return;
    }
    auto* tls = static_cast<TlsConn*>(mem_calloc(1, sizeof(TlsConn)));
    if (tls == nullptr) {
        error(c, "TLS OOM");
        return;
    }
    c->tls = tls;

    mbedtls_ssl_init(&tls->ssl);
    mbedtls_ssl_config_init(&tls->conf);
    mbedtls_x509_crt_init(&tls->ca);
    mbedtls_x509_crt_init(&tls->cert);
    mbedtls_pk_init(&tls->pk);

    int rc = mbedtls_ssl_config_defaults(
        &tls->conf,
        c->is_client ? MBEDTLS_SSL_IS_CLIENT : MBEDTLS_SSL_IS_SERVER,
        MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    if (rc != 0) {
        log_mbed_err(c, "tls defaults", rc);
        error(c, "TLS config failed");
        return tls_free(c);
    }
    mbedtls_ssl_conf_rng(&tls->conf, mbedtls_ctr_drbg_random, &ctx->drbg);

    // Peer verification. skip_verification or an absent CA disables it; note
    // that VERIFY_NONE on a TLS 1.3 client is not honored by mbedTLS, so a
    // client requiring no verification should pin TLS 1.2 separately.
    bool verify = !opts->skip_verification && opts->ca.len > 0;
    if (!verify) {
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_NONE);
    } else {
        if (!load_cert(opts->ca, &tls->ca)) {
            error(c, "TLS CA load failed");
            return tls_free(c);
        }
        mbedtls_ssl_conf_ca_chain(&tls->conf, &tls->ca, nullptr);
        mbedtls_ssl_conf_authmode(&tls->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    }

    // Own certificate + private key (required server-side, optional for a
    // client doing mutual TLS).
    if (!load_cert(opts->cert, &tls->cert) || !load_key(opts->key, &tls->pk,
                                                         &ctx->drbg)) {
        error(c, "TLS cert/key load failed");
        return tls_free(c);
    }
    if (tls->cert.version != 0) {
        rc = mbedtls_ssl_conf_own_cert(&tls->conf, &tls->cert, &tls->pk);
        if (rc != 0) {
            log_mbed_err(c, "own cert", rc);
            error(c, "TLS own-cert failed");
            return tls_free(c);
        }
    }

    rc = mbedtls_ssl_setup(&tls->ssl, &tls->conf);
    if (rc != 0) {
        log_mbed_err(c, "ssl setup", rc);
        error(c, "TLS setup failed");
        return tls_free(c);
    }

    // SNI / hostname verification for clients. opts->name selects the server
    // name presented and checked against the peer certificate.
    if (c->is_client && opts->name.len > 0) {
        char host[256];
        size_t n = opts->name.len < sizeof(host) - 1 ? opts->name.len
                                                      : sizeof(host) - 1;
        memcpy(host, opts->name.buf, n);
        host[n] = '\0';
        mbedtls_ssl_set_hostname(&tls->ssl, host);
    }

    mbedtls_ssl_set_bio(&tls->ssl, c, bio_send, bio_recv, nullptr);
    c->is_tls = 1;
    c->is_tls_hs = 1;
}

void tls_handshake(struct Connection* c)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    int rc = mbedtls_ssl_handshake(&tls->ssl);
    if (rc == 0) {
        c->is_tls_hs = 0;
        call(c, MG_EV_TLS_HS, nullptr);
    } else if (rc == MBEDTLS_ERR_SSL_WANT_READ
               || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
        // Handshake still in flight; resume on the next readable/writable poll.
    } else {
        log_mbed_err(c, "handshake", rc);
        error(c, "TLS handshake failed");
    }
}

long tls_recv(struct Connection* c, void* buf, size_t len)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    int n = mbedtls_ssl_read(&tls->ssl, static_cast<unsigned char*>(buf), len);
    if (n == MBEDTLS_ERR_SSL_WANT_READ || n == MBEDTLS_ERR_SSL_WANT_WRITE)
        return MG_IO_WAIT;
#if defined(MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
    // TLS 1.3 post-handshake ticket: not application data, so wait for more.
    if (n == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET)
        return MG_IO_WAIT;
#endif
    if (n <= 0)
        return MG_IO_ERR; // includes a clean close_notify -> tear the conn down
    return n;
}

long tls_send(struct Connection* c, const void* buf, size_t len)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    // If a prior write left a record half-flushed, mbedTLS demands the same
    // (buf,len) on retry. The connection send buffer can grow or move between
    // polls, so replay the pinned arguments rather than the current ones.
    // mbedTLS flushes its internal ciphertext on this path and does not re-read
    // the plaintext, so the pinned pointer need only match the original length.
    bool was_pending = c->is_tls_throttled;
    int n = was_pending
        ? mbedtls_ssl_write(&tls->ssl, tls->pending_buf, tls->pending_len)
        : mbedtls_ssl_write(&tls->ssl,
                            static_cast<const unsigned char*>(buf), len);
    c->is_tls_throttled = (n == MBEDTLS_ERR_SSL_WANT_READ
                           || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    if (was_pending)
        return MG_IO_WAIT; // we replayed the pinned record, not the new bytes
    if (c->is_tls_throttled) {
        // mbedTLS has already encrypted and buffered all `len` bytes; report
        // them as written so the caller advances its send buffer, and pin the
        // arguments for the flush retry.
        tls->pending_buf = static_cast<const unsigned char*>(buf);
        tls->pending_len = len;
        return static_cast<long>(len);
    }
    if (n <= 0)
        return MG_IO_ERR;
    return n;
}

void tls_flush(struct Connection* c)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    if (c->is_tls_throttled) {
        int n = mbedtls_ssl_write(&tls->ssl, tls->pending_buf,
                                  tls->pending_len);
        c->is_tls_throttled = (n == MBEDTLS_ERR_SSL_WANT_READ
                               || n == MBEDTLS_ERR_SSL_WANT_WRITE);
    }
}

size_t tls_pending(struct Connection* c)
{
    auto* tls = static_cast<TlsConn*>(c->tls);
    return tls == nullptr ? 0 : mbedtls_ssl_get_bytes_avail(&tls->ssl);
}

} // namespace nanosrv
