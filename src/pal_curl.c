/*
 * GNU GPLv3 License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/* ═══════════════════════════════════════════════════════════════════════════
 * pal_curl.c — Complete libcurl shim for PS4/PS5
 *
 * ── ARCHITECTURE ────────────────────────────────────────────────────────────
 *
 * curl_easy_perform() orchestrates the following pipeline:
 *
 *   curl_easy_perform()
 *     └─ perform_one()          ← one HTTP transaction (no redirect logic)
 *           ├─ url_parse()
 *           ├─ net_connect()    ← getaddrinfo + non-blocking connect/select
 *           ├─ build_request()  ← assemble HTTP/1.1 request headers
 *           ├─ send_all()       ← write-loop with EINTR retry
 *           ├─ http_recv_headers()  ← accumulate until \r\n\r\n
 *           ├─ parse_status_line()
 *           ├─ parse_header_fields() ← Content-Length / Transfer-Encoding / Location
 *           └─ stream_body_*()  ← one of three body readers:
 *                 stream_body_known_length()   content-length based
 *                 stream_body_chunked()        chunked/TE decoder
 *                 stream_body_until_close()    read-until-EOF
 *
 * ── KEY DESIGN DECISIONS ────────────────────────────────────────────────────
 *
 * getaddrinfo instead of gethostbyname:
 *   gethostbyname() is deprecated, non-reentrant (global h_errno), and
 *   does not support IPv6.  getaddrinfo() is the POSIX.1-2008 replacement
 *   and is safe to call concurrently from different threads.
 *
 * Non-blocking connect with select() timeout:
 *   The original code stored CURLOPT_CONNECTTIMEOUT but never used it.
 *   We set O_NONBLOCK before connect(), then wait in select() for the
 *   writability event.  getsockopt(SO_ERROR) confirms actual success.
 *   The socket is restored to blocking mode before HTTP I/O.
 *
 * Separate header and body buffers:
 *   The original code used a single 16 KB buffer for both headers and body,
 *   with an ad-hoc "header_len" counter reset to 0 after headers were found.
 *   This led to incorrect body offset calculations when the server sent
 *   headers and body in the same TCP segment.  We now use:
 *     hdr_buf[HDR_BUF_SIZE]  — stack-allocated, holds only headers +
 *                               the first partial body chunk.
 *     body_buf (heap)        — used exclusively for body streaming.
 *
 * HTTP/1.1 with chunked Transfer-Encoding:
 *   HTTP/1.0 was used to avoid chunked decoding.  Modern servers (even on
 *   local networks) frequently respond with HTTP/1.1 chunked even to
 *   HTTP/1.0 requests.  We now send HTTP/1.1 + Connection: close and
 *   implement a proper state-machine chunked decoder (chunked_process()).
 *   Connection: close ensures no keep-alive complexity.
 *
 * Overflow-safe arithmetic:
 *   content_length was previously read with strtod(), which loses precision
 *   for files > 2^53 bytes (~8 PB).  We use strtoll() instead.
 *   All comparisons between uint32_t offsets and uint64_t file sizes widen
 *   operands explicitly before arithmetic.
 *
 * EINTR handling:
 *   All send()/recv() calls are retried on EINTR.  A signal (e.g. from
 *   the debugger or a real-time timer) no longer produces spurious I/O errors.
 *
 * CURL_WRITEFUNC_PAUSE:
 *   The original code looped forever on PAUSE with only usleep().
 *   We retry up to PAL_PAUSE_MAX_RETRIES times then abort with
 *   CURLE_ABORTED_BY_CALLBACK.  This prevents a stuck download from
 *   blocking a thread forever.
 *
 * ── MISRA C:2012 DEVIATIONS ─────────────────────────────────────────────────
 *   Rule 15.5 — single exit point: violated in short validation sequences
 *               to avoid deeply-nested if-else ladders.
 *   Rule 21.3 — dynamic memory: malloc/free used only for body_buf
 *               and curl_slist nodes.  The ctx itself uses calloc/free.
 *   Rule 13.1 — side effects in initializers: none present.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "pal_curl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <strings.h>   /* strncasecmp */
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

int gettimeofday(struct timeval *tv, void *tz);

/* ── Compile-time tuning constants ─────────────────────────────────────── */

#define DEFAULT_UA              "zftpd-ps/2.0"
#define DEFAULT_CONN_MS         30000L    /* 30 s default connect timeout */
#define DEFAULT_TIMEOUT_MS      0L        /* 0 = no transfer timeout */
#define DEFAULT_MAX_REDIRS      10L

/** Maximum total bytes for HTTP response headers (stack). */
#define HDR_BUF_SIZE            8192U
/** HTTP request assembly buffer (stack). */
#define REQ_BUF_SIZE            8192U
/** Body streaming chunk (heap-allocated once per perform). */
#define BODY_BUF_SIZE           32768U

/** Max retries when write callback returns CURL_WRITEFUNC_PAUSE. */
#define PAL_PAUSE_MAX_RETRIES   20
/** Sleep between PAUSE retries (100 ms). */
#define PAL_PAUSE_DELAY_US      100000U

/* ── Internal context ──────────────────────────────────────────────────── */

typedef struct {
    /* ── Request configuration ──────────────────────────────────── */
    char    url[2048U];
    char    useragent[256U];
    char    range[64U];         /* e.g. "0-4095" */
    char   *postfields;         /* caller-owned — not copied */
    long    postfieldsize;      /* -1 or 0 → use strlen(postfields) */
    curl_slist *httpheader;     /* caller-owned — not copied */
    char   *errbuf;             /* caller-supplied CURL_ERROR_SIZE buffer */

    /* ── Callbacks ──────────────────────────────────────────────── */
    curl_write_callback     write_cb;
    void                   *write_data;
    curl_xferinfo_callback  xferinfo_cb;
    void                   *xferinfo_data;

    /* ── Behavior ───────────────────────────────────────────────── */
    long    follow_location;
    long    max_redirs;
    long    connect_timeout_ms;
    long    timeout_ms;
    long    low_speed_limit;    /* bytes/s threshold */
    long    low_speed_time;     /* seconds below threshold before abort */
    int     nobody;             /* 1 = HEAD */
    int     post;               /* 1 = POST */
    int     verbose;            /* 1 = print debug to stderr */
    int     noprogress;         /* 1 = suppress xferinfo callback */

    /* ── Response info (populated by perform) ───────────────────── */
    long    response_code;
    double  content_length_download;
    double  speed_download;
    double  size_download;
    double  total_time;
} pal_curl_ctx_t;

/* ── HTTP response metadata ─────────────────────────────────────────────── */

typedef struct {
    long    status_code;
    int64_t content_length;     /* -1 if absent */
    int     chunked;            /* Transfer-Encoding: chunked */
    char    location[2048U];    /* Redirect target, if any */
} http_resp_t;

/* ── Chunked transfer decoder state ─────────────────────────────────────── */

typedef enum {
    CS_SIZE    = 0,   /* Reading hex chunk-size line */
    CS_DATA    = 1,   /* Reading chunk payload */
    CS_CRLF    = 2,   /* Consuming \r\n after payload */
    CS_TRAILER = 3,   /* Consuming optional trailer headers */
    CS_DONE    = 4    /* Terminal zero-chunk processed */
} chunked_state_e;

typedef struct {
    chunked_state_e state;
    char            size_buf[24U];  /* Hex digits + optional extensions */
    size_t          size_pos;
    uint64_t        remaining;      /* Bytes left in current chunk */
    int             crlf_pos;       /* 0 or 1 while consuming post-data \r\n */
} chunked_ctx_t;

