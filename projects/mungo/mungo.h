// Copyright (c) 2004-2013 Sergey Lyubka
// Copyright (c) 2013-2025 Cesanta Software Limited
// All rights reserved
//
// This software is dual-licensed: you can redistribute it and/or modify
// it under the terms of the GNU General Public License version 2 as
// published by the Free Software Foundation. For the terms of this
// license, see http://www.gnu.org/licenses/
//
// You are free to use this software under the terms of the GNU General
// Public License, but WITHOUT ANY WARRANTY; without even the implied
// warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU General Public License for more details.
//
// Alternatively, you can license this software under a commercial
// license, as set out in https://www.mongoose.ws/licensing/
//
// SPDX-License-Identifier: GPL-2.0-only or commercial
//
// Mungo: a minimal subset of Mongoose 7.21, extracted for use as an
// HTTP server suitable for OpenAI-compatible API endpoints.

#ifndef MUNGO_H
#define MUNGO_H

#ifdef __cplusplus
extern "C" {
#endif

// ---------------------------------------------------------------------------
// Architecture detection (UNIX and WIN32 only)
// ---------------------------------------------------------------------------
#define MG_ARCH_UNIX 1
#define MG_ARCH_WIN32 2

#if !defined(MG_ARCH)
#if defined(__unix__) || defined(__APPLE__)
#define MG_ARCH MG_ARCH_UNIX
#elif defined(_WIN32)
#define MG_ARCH MG_ARCH_WIN32
#endif
#endif

#if !defined(MG_ARCH)
#error "MG_ARCH is not specified and we couldn't guess it."
#endif

#define MG_BIG_ENDIAN (*(uint16_t*)"\0\xff" < 0x100)

// ---------------------------------------------------------------------------
// Platform includes
// ---------------------------------------------------------------------------
#if MG_ARCH == MG_ARCH_UNIX

#define _DARWIN_UNLIMITED_SELECT 1

#if defined(__APPLE__)
#include <mach/mach_time.h>
#endif

#if !defined(MG_ENABLE_EPOLL) && defined(__linux__)
#define MG_ENABLE_EPOLL 1
#elif !defined(MG_ENABLE_POLL)
#define MG_ENABLE_POLL 1
#endif

#include <arpa/inet.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(MG_ENABLE_EPOLL) && MG_ENABLE_EPOLL
#include <sys/epoll.h>
#elif defined(MG_ENABLE_POLL) && MG_ENABLE_POLL
#include <poll.h>
#else
#include <sys/select.h>
#endif

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#ifndef MG_PATH_MAX
#define MG_PATH_MAX FILENAME_MAX
#endif

#ifndef MG_IO_SIZE
#define MG_IO_SIZE 16384
#endif

#endif // MG_ARCH == MG_ARCH_UNIX

#if MG_ARCH == MG_ARCH_WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <io.h>
#include <limits.h>
#include <process.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <winerror.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#ifndef MG_ENABLE_POLL
#define MG_ENABLE_POLL 0
#endif

#ifndef MG_ENABLE_EPOLL
#define MG_ENABLE_EPOLL 0
#endif

#ifndef MG_ENABLE_WINSOCK
#define MG_ENABLE_WINSOCK 1
#endif

#if MG_ENABLE_WINSOCK
#pragma comment(lib, "ws2_32.lib")
#define MG_INVALID_SOCKET INVALID_SOCKET
#define MG_SOCKET_TYPE SOCKET
typedef int socklen_t;
#define closesocket(x) closesocket(x)
#ifndef SO_EXCLUSIVEADDRUSE
#define SO_EXCLUSIVEADDRUSE ((int)(~SO_REUSEADDR))
#endif
#define MG_SOCK_ERR(errcode) ((errcode) < 0 ? WSAGetLastError() : 0)
#define MG_SOCK_PENDING(errcode)                                              \
    (((errcode) < 0)                                                          \
     && (WSAGetLastError() == WSAEINTR || WSAGetLastError() == WSAEINPROGRESS \
         || WSAGetLastError() == WSAEWOULDBLOCK))
#define MG_SOCK_RESET(errcode)                                                \
    (((errcode) < 0) && (WSAGetLastError() == WSAECONNRESET))
#endif // MG_ENABLE_WINSOCK

#define MG_DIRSEP '\\'

#ifndef MG_PATH_MAX
#define MG_PATH_MAX FILENAME_MAX
#endif

#ifndef MG_IO_SIZE
#define MG_IO_SIZE 16384
#endif

#ifndef SIGPIPE
#define SIGPIPE 0
#endif

#ifndef __cplusplus
#define snprintf _snprintf
#define vsnprintf _vsnprintf
#ifndef strdup
#define strdup(x) _strdup(x)
#endif
#endif

#ifndef alloca
#define alloca(a) _alloca(a)
#endif

typedef unsigned long nfds_t;

#endif // MG_ARCH == MG_ARCH_WIN32

// ---------------------------------------------------------------------------
// Feature flags and config defaults
// ---------------------------------------------------------------------------
#ifndef MG_ENABLE_LOG
#define MG_ENABLE_LOG 1
#endif

#ifndef MG_ENABLE_SOCKET
#define MG_ENABLE_SOCKET 1
#endif

#ifndef MG_ENABLE_POLL
// already set per-platform above
#endif

#ifndef MG_ENABLE_EPOLL
// already set per-platform above
#endif

#ifndef MG_ENABLE_IPV6
#define MG_ENABLE_IPV6 0
#endif

#ifndef MG_IO_SIZE
#define MG_IO_SIZE 512
#endif

#ifndef MG_MAX_RECV_SIZE
#define MG_MAX_RECV_SIZE (3UL * 1024UL * 1024UL)
#endif

#ifndef MG_DATA_SIZE
#define MG_DATA_SIZE 32
#endif

#ifndef MG_MAX_HTTP_HEADERS
#define MG_MAX_HTTP_HEADERS 30
#endif

enum { MG_IO_ERR = -1, MG_IO_WAIT = -2, MG_IO_RESET = -3 };

#define MG_TLS_NONE 0
#define MG_TLS_MBED 1
#define MG_TLS_OPENSSL 2
#define MG_TLS_BUILTIN 3
#define MG_TLS_CUSTOM 4
#define MG_TLS_WOLFSSL 5

#ifndef MG_TLS
#define MG_TLS MG_TLS_NONE
#endif

#ifndef MG_PATH_MAX
#ifdef PATH_MAX
#define MG_PATH_MAX PATH_MAX
#else
#define MG_PATH_MAX 128
#endif
#endif

#ifndef MG_SOCK_LISTEN_BACKLOG_SIZE
#define MG_SOCK_LISTEN_BACKLOG_SIZE 128
#endif

#ifndef MG_DIRSEP
#define MG_DIRSEP '/'
#endif

#ifndef MG_INVALID_SOCKET
#define MG_INVALID_SOCKET (-1)
#endif

#ifndef MG_SOCKET_TYPE
#define MG_SOCKET_TYPE int
#endif

// Epoll macros
#if MG_ENABLE_EPOLL
#define MG_EPOLL_ADD(c)                                                       \
    do {                                                                      \
        struct epoll_event ev = { EPOLLIN | EPOLLERR | EPOLLHUP, { c } };     \
        epoll_ctl(c->mgr->epoll_fd, EPOLL_CTL_ADD, (int)(size_t)c->fd, &ev);  \
    } while (0)
#define MG_EPOLL_MOD(c, wr)                                                   \
    do {                                                                      \
        struct epoll_event ev = { EPOLLIN | EPOLLERR | EPOLLHUP, { c } };     \
        if (wr)                                                               \
            ev.events |= EPOLLOUT;                                            \
        epoll_ctl(c->mgr->epoll_fd, EPOLL_CTL_MOD, (int)(size_t)c->fd, &ev);  \
    } while (0)
#else
#define MG_EPOLL_ADD(c)
#define MG_EPOLL_MOD(c, wr)
#endif

// Profiling stubs (disabled)
#define MG_PROF_INIT(c)
#define MG_PROF_FREE(c)
#define MG_PROF_ADD(c, name)
#define MG_PROF_DUMP(c)

#ifndef MG_ENABLE_ASSERT
#define MG_ENABLE_ASSERT 0
#endif

#if MG_ENABLE_ASSERT
#include <assert.h>
#elif !defined(assert)
#define assert(x)
#endif

// ---------------------------------------------------------------------------
// Byte order helpers
// ---------------------------------------------------------------------------
#define MG_U8P(ADDR) ((uint8_t*)(ADDR))

#define MG_LOAD_BE16(p)                                                       \
    ((uint16_t)(((uint16_t)MG_U8P(p)[0] << 8U) | MG_U8P(p)[1]))
#define MG_LOAD_BE32(p)                                                       \
    ((uint32_t)(((uint32_t)MG_U8P(p)[0] << 24U)                               \
                | ((uint32_t)MG_U8P(p)[1] << 16U)                             \
                | ((uint32_t)MG_U8P(p)[2] << 8U) | MG_U8P(p)[3]))
#define MG_LOAD_BE64(p)                                                       \
    ((uint64_t)(((uint64_t)MG_U8P(p)[0] << 56U)                               \
                | ((uint64_t)MG_U8P(p)[1] << 48U)                             \
                | ((uint64_t)MG_U8P(p)[2] << 40U)                             \
                | ((uint64_t)MG_U8P(p)[3] << 32U)                             \
                | ((uint64_t)MG_U8P(p)[4] << 24U)                             \
                | ((uint64_t)MG_U8P(p)[5] << 16U)                             \
                | ((uint64_t)MG_U8P(p)[6] << 8U) | MG_U8P(p)[7]))

uint16_t mg_ntohs(uint16_t net);
uint32_t mg_ntohl(uint32_t net);
uint64_t mg_ntohll(uint64_t net);
#define mg_htons(x) mg_ntohs(x)
#define mg_htonl(x) mg_ntohl(x)
#define mg_htonll(x) mg_ntohll(x)

// ---------------------------------------------------------------------------
// Utility macros
// ---------------------------------------------------------------------------
#define MG_U32(a, b, c, d)                                                    \
    (((uint32_t)((a) & 255) << 24) | ((uint32_t)((b) & 255) << 16)            \
     | ((uint32_t)((c) & 255) << 8) | (uint32_t)((d) & 255))

#define MG_IPV4(a, b, c, d) mg_htonl(MG_U32(a, b, c, d))

#define MG_IPADDR_PARTS(ADDR)                                                 \
    MG_U8P(ADDR)[0], MG_U8P(ADDR)[1], MG_U8P(ADDR)[2], MG_U8P(ADDR)[3]

// Linked list macros
#define LIST_ADD_HEAD(type_, head_, elem_)                                    \
    do {                                                                      \
        (elem_)->next = (*head_);                                             \
        *(head_) = (elem_);                                                   \
    } while (0)

#define LIST_DELETE(type_, head_, elem_)                                      \
    do {                                                                      \
        type_** h = head_;                                                    \
        while (*h != (elem_))                                                 \
            h = &(*h)->next;                                                  \
        *h = (elem_)->next;                                                   \
    } while (0)

// ---------------------------------------------------------------------------
// mg_str: string slice
// ---------------------------------------------------------------------------
struct mg_str {
    char* buf;
    size_t len;
};

#define mg_str(s) mg_str_s(s)

struct mg_str mg_str_s(const char* s);
struct mg_str mg_str_n(const char* s, size_t n);
int mg_casecmp(const char* s1, const char* s2);
int mg_strcmp(const struct mg_str str1, const struct mg_str str2);
int mg_strcasecmp(const struct mg_str str1, const struct mg_str str2);
struct mg_str mg_strdup(const struct mg_str s);
bool mg_match(struct mg_str str, struct mg_str pattern, struct mg_str* caps);
bool mg_span(struct mg_str s, struct mg_str* a, struct mg_str* b, char delim);
bool mg_str_to_num(struct mg_str, int base, void* val, size_t val_len);

// ---------------------------------------------------------------------------
// mg_iobuf: growable IO buffer
// ---------------------------------------------------------------------------
struct mg_iobuf {
    unsigned char* buf;
    size_t size;
    size_t len;
    size_t align;
};

bool mg_iobuf_init(struct mg_iobuf*, size_t, size_t);
bool mg_iobuf_resize(struct mg_iobuf*, size_t);
void mg_iobuf_free(struct mg_iobuf*);
size_t mg_iobuf_add(struct mg_iobuf*, size_t, const void*, size_t);
size_t mg_iobuf_del(struct mg_iobuf*, size_t ofs, size_t len);

// ---------------------------------------------------------------------------
// Printf engine
// ---------------------------------------------------------------------------
typedef void (*mg_pfn_t)(char, void*);
typedef size_t (*mg_pm_t)(mg_pfn_t, void*, va_list*);

size_t mg_vxprintf(void (*)(char, void*), void*, const char* fmt, va_list*);
size_t mg_xprintf(void (*fn)(char, void*), void*, const char* fmt, ...);
size_t mg_vsnprintf(char* buf, size_t len, const char* fmt, va_list* ap);
size_t mg_snprintf(char*, size_t, const char* fmt, ...);
char* mg_vmprintf(const char* fmt, va_list* ap);
char* mg_mprintf(const char* fmt, ...);

// %M print helpers
size_t mg_print_base64(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_esc(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_hex(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_ip(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_ip_port(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_ip4(void (*out)(char, void*), void* arg, va_list* ap);
size_t mg_print_ip6(void (*out)(char, void*), void* arg, va_list* ap);

// Output functions
void mg_pfn_iobuf(char ch, void* param);
void mg_pfn_iobuf_noresize(char ch, void* param);
void mg_pfn_stdout(char c, void* param);

#define MG_ESC(str) mg_print_esc, 0, (str)

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
enum { MG_LL_NONE, MG_LL_ERROR, MG_LL_INFO, MG_LL_DEBUG, MG_LL_VERBOSE };
extern int mg_log_level;

void mg_log(const char* fmt, ...);
void mg_log_prefix(int ll, const char* file, int line, const char* fname);
void mg_hexdump(const void* buf, size_t len);
void mg_log_set_fn(mg_pfn_t fn, void* param);

#define mg_log_set(level_) mg_log_level = (level_)

#if MG_ENABLE_LOG
#define MG___FUNC__ __func__
#define MG_LOG(level, args)                                                   \
    do {                                                                      \
        if ((level) <= mg_log_level) {                                        \
            mg_log_prefix((level), __FILE__, __LINE__, MG___FUNC__);          \
            mg_log args;                                                      \
        }                                                                     \
    } while (0)
#else
#define MG_LOG(level, args)                                                   \
    do {                                                                      \
        if (0)                                                                \
            mg_log args;                                                      \
    } while (0)
#endif

#define MG_ERROR(args) MG_LOG(MG_LL_ERROR, args)
#define MG_INFO(args) MG_LOG(MG_LL_INFO, args)
#define MG_DEBUG(args) MG_LOG(MG_LL_DEBUG, args)
#define MG_VERBOSE(args) MG_LOG(MG_LL_VERBOSE, args)

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------
struct mg_timer {
    uint64_t period_ms;
    uint64_t expire;
    unsigned flags;
#define MG_TIMER_ONCE 0
#define MG_TIMER_REPEAT 1
#define MG_TIMER_RUN_NOW 2
#define MG_TIMER_CALLED 4
#define MG_TIMER_AUTODELETE 8
    void (*fn)(void*);
    void* arg;
    struct mg_timer* next;
};

void mg_timer_init(struct mg_timer** head, struct mg_timer* timer,
                   uint64_t milliseconds, unsigned flags, void (*fn)(void*),
                   void* arg);
void mg_timer_free(struct mg_timer** head, struct mg_timer*);
void mg_timer_poll(struct mg_timer** head, uint64_t new_ms);
bool mg_timer_expired(uint64_t* expiration, uint64_t period, uint64_t now);

// ---------------------------------------------------------------------------
// Utility functions
// ---------------------------------------------------------------------------
void* mg_calloc(size_t count, size_t size);
void mg_free(void* ptr);
void mg_bzero(volatile unsigned char* buf, size_t len);
bool mg_random(void* buf, size_t len);
char* mg_random_str(char* buf, size_t len);
uint64_t mg_millis(void);

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------
unsigned short mg_url_port(const char* url);
int mg_url_is_ssl(const char* url);
struct mg_str mg_url_host(const char* url);
const char* mg_url_uri(const char* url);

// ---------------------------------------------------------------------------
// Networking core
// ---------------------------------------------------------------------------

// Forward declaration to break circular reference between mg_connection and
// mg_mgr: both reference each other via pointer.
struct mg_connection;

// Event handler callback type (must precede struct mg_connection)
typedef void (*mg_event_handler_t)(struct mg_connection*, int ev,
                                   void* ev_data);

// Event types
enum {
    MG_EV_ERROR,
    MG_EV_OPEN,
    MG_EV_POLL,
    MG_EV_RESOLVE,
    MG_EV_CONNECT,
    MG_EV_ACCEPT,
    MG_EV_TLS_HS,
    MG_EV_READ,
    MG_EV_WRITE,
    MG_EV_CLOSE,
    MG_EV_HTTP_HDRS,
    MG_EV_HTTP_MSG,
    MG_EV_WS_OPEN,
    MG_EV_WS_MSG,
    MG_EV_WS_CTL,
    MG_EV_WAKEUP = 20,
    MG_EV_USER = 100
};

struct mg_addr {
    union {
        uint8_t ip[16];
        uint32_t ip4;
        uint64_t ip6[2];
    } addr;
    uint16_t port;
    uint8_t scope_id;
    bool is_ip6;
};

struct mg_dns {
    const char* url;
    struct mg_connection* c;
};

#define MG_DNS_RTYPE_A 1
#define MG_DNS_RTYPE_PTR 12
#define MG_DNS_RTYPE_TXT 16
#define MG_DNS_RTYPE_AAAA 28
#define MG_DNS_RTYPE_SRV 33

struct mg_dns_message {
    uint16_t txnid;
    bool resolved;
    struct mg_addr addr;
    char name[256];
};

struct mg_dns_header {
    uint16_t txnid;
    uint16_t flags;
    uint16_t num_questions;
    uint16_t num_answers;
    uint16_t num_authority_prs;
    uint16_t num_other_prs;
};

struct mg_dns_rr {
    uint16_t nlen;
    uint16_t atype;
    uint16_t aclass;
    uint16_t alen;
};

struct mg_mgr {
    struct mg_connection* conns;
    struct mg_dns dns4;
    struct mg_dns dns6;
    int dnstimeout;
    bool use_dns6;
    unsigned long nextid;
    void* userdata;
    void* tls_ctx;
    uint16_t mqtt_id;
    void* active_dns_requests;
    struct mg_timer* timers;
    int epoll_fd;
    void* ifp;
    size_t extraconnsize;
    MG_SOCKET_TYPE pipe;
};

struct mg_connection {
    struct mg_connection* next;
    struct mg_mgr* mgr;
    struct mg_addr loc;
    struct mg_addr rem;
    void* fd;
    unsigned long id;
    struct mg_iobuf recv;
    struct mg_iobuf send;
    struct mg_iobuf prof;
    struct mg_iobuf rtls;
    mg_event_handler_t fn;
    void* fn_data;
    mg_event_handler_t pfn;
    void* pfn_data;
    char data[MG_DATA_SIZE];
    void* tls;
    unsigned is_listening : 1;
    unsigned is_client : 1;
    unsigned is_accepted : 1;
    unsigned is_resolving : 1;
    unsigned is_arplooking : 1;
    unsigned is_connecting : 1;
    unsigned is_tls : 1;
    unsigned is_tls_hs : 1;
    unsigned is_udp : 1;
    unsigned is_websocket : 1;
    unsigned is_mqtt5 : 1;
    unsigned is_hexdumping : 1;
    unsigned is_draining : 1;
    unsigned is_closing : 1;
    unsigned is_full : 1;
    unsigned is_tls_throttled : 1;
    unsigned is_resp : 1;
    unsigned is_readable : 1;
    unsigned is_writable : 1;
};

void mg_call(struct mg_connection* c, int ev, void* ev_data);
void mg_error(struct mg_connection* c, const char* fmt, ...);

void mg_mgr_poll(struct mg_mgr*, int ms);
void mg_mgr_init(struct mg_mgr*);
void mg_mgr_free(struct mg_mgr*);

struct mg_connection* mg_listen(struct mg_mgr*, const char* url,
                                mg_event_handler_t fn, void* fn_data);
bool mg_send(struct mg_connection*, const void*, size_t);
size_t mg_printf(struct mg_connection*, const char* fmt, ...);
size_t mg_vprintf(struct mg_connection*, const char* fmt, va_list* ap);
bool mg_aton(struct mg_str str, struct mg_addr* addr);
bool mg_wakeup(struct mg_mgr*, unsigned long id, const void* buf, size_t len);

struct mg_connection* mg_alloc_conn(struct mg_mgr*);
void mg_close_conn(struct mg_connection* c);
bool mg_open_listener(struct mg_connection* c, const char* url);

struct mg_connection* mg_connect(struct mg_mgr*, const char* url,
                                 mg_event_handler_t fn, void* fn_data);
struct mg_connection* mg_connect_svc(struct mg_mgr*, const char* url,
                                     mg_event_handler_t fn, void* fn_data,
                                     mg_event_handler_t pfn, void* pfn_data);
void mg_connect_resolved(struct mg_connection*);
struct mg_connection* mg_wrapfd(struct mg_mgr* mgr, int fd,
                                mg_event_handler_t fn, void* fn_data);
long mg_io_recv(struct mg_connection* c, void* buf, size_t len);
long mg_io_send(struct mg_connection* c, const void* buf, size_t len);

void mg_resolve(struct mg_connection*, const char* url);
void mg_resolve_cancel(struct mg_connection*);
bool mg_dns_parse(const uint8_t* buf, size_t len, struct mg_dns_message*);
size_t mg_dns_parse_rr(const uint8_t* buf, size_t len, size_t ofs,
                       bool is_question, struct mg_dns_rr*);

struct mg_timer* mg_timer_add(struct mg_mgr* mgr, uint64_t milliseconds,
                              unsigned flags, void (*fn)(void*), void* arg);

// ---------------------------------------------------------------------------
// HTTP
// ---------------------------------------------------------------------------
struct mg_http_header {
    struct mg_str name;
    struct mg_str value;
};

struct mg_http_message {
    struct mg_str method, uri, query, proto;
    struct mg_http_header headers[MG_MAX_HTTP_HEADERS];
    struct mg_str body;
    struct mg_str head;
    struct mg_str message;
};

int mg_http_parse(const char* s, size_t len, struct mg_http_message*);
int mg_http_get_request_len(const unsigned char* buf, size_t buf_len);
void mg_http_printf_chunk(struct mg_connection* cnn, const char* fmt, ...);
void mg_http_write_chunk(struct mg_connection* c, const char* buf, size_t len);
struct mg_connection* mg_http_listen(struct mg_mgr*, const char* url,
                                     mg_event_handler_t fn, void* fn_data);
void mg_http_reply(struct mg_connection*, int status_code, const char* headers,
                   const char* body_fmt, ...);
struct mg_str* mg_http_get_header(struct mg_http_message*, const char* name);
struct mg_str mg_http_var(struct mg_str buf, struct mg_str name);
int mg_http_get_var(const struct mg_str*, const char* name, char*, size_t);
int mg_url_decode(const char* s, size_t n, char* to, size_t to_len, int form);
int mg_http_status(const struct mg_http_message* hm);
struct mg_connection* mg_http_connect(struct mg_mgr*, const char* url,
                                      mg_event_handler_t fn, void* fn_data);
void mg_http_bauth(struct mg_connection*, const char* user, const char* pass);
struct mg_http_part {
    struct mg_str name;
    struct mg_str filename;
    struct mg_str body;
};
size_t mg_http_next_multipart(struct mg_str body, size_t ofs,
                              struct mg_http_part* part);
void mg_http_creds(struct mg_http_message* hm, char* user, size_t userlen,
                   char* pass, size_t passlen);
struct mg_str mg_http_get_header_var(struct mg_str s, struct mg_str v);
size_t mg_url_encode(const char* s, size_t sl, char* buf, size_t len);
bool mg_wakeup_init(struct mg_mgr* mgr);

// ---------------------------------------------------------------------------
// TLS stubs
// ---------------------------------------------------------------------------
struct mg_tls_opts {
    struct mg_str ca;
    struct mg_str cert;
    struct mg_str key;
    struct mg_str name;
    int skip_verification;
};
void mg_tls_init(struct mg_connection*, const struct mg_tls_opts*);
void mg_tls_handshake(struct mg_connection*);
void mg_tls_free(struct mg_connection*);
long mg_tls_recv(struct mg_connection*, void* buf, size_t len);
long mg_tls_send(struct mg_connection*, const void* buf, size_t len);
size_t mg_tls_pending(struct mg_connection*);
void mg_tls_flush(struct mg_connection*);
void mg_tls_ctx_init(struct mg_mgr*);
void mg_tls_ctx_free(struct mg_mgr*);

// ---------------------------------------------------------------------------
// Base64
// ---------------------------------------------------------------------------
size_t mg_base64_update(unsigned char ch, char* to, size_t n);
size_t mg_base64_final(char* to, size_t n);
size_t mg_base64_encode(const unsigned char* p, size_t n, char* to, size_t dl);
size_t mg_base64_decode(const char* src, size_t n, char* dst, size_t dl);

// ---------------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------------
#ifndef MG_JSON_MAX_DEPTH
#define MG_JSON_MAX_DEPTH 30
#endif

enum { MG_JSON_TOO_DEEP = -1, MG_JSON_INVALID = -2, MG_JSON_NOT_FOUND = -3 };

int mg_json_get(struct mg_str json, const char* path, int* toklen);
struct mg_str mg_json_get_tok(struct mg_str json, const char* path);
bool mg_json_get_num(struct mg_str json, const char* path, double* v);
bool mg_json_get_bool(struct mg_str json, const char* path, bool* v);
long mg_json_get_long(struct mg_str json, const char* path, long dflt);
char* mg_json_get_str(struct mg_str json, const char* path);
bool mg_json_unescape(struct mg_str str, char* buf, size_t len);
size_t mg_json_next(struct mg_str obj, size_t ofs, struct mg_str* key,
                    struct mg_str* val);

// ---------------------------------------------------------------------------
// SHA-1 (required by WebSocket handshake)
// ---------------------------------------------------------------------------
typedef struct {
    uint32_t state[5];
    uint32_t count[2];
    unsigned char buffer[64];
} mg_sha1_ctx;

void mg_sha1_init(mg_sha1_ctx*);
void mg_sha1_update(mg_sha1_ctx*, const unsigned char* data, size_t len);
void mg_sha1_final(unsigned char digest[20], mg_sha1_ctx*);

// ---------------------------------------------------------------------------
// WebSocket
// ---------------------------------------------------------------------------
#define WEBSOCKET_OP_CONTINUE 0
#define WEBSOCKET_OP_TEXT 1
#define WEBSOCKET_OP_BINARY 2
#define WEBSOCKET_OP_CLOSE 8
#define WEBSOCKET_OP_PING 9
#define WEBSOCKET_OP_PONG 10

struct mg_ws_message {
    struct mg_str data;
    uint8_t flags;
};

struct mg_connection* mg_ws_connect(struct mg_mgr*, const char* url,
                                    mg_event_handler_t fn, void* fn_data,
                                    const char* fmt, ...);
void mg_ws_upgrade(struct mg_connection*, struct mg_http_message*,
                   const char* fmt, ...);
size_t mg_ws_send(struct mg_connection*, const void* buf, size_t len, int op);
size_t mg_ws_wrap(struct mg_connection*, size_t len, int op);
size_t mg_ws_printf(struct mg_connection* c, int op, const char* fmt, ...);
size_t mg_ws_vprintf(struct mg_connection* c, int op, const char* fmt,
                     va_list*);

#ifdef __cplusplus
}
#endif

#endif // MUNGO_H
