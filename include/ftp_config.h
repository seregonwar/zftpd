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
 * @file ftp_config.h
 * @brief Compile-time configuration for multi-platform FTP server
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 */

#ifndef FTP_CONFIG_H
#define FTP_CONFIG_H

#include <stddef.h>
#include <stdint.h>

/*===========================================================================*
 * VERSION INFORMATION
 *===========================================================================*/

#ifndef RELEASE_VERSION
#define RELEASE_VERSION "1.2.2"
#endif

/*===========================================================================*
 * SERVER CONFIGURATION
 *===========================================================================*/

/**
 * Default FTP server port
 * @note Well-known FTP port is 21, but requires root on POSIX systems
 * @note Using 2121 as default for unprivileged operation on POSIX systems
 * @note Using 2122 as default on PS4/PS5 because 2121 may be occupied
 */
#ifndef FTP_DEFAULT_PORT
#if defined(PS4) || defined(PS5)
#define FTP_DEFAULT_PORT 2122U
#else
#define FTP_DEFAULT_PORT 2121U
#endif
#endif

#ifndef FTP_STREAM_BUFFER_SIZE
#if defined(PS5)
#define FTP_STREAM_BUFFER_SIZE 1048576U /* 1 MB  — PS5 has plenty of RAM  */
#elif defined(PS4)
#define FTP_STREAM_BUFFER_SIZE 262144U /* 256 KB                         */
#else
#define FTP_STREAM_BUFFER_SIZE 524288U /* 512 KB — saturates GbE links   */
#endif
#endif

#ifndef FTP_STREAM_BUFFER_COUNT
#define FTP_STREAM_BUFFER_COUNT FTP_MAX_SESSIONS
#endif

/**
 * Maximum number of concurrent client connections
 * @note This is a hard limit to prevent resource exhaustion
 * @note Each client requires ~2KB of memory plus thread stack
 */
#ifndef FTP_MAX_SESSIONS
#define FTP_MAX_SESSIONS 32U
#endif

/**
 * Session idle timeout in seconds
 * @note Client disconnected after this period of inactivity
 */
#ifndef FTP_SESSION_TIMEOUT
#if defined(PS5) || defined(PS4)
#define FTP_SESSION_TIMEOUT 7200U
#else
#define FTP_SESSION_TIMEOUT 300U
#endif
#endif

#ifndef FTP_CTRL_IO_TIMEOUT_MS
#define FTP_CTRL_IO_TIMEOUT_MS 1000U
#endif

/**
 * Data socket I/O timeout (recv/send) in milliseconds
 *
 *   Prevents infinite stalls when disk write-back cache fills
 *   or the remote client disappears.  2 minutes is generous
 *   enough for slow USB drives while still detecting dead links.
 */
#ifndef FTP_DATA_IO_TIMEOUT_MS
#define FTP_DATA_IO_TIMEOUT_MS 120000U
#endif

/**
 * SO_LINGER timeout for data sockets (seconds)
 *
 *   After close(), the kernel keeps the socket open for this
 *   long to flush remaining data.  Prevents ECONNRESET on the
 *   client when the server closes right after the last write.
 */
#ifndef FTP_DATA_LINGER_TIMEOUT_S
#define FTP_DATA_LINGER_TIMEOUT_S 10
#endif

#ifndef FTP_DATA_CONNECT_TIMEOUT_MS
#define FTP_DATA_CONNECT_TIMEOUT_MS 15000U
#endif

/**
 * Listen backlog for accept queue
 * @note Number of pending connections before refusing new ones
 */
#ifndef FTP_LISTEN_BACKLOG
#define FTP_LISTEN_BACKLOG 8U
#endif

/*===========================================================================*
 * BUFFER SIZES
 *===========================================================================*/

/**
 * File transfer buffer size (must be power of 2)
 * @note 64KB is optimal for most network/disk combinations
 * @note Larger buffers provide diminishing returns
 */
#ifndef FTP_BUFFER_SIZE
#if defined(PS5)
#define FTP_BUFFER_SIZE 1048576U /* 1 MB    */
#elif defined(PS4)
#define FTP_BUFFER_SIZE 262144U /* 256 KB  */
#else
#define FTP_BUFFER_SIZE 524288U /* 512 KB  */
#endif
#endif

/**
 * Command line buffer size (RFC 959: max 512 bytes)
 * @note Includes command + arguments + CRLF
 */
#ifndef FTP_CMD_BUFFER_SIZE
#define FTP_CMD_BUFFER_SIZE 512U
#endif

/**
 * Reply line buffer size
 * @note Should accommodate longest possible reply
 */
#ifndef FTP_REPLY_BUFFER_SIZE
#define FTP_REPLY_BUFFER_SIZE 1024U
#endif

/**
 * Directory listing line buffer size
 * @note Long filenames may require larger buffer
 */
#ifndef FTP_LIST_LINE_SIZE
#define FTP_LIST_LINE_SIZE 512U
#endif

/*===========================================================================*
 * PATH LIMITS (Platform-dependent)
 *===========================================================================*/

