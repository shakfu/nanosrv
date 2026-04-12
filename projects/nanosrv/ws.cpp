#include "nanosrv/nanosrv.hpp"

namespace nanosrv {

// ---- module: ws ----

struct ws_msg {
    uint8_t flags;
    size_t header_len;
    size_t data_len;
};

size_t ws_vprintf(struct Connection* c, int op, const char* fmt,
                     va_list* ap)
{
    size_t len = c->send.len;
    size_t n = vxprintf(pfn_iobuf, &c->send, fmt, ap);
    ws_wrap(c, c->send.len - len, op);
    return n;
}

size_t ws_printf(struct Connection* c, int op, const char* fmt, ...)
{
    size_t len = 0;
    va_list ap;
    va_start(ap, fmt);
    len = ws_vprintf(c, op, fmt, &ap);
    va_end(ap);
    return len;
}

static void ws_handshake(struct Connection* c, const struct Str* wskey,
                         const struct Str* wsproto, const char* fmt,
                         va_list* ap)
{
    const char* magic = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    unsigned char sha[20], b64_sha[30];

    Sha1Ctx sha_ctx;
    sha1_init(&sha_ctx);
    sha1_update(&sha_ctx, reinterpret_cast<const unsigned char*>(wskey->buf), wskey->len);
    sha1_update(&sha_ctx, reinterpret_cast<const unsigned char*>(magic), 36);
    sha1_final(sha, &sha_ctx);
    base64_encode(sha, sizeof(sha), reinterpret_cast<char*>(b64_sha), sizeof(b64_sha));
    xprintf(pfn_iobuf, &c->send,
               "HTTP/1.1 101 Switching Protocols\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Accept: %s\r\n",
               b64_sha);
    if (fmt != NULL)
        vxprintf(pfn_iobuf, &c->send, fmt, ap);
    if (wsproto != NULL) {
        conn_printf(c, "Sec-WebSocket-Protocol: %.*s\r\n", static_cast<int>(wsproto->len),
                  wsproto->buf);
    }
    if (!send_data(c, "\r\n", 2))
        error(c, "OOM");
}

static uint32_t be32(const uint8_t* p)
{
    return ((static_cast<uint32_t>(p[3])) << 0) | ((static_cast<uint32_t>(p[2])) << 8)
        | ((static_cast<uint32_t>(p[1])) << 16) | ((static_cast<uint32_t>(p[0])) << 24);
}

static size_t ws_process(uint8_t* buf, size_t len, struct ws_msg* msg)
{
    size_t i, n = 0, mask_len = 0;
    memset(msg, 0, sizeof(*msg));
    if (len >= MG_WS_MIN_HEADER_SIZE) {
        n = buf[1] & MG_WS_PAYLOAD_LEN_MASK;
        mask_len = buf[1] & MG_WS_MASK_BIT ? MG_WS_MASK_LEN : 0;
        msg->flags = buf[0];
        if (n < MG_WS_EXTENDED_PAYLOAD_16 && len >= mask_len) {
            msg->data_len = n;
            msg->header_len = MG_WS_MIN_HEADER_SIZE + mask_len;
        } else if (n == MG_WS_EXTENDED_PAYLOAD_16 && len >= 4 + mask_len) {
            msg->header_len = 4 + mask_len;
            msg->data_len = ((static_cast<size_t>(buf[2])) << 8) | buf[3];
        } else if (len >= 10 + mask_len) {
            msg->header_len = 10 + mask_len;
            msg->data_len = static_cast<size_t>((static_cast<uint64_t>(be32(buf + 2)) << 32)
                                     + be32(buf + 6));
        }
    }
    if (msg->data_len > MG_WS_MAX_DATA_LEN)
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
    buf[0] = static_cast<uint8_t>(op | MG_WS_FIN_BIT);
    if (len < MG_WS_EXTENDED_PAYLOAD_16) {
        buf[1] = static_cast<unsigned char>(len);
        n = 2;
    } else if (len < 65536) {
        uint16_t tmp = mg_htons(static_cast<uint16_t>(len));
        buf[1] = MG_WS_EXTENDED_PAYLOAD_16;
        memcpy(&buf[2], &tmp, sizeof(tmp));
        n = 4;
    } else {
        uint32_t tmp;
        buf[1] = MG_WS_EXTENDED_PAYLOAD_64;
        tmp = mg_htonl(static_cast<uint32_t>((static_cast<uint64_t>(len)) >> 32));
        memcpy(&buf[2], &tmp, sizeof(tmp));
        tmp = mg_htonl(static_cast<uint32_t>(len & 0xffffffffU));
        memcpy(&buf[6], &tmp, sizeof(tmp));
        n = 10;
    }
    if (is_client) {
        buf[1] |= MG_WS_MASK_BIT;
        random_(&buf[n], MG_WS_MASK_LEN);
        n += MG_WS_MASK_LEN;
    }
    return n;
}

static void ws_mask(struct Connection* c, size_t len)
{
    if (c->is_client && c->send.buf != NULL) {
        size_t i;
        uint8_t *p = c->send.buf + c->send.len - len, *mask = p - 4;
        for (i = 0; i < len; i++)
            p[i] ^= mask[i & 3];
    }
}

size_t ws_send(struct Connection* c, const void* buf, size_t len, int op)
{
    uint8_t header[MG_WS_MAX_HEADER_SIZE];
    size_t header_len = mkhdr(len, op, c->is_client, header);
    if (!send_data(c, header, header_len))
        return 0;
    if (!send_data(c, buf, len))
        return header_len;
    MG_VERBOSE(("WS out: %d [%.*s]", (int)len, (int)len, buf));
    ws_mask(c, len);
    return header_len + len;
}

static bool ws_client_handshake(struct Connection* c)
{
    int n = http_get_request_len(c->recv.buf, c->recv.len);
    if (n < 0) {
        error(c, "not http");
    } else if (n > 0) {
        if (n < MG_WS_HANDSHAKE_MIN_LEN || memcmp(c->recv.buf + 9, MG_WS_HANDSHAKE_STATUS, MG_WS_HANDSHAKE_STATUS_LEN) != 0) {
            error(c, "ws handshake error");
        } else {
            struct HttpMessage hm;
            if (http_parse(reinterpret_cast<char*>(c->recv.buf), c->recv.len, &hm)) {
                c->is_websocket = 1;
                call(c, MG_EV_WS_OPEN, &hm);
            } else {
                error(c, "ws handshake error");
            }
        }
        iobuf_del(&c->recv, 0, static_cast<size_t>(n));
    } else {
        return true;
    }
    return false;
}

static void ws_cb(struct Connection* c, int ev, void* ev_data)
{
    struct ws_msg msg;
    size_t ofs = reinterpret_cast<size_t>(c->pfn_data);

    if (ev == MG_EV_READ) {
        if (c->is_client && !c->is_websocket && ws_client_handshake(c))
            return;

        while (ws_process(c->recv.buf + ofs, c->recv.len - ofs, &msg) > 0) {
            char* s = reinterpret_cast<char*>(c->recv.buf) + ofs + msg.header_len;
            struct WsMessage m;
            size_t len;
            uint8_t final, op;
            m.data.buf = s, m.data.len = msg.data_len, m.flags = msg.flags;
            len = msg.header_len + msg.data_len;
            final = msg.flags & 128;
            op = msg.flags & 15;
            switch (op) {
            case WEBSOCKET_OP_CONTINUE:
                call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_PING:
                MG_DEBUG(("%s", "WS PONG"));
                ws_send(c, s, msg.data_len, WEBSOCKET_OP_PONG);
                call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_PONG:
                call(c, MG_EV_WS_CTL, &m);
                break;
            case WEBSOCKET_OP_TEXT:
            case WEBSOCKET_OP_BINARY:
                if (final)
                    call(c, MG_EV_WS_MSG, &m);
                break;
            case WEBSOCKET_OP_CLOSE:
                MG_DEBUG(("%lu WS CLOSE", c->id));
                call(c, MG_EV_WS_CTL, &m);
                ws_send(c, m.data.buf, m.data.len, WEBSOCKET_OP_CLOSE);
                c->is_draining = 1;
                break;
            default:
                error(c, "unknown WS op %d", op);
                break;
            }

            if (final == 0 || op == 0) {
                if (op)
                    ofs++, len--, msg.header_len--;
                iobuf_del(&c->recv, ofs, msg.header_len);
                len -= msg.header_len;
                ofs += len;
                c->pfn_data = reinterpret_cast<void*>(ofs);
            }
            if (final && op)
                iobuf_del(&c->recv, ofs, len);
            if (final && !op && (ofs > 0)) {
                m.flags = c->recv.buf[0];
                m.data = str_n(reinterpret_cast<char*>(&c->recv.buf[1]), static_cast<size_t>(ofs - 1));
                call(c, MG_EV_WS_MSG, &m);
                iobuf_del(&c->recv, 0, ofs);
                ofs = 0;
                c->pfn_data = NULL;
            }
        }
    }
    (void)ev_data;
}

struct Connection* ws_connect(struct Mgr* mgr, const char* url,
                                    EventHandler fn, void* fn_data,
                                    const char* fmt, ...)
{
    struct Connection* c = connect(mgr, url, fn, fn_data);
    if (c != NULL) {
        char nonce[16], key[30];
        struct Str host = url_host(url);
        random_(nonce, sizeof(nonce));
        base64_encode(reinterpret_cast<unsigned char*>(nonce), sizeof(nonce), key,
                         sizeof(key));
        xprintf(pfn_iobuf, &c->send,
                   "GET %s HTTP/1.1\r\n"
                   "Upgrade: websocket\r\n"
                   "Host: %.*s\r\n"
                   "Connection: Upgrade\r\n"
                   "Sec-WebSocket-Version: 13\r\n"
                   "Sec-WebSocket-Key: %s\r\n",
                   url_uri(url), (int)host.len, host.buf, key);
        if (fmt != NULL) {
            va_list ap;
            va_start(ap, fmt);
            vxprintf(pfn_iobuf, &c->send, fmt, &ap);
            va_end(ap);
        }
        xprintf(pfn_iobuf, &c->send, "\r\n");
        c->pfn = ws_cb;
        c->pfn_data = NULL;
    }
    return c;
}

void ws_upgrade(struct Connection* c, struct HttpMessage* hm,
                   const char* fmt, ...)
{
    struct Str* wskey = http_get_header(hm, "Sec-WebSocket-Key");
    c->pfn = ws_cb;
    c->pfn_data = NULL;
    if (wskey == NULL) {
        http_reply(c, 426, "", "WS upgrade expected\n");
        c->is_draining = 1;
    } else {
        struct Str* wsproto = http_get_header(hm,
                                                    "Sec-WebSocket-Protocol");
        va_list ap;
        va_start(ap, fmt);
        ws_handshake(c, wskey, wsproto, fmt, &ap);
        va_end(ap);
        c->is_websocket = 1;
        c->is_resp = 0;
        call(c, MG_EV_WS_OPEN, hm);
    }
}

size_t ws_wrap(struct Connection* c, size_t len, int op)
{
    uint8_t header[14], *p;
    size_t header_len = mkhdr(len, op, c->is_client, header);

    if (iobuf_add(&c->send, c->send.len, NULL, header_len) != 0) {
        p = &c->send.buf[c->send.len - len];
        memmove(p, p - header_len, len);
        memcpy(p - header_len, header, header_len);
        ws_mask(c, len);
    }
    return c->send.len;
}

} // namespace nanosrv
