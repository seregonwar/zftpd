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
 * @file pal_network.h
 * @brief Platform-agnostic network I/O operations
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 * DESIGN: Zero-overhead abstraction via compile-time selection
 * PERFORMANCE: Inline functions and macros (no runtime cost)
 *
 */

#ifndef PAL_NETWORK_H
#define PAL_NETWORK_H

#include "ftp_types.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

/*===========================================================================*
 * PLATFORM DETECTION
 *===========================================================================*/

#if defined(__PS3__)
#define PLATFORM_PS3 1
#elif defined(__ORBIS__) || defined(PS4)
#define PLATFORM_PS4 1
#elif defined(__PROSPERO__) || defined(PS5)
#define PLATFORM_PS5 1
#elif defined(__linux__)
#define PLATFORM_LINUX 1
#elif defined(__FreeBSD__)
#define PLATFORM_FREEBSD 1
#else
#define PLATFORM_POSIX 1
#endif

/*===========================================================================*
 * PLATFORM-SPECIFIC INCLUDES
 *===========================================================================*/

#ifdef PLATFORM_PS4
/* PS4 payload environment provides BSD/POSIX networking via libc */
#endif

#ifdef PLATFORM_PS5
/* PS5 uses standard BSD sockets via syscalls */
#include <sys/syscall.h>
#endif

#ifdef PLATFORM_PS3
#include <net/net.h>
/* PS3 uses custom network stack */
#endif

/*===========================================================================*
 * SOCKET TYPE ABSTRACTION
 *===========================================================================*/

typedef int socket_t;

#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)

/*===========================================================================*
 * NETWORK API MACROS
 *===========================================================================*/

#ifdef PLATFORM_PS4
/**
 * PS4 socket operations (BSD/POSIX)
 */
#define PAL_SOCKET(domain, type, proto) socket((domain), (type), (proto))

#define PAL_BIND(s, addr, len) bind((s), (addr), (len))

#define PAL_LISTEN(s, backlog) listen((s), (backlog))

#define PAL_ACCEPT(s, addr, len) accept((s), (addr), (len))

#define PAL_CONNECT(s, addr, len) connect((s), (addr), (len))

#define PAL_SEND(s, buf, len, flags) send((s), (buf), (len), (flags))

#define PAL_RECV(s, buf, len, flags) recv((s), (buf), (len), (flags))

#define PAL_SENDTO(s, buf, len, flags, addr, addrlen)                          \
  sendto((s), (buf), (len), (flags), (addr), (addrlen))

#define PAL_RECVFROM(s, buf, len, flags, addr, addrlen)                        \
  recvfrom((s), (buf), (len), (flags), (addr), (addrlen))

#define PAL_CLOSE(s) close(s)

#define PAL_SETSOCKOPT(s, level, optname, optval, optlen)                      \
  setsockopt((s), (level), (optname), (optval), (optlen))

#define PAL_GETSOCKOPT(s, level, optname, optval, optlen)                      \
  getsockopt((s), (level), (optname), (optval), (optlen))

#define PAL_GETSOCKNAME(s, addr, len) getsockname((s), (addr), (len))

#define PAL_GETPEERNAME(s, addr, len) getpeername((s), (addr), (len))

#define PAL_HTONL(x) htonl(x)
#define PAL_HTONS(x) htons(x)
#define PAL_NTOHL(x) ntohl(x)
#define PAL_NTOHS(x) ntohs(x)

#define PAL_INET_NTOP(af, src, dst, size) inet_ntop((af), (src), (dst), (size))

#define PAL_INET_PTON(af, src, dst) inet_pton((af), (src), (dst))

#elif defined(PLATFORM_PS5)
/**
 * PS5 socket operations (standard BSD via syscalls)
 */
#define PAL_SOCKET(domain, type, proto) socket((domain), (type), (proto))

#define PAL_BIND(s, addr, len) bind((s), (addr), (len))

#define PAL_LISTEN(s, backlog) listen((s), (backlog))

#define PAL_ACCEPT(s, addr, len) accept((s), (addr), (len))

