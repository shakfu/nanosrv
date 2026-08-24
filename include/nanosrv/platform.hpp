#pragma once

// rand_s() (used for randomness on Windows in util.cpp) is only declared by
// <stdlib.h> when _CRT_RAND_S is defined *before* the first include of it.
// Define it here, ahead of every include, so the MSVC build sees the prototype.
#if defined(_WIN32) && !defined(_CRT_RAND_S)
#define _CRT_RAND_S
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

// Readiness backend selection.
//
// io_uring is opt-in (-DMG_ENABLE_IO_URING=1), not auto-detected. It used to be
// enabled by __has_include(<liburing.h>), which made the poller depend on
// whether a development package happened to be installed on the build machine:
// two builds of the same source, same compiler, could use different pollers,
// which is a poor property for reproducible builds and for benchmark
// comparisons. The auto-detection was also broken -- setting
// MG_ENABLE_IO_URING=1 explicitly skipped this branch and fell through to the
// epoll branch, enabling both and tripping the mutual-exclusion static_assert
// below. Note the backend is used in readiness (POLL_ADD) mode rather than for
// submitted async I/O, so it offers no throughput advantage over epoll today.
#if !defined(MG_ENABLE_KQUEUE) && (defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || defined(__NetBSD__))
#define MG_ENABLE_KQUEUE 1
#elif defined(__linux__)
#if defined(MG_ENABLE_IO_URING) && MG_ENABLE_IO_URING
#if !__has_include(<liburing.h>)
#error "MG_ENABLE_IO_URING=1 but <liburing.h> was not found (install liburing-dev)"
#endif
#elif !defined(MG_ENABLE_EPOLL)
#define MG_ENABLE_EPOLL 1
#endif
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

#if defined(MG_ENABLE_KQUEUE) && MG_ENABLE_KQUEUE
#include <sys/event.h>
#endif
#if defined(MG_ENABLE_IO_URING) && MG_ENABLE_IO_URING
#include <liburing.h>
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

// Normalize the readiness-backend selectors so every one is defined to 0 or 1.
// The per-platform logic above defines exactly one of them (or none, in which
// case select() is the fallback); make the rest explicit 0 so they can appear
// in static_assert expressions and plain arithmetic without relying on the
// "undefined identifier evaluates to 0 in #if" rule.
#ifndef MG_ENABLE_EPOLL
#define MG_ENABLE_EPOLL 0
#endif
#ifndef MG_ENABLE_KQUEUE
#define MG_ENABLE_KQUEUE 0
#endif
#ifndef MG_ENABLE_POLL
#define MG_ENABLE_POLL 0
#endif
#ifndef MG_ENABLE_IO_URING
#define MG_ENABLE_IO_URING 0
#endif

// The config matrix must be internally consistent. Catch a bad
// MG_ARCH/MG_ENABLE_* combination here, at compile time, rather than as a
// confusing failure deep in the socket layer at runtime or link time.
static_assert(MG_ARCH == MG_ARCH_UNIX || MG_ARCH == MG_ARCH_WIN32,
              "MG_ARCH must be MG_ARCH_UNIX or MG_ARCH_WIN32");
// The readiness backends are mutually exclusive; at most one may be selected.
// (When none is selected, the code falls back to select().)
static_assert(MG_ENABLE_EPOLL + MG_ENABLE_KQUEUE + MG_ENABLE_POLL
                  + MG_ENABLE_IO_URING <= 1,
              "At most one of MG_ENABLE_EPOLL / KQUEUE / POLL / IO_URING "
              "may be enabled");
// epoll, kqueue and io_uring are POSIX-only and cannot pair with the Win32 arch.
static_assert(MG_ARCH != MG_ARCH_WIN32
                  || (MG_ENABLE_EPOLL == 0 && MG_ENABLE_KQUEUE == 0
                      && MG_ENABLE_IO_URING == 0),
              "epoll / kqueue / io_uring are not available on MG_ARCH_WIN32");
// The socket backend underpins the entire networking core.
static_assert(MG_ENABLE_SOCKET != 0, "MG_ENABLE_SOCKET must be enabled");

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