#if defined(PS4) || defined(PS5)
/**
 * PlayStation maximum path length
 * @note PS4/PS5 use custom BSD with 1024-byte limit
 */
#ifndef FTP_PATH_MAX
#define FTP_PATH_MAX 1024U
#endif
#elif defined(PS3)
/**
 * PlayStation 3 maximum path length
 * @note PS3 has more limited path support
 */
#ifndef FTP_PATH_MAX
#define FTP_PATH_MAX 512U
#endif
#else
/**
 * POSIX maximum path length
 * @note Linux typically supports 4096 bytes
 */
#ifndef FTP_PATH_MAX
#define FTP_PATH_MAX 4096U
#endif
#endif

/**
 * Maximum directory nesting depth
 * @note Prevents stack overflow in recursive operations
 * @note Protects against malicious deep directory structures
 */
#ifndef FTP_MAX_PATH_DEPTH
#define FTP_MAX_PATH_DEPTH 32U
#endif

/**
 * Maximum symlink recursion depth
 * @note Prevents symlink loops
 */
#ifndef FTP_MAX_SYMLINK_DEPTH
#define FTP_MAX_SYMLINK_DEPTH 8U
#endif

/*===========================================================================*
 * FEATURE FLAGS
 *===========================================================================*/

/**
 * Enable MLST/MLSD commands (RFC 3659)
 * @note Modern directory listing with machine-readable format
 */
#ifndef FTP_ENABLE_MLST
#define FTP_ENABLE_MLST 1
#endif

/**
 * Enable UTF-8 filename support
 * @note PS3 does not support UTF-8, disable for that platform
 */
#ifndef FTP_ENABLE_UTF8
#if defined(PS3)
#define FTP_ENABLE_UTF8 0
#else
#define FTP_ENABLE_UTF8 1
#endif
#endif

/**
 * Enable SIZE command
 * @note Returns file size in bytes
 */
#ifndef FTP_ENABLE_SIZE
#define FTP_ENABLE_SIZE 1
#endif

/**
 * Enable MDTM command
 * @note Returns file modification time
 */
#ifndef FTP_ENABLE_MDTM
#define FTP_ENABLE_MDTM 1
#endif

/**
 * Enable REST command
 * @note Resume interrupted transfers
 */
#ifndef FTP_ENABLE_REST
#define FTP_ENABLE_REST 1
#endif

/**
 * Enable ChaCha20 stream encryption (AUTH XCRYPT)
 *
 *   ON  (1) : Linux, macOS — encrypts ctrl + data channels
 *   OFF (0) : PS4, PS5    — local-network trust, no overhead
 *
 * @note Encryption is negotiated per-session via AUTH XCRYPT.
 *       Clients that do not send AUTH XCRYPT transfer in cleartext.
 */
#ifndef FTP_ENABLE_CRYPTO
#if defined(PS4) || defined(PS5) || defined(PLATFORM_PS4) ||                   \
    defined(PLATFORM_PS5)
#define FTP_ENABLE_CRYPTO 0
#else
#define FTP_ENABLE_CRYPTO 1
#endif
#endif

/**
 * Pre-shared key for ChaCha20 encryption (256-bit / 32 bytes)
 *
 * Override at compile time:
 *   -DFTP_CRYPTO_PSK='{ 0x01,0x02,...,0x20 }'
 *
 * @warning Change this default before deploying to production!
 */
#ifndef FTP_CRYPTO_PSK
#define FTP_CRYPTO_PSK                                                         \
  {0x7A, 0x46, 0x54, 0x50, 0x44, 0x2D, 0x43, 0x68, 0x61, 0x43, 0x68,           \
   0x61, 0x32, 0x30, 0x2D, 0x4B, 0x65, 0x79, 0x2D, 0x44, 0x65, 0x66,           \
   0x61, 0x75, 0x6C, 0x74, 0x21, 0x40, 0x23, 0x24, 0x25, 0x5E}
#endif

/*===========================================================================*
 * PERFORMANCE TUNING
 *===========================================================================*/

#ifndef FTP_LIST_SAFE_MODE
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define FTP_LIST_SAFE_MODE 1
#else
#define FTP_LIST_SAFE_MODE 0
#endif
#endif

/**
 * Enable TCP_NODELAY (disable Nagle's algorithm)
 * @note Reduces latency for small packets (control commands)
 * @note Recommended: enabled for FTP control connection
 */
#ifndef FTP_TCP_NODELAY
#define FTP_TCP_NODELAY 1
#endif

/**
 * TCP send buffer size in bytes
 *
 *   Sized to fill the bandwidth-delay product (BDP):
 *     GbE 1 ms RTT  ->  BDP = 125 KB  ->  1 MB is generous
 *     WiFi 5 ms RTT ->  BDP = 62 KB   ->  1 MB has headroom
 */
#ifndef FTP_TCP_SNDBUF
#define FTP_TCP_SNDBUF 1048576U
#endif

/**
 * TCP receive buffer size in bytes
 */
#ifndef FTP_TCP_RCVBUF
#define FTP_TCP_RCVBUF 1048576U
#endif