/* ── Speed guard (CURLOPT_LOW_SPEED_*) ─────────────────────────────────── */

typedef struct {
    time_t   last_sec;      /* wall-clock second of last measurement reset */
    uint64_t sec_bytes;     /* bytes accumulated since last_sec */
    int      slow_count;    /* consecutive seconds below threshold */
} speed_guard_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * §1 — String and error-buffer helpers
 * ═════════════════════════════════════════════════════════════════════════*/

/** Skip ASCII horizontal whitespace. */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') { s++; }
    return s;
}

/**
 * @brief Case-insensitive test whether a header line starts with prefix.
 * @return Pointer to the value (after prefix) or NULL.
 */
static const char *header_field(const char *line, const char *prefix,
                                 size_t prefix_len)
{
    if (strncasecmp(line, prefix, prefix_len) != 0) { return NULL; }
    return line + prefix_len;
}

/**
 * @brief Append a printf-style string into buf at *pos, tracking remaining
 *        space.  Returns 0 on success, -1 if the result would be truncated.
 *
 * DESIGN RATIONALE:
 *   The original code used bare snprintf() and checked the return value
 *   inline each time, mixing logic with error handling.  This helper
 *   centralises the truncation check and keeps build_request() readable.
 */
__attribute__((format(printf, 4, 5)))
static int append_str(char *buf, size_t buf_size, size_t *pos,
                       const char *fmt, ...)
{
    if (*pos >= buf_size) { return -1; }

    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf + *pos, buf_size - *pos, fmt, ap);
    va_end(ap);

    if (n < 0 || (size_t)n >= (buf_size - *pos)) { return -1; }
    *pos += (size_t)n;
    return 0;
}

/**
 * @brief Write an error message into ctx->errbuf if it was set.
 * @note  Never calls fprintf — all diagnostic output is opt-in via
 *        CURLOPT_VERBOSE to stderr, not via errbuf writes.
 */
static void set_errbuf(pal_curl_ctx_t *ctx, const char *msg)
{
    if ((ctx == NULL) || (ctx->errbuf == NULL) || (msg == NULL)) { return; }
    strncpy(ctx->errbuf, msg, (size_t)(CURL_ERROR_SIZE - 1));
    ctx->errbuf[CURL_ERROR_SIZE - 1] = '\0';
}

#define PAL_LOG(ctx, ...) \
    do { if ((ctx) != NULL && (ctx)->verbose) { fprintf(stderr, "[pal_curl] " __VA_ARGS__); } } while (0)

/* ═══════════════════════════════════════════════════════════════════════════
 * §2 — URL parser
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Decompose an http:// URL into host, port, and path.
 *
 * Handles:
 *   http://host/path
 *   http://host:port/path?query
 *   http://host/path#fragment      (fragment stripped)
 *
 * https:// → CURLE_UNSUPPORTED_PROTOCOL.
 * Missing scheme or empty host → CURLE_URL_MALFORMAT.
 * Port outside [1, 65535] → CURLE_URL_MALFORMAT.
 *
 * @param[in]  url        NUL-terminated input URL.
 * @param[out] host       Destination for hostname (must be ≥ host_size bytes).
 * @param[in]  host_size  Size of host buffer.
 * @param[out] port       Decoded TCP port.
 * @param[out] path       Destination for request path (must be ≥ path_size bytes).
 * @param[in]  path_size  Size of path buffer.
 *
 * @return CURLE_OK on success.
 *
 * @note Thread-safety: pure function (no shared state).
 * @note WCET: O(len(url)).
 */
static CURLcode url_parse(const char *url,
                           char *host, size_t host_size,
                           int *port,
                           char *path, size_t path_size)
{
    if ((url == NULL) || (host == NULL) || (port == NULL) ||
        (path == NULL) || (host_size < 2U) || (path_size < 2U)) {
        return CURLE_URL_MALFORMAT;
    }

    const char *p = url;
    if (strncmp(p, "http://", 7U) == 0) {
        p += 7U;
    } else if (strncmp(p, "https://", 8U) == 0) {
        return CURLE_UNSUPPORTED_PROTOCOL;
    } else {
        return CURLE_URL_MALFORMAT;
    }

    /* Locate the path separator. */
    const char *slash = strchr(p, '/');

    /* Find port separator, only if it precedes slash (or there is no slash). */
    const char *colon = strchr(p, ':');
    if ((colon != NULL) && (slash != NULL) && (colon > slash)) {
        colon = NULL; /* colon belongs to the path, not the authority */
    }

    /* Decode host. */
    const char *host_end = (colon != NULL) ? colon
                         : (slash != NULL) ? slash
                         : (p + strlen(p));
    size_t host_len = (size_t)(host_end - p);
    if ((host_len == 0U) || (host_len >= host_size)) {
        return CURLE_URL_MALFORMAT;
    }
    (void)memcpy(host, p, host_len);
    host[host_len] = '\0';

    /* Decode port. */
    if (colon != NULL) {
        const char *port_end = (slash != NULL) ? slash : (colon + 1U + strlen(colon + 1U));
        size_t port_len = (size_t)(port_end - (colon + 1U));
        if (port_len == 0U || port_len >= 6U) {
            return CURLE_URL_MALFORMAT;
        }
        char port_buf[6U];
        (void)memcpy(port_buf, colon + 1U, port_len);
        port_buf[port_len] = '\0';
        char *endp;
        long lport = strtol(port_buf, &endp, 10);
        if ((*endp != '\0') || (lport <= 0L) || (lport > 65535L)) {
            return CURLE_URL_MALFORMAT;
        }
        *port = (int)lport;
    } else {
        *port = 80;
    }

    /* Decode path, stripping fragment. */
    const char *path_start = (slash != NULL) ? slash : "/";
    const char *fragment   = strchr(path_start, '#');
    size_t path_len = (fragment != NULL) ? (size_t)(fragment - path_start)
                                         : strlen(path_start);
    if (path_len == 0U) {
        path_start = "/";
        path_len   = 1U;
    }
    if (path_len >= path_size) { return CURLE_URL_MALFORMAT; }
    (void)memcpy(path, path_start, path_len);
    path[path_len] = '\0';

    return CURLE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §3 — Network utilities
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Wait for a socket to become readable or writable, with timeout.
 *
 * @param sock        File descriptor.
 * @param for_write   Non-zero to wait for write-readiness, zero for read.
 * @param timeout_ms  Milliseconds to wait.  0 or negative → poll (no wait).
 *
 * @return >0 descriptor ready, 0 timeout, <0 error (check errno).
 *
 * @note Thread-safety: pure, no shared state.
 * @note WCET: up to timeout_ms milliseconds.
 */
static int socket_wait(int sock, int for_write, long timeout_ms)
{
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);

    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000L;
    tv.tv_usec = (timeout_ms % 1000L) * 1000L;

    fd_set *rfds = for_write ? NULL : &fds;
    fd_set *wfds = for_write ? &fds  : NULL;

    return select(sock + 1, rfds, wfds, NULL,
                  (timeout_ms > 0L) ? &tv : NULL);
}

/**
 * @brief Resolve host and connect with an optional timeout.
 *
 * Uses getaddrinfo() for IPv4/IPv6 support and tries each returned address
 * in order until one connects.  If timeout_ms > 0, the socket is set to
 * non-blocking for the connect() call and restored to blocking on success.
 *
 * @param host        NUL-terminated hostname or IP address.
 * @param port        TCP port [1, 65535].
 * @param timeout_ms  Connect timeout in ms (0 = blocking with no timeout).
 * @param err_out     Receives CURLE_COULDNT_RESOLVE_HOST or
 *                    CURLE_COULDNT_CONNECT on failure.
 *
 * @return Non-negative socket fd on success, -1 on failure.
 *
 * @note Thread-safety: getaddrinfo() is thread-safe on POSIX.
 * @note WCET: up to timeout_ms ms (DNS resolution not bounded).
 */
