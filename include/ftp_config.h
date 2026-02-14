/**
 * @file ftp_config.h
 * @brief Compile-time configuration for multi-platform FTP server
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * SAFETY CLASSIFICATION: Embedded systems, production-grade
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#ifndef FTP_CONFIG_H
#define FTP_CONFIG_H

#include <stdint.h>
#include <stddef.h>

/*===========================================================================*
 * VERSION INFORMATION
 *===========================================================================*/

#ifndef RELEASE_VERSION
#define RELEASE_VERSION "1.0.0"
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
#define FTP_STREAM_BUFFER_SIZE 65536U
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
#define FTP_MAX_SESSIONS 16U
#endif

/**
 * Session idle timeout in seconds
 * @note Client disconnected after this period of inactivity
 */
#ifndef FTP_SESSION_TIMEOUT
#define FTP_SESSION_TIMEOUT 300U
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
#define FTP_BUFFER_SIZE 65536U
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

/*===========================================================================*
 * PERFORMANCE TUNING
 *===========================================================================*/

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
 * @note Larger buffers improve throughput for bulk transfers
 * @note 256KB is optimal for gigabit networks
 */
#ifndef FTP_TCP_SNDBUF
#define FTP_TCP_SNDBUF 262144U
#endif

/**
 * TCP receive buffer size in bytes
 * @note Should match send buffer size
 */
#ifndef FTP_TCP_RCVBUF
#define FTP_TCP_RCVBUF 262144U
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
 * @note Console environments may require larger stacks due to libc/network internals
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
_Static_assert(FTP_MAX_SESSIONS > 0U,
               "FTP_MAX_SESSIONS must be > 0");

/* Ensure reasonable session limit (resource exhaustion) */
_Static_assert(FTP_MAX_SESSIONS <= 256U,
               "FTP_MAX_SESSIONS must be <= 256");

/* Ensure path depth is reasonable */
_Static_assert(FTP_MAX_PATH_DEPTH > 0U && FTP_MAX_PATH_DEPTH <= 128U,
               "FTP_MAX_PATH_DEPTH must be 1-128");

/* Ensure stack size is sufficient (minimum 32KB) */
_Static_assert(FTP_THREAD_STACK_SIZE >= 32768U,
               "FTP_THREAD_STACK_SIZE must be >= 32KB");

#endif /* FTP_CONFIG_H */