/**
 * Enable SO_KEEPALIVE
 * @note Detects dead connections via TCP keepalive probes
 */
#ifndef FTP_TCP_KEEPALIVE
#define FTP_TCP_KEEPALIVE 1
#endif

/**
 * Keepalive idle time in seconds
 * @note Time before first keepalive probe
 */
#ifndef FTP_TCP_KEEPIDLE
#define FTP_TCP_KEEPIDLE 60U
#endif

/**
 * Keepalive interval in seconds
 * @note Time between keepalive probes
 */
#ifndef FTP_TCP_KEEPINTVL
#define FTP_TCP_KEEPINTVL 10U
#endif

/**
 * Keepalive probe count
 * @note Number of probes before considering connection dead
 */
#ifndef FTP_TCP_KEEPCNT
#define FTP_TCP_KEEPCNT 3U
#endif

#ifndef FTP_SOCKET_TELEMETRY
#define FTP_SOCKET_TELEMETRY 0
#endif

/*===========================================================================*
 * SECURITY LIMITS
 *===========================================================================*/

/**
 * Maximum authentication attempts before disconnect
 * @note Prevents brute-force password attacks
 */
#ifndef FTP_MAX_AUTH_ATTEMPTS
#define FTP_MAX_AUTH_ATTEMPTS 3U
#endif

/**
 * Delay after failed authentication (seconds)
 * @note Slows down brute-force attempts
 */
#ifndef FTP_AUTH_DELAY
#define FTP_AUTH_DELAY 2U
#endif

/**
 * Maximum filename length
 * @note Conservative limit to prevent buffer overflows
 */
#ifndef FTP_MAX_FILENAME_LEN
#define FTP_MAX_FILENAME_LEN 255U
#endif

/*===========================================================================*
 * THREAD CONFIGURATION
 *===========================================================================*/

/**
 * Thread stack size in bytes
 * @note Console environments may require larger stacks due to libc/network
 * internals
 * @note See whitepaper section 4.2 for worst-case analysis
 */
#ifndef FTP_THREAD_STACK_SIZE
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define FTP_THREAD_STACK_SIZE 524288U
#else
#define FTP_THREAD_STACK_SIZE 65536U
#endif
#endif

/*===========================================================================*
 * DEBUG AND LOGGING
 *===========================================================================*/

/**
 * Enable debug logging
 * @note Set to 1 for development, 0 for production
 */
#ifndef FTP_DEBUG
#define FTP_DEBUG 0
#endif

/**
 * Enable verbose command logging
 * @note Logs all FTP commands (privacy concern in production)
 */
#ifndef FTP_LOG_COMMANDS
#define FTP_LOG_COMMANDS 0
#endif

/**
 * Enable performance statistics
 * @note Track throughput, latency, error rates
 */
#ifndef FTP_ENABLE_STATS
#define FTP_ENABLE_STATS 1
#endif

#ifndef FTP_TRANSFER_RATE_LIMIT_BPS
#define FTP_TRANSFER_RATE_LIMIT_BPS 0U
#endif

#ifndef FTP_TRANSFER_RATE_BURST_BYTES
#define FTP_TRANSFER_RATE_BURST_BYTES (FTP_TRANSFER_RATE_LIMIT_BPS)
#endif

#if defined(PLATFORM_PS5)
#undef FTP_TRANSFER_RATE_LIMIT_BPS
#define FTP_TRANSFER_RATE_LIMIT_BPS 0U
#undef FTP_TRANSFER_RATE_BURST_BYTES
#define FTP_TRANSFER_RATE_BURST_BYTES 0U
#endif

/*===========================================================================*
 * COMPILE-TIME ASSERTIONS
 *===========================================================================*/

/* Ensure buffer size is power of 2 (optimization) */
_Static_assert((FTP_BUFFER_SIZE & (FTP_BUFFER_SIZE - 1U)) == 0U,
               "FTP_BUFFER_SIZE must be power of 2");

/* Ensure command buffer meets RFC 959 requirement */
_Static_assert(FTP_CMD_BUFFER_SIZE >= 512U,
               "FTP_CMD_BUFFER_SIZE must be >= 512 bytes (RFC 959)");

/* Ensure at least one session allowed */
_Static_assert(FTP_MAX_SESSIONS > 0U, "FTP_MAX_SESSIONS must be > 0");

/* Ensure reasonable session limit (resource exhaustion) */
_Static_assert(FTP_MAX_SESSIONS <= 256U, "FTP_MAX_SESSIONS must be <= 256");

/* Ensure path depth is reasonable */
_Static_assert(FTP_MAX_PATH_DEPTH > 0U && FTP_MAX_PATH_DEPTH <= 128U,
               "FTP_MAX_PATH_DEPTH must be 1-128");

/* Ensure stack size is sufficient (minimum 32KB) */
_Static_assert(FTP_THREAD_STACK_SIZE >= 32768U,
               "FTP_THREAD_STACK_SIZE must be >= 32KB");

#endif /* FTP_CONFIG_H */