// Kqueue macros
#if MG_ENABLE_KQUEUE
#define MG_KQUEUE_ADD(c)                                                      \
    do {                                                                      \
        struct kevent evs[2];                                                 \
        EV_SET(&evs[0], (int)(size_t)c->fd, EVFILT_READ, EV_ADD, 0, 0, c); \
        EV_SET(&evs[1], (int)(size_t)c->fd, EVFILT_WRITE, EV_ADD | EV_DISABLE, 0, 0, c); \
        kevent(c->mgr->epoll_fd, evs, 2, NULL, 0, NULL);                     \
    } while (0)
#define MG_KQUEUE_MOD(c, wr)                                                  \
    do {                                                                      \
        struct kevent ev;                                                     \
        if (wr)                                                               \
            EV_SET(&ev, (int)(size_t)c->fd, EVFILT_WRITE, EV_ENABLE, 0, 0, c); \
        else                                                                  \
            EV_SET(&ev, (int)(size_t)c->fd, EVFILT_WRITE, EV_DISABLE, 0, 0, c); \
        kevent(c->mgr->epoll_fd, &ev, 1, NULL, 0, NULL);                     \
    } while (0)
#else
#define MG_KQUEUE_ADD(c)
#define MG_KQUEUE_MOD(c, wr)
#endif

// Profiling stubs (disabled)
#define MG_PROF_INIT(c)
#define MG_PROF_FREE(c)
#define MG_PROF_ADD(c, name)
#define MG_PROF_DUMP(c)

// ---------------------------------------------------------------------------
// DNS protocol constants
// ---------------------------------------------------------------------------
#define MG_DNS_MAX_RECURSION_DEPTH 5
#define MG_DNS_PACKET_MAX_SIZE 512
#define MG_DNS_MAX_ANSWERS 10
#define MG_DNS_IPV4_ADDR_LEN 4
#define MG_DNS_IPV6_ADDR_LEN 16
#define MG_DNS_CLASS_IN 1
#define MG_DNS_QUERY_FLAG 0x100
#define MG_DNS_NAME_MAX 256

// DNS defaults
#define MG_DEFAULT_DNS_TIMEOUT_MS 3000
// Default bound (ms) on how long a client-initiated connection may spend
// resolving + connecting before the event loop gives up. Legitimate connects
// finish far under this; it exists to reap a hung outbound connect(). Set to 0
// via set_connect_timeout() to disable.
#define MG_DEFAULT_CONNECT_TIMEOUT_MS 30000
#define MG_DEFAULT_DNS4_URL "udp://8.8.8.8:53"
#define MG_DEFAULT_DNS6_URL "udp://[2001:4860:4860::8888]:53"

// DNS compression pointer mask
#define MG_DNS_COMPRESS_MASK 0xc0
#define MG_DNS_COMPRESS_PTR_MASK 0x3f

// ---------------------------------------------------------------------------
// HTTP protocol constants
// ---------------------------------------------------------------------------
#define MG_HTTP_VERSION_PREFIX "HTTP/"
#define MG_HTTP_VERSION_PREFIX_LEN 5
#define MG_HTTP_VERSION_FULL_LEN 8

#define MG_HTTP_STATUS_NO_CONTENT 204
#define MG_HTTP_STATUS_NOT_MODIFIED 304

// Auth header prefix lengths (strlen of "Basic " and "Bearer ")
#define MG_HTTP_AUTH_BASIC_PREFIX "Basic "
#define MG_HTTP_AUTH_BASIC_PREFIX_LEN 6
#define MG_HTTP_AUTH_BEARER_PREFIX "Bearer "
#define MG_HTTP_AUTH_BEARER_PREFIX_LEN 7
#define MG_HTTP_AUTH_HEADER "Authorization: Basic "
#define MG_HTTP_AUTH_HEADER_LEN 21

// Chunked transfer encoding
#define MG_HTTP_CHUNK_SIZE_HEX_WIDTH 8
#define MG_HTTP_CHUNK_PLACEHOLDER "        \r\n"
#define MG_HTTP_CHUNK_PLACEHOLDER_LEN 10

