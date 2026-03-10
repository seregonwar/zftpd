/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

/**
 * @file http_config.h
 * @brief HTTP server configuration
 */

#ifndef HTTP_CONFIG_H
#define HTTP_CONFIG_H

#include <stdint.h>

/* Compile-time feature toggle */
#ifndef ENABLE_ZHTTPD
#define ENABLE_ZHTTPD 1
#endif

#ifndef ENABLE_HTTP_GZIP
#define ENABLE_HTTP_GZIP 0
#endif

#ifndef HTTP_DEBUG_LOG_HEADERS
#define HTTP_DEBUG_LOG_HEADERS 0
#endif

/* Server configuration */
#define HTTP_DEFAULT_PORT 8888
#define HTTP_MAX_CONNECTIONS 100
#define HTTP_REQUEST_TIMEOUT 30
#define HTTP_KEEPALIVE_TIMEOUT 60

/* Buffer sizes */
#define HTTP_REQUEST_BUFFER_SIZE 8192
#define HTTP_RESPONSE_BUFFER_SIZE 8192
#define HTTP_URI_MAX_LENGTH 2048
#define HTTP_HEADER_MAX_COUNT 32
#define HTTP_HEADER_LINE_MAX 1024

/*
 * File transfer chunk size for sendfile() in /api/download.
 *
 * PS5 REGRESSION NOTE:
 *   PS5's modified FreeBSD kernel triggers an internal buffer limit with
 *   sendfile() chunks >= 1 MB, returning EAGAIN (sbytes = 0) even on
 *   nominally-blocking sockets.  Each EAGAIN costs a usleep(1 ms) yield
 *   (to avoid busy-spinning).  At 1 MB/chunk: 1.2 GB / 1 MB × 1 ms =
 *   ~1.2 s of extra sleep latency per download — the observed regression
 *   ("previously downloaded 1.2 GB immediately").
 *
 *   512 KB chunks stay well below the 1 MB trigger threshold, eliminating
 *   the EAGAIN storms while keeping syscall count reasonable (2× increase
 *   vs 1 MB, negligible vs I/O latency).
 */
#define HTTP_SENDFILE_CHUNK_SIZE (512 * 1024)

/* Thread stack size (bytes) */
#ifndef HTTP_THREAD_STACK_SIZE
#define HTTP_THREAD_STACK_SIZE (512U * 1024U)
#endif

/* CSRF token length in hex characters (32 hex = 16 random bytes) */
#define HTTP_CSRF_TOKEN_LENGTH 32

/*---------------------------------------------------------------------------*
 * Upload feature toggle (disabled by default for security)
 *---------------------------------------------------------------------------*/
#ifndef ENABLE_WEB_UPLOAD
#define ENABLE_WEB_UPLOAD 0
#endif

/*---------------------------------------------------------------------------*
 * Upload streaming performance tuning
 *
 * HTTP_UPLOAD_CHUNK_SIZE
 *   Heap-allocated read buffer used exclusively while an upload is active.
 *   Each event-loop iteration drains up to this many bytes from the socket.
 *
 *   Rationale:
 *     The connection's header buffer (HTTP_REQUEST_BUFFER_SIZE = 8 KB) is
 *     reused during streaming, capping each read() at 8 KB.  At 113 MB/s
 *     that requires ~13 800 read() + kqueue round-trips per second — well
 *     beyond what a single-threaded event loop can sustain.
 *
 *     256 KB reduces the required syscall rate to ~440/s at 113 MB/s,
 *     comfortably within budget while keeping per-upload heap overhead low.
 *     The buffer is allocated on upload start and freed on completion, so
 *     idle connections pay no memory cost.
 *
 * HTTP_UPLOAD_RCVBUF_SIZE
 *   SO_RCVBUF hint set on each accepted client socket.  A larger kernel
 *   receive buffer absorbs TCP bursts between event-loop wakeups and keeps
 *   the sender's congestion window open.  2 MB is sufficient for RTTs up
 *   to ~140 µs at 113 MB/s (BDP = 113e6 * 140e-6 ≈ 15 KB; 2 MB is 133×
 *   the BDP — intentionally oversized so the kernel never stalls).
 *
 *   IMPORTANT: This is a hint only.  The kernel caps it at
 *   net.core.rmem_max (Linux) or kern.ipc.maxsockbuf (FreeBSD/PS5).
 *   Setting it higher than the system maximum is silently ignored.
 *---------------------------------------------------------------------------*/
#ifndef HTTP_UPLOAD_CHUNK_SIZE
#define HTTP_UPLOAD_CHUNK_SIZE    (512U * 1024U)   /* 512 KB per active upload */
#endif

#ifndef HTTP_UPLOAD_RCVBUF_SIZE
#define HTTP_UPLOAD_RCVBUF_SIZE   (2U * 1024U * 1024U) /* 2 MB SO_RCVBUF hint */
#endif

#endif /* HTTP_CONFIG_H */