#define PAL_CONNECT(s, addr, len) connect((s), (addr), (len))

#define PAL_SEND(s, buf, len, flags) send((s), (buf), (len), (flags))

#define PAL_RECV(s, buf, len, flags) recv((s), (buf), (len), (flags))

#define PAL_SENDTO(s, buf, len, flags, addr, addrlen)                          \
  sendto((s), (buf), (len), (flags), (addr), (addrlen))

#define PAL_RECVFROM(s, buf, len, flags, addr, addrlen)                        \
  recvfrom((s), (buf), (len), (flags), (addr), (addrlen))

#define PAL_CLOSE(s) close(s)

#define PAL_SETSOCKOPT(s, level, optname, optval, optlen)                      \
  setsockopt((s), (level), (optname), (optval), (optlen))

#define PAL_GETSOCKOPT(s, level, optname, optval, optlen)                      \
  getsockopt((s), (level), (optname), (optval), (optlen))

#define PAL_GETSOCKNAME(s, addr, len) getsockname((s), (addr), (len))

#define PAL_GETPEERNAME(s, addr, len) getpeername((s), (addr), (len))

#define PAL_HTONL(x) htonl(x)
#define PAL_HTONS(x) htons(x)
#define PAL_NTOHL(x) ntohl(x)
#define PAL_NTOHS(x) ntohs(x)

#define PAL_INET_NTOP(af, src, dst, size) inet_ntop((af), (src), (dst), (size))

#define PAL_INET_PTON(af, src, dst) inet_pton((af), (src), (dst))

#else /* POSIX (Linux, FreeBSD, etc.) */
/**
 * Standard POSIX socket operations
 */
#define PAL_SOCKET socket
#define PAL_BIND bind
#define PAL_LISTEN listen
#define PAL_ACCEPT accept
#define PAL_CONNECT connect
#define PAL_SEND send
#define PAL_RECV recv
#define PAL_SENDTO sendto
#define PAL_RECVFROM recvfrom
#define PAL_CLOSE close
#define PAL_SETSOCKOPT setsockopt
#define PAL_GETSOCKOPT getsockopt
#define PAL_GETSOCKNAME getsockname
#define PAL_GETPEERNAME getpeername
#define PAL_HTONL htonl
#define PAL_HTONS htons
#define PAL_NTOHL ntohl
#define PAL_NTOHS ntohs
#define PAL_INET_NTOP inet_ntop
#define PAL_INET_PTON inet_pton
#endif

/*===========================================================================*
 * NETWORK INITIALIZATION
 *===========================================================================*/

/**
 * @brief Initialize platform network subsystem
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @note PS4: Initializes libkernel networking
 * @note PS5: No-op (network always available)
 * @note POSIX: No-op (network always available)
 *
 * @note Thread-safety: Must be called before any network operations
 * @note Call only once during application startup
 */
ftp_error_t pal_network_init(void);

/**
 * @brief Cleanup platform network subsystem
 *
 * @note PS4: Terminates libkernel networking
 * @note PS5/POSIX: No-op
 *
 * @note Thread-safety: Must be called after all network operations complete
 * @note Call only once during application shutdown
 */
void pal_network_fini(void);

/*===========================================================================*
 * SOCKET CONFIGURATION
 *===========================================================================*/

/**
 * @brief Configure socket for optimal FTP performance
 *
 * @param fd Socket file descriptor
 *
 * @return FTP_OK on success, negative error code on failure
 * @retval FTP_OK All options set successfully
 * @retval FTP_ERR_INVALID_PARAM Invalid socket descriptor
 * @retval FTP_ERR_SOCKET_SEND setsockopt() failed
 *
 * @pre fd >= 0
 * @pre fd is a valid socket descriptor
 *
 * @note Sets the following options:
 *       - TCP_NODELAY (disable Nagle's algorithm)
 *       - SO_SNDBUF (send buffer size)
 *       - SO_RCVBUF (receive buffer size)
 *       - SO_KEEPALIVE (enable keepalive)
 *       - TCP_KEEPIDLE, TCP_KEEPINTVL, TCP_KEEPCNT (keepalive params)
 *
 * @note Thread-safety: Safe to call on different sockets concurrently
 */