static int net_connect(const char *host, int port, long timeout_ms,
                        CURLcode *err_out)
{
    *err_out = CURLE_COULDNT_CONNECT;

    char port_str[12U];
    (void)snprintf(port_str, sizeof(port_str), "%d", port);

    struct addrinfo hints;
    (void)memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    struct addrinfo *result = NULL;
    if (getaddrinfo(host, port_str, &hints, &result) != 0) {
        *err_out = CURLE_COULDNT_RESOLVE_HOST;
        return -1;
    }

    int sock = -1;
    for (struct addrinfo *rp = result; rp != NULL; rp = rp->ai_next) {
        sock = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (sock < 0) { continue; }

        if (timeout_ms > 0L) {
            int flags = fcntl(sock, F_GETFL, 0);
            if (fcntl(sock, F_SETFL, flags | O_NONBLOCK) != 0) {
                close(sock); sock = -1; continue;
            }
        }

        int r = connect(sock, rp->ai_addr, rp->ai_addrlen);

        if (r == 0) { break; } /* Immediate success */

        if ((r < 0) && (errno == EINPROGRESS) && (timeout_ms > 0L)) {
            int sel = socket_wait(sock, 1, timeout_ms);
            if (sel > 0) {
                int   so_err = 0;
                socklen_t sl = (socklen_t)sizeof(so_err);
                (void)getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &sl);
                if (so_err == 0) { break; } /* Connected */
            }
        }

        close(sock);
        sock = -1;
    }

    freeaddrinfo(result);

    if (sock < 0) { return -1; }

    /* Restore blocking mode. */
    if (timeout_ms > 0L) {
        int flags = fcntl(sock, F_GETFL, 0);
        (void)fcntl(sock, F_SETFL, flags & ~O_NONBLOCK);
    }

    /* Disable Nagle algorithm: reduces latency for small HTTP requests. */
    int one = 1;
    (void)setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, (socklen_t)sizeof(one));

    return sock;
}

/**
 * @brief Reliably write all len bytes to sock, retrying on EINTR and
 *        partial writes.  Respects timeout_ms between attempts.
 *
 * @return 0 on success, -1 on timeout or send error.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: up to timeout_ms per send attempt.
 */