// Content-Length field formatting in http_reply
#define MG_HTTP_CONTENT_LEN_MIN_SIZE 16
#define MG_HTTP_CONTENT_LEN_OFFSET 15
#define MG_HTTP_CONTENT_LEN_MAX_WIDTH 11

// Multipart header field names
#define MG_HTTP_CONTENT_DISPOSITION "Content-Disposition"
#define MG_HTTP_CONTENT_DISPOSITION_LEN 19
#define MG_HTTP_FORM_NAME "name"
#define MG_HTTP_FORM_NAME_LEN 4
#define MG_HTTP_FORM_FILENAME "filename"
#define MG_HTTP_FORM_FILENAME_LEN 8

// Basic auth buffer size
#define MG_HTTP_BAUTH_OVERHEAD 36

// Access token cookie name
#define MG_HTTP_ACCESS_TOKEN_COOKIE "access_token"
#define MG_HTTP_ACCESS_TOKEN_COOKIE_LEN 12

// ---------------------------------------------------------------------------
// WebSocket constants
// ---------------------------------------------------------------------------
#define MG_WS_FIN_BIT 128
#define MG_WS_MASK_BIT 128
#define MG_WS_PAYLOAD_LEN_MASK 0x7f
#define MG_WS_MIN_HEADER_SIZE 2
#define MG_WS_EXTENDED_PAYLOAD_16 126
#define MG_WS_EXTENDED_PAYLOAD_64 127
#define MG_WS_MASK_LEN 4
#define MG_WS_MAX_HEADER_SIZE 14
#define MG_WS_MAX_DATA_LEN (1024UL * 1024UL * 1024UL)
#define MG_WS_HANDSHAKE_MIN_LEN 15
#define MG_WS_HANDSHAKE_STATUS "101"
#define MG_WS_HANDSHAKE_STATUS_LEN 3

// ---------------------------------------------------------------------------
// UTF-8 byte masks
// ---------------------------------------------------------------------------
#define MG_UTF8_2BYTE_MASK 0xe0
#define MG_UTF8_2BYTE_VALUE 0xc0
#define MG_UTF8_3BYTE_MASK 0xf0
#define MG_UTF8_3BYTE_VALUE 0xe0
#define MG_UTF8_4BYTE_MASK 0xf8
#define MG_UTF8_4BYTE_VALUE 0xf0

// ---------------------------------------------------------------------------
// IPv4 / IPv6 address parsing
// ---------------------------------------------------------------------------
#define MG_IPV4_NUM_DOTS 3
#define MG_IPV4_OCTET_MAX 255
#define MG_IPV4_LOCALHOST 0x7f000001U

// IPv4-mapped IPv6 address offsets
#define MG_IPV6_V4MAPPED_MIN_LEN 14
#define MG_IPV6_V4MAPPED_PAD_OFFSET 10
#define MG_IPV6_V4MAPPED_DATA_OFFSET 12
#define MG_IPV6_ADDR_BYTES 14
#define MG_IPV6_NO_DOUBLE_COLON 42

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------
#define MG_URL_PROTO_PREFIX_LEN 4
#define MG_URL_ENCODED_CHAR_WIDTH 3
#define MG_URL_ENCODE_MIN_BUF 4

// ---------------------------------------------------------------------------
// TLS
// ---------------------------------------------------------------------------
// Maximum TLS record size: 16 KB payload + header/MAC/padding overhead
#define MG_TLS_RECORD_MAX_SIZE (16 * 1024 + 40)

// ---------------------------------------------------------------------------
// Logging / hexdump
// ---------------------------------------------------------------------------
#define MG_LOG_PREFIX_BUF_SIZE 41
#define MG_HEXDUMP_BYTES_PER_LINE 16

// ---------------------------------------------------------------------------
// Float/double formatting
// ---------------------------------------------------------------------------
#define MG_FLOAT_MAX_EXPONENT 400
#define MG_FLOAT_DEFAULT_PRECISION 6
#define MG_DOUBLE_MAX_EXPONENT 308