ftp_error_t pal_socket_configure(socket_t fd);

/**
 * @brief Configure data socket for bulk file transfer
 *
 * Unlike pal_socket_configure() (ctrl channel), this function:
 *   - Enables Nagle (TCP_NODELAY=0) for bulk coalescing
 *   - Sets SO_RCVTIMEO / SO_SNDTIMEO to FTP_DATA_IO_TIMEOUT_MS
 *   - Sets SO_LINGER to flush remaining data on close
 *   - Keeps SO_KEEPALIVE and large SO_SNDBUF / SO_RCVBUF
 *
 * @param fd Socket file descriptor
 * @return FTP_OK on success
 */
ftp_error_t pal_socket_configure_data(socket_t fd);

/**
 * @brief Set socket to non-blocking mode
 *
 * @param fd Socket file descriptor
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre fd >= 0
 */
ftp_error_t pal_socket_set_nonblocking(socket_t fd);

/**
 * @brief Set socket to blocking mode
 *
 * @param fd Socket file descriptor
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre fd >= 0
 */
ftp_error_t pal_socket_set_blocking(socket_t fd);

/**
 * @brief Enable address reuse on socket
 *
 * @param fd Socket file descriptor
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre fd >= 0
 *
 * @note Required to avoid "Address already in use" errors on restart
 */
ftp_error_t pal_socket_set_reuseaddr(socket_t fd);

/**
 * @brief Cork data socket (hold TCP segments for coalescing)
 *
 * Call before a burst of small sends (e.g. sendfile loop).
 * Accumulated data is flushed as large MSS packets on uncork.
 *
 *   Linux:     TCP_CORK
 *   BSD/PS5:   TCP_NOPUSH
 */
void pal_socket_cork(socket_t fd);

/**
 * @brief Uncork data socket (flush accumulated segments)
 */
void pal_socket_uncork(socket_t fd);

ftp_error_t pal_socket_set_timeouts(socket_t fd, uint32_t recv_timeout_ms,
                                    uint32_t send_timeout_ms);

ssize_t pal_send_all(socket_t fd, const void *buffer, size_t length, int flags);

/*===========================================================================*
 * UTILITY FUNCTIONS
 *===========================================================================*/

/**
 * @brief Get IP address from sockaddr structure
 *
 * @param addr   Socket address structure
 * @param buffer Output buffer for IP string
 * @param size   Size of output buffer
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre addr != NULL
 * @pre buffer != NULL
 * @pre size >= INET_ADDRSTRLEN
 *
 * @post buffer contains null-terminated IP address string
 *
 * @note Thread-safety: Safe (no shared state)
 */
ftp_error_t pal_sockaddr_to_ip(const struct sockaddr_in *addr, char *buffer,
                               size_t size);

ftp_error_t pal_network_get_primary_ip(char *buffer, size_t size);

/**
 * @brief Get port number from sockaddr structure
 *
 * @param addr Socket address structure
 *
 * @return Port number in host byte order, or 0 on error
 *
 * @pre addr != NULL
 *
 * @note Thread-safety: Safe (no shared state)
 */
uint16_t pal_sockaddr_get_port(const struct sockaddr_in *addr);

/**
 * @brief Create sockaddr_in structure from IP and port
 *
 * @param ip   IP address string (e.g., "192.168.1.1")
 * @param port Port number (host byte order)
 * @param addr Output sockaddr_in structure
 *
 * @return FTP_OK on success, negative error code on failure
 *
 * @pre ip != NULL
 * @pre addr != NULL
 * @pre port > 0
 *
 * @post addr is initialized with IP and port
 *
 * @note Thread-safety: Safe (no shared state)
 */
ftp_error_t pal_make_sockaddr(const char *ip, uint16_t port,
                              struct sockaddr_in *addr);

#endif /* PAL_NETWORK_H */