static int send_all(int sock, const void *data, size_t len, long timeout_ms)
{
    const char *p = (const char *)data;

    while (len > 0U) {
        if (timeout_ms > 0L) {
            if (socket_wait(sock, 1, timeout_ms) <= 0) { return -1; }
        }

        ssize_t n;
        do { n = send(sock, p, len, 0); } while ((n < 0) && (errno == EINTR));

        if (n <= 0) { return -1; }
        p   += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/**
 * @brief Read up to len bytes from sock, with optional timeout.
 *        Retries on EINTR.
 *
 * @return Bytes read (≥ 0), or -1 on error/timeout.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: up to timeout_ms ms, plus kernel recv latency.
 */
static ssize_t recv_timed(int sock, void *buf, size_t len, long timeout_ms)
{
    if (timeout_ms > 0L) {
        int sel = socket_wait(sock, 0, timeout_ms);
        if (sel < 0) { return -1; }
        if (sel == 0) { errno = ETIMEDOUT; return -1; }
    }

    ssize_t n;
    do { n = recv(sock, buf, len, 0); } while ((n < 0) && (errno == EINTR));
    return n;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §4 — HTTP request builder
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Assemble an HTTP/1.1 request into buf.
 *
 * Includes Host, User-Agent, Accept, Connection: close, and optionally:
 * Range, Content-Type/Content-Length (for POST), and caller-supplied
 * headers (curl_slist).
 *
 * @param ctx       Handle context.
 * @param method    "GET", "POST", or "HEAD".
 * @param host      Hostname (for Host: header).
 * @param port      TCP port.
 * @param path      Request path + query string.
 * @param buf       Destination buffer.
 * @param buf_size  Size of buf.
 *
 * @return Length of assembled request in bytes, or -1 on truncation.
 *
 * @note Thread-safety: pure (no shared state).
 * @note WCET: O(n) where n is total header string length.
 */
static int build_request(const pal_curl_ctx_t *ctx,
                          const char *method, const char *host, int port,
                          const char *path, char *buf, size_t buf_size)
{
    size_t pos = 0U;
    const char *ua = (ctx->useragent[0] != '\0') ? ctx->useragent : DEFAULT_UA;

    if (append_str(buf, buf_size, &pos,
                   "%s %s HTTP/1.1\r\n"
                   "Host: %s:%d\r\n"
                   "User-Agent: %s\r\n"
                   "Accept: */*\r\n"
                   "Connection: close\r\n",
                   method, path, host, port, ua) != 0) { return -1; }

    if (ctx->range[0] != '\0') {
        if (append_str(buf, buf_size, &pos, "Range: bytes=%s\r\n",
                       ctx->range) != 0) { return -1; }
    }

    if ((ctx->post != 0) && (ctx->postfields != NULL)) {
        long plen = (ctx->postfieldsize > 0L) ? ctx->postfieldsize
                                               : (long)strlen(ctx->postfields);
        if (append_str(buf, buf_size, &pos,
                       "Content-Type: application/x-www-form-urlencoded\r\n"
                       "Content-Length: %ld\r\n", plen) != 0) { return -1; }
    }

    for (const curl_slist *h = ctx->httpheader; h != NULL; h = h->next) {
        if (h->data == NULL) { continue; }
        if (append_str(buf, buf_size, &pos, "%s\r\n", h->data) != 0) {
            return -1;
        }
    }

    if (append_str(buf, buf_size, &pos, "\r\n") != 0) { return -1; }

    return (int)pos;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §5 — HTTP response header reader and parser
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Read from sock until the end-of-headers sentinel "\r\n\r\n" is
 *        found or hdr_buf is full.
 *
 * On success, hdr_buf[0 .. hdr_len) contains the complete header block
 * (including the terminal \r\n\r\n), and hdr_buf[hdr_len .. body_prefix_len)
 * contains any body bytes that arrived with the same TCP segments.
 *
 * hdr_buf is NUL-terminated after the last received byte.
 *
 * @param sock              Connected socket.
 * @param timeout_ms        Per-recv timeout.
 * @param hdr_buf           Caller-supplied buffer (HDR_BUF_SIZE bytes).
 * @param hdr_buf_size      sizeof(hdr_buf).
 * @param hdr_len_out       Receives length of headers incl. \r\n\r\n.
 * @param body_prefix_out   Receives byte count of body data in hdr_buf
 *                           after the header end.
 *
 * @return CURLE_OK, CURLE_GOT_NOTHING, or CURLE_RECV_ERROR.
 *
 * @note Thread-safety: NOT thread-safe.
 */
static CURLcode http_recv_headers(int sock, long timeout_ms,
                                   char *hdr_buf, size_t hdr_buf_size,
                                   size_t *hdr_len_out,
                                   size_t *body_prefix_out)
{
    size_t pos = 0U;

    while (pos < hdr_buf_size - 1U) {
        ssize_t n = recv_timed(sock, hdr_buf + pos,
                               hdr_buf_size - 1U - pos, timeout_ms);
        if (n < 0) { return CURLE_RECV_ERROR; }
        if (n == 0) {
            return (pos == 0U) ? CURLE_GOT_NOTHING : CURLE_RECV_ERROR;
        }

        pos += (size_t)n;
        hdr_buf[pos] = '\0';

        const char *end = strstr(hdr_buf, "\r\n\r\n");
        if (end != NULL) {
            *hdr_len_out     = (size_t)(end - hdr_buf) + 4U;
            *body_prefix_out = pos - *hdr_len_out;
            return CURLE_OK;
        }
    }

    return CURLE_RECV_ERROR; /* Headers exceeded HDR_BUF_SIZE */
}

/**
 * @brief Parse the HTTP status code from the first line of hdr_buf.
 *
 * @param hdr_buf     NUL-terminated header buffer.
 * @param status_out  Receives the numeric status code.
 *
 * @return 0 on success, -1 if the line is malformed.
 *
 * @note Thread-safety: pure.
 */
static int parse_status_line(const char *hdr_buf, long *status_out)
{
    /* Expect "HTTP/1.x NNN ..." */
    if (strncmp(hdr_buf, "HTTP/1.", 7U) != 0) { return -1; }

    const char *sp = strchr(hdr_buf + 7U, ' ');
    if (sp == NULL) { return -1; }

    char *endp;
    long code = strtol(sp + 1, &endp, 10);
    if ((endp == sp + 1) || (code < 100L) || (code > 999L)) { return -1; }

    *status_out = code;
    return 0;
}

/**
 * @brief Extract well-known fields from response headers.
 *
 * Parses headers line by line.  Each line is temporarily NUL-terminated
 * then restored, so hdr_buf must be writable.
 *
 * Extracted:
 *   Content-Length     → info->content_length  (int64_t; -1 if absent)
 *   Transfer-Encoding  → info->chunked          (1 if "chunked")
 *   Location           → info->location[]
 *
 * @param hdr_buf   Writable header buffer (NUL-terminated).
 * @param hdr_len   Byte count of header block, excluding terminal \r\n\r\n.
 * @param info      Output struct (caller must zero-initialise).
 *
 * @note Thread-safety: NOT thread-safe (temporarily modifies hdr_buf).
 * @note WCET: O(hdr_len).
 */
static void parse_header_fields(char *hdr_buf, size_t hdr_len,
                                 http_resp_t *info)
{
    info->content_length = -1;
    info->chunked        = 0;
    info->location[0]    = '\0';

    /* Skip the status line. */
    char *p = strstr(hdr_buf, "\r\n");
    if (p == NULL) { return; }
    p += 2;

    char *hdr_end = hdr_buf + hdr_len;

    while (p < hdr_end) {
        char *next     = strstr(p, "\r\n");
        size_t line_len = (next != NULL) ? (size_t)(next - p)
                                         : (size_t)(hdr_end - p);
        if (line_len == 0U) { break; }

        /* Temporarily NUL-terminate this header line. */
        char saved        = p[line_len];
        p[line_len]       = '\0';

        const char *val;
        if ((val = header_field(p, "Content-Length:", 15U)) != NULL) {
            info->content_length = strtoll(skip_ws(val), NULL, 10);
        } else if ((val = header_field(p, "Transfer-Encoding:", 18U)) != NULL) {
            if (strncasecmp(skip_ws(val), "chunked", 7U) == 0) {
                info->chunked = 1;
            }
        } else if ((val = header_field(p, "Location:", 9U)) != NULL) {
            const char *loc = skip_ws(val);
            strncpy(info->location, loc, sizeof(info->location) - 1U);
            info->location[sizeof(info->location) - 1U] = '\0';
        }

        p[line_len] = saved;
        if (next == NULL) { break; }
        p = next + 2;
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §6 — Chunked Transfer-Encoding decoder
 * ═════════════════════════════════════════════════════════════════════════*/

/** Sentinel: chunked_process() sets this to signal the final zero chunk. */
#define PAL_CHUNKED_DONE  1000

/**
 * @brief Streaming state-machine chunked decoder.
 *
 * Processes in_len bytes of raw (possibly partial) chunked stream data,
 * writing decoded payload via write_cb.  May be called incrementally:
 * state persists in cc across calls.
 *
 * States:
 *   CS_SIZE    Accumulating the hex chunk-size line (+ optional extensions).
 *   CS_DATA    Forwarding chunk payload bytes to the write callback.
 *   CS_CRLF    Consuming the mandatory \r\n after payload.
 *   CS_TRAILER Consuming optional trailers after the zero-length chunk.
 *   CS_DONE    Terminal state; further calls return PAL_CHUNKED_DONE.
 *
 * @param cc          Persistent decoder state (caller-allocated).
 * @param in          Input buffer of raw chunked data.
 * @param in_len      Byte count in in[].
 * @param write_cb    Write callback (may be NULL).
 * @param write_data  Userdata for write_cb.
 * @param written_out Incremented with decoded bytes written to callback.
 *
 * @return CURLE_OK to continue, PAL_CHUNKED_DONE when complete,
 *         CURLE_WRITE_ERROR or CURLE_RECV_ERROR on errors.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(in_len).
 */
static int chunked_process(chunked_ctx_t *cc,
                            const uint8_t *in, size_t in_len,
                            curl_write_callback write_cb, void *write_data,
                            uint64_t *written_out)
{
    size_t i = 0U;

    while (i < in_len) {
        switch (cc->state) {

        case CS_SIZE: {
            char c = (char)in[i++];
            if (c == '\r') {
                /* skip: we parse on the \n */
            } else if (c == '\n') {
                cc->size_buf[cc->size_pos] = '\0';
                /* Strip chunk extensions (;params) before the hex number. */
                char *semi = strchr(cc->size_buf, ';');
                if (semi != NULL) { *semi = '\0'; }
                char *endp;
                cc->remaining = (uint64_t)strtoull(cc->size_buf, &endp, 16);
                cc->size_pos  = 0U;
                if (endp == cc->size_buf) { return CURLE_RECV_ERROR; }
                cc->state = (cc->remaining == 0U) ? CS_TRAILER : CS_DATA;
            } else {
                if (cc->size_pos >= sizeof(cc->size_buf) - 1U) {
                    return CURLE_RECV_ERROR;
                }
                cc->size_buf[cc->size_pos++] = c;
            }
            break;
        }

        case CS_DATA: {
            size_t avail    = in_len - i;
            size_t to_write = (avail < (size_t)cc->remaining)
                              ? avail : (size_t)cc->remaining;

            if ((to_write > 0U) && (write_cb != NULL)) {
                size_t wrote = write_cb((void *)(uintptr_t)(in + i), 1U, to_write,
                                        write_data);
                if (wrote == (size_t)CURL_WRITEFUNC_PAUSE) {
                    return CURLE_ABORTED_BY_CALLBACK;
                }
                if (wrote != to_write) { return CURLE_WRITE_ERROR; }
                *written_out += to_write;
            }
            i += to_write;
            cc->remaining -= (uint64_t)to_write;

            if (cc->remaining == 0U) {
                cc->state    = CS_CRLF;
                cc->crlf_pos = 0;
            }
            break;
        }

        case CS_CRLF:
            /*
             * Consume exactly \r\n following the chunk data.
             * RFC 7230 §4.1 requires the CRLF to be present.
             */
            if ((in[i] == (uint8_t)'\r') && (cc->crlf_pos == 0)) {
                cc->crlf_pos = 1;
            } else if ((in[i] == (uint8_t)'\n') && (cc->crlf_pos == 1)) {
                cc->crlf_pos = 0;
                cc->state    = CS_SIZE;
            } else {
                return CURLE_RECV_ERROR;
            }
            i++;
            break;

        case CS_TRAILER:
            /*
             * Trailer headers after "0\r\n".  With Connection: close we
             * will not reuse the connection, so we declare done immediately
             * rather than parsing trailer key-value pairs.
             */
            cc->state = CS_DONE;
            return PAL_CHUNKED_DONE;

        case CS_DONE:
            return PAL_CHUNKED_DONE;

        default:
            return CURLE_RECV_ERROR;
        }
    }

    return (cc->state == CS_DONE) ? PAL_CHUNKED_DONE : CURLE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §7 — Speed guard
 * ═════════════════════════════════════════════════════════════════════════*/

static void speed_guard_init(speed_guard_t *sg, const pal_curl_ctx_t *ctx)
{
    sg->last_sec   = time(NULL);
    sg->sec_bytes  = 0U;
    sg->slow_count = 0;
    (void)ctx;
}

/**
 * @brief Update speed measurement; return 0 if low-speed threshold exceeded.
 *
 * @param sg         Speed guard state.
 * @param new_bytes  Bytes received in this increment.
 * @param ctx        Handle context (for limit/time thresholds).
 *
 * @return 1 if speed is OK or no limit is set, 0 if should abort.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1).
 */
static int speed_guard_update(speed_guard_t *sg, size_t new_bytes,
                               const pal_curl_ctx_t *ctx)
{
    sg->sec_bytes += (uint64_t)new_bytes;

    if (ctx->low_speed_limit <= 0L) { return 1; } /* no limit configured */

    time_t  now     = time(NULL);
    time_t  elapsed = now - sg->last_sec;

    if (elapsed < 1) { return 1; } /* not a full second yet */

    long speed = (elapsed > 0) ? (long)(sg->sec_bytes / (uint64_t)elapsed) : 0L;

    if (speed < ctx->low_speed_limit) {
        sg->slow_count += (int)elapsed;
        if ((ctx->low_speed_time > 0L) &&
            (sg->slow_count >= (int)ctx->low_speed_time)) {
            return 0; /* too slow for too long */
        }
    } else {
        sg->slow_count = 0;
    }

    sg->sec_bytes = 0U;
    sg->last_sec  = now;
    return 1;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §8 — Write-callback wrapper (PAUSE handling)
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Call write_cb with PAUSE retry.
 *
 * If the callback returns CURL_WRITEFUNC_PAUSE, sleep PAL_PAUSE_DELAY_US
 * and retry, up to PAL_PAUSE_MAX_RETRIES times, before aborting.
 *
 * DESIGN RATIONALE:
 *   The original code looped forever (while written == 0 || written == PAUSE)
 *   with no exit condition other than success.  A misbehaving callback could
 *   block the calling thread indefinitely.  The retry limit caps exposure.
 *
 * @param cb       Write callback (may be NULL — returns CURLE_OK).
 * @param ptr      Data pointer (cast to non-const for callback ABI compat).
 * @param nmemb    Byte count.
 * @param userdata User data pointer.
 *
 * @return CURLE_OK, CURLE_WRITE_ERROR, or CURLE_ABORTED_BY_CALLBACK.
 *
 * @note Thread-safety: NOT thread-safe.
 */
static CURLcode invoke_write_cb(curl_write_callback cb, const void *ptr,
                                 size_t nmemb, void *userdata)
{
    if ((cb == NULL) || (nmemb == 0U)) { return CURLE_OK; }

    for (int attempt = 0; attempt < PAL_PAUSE_MAX_RETRIES; attempt++) {
        size_t wrote = cb((void *)(uintptr_t)ptr, 1U, nmemb, userdata);
        if (wrote == nmemb) { return CURLE_OK; }
        if (wrote == (size_t)CURL_WRITEFUNC_PAUSE) {
            { struct timespec ts = { 0L, (long)(PAL_PAUSE_DELAY_US) * 1000L }; (void)nanosleep(&ts, NULL); }
            continue;
        }
        return CURLE_WRITE_ERROR;
    }
    return CURLE_ABORTED_BY_CALLBACK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §9 — Body streaming
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Stream a body of known Content-Length bytes.
 *
 * First drains any body bytes already in body_prefix (received alongside
 * the headers), then reads the remainder from sock.
 *
 * @param sock              Connected socket.
 * @param body_prefix       Pointer into the header buffer after header end.
 * @param body_prefix_len   Bytes already received (may be 0).
 * @param total             Exact Content-Length.
 * @param ctx               Handle context.
 * @param buf               Heap buffer (BODY_BUF_SIZE bytes).
 * @param buf_size          sizeof(buf).
 * @param bytes_out         Incremented with bytes delivered to callback.
 *
 * @return CURLE_OK, CURLE_PARTIAL_FILE, CURLE_RECV_ERROR,
 *         CURLE_WRITE_ERROR, CURLE_OPERATION_TIMEDOUT,
 *         or CURLE_ABORTED_BY_CALLBACK.
 *
 * @note Thread-safety: NOT thread-safe.
 */
static CURLcode stream_body_known_length(int sock,
                                          const uint8_t *body_prefix,
                                          size_t body_prefix_len,
                                          uint64_t total,
                                          pal_curl_ctx_t *ctx,
                                          uint8_t *buf, size_t buf_size,
                                          uint64_t *bytes_out)
{
    speed_guard_t sg;
    speed_guard_init(&sg, ctx);

    uint64_t remaining = total;

    /* Drain prefix */
    if (body_prefix_len > 0U) {
        size_t to_write = (body_prefix_len > (size_t)remaining)
                          ? (size_t)remaining : body_prefix_len;
        CURLcode rc = invoke_write_cb(ctx->write_cb, body_prefix, to_write,
                                       ctx->write_data);
        if (rc != CURLE_OK) { return rc; }
        *bytes_out += to_write;
        remaining  -= (uint64_t)to_write;
    }

    while (remaining > 0U) {
        size_t  want = (remaining < (uint64_t)buf_size)
                       ? (size_t)remaining : buf_size;
        ssize_t n    = recv_timed(sock, buf, want, ctx->timeout_ms);
        if (n < 0)  { return CURLE_RECV_ERROR; }
        if (n == 0) { return CURLE_PARTIAL_FILE; }

        CURLcode rc = invoke_write_cb(ctx->write_cb, buf, (size_t)n,
                                       ctx->write_data);
        if (rc != CURLE_OK) { return rc; }
        *bytes_out += (size_t)n;
        remaining  -= (uint64_t)n;

        if (!speed_guard_update(&sg, (size_t)n, ctx)) {
            return CURLE_OPERATION_TIMEDOUT;
        }
        if ((ctx->xferinfo_cb != NULL) && (ctx->noprogress == 0)) {
            int r = ctx->xferinfo_cb(ctx->xferinfo_data,
                                      (curl_off_t)total,
                                      (curl_off_t)*bytes_out,
                                      0, 0);
            if (r != 0) { return CURLE_ABORTED_BY_CALLBACK; }
        }
    }
    return CURLE_OK;
}

/**
 * @brief Stream a chunked-encoded body.
 *
 * Passes bytes to the chunked state machine, which decodes and forwards
 * payload to write_cb.
 *
 * @note Thread-safety: NOT thread-safe.
 */
static CURLcode stream_body_chunked(int sock,
                                     const uint8_t *body_prefix,
                                     size_t body_prefix_len,
                                     pal_curl_ctx_t *ctx,
                                     uint8_t *buf, size_t buf_size,
                                     uint64_t *bytes_out)
{
    chunked_ctx_t cc;
    (void)memset(&cc, 0, sizeof(cc));
    cc.state = CS_SIZE;

    speed_guard_t sg;
    speed_guard_init(&sg, ctx);

    /* Process any body bytes that arrived with the headers. */
    if (body_prefix_len > 0U) {
        int r = chunked_process(&cc, body_prefix, body_prefix_len,
                                 ctx->write_cb, ctx->write_data, bytes_out);
        if (r == PAL_CHUNKED_DONE) { return CURLE_OK; }
        if (r != CURLE_OK)         { return (CURLcode)r; }
    }

    while (1) {
        ssize_t n = recv_timed(sock, buf, buf_size, ctx->timeout_ms);
        if (n < 0) { return CURLE_RECV_ERROR; }
        if (n == 0) {
            /* EOF: acceptable only if we've seen the terminal chunk. */
            return ((cc.state == CS_DONE) || (cc.state == CS_TRAILER))
                   ? CURLE_OK : CURLE_PARTIAL_FILE;
        }

        uint64_t prev = *bytes_out;
        int r = chunked_process(&cc, buf, (size_t)n,
                                 ctx->write_cb, ctx->write_data, bytes_out);

        if (!speed_guard_update(&sg, (size_t)(*bytes_out - prev), ctx)) {
            return CURLE_OPERATION_TIMEDOUT;
        }
        if (r == PAL_CHUNKED_DONE) { return CURLE_OK; }
        if (r != CURLE_OK)         { return (CURLcode)r; }

        if ((ctx->xferinfo_cb != NULL) && (ctx->noprogress == 0)) {
            int pr = ctx->xferinfo_cb(ctx->xferinfo_data, 0,
                                       (curl_off_t)*bytes_out, 0, 0);
            if (pr != 0) { return CURLE_ABORTED_BY_CALLBACK; }
        }
    }
}

/**
 * @brief Stream a body with no Content-Length until connection close.
 *
 * HTTP/1.0-style or HTTP/1.1 with Connection: close and no Content-Length.
 *
 * @note Thread-safety: NOT thread-safe.
 */
static CURLcode stream_body_until_close(int sock,
                                         const uint8_t *body_prefix,
                                         size_t body_prefix_len,
                                         pal_curl_ctx_t *ctx,
                                         uint8_t *buf, size_t buf_size,
                                         uint64_t *bytes_out)
{
    speed_guard_t sg;
    speed_guard_init(&sg, ctx);

    if (body_prefix_len > 0U) {
        CURLcode rc = invoke_write_cb(ctx->write_cb, body_prefix,
                                       body_prefix_len, ctx->write_data);
        if (rc != CURLE_OK) { return rc; }
        *bytes_out += body_prefix_len;
    }

    while (1) {
        ssize_t n = recv_timed(sock, buf, buf_size, ctx->timeout_ms);
        if (n < 0)  { return CURLE_RECV_ERROR; }
        if (n == 0) { break; } /* Clean EOF */

        CURLcode rc = invoke_write_cb(ctx->write_cb, buf, (size_t)n,
                                       ctx->write_data);
        if (rc != CURLE_OK) { return rc; }
        *bytes_out += (size_t)n;

        if (!speed_guard_update(&sg, (size_t)n, ctx)) {
            return CURLE_OPERATION_TIMEDOUT;
        }
        if ((ctx->xferinfo_cb != NULL) && (ctx->noprogress == 0)) {
            int r = ctx->xferinfo_cb(ctx->xferinfo_data, 0,
                                      (curl_off_t)*bytes_out, 0, 0);
            if (r != 0) { return CURLE_ABORTED_BY_CALLBACK; }
        }
    }
    return CURLE_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §10 — Redirect URL resolver
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Resolve a Location: header value against the current request URL.
 *
 * If location is an absolute http:// URL it is copied verbatim.
 * If it starts with '/' it is treated as an absolute path on the same origin.
 * Otherwise it is treated as a relative path on the same origin.
 *
 * @param base_url   The URL of the request that triggered the redirect.
 * @param location   Location header value.
 * @param out        Output buffer for the resolved URL.
 * @param out_size   sizeof(out).
 *
 * @note Thread-safety: pure (no shared state).
 * @note WCET: O(len(base_url) + len(location)).
 */
static void resolve_redirect(const char *base_url, const char *location,
                              char *out, size_t out_size)
{
    /* Absolute URL — use as-is. */
    if ((strncmp(location, "http://", 7U) == 0) ||
        (strncmp(location, "https://", 8U) == 0)) {
        strncpy(out, location, out_size - 1U);
        out[out_size - 1U] = '\0';
        return;
    }

    /* Relative — extract origin from base_url. */
    char host[256U];
    char dummy[8U];
    int  port;
    if (url_parse(base_url, host, sizeof(host), &port, dummy, sizeof(dummy))
        != CURLE_OK) {
        strncpy(out, location, out_size - 1U);
        out[out_size - 1U] = '\0';
        return;
    }

    if (location[0] == '/') {
        (void)snprintf(out, out_size, "http://%s:%d%s", host, port, location);
    } else {
        (void)snprintf(out, out_size, "http://%s:%d/%s", host, port, location);
    }
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §11 — Single-request performer
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Execute one HTTP transaction (no redirect following).
 *
 * On return:
 *  - *status_out  = HTTP status code (e.g. 200, 301, 404)
 *  - redirect_out = Location: value if 3xx (may be "")
 *  - *bytes_out   = bytes delivered to write_cb
 *
 * @return CURLE_OK or a specific CURLcode error.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: Unbounded (network I/O).
 */
static CURLcode perform_one(pal_curl_ctx_t *ctx, const char *url,
                             long *status_out,
                             char *redirect_out, size_t redirect_max,
                             uint64_t *bytes_out)
{
    /* ── Parse URL ──────────────────────────────────────────────── */
    char host[256U], path[1800U];
    int  port;
    CURLcode rc = url_parse(url, host, sizeof(host), &port, path, sizeof(path));
    if (rc != CURLE_OK) {
        set_errbuf(ctx, "URL parse failed");
        return rc;
    }

    /* ── Connect ────────────────────────────────────────────────── */
    CURLcode conn_err;
    long conn_to = (ctx->connect_timeout_ms > 0L) ? ctx->connect_timeout_ms
                                                   : DEFAULT_CONN_MS;
    int sock = net_connect(host, port, conn_to, &conn_err);
    if (sock < 0) {
        set_errbuf(ctx, "Connection failed");
        return conn_err;
    }
    PAL_LOG(ctx, "Connected to %s:%d\n", host, port);

    /* ── Build and send request ─────────────────────────────────── */
    char req_buf[REQ_BUF_SIZE];
    const char *method = (ctx->nobody != 0) ? "HEAD"
                       : (ctx->post   != 0) ? "POST"
                       :                      "GET";
    int req_len = build_request(ctx, method, host, port, path,
                                req_buf, sizeof(req_buf));
    if (req_len <= 0) {
        close(sock);
        set_errbuf(ctx, "Request too large for buffer");
        return CURLE_SEND_ERROR;
    }

    long send_to = (ctx->timeout_ms > 0L) ? ctx->timeout_ms : conn_to;
    if (send_all(sock, req_buf, (size_t)req_len, send_to) != 0) {
        close(sock);
        set_errbuf(ctx, "Failed to send request headers");
        return CURLE_SEND_ERROR;
    }
    PAL_LOG(ctx, "→ %s %s\n", method, path);

    /* ── Send POST body ─────────────────────────────────────────── */
    if ((ctx->post != 0) && (ctx->postfields != NULL)) {
        long plen = (ctx->postfieldsize > 0L) ? ctx->postfieldsize
                                               : (long)strlen(ctx->postfields);
        if (send_all(sock, ctx->postfields, (size_t)plen, send_to) != 0) {
            close(sock);
            set_errbuf(ctx, "Failed to send POST body");
            return CURLE_SEND_ERROR;
        }
    }

    /* ── Read response headers ──────────────────────────────────── */
    char   hdr_buf[HDR_BUF_SIZE];
    size_t hdr_len       = 0U;
    size_t body_pfx_len  = 0U;
    long   recv_to = (ctx->timeout_ms > 0L) ? ctx->timeout_ms : DEFAULT_CONN_MS;

    rc = http_recv_headers(sock, recv_to, hdr_buf, sizeof(hdr_buf),
                           &hdr_len, &body_pfx_len);
    if (rc != CURLE_OK) {
        close(sock);
        set_errbuf(ctx, "Failed to receive response headers");
        return rc;
    }

    /* ── Parse status line ──────────────────────────────────────── */
    if (parse_status_line(hdr_buf, status_out) != 0) {
        close(sock);
        set_errbuf(ctx, "Malformed HTTP status line");
        return CURLE_RECV_ERROR;
    }
    PAL_LOG(ctx, "← HTTP %ld\n", *status_out);

    /* ── Parse header fields ─────────────────────────────────────── */
    http_resp_t resp;
    (void)memset(&resp, 0, sizeof(resp));
    /*
     * hdr_len includes the terminal \r\n\r\n (4 bytes).
     * parse_header_fields() should not see those 4 bytes as a header line.
     */
    parse_header_fields(hdr_buf, (hdr_len >= 4U) ? (hdr_len - 4U) : 0U,
                        &resp);
    resp.status_code = *status_out;

    ctx->content_length_download = (resp.content_length >= 0)
                                   ? (double)resp.content_length : -1.0;

    /* ── Redirect URL ────────────────────────────────────────────── */
    if ((redirect_out != NULL) && (resp.location[0] != '\0')) {
        resolve_redirect(url, resp.location, redirect_out, redirect_max);
    } else if (redirect_out != NULL) {
        redirect_out[0] = '\0';
    }

    /* ── HEAD / error: no body to read ─────────────────────────── */
    if ((ctx->nobody != 0) || (*status_out >= 400)) {
        close(sock);
        *bytes_out = 0U;
        return CURLE_OK;
    }

    /* ── Allocate body streaming buffer ─────────────────────────── */
    uint8_t *body_buf = (uint8_t *)malloc(BODY_BUF_SIZE);
    if (body_buf == NULL) {
        close(sock);
        set_errbuf(ctx, "Out of memory for body buffer");
        return CURLE_OUT_OF_MEMORY;
    }

    const uint8_t *pfx = (const uint8_t *)(hdr_buf + hdr_len);

    /* ── Stream body ─────────────────────────────────────────────── */
    if (resp.chunked != 0) {
        rc = stream_body_chunked(sock, pfx, body_pfx_len, ctx,
                                  body_buf, BODY_BUF_SIZE, bytes_out);
    } else if (resp.content_length >= 0) {
        rc = stream_body_known_length(sock, pfx, body_pfx_len,
                                       (uint64_t)resp.content_length,
                                       ctx, body_buf, BODY_BUF_SIZE,
                                       bytes_out);
    } else {
        rc = stream_body_until_close(sock, pfx, body_pfx_len, ctx,
                                      body_buf, BODY_BUF_SIZE, bytes_out);
    }

    free(body_buf);
    close(sock);
    return rc;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §12 — Public API
 * ═════════════════════════════════════════════════════════════════════════*/

CURLcode curl_global_init(long flags)
{
    (void)flags;
    return CURLE_OK;
}

void curl_global_cleanup(void)
{
    /* No-op. */
}

CURL *curl_easy_init(void)
{
    pal_curl_ctx_t *ctx = (pal_curl_ctx_t *)calloc(1U, sizeof(pal_curl_ctx_t));
    if (ctx == NULL) { return NULL; }
    ctx->max_redirs          = DEFAULT_MAX_REDIRS;
    ctx->connect_timeout_ms  = DEFAULT_CONN_MS;
    ctx->timeout_ms          = DEFAULT_TIMEOUT_MS;
    ctx->noprogress          = 1;
    ctx->content_length_download = -1.0;
    return (CURL *)ctx;
}

void curl_easy_reset(CURL *handle)
{
    if (handle == NULL) { return; }
    pal_curl_ctx_t *ctx = (pal_curl_ctx_t *)handle;
    (void)memset(ctx, 0, sizeof(*ctx));
    ctx->max_redirs              = DEFAULT_MAX_REDIRS;
    ctx->connect_timeout_ms      = DEFAULT_CONN_MS;
    ctx->timeout_ms              = DEFAULT_TIMEOUT_MS;
    ctx->noprogress              = 1;
    ctx->content_length_download = -1.0;
}

void curl_easy_cleanup(CURL *handle)
{
    if (handle == NULL) { return; }
    free(handle);
}

CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...)
{
    if (handle == NULL) { return CURLE_UNKNOWN_OPTION; }
    pal_curl_ctx_t *ctx = (pal_curl_ctx_t *)handle;

    va_list ap;
    va_start(ap, option);

    CURLcode result = CURLE_OK;

    switch (option) {

    /* ── Object-pointer options ─────────────────────────────────── */
    case CURLOPT_URL: {
        const char *url = va_arg(ap, const char *);
        if (url != NULL) {
            strncpy(ctx->url, url, sizeof(ctx->url) - 1U);
            ctx->url[sizeof(ctx->url) - 1U] = '\0';
        }
        break;
    }
    case CURLOPT_WRITEDATA:
        ctx->write_data = va_arg(ap, void *);
        break;
    case CURLOPT_ERRORBUFFER:
        ctx->errbuf = va_arg(ap, char *);
        break;
    case CURLOPT_POSTFIELDS: {
        ctx->postfields = va_arg(ap, char *); /* caller owns */
        ctx->post = 1;
        break;
    }
    case CURLOPT_USERAGENT: {
        const char *ua = va_arg(ap, const char *);
        if (ua != NULL) {
            strncpy(ctx->useragent, ua, sizeof(ctx->useragent) - 1U);
            ctx->useragent[sizeof(ctx->useragent) - 1U] = '\0';
        }
        break;
    }
    case CURLOPT_HTTPHEADER:
        ctx->httpheader = va_arg(ap, curl_slist *);
        break;
    case CURLOPT_RANGE: {
        const char *r = va_arg(ap, const char *);
        if (r != NULL) {
            strncpy(ctx->range, r, sizeof(ctx->range) - 1U);
            ctx->range[sizeof(ctx->range) - 1U] = '\0';
        } else {
            ctx->range[0] = '\0';
        }
        break;
    }
    case CURLOPT_XFERINFODATA:
        ctx->xferinfo_data = va_arg(ap, void *);
        break;

    /* ── Long options ────────────────────────────────────────────── */
    case CURLOPT_VERBOSE:
        ctx->verbose = (int)va_arg(ap, long);
        break;
    case CURLOPT_NOPROGRESS:
        ctx->noprogress = (int)va_arg(ap, long);
        break;
    case CURLOPT_NOBODY:
        ctx->nobody = (int)va_arg(ap, long);
        break;
    case CURLOPT_POST:
        ctx->post = (int)va_arg(ap, long);
        break;
    case CURLOPT_FOLLOWLOCATION:
        ctx->follow_location = va_arg(ap, long);
        break;
    case CURLOPT_MAXREDIRS:
        ctx->max_redirs = va_arg(ap, long);
        break;
    case CURLOPT_POSTFIELDSIZE:
        ctx->postfieldsize = va_arg(ap, long);
        break;
    case CURLOPT_TIMEOUT:
        ctx->timeout_ms = va_arg(ap, long) * 1000L;
        break;
    case CURLOPT_TIMEOUT_MS:
        ctx->timeout_ms = va_arg(ap, long);
        break;
    case CURLOPT_CONNECTTIMEOUT:
        ctx->connect_timeout_ms = va_arg(ap, long) * 1000L;
        break;
    case CURLOPT_CONNECTTIMEOUT_MS:
        ctx->connect_timeout_ms = va_arg(ap, long);
        break;
    case CURLOPT_LOW_SPEED_LIMIT:
        ctx->low_speed_limit = va_arg(ap, long);
        break;
    case CURLOPT_LOW_SPEED_TIME:
        ctx->low_speed_time = va_arg(ap, long);
        break;
    case CURLOPT_SSL_VERIFYPEER:
        (void)va_arg(ap, long); /* accepted, ignored */
        break;

    /* ── Function-pointer options ───────────────────────────────── */
    case CURLOPT_WRITEFUNCTION:
        ctx->write_cb = va_arg(ap, curl_write_callback);
        break;
    case CURLOPT_XFERINFOFUNCTION:
        ctx->xferinfo_cb = va_arg(ap, curl_xferinfo_callback);
        break;

    default:
        (void)va_arg(ap, void *); /* consume unknown argument */
        result = CURLE_UNKNOWN_OPTION;
        break;
    }

    va_end(ap);
    return result;
}

CURLcode curl_easy_perform(CURL *handle)
{
    if (handle == NULL) { return CURLE_UNKNOWN_OPTION; }
    pal_curl_ctx_t *ctx = (pal_curl_ctx_t *)handle;

    if (ctx->url[0] == '\0') {
        set_errbuf(ctx, "No URL set");
        return CURLE_URL_MALFORMAT;
    }

    /* Reset per-transfer response fields. */
    ctx->response_code           = 0L;
    ctx->size_download           = 0.0;
    ctx->speed_download          = 0.0;
    ctx->total_time              = 0.0;
    ctx->content_length_download = -1.0;

    struct timeval tv_start, tv_end;
    (void)gettimeofday(&tv_start, NULL);

    char current_url[sizeof(ctx->url)];
    strncpy(current_url, ctx->url, sizeof(current_url) - 1U);
    current_url[sizeof(current_url) - 1U] = '\0';

    int      redirects = 0;
    CURLcode rc        = CURLE_OK;

    while (1) {
        if ((ctx->max_redirs >= 0L) && (redirects > (int)ctx->max_redirs)) {
            rc = CURLE_TOO_MANY_REDIRECTS;
            break;
        }

        long   status = 0L;
        char   redir_url[sizeof(current_url)];
        redir_url[0] = '\0';
        uint64_t bytes = 0U;

        rc = perform_one(ctx, current_url, &status, redir_url,
                         sizeof(redir_url), &bytes);
        if (rc != CURLE_OK) { break; }

        ctx->response_code  = status;
        ctx->size_download += (double)bytes;

        /* Follow redirects. */
        if ((status >= 300L) && (status < 400L) &&
            (ctx->follow_location != 0L) && (redir_url[0] != '\0')) {
            strncpy(current_url, redir_url, sizeof(current_url) - 1U);
            current_url[sizeof(current_url) - 1U] = '\0';
            redirects++;
            PAL_LOG(ctx, "Redirect %d → %s\n", redirects, current_url);
            continue;
        }

        if (status >= 400L) {
            set_errbuf(ctx, "HTTP error status");
            rc = CURLE_HTTP_RETURNED_ERROR;
        }
        break;
    }

    (void)gettimeofday(&tv_end, NULL);
    ctx->total_time = (double)(tv_end.tv_sec  - tv_start.tv_sec) +
                      (double)(tv_end.tv_usec - tv_start.tv_usec) * 1e-6;
    if (ctx->total_time > 0.0) {
        ctx->speed_download = ctx->size_download / ctx->total_time;
    }

    return rc;
}

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...)
{
    if (handle == NULL) { return CURLE_UNKNOWN_OPTION; }
    const pal_curl_ctx_t *ctx = (const pal_curl_ctx_t *)handle;

    va_list ap;
    va_start(ap, info);

    CURLcode result = CURLE_OK;

    switch (info) {
    case CURLINFO_RESPONSE_CODE: {
        long *lp = va_arg(ap, long *);
        if (lp != NULL) { *lp = ctx->response_code; }
        break;
    }
    case CURLINFO_CONTENT_LENGTH_DOWNLOAD: {
        double *dp = va_arg(ap, double *);
        if (dp != NULL) { *dp = ctx->content_length_download; }
        break;
    }
    case CURLINFO_SIZE_DOWNLOAD: {
        double *dp = va_arg(ap, double *);
        if (dp != NULL) { *dp = ctx->size_download; }
        break;
    }
    case CURLINFO_SPEED_DOWNLOAD: {
        double *dp = va_arg(ap, double *);
        if (dp != NULL) { *dp = ctx->speed_download; }
        break;
    }
    case CURLINFO_TOTAL_TIME: {
        double *dp = va_arg(ap, double *);
        if (dp != NULL) { *dp = ctx->total_time; }
        break;
    }
    default:
        (void)va_arg(ap, void *);
        result = CURLE_UNKNOWN_OPTION;
        break;
    }

    va_end(ap);
    return result;
}

const char *curl_easy_strerror(CURLcode code)
{
    switch (code) {
    case CURLE_OK:                    return "No error";
    case CURLE_UNSUPPORTED_PROTOCOL:  return "Unsupported protocol";
    case CURLE_URL_MALFORMAT:         return "URL malformed or missing";
    case CURLE_COULDNT_RESOLVE_HOST:  return "Could not resolve host";
    case CURLE_COULDNT_CONNECT:       return "Failed to connect to host";
    case CURLE_PARTIAL_FILE:          return "Transfer ended prematurely";
    case CURLE_HTTP_RETURNED_ERROR:   return "HTTP response code indicates error";
    case CURLE_WRITE_ERROR:           return "Write callback returned error";
    case CURLE_OUT_OF_MEMORY:         return "Out of memory";
    case CURLE_OPERATION_TIMEDOUT:    return "Operation timed out";
    case CURLE_ABORTED_BY_CALLBACK:   return "Aborted by callback";
    case CURLE_TOO_MANY_REDIRECTS:    return "Too many redirects";
    case CURLE_UNKNOWN_OPTION:        return "Unknown option";
    case CURLE_GOT_NOTHING:           return "Server returned no data";
    case CURLE_SEND_ERROR:            return "Failed sending data to peer";
    case CURLE_RECV_ERROR:            return "Failure receiving data from peer";
    default:                          return "Unknown error code";
    }
}

const char *curl_version(void)
{
    return "pal_curl/2.0 (PS4/PS5 shim; HTTP/1.1; no TLS)";
}

/* ═══════════════════════════════════════════════════════════════════════════
 * §13 — curl_slist
 * ═════════════════════════════════════════════════════════════════════════*/

curl_slist *curl_slist_append(curl_slist *list, const char *data)
{
    if (data == NULL) { return list; }

    curl_slist *node = (curl_slist *)malloc(sizeof(curl_slist));
    if (node == NULL) { return list; } /* allocation failed; return unchanged */

    node->data = strdup(data);
    if (node->data == NULL) { free(node); return list; }
    node->next = NULL;

    if (list == NULL) { return node; }

    /* Walk to tail. */
    curl_slist *tail = list;
    while (tail->next != NULL) { tail = tail->next; }
    tail->next = node;
    return list;
}

void curl_slist_free_all(curl_slist *list)
{
    while (list != NULL) {
        curl_slist *next = list->next;
        free(list->data);
        free(list);
        list = next;
    }
}