// ---------------------------------------------------------------------------
// SHA-1 round constants
// ---------------------------------------------------------------------------
#define MG_SHA1_K0 0x5A827999
#define MG_SHA1_K1 0x6ED9EBA1
#define MG_SHA1_K2 0x8F1BBCDC
#define MG_SHA1_K3 0xCA62C1D6

// ---------------------------------------------------------------------------
// Printf formatting
// ---------------------------------------------------------------------------
#define MG_PRINTF_TMP_BUF_SIZE 40

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

namespace nanosrv {

uint16_t ntohs_(uint16_t net);
uint32_t ntohl_(uint32_t net);
uint64_t ntohll_(uint64_t net);

} // namespace nanosrv

#define mg_htons(x) nanosrv::ntohs_(x)
#define mg_htonl(x) nanosrv::ntohl_(x)
#define mg_htonll(x) nanosrv::ntohll_(x)

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

// Type-safe intrusive linked list operations (C++ alternative to macros above)
namespace nanosrv::detail {
template <typename T>
inline void list_push_front(T** head, T* elem) {
    elem->next = *head;
    *head = elem;
}
template <typename T>
inline void list_remove(T** head, T* elem) {
    T** h = head;
    while (*h != elem)
        h = &(*h)->next;
    *h = elem->next;
}
} // namespace nanosrv::detail

// ---------------------------------------------------------------------------
// Printf output function types (needed by logging macros below)
// ---------------------------------------------------------------------------
typedef void (*PrintFn)(char, void*);

#define MG_ESC(str) print_esc, 0, (str)

// ---------------------------------------------------------------------------
// Logging
// ---------------------------------------------------------------------------
enum { MG_LL_NONE, MG_LL_ERROR, MG_LL_INFO, MG_LL_DEBUG, MG_LL_VERBOSE };

namespace nanosrv {

extern int log_level;

void log(const char* fmt, ...);
void log_prefix(int ll, const char* file, int line, const char* fname);
void hexdump(const void* buf, size_t len);
void log_set_fn(PrintFn fn, void* param);

} // namespace nanosrv

#define mg_log_set(level_) nanosrv::log_level = (level_)

// Compile-time log floor. Sites whose level exceeds MG_LOG_LEVEL_MAX are dead
// code (the `(level) <= MG_LOG_LEVEL_MAX` term is a compile-time constant, so
// the compiler drops the branch, its argument evaluation, and its string
// literals). This is stronger than the runtime `log_level` gate: a compiled-out
// site cannot be re-enabled by mg_log_set(). Default: keep Debug/Verbose in
// debug builds, compile them out in release (NDEBUG). Override on the command
// line, e.g. -DMG_LOG_LEVEL_MAX=MG_LL_VERBOSE to keep everything in a release
// build, or =MG_LL_ERROR to strip all but errors.
#ifndef MG_LOG_LEVEL_MAX
#ifdef NDEBUG
#define MG_LOG_LEVEL_MAX MG_LL_INFO
#else
#define MG_LOG_LEVEL_MAX MG_LL_VERBOSE
#endif
#endif

#if MG_ENABLE_LOG
#define MG___FUNC__ __func__
#define MG_LOG(level, args)                                                   \
    do {                                                                      \
        if ((level) <= MG_LOG_LEVEL_MAX && (level) <= nanosrv::log_level) {   \
            nanosrv::log_prefix((level), __FILE__, __LINE__, MG___FUNC__);   \
            nanosrv::log args;                                               \
        }                                                                     \
    } while (0)
#else
#define MG_LOG(level, args)                                                   \
    do {                                                                      \
        if (0)                                                                \
            nanosrv::log args;                                               \
    } while (0)
#endif

#define MG_ERROR(args) MG_LOG(MG_LL_ERROR, args)
#define MG_INFO(args) MG_LOG(MG_LL_INFO, args)
#define MG_DEBUG(args) MG_LOG(MG_LL_DEBUG, args)
#define MG_VERBOSE(args) MG_LOG(MG_LL_VERBOSE, args)
