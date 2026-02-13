/**
 * @file pal_network.c
 * @brief Platform Abstraction Layer - Network Implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * SAFETY CLASSIFICATION: Embedded systems, production-grade
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#include "pal_network.h"
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

/*===========================================================================*
 * NETWORK INITIALIZATION
 *===========================================================================*/

/**
 * @brief Initialize network subsystem
 */
ftp_error_t pal_network_init(void)
{
#if defined(PLATFORM_PS3)
    static atomic_int initialized = ATOMIC_VAR_INIT(0);
    
    if (atomic_load(&initialized) != 0) {
        return FTP_OK;
    }
    
    /* Initialize PS3 network */
    int ret = netInitialize();
    if (ret < 0) {
        return FTP_ERR_SOCKET_CREATE;
    }
    
    atomic_store(&initialized, 1);
    return FTP_OK;
    
#else
    /* POSIX: Network always available */
    return FTP_OK;
#endif
}

/**
 * @brief Cleanup network subsystem
 */
void pal_network_fini(void)
{
#if defined(PLATFORM_PS3)
    netFinalize();
#else
    /* POSIX: No cleanup needed */
#endif
}

/*===========================================================================*
 * SOCKET CONFIGURATION
 *===========================================================================*/

/**
 * @brief Configure socket for optimal performance
 */
ftp_error_t pal_socket_configure(socket_t fd)
{
    int ret;
    
    /* Validate socket descriptor */
    if (fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
#if FTP_TCP_NODELAY
    /* Disable Nagle's algorithm (reduce latency) */
    {
        int nodelay = 1;
        ret = PAL_SETSOCKOPT(fd, IPPROTO_TCP, TCP_NODELAY,
                             &nodelay, sizeof(nodelay));
        if (ret < 0) {
            /* Non-fatal: continue with other options */
        }
    }
#endif
    
    /* Set send buffer size */
    {
        int sndbuf = (int)FTP_TCP_SNDBUF;
        ret = PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_SNDBUF,
                             &sndbuf, sizeof(sndbuf));
        if (ret < 0) {
            /* Non-fatal */
        }
    }
    
    /* Set receive buffer size */
    {
        int rcvbuf = (int)FTP_TCP_RCVBUF;
        ret = PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF,
                             &rcvbuf, sizeof(rcvbuf));
        if (ret < 0) {
            /* Non-fatal */
        }
    }
    
#if FTP_TCP_KEEPALIVE
    /* Enable TCP keepalive */
    {
        int keepalive = 1;
        ret = PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_KEEPALIVE,
                             &keepalive, sizeof(keepalive));
        if (ret < 0) {
            /* Non-fatal */
        }
    }
    
    /* Set keepalive parameters (Linux/FreeBSD specific) */
#if defined(__linux__) || defined(__FreeBSD__) || defined(PLATFORM_PS5)
    {
        int idle = (int)FTP_TCP_KEEPIDLE;
        int intvl = (int)FTP_TCP_KEEPINTVL;
        int cnt = (int)FTP_TCP_KEEPCNT;
        
        (void)PAL_SETSOCKOPT(fd, IPPROTO_TCP, TCP_KEEPIDLE,
                             &idle, sizeof(idle));
        (void)PAL_SETSOCKOPT(fd, IPPROTO_TCP, TCP_KEEPINTVL,
                             &intvl, sizeof(intvl));
        (void)PAL_SETSOCKOPT(fd, IPPROTO_TCP, TCP_KEEPCNT,
                             &cnt, sizeof(cnt));
    }
#endif
#endif /* FTP_TCP_KEEPALIVE */
    
    return FTP_OK;
}

/**
 * @brief Set socket to non-blocking mode
 */
ftp_error_t pal_socket_set_nonblocking(socket_t fd)
{
    if (fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* POSIX: Use fcntl */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/**
 * @brief Set socket to blocking mode
 */
ftp_error_t pal_socket_set_blocking(socket_t fd)
{
    if (fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* POSIX: Use fcntl */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    if (fcntl(fd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/**
 * @brief Enable address reuse
 */
ftp_error_t pal_socket_set_reuseaddr(socket_t fd)
{
    if (fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    int optval = 1;
    int ret = PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_REUSEADDR,
                             &optval, sizeof(optval));
    if (ret < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/*===========================================================================*
 * UTILITY FUNCTIONS
 *===========================================================================*/

/**
 * @brief Extract IP address from sockaddr
 */
ftp_error_t pal_sockaddr_to_ip(const struct sockaddr_in *addr,
                                char *buffer,
                                size_t size)
{
    /* Validate parameters */
    if ((addr == NULL) || (buffer == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (size < INET_ADDRSTRLEN) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Convert IP to string */
    const char *result = PAL_INET_NTOP(AF_INET, &addr->sin_addr,
                                        buffer, (socklen_t)size);
    if (result == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    return FTP_OK;
}

/**
 * @brief Extract port from sockaddr
 */
uint16_t pal_sockaddr_get_port(const struct sockaddr_in *addr)
{
    if (addr == NULL) {
        return 0U;
    }
    
    return PAL_NTOHS(addr->sin_port);
}

/**
 * @brief Create sockaddr from IP and port
 */
ftp_error_t pal_make_sockaddr(const char *ip,
                               uint16_t port,
                               struct sockaddr_in *addr)
{
    /* Validate parameters */
    if ((ip == NULL) || (addr == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (port == 0U) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Zero-initialize structure */
    memset(addr, 0, sizeof(*addr));
    
    /* Set address family */
    addr->sin_family = AF_INET;
    
    /* Convert IP string to binary */
    int ret = PAL_INET_PTON(AF_INET, ip, &addr->sin_addr);
    if (ret != 1) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Set port (convert to network byte order) */
    addr->sin_port = PAL_HTONS(port);
    
    return FTP_OK;
}

ftp_error_t pal_network_get_primary_ip(char *buffer, size_t size)
{
    if (buffer == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    if (size < INET_ADDRSTRLEN) {
        return FTP_ERR_INVALID_PARAM;
    }

    int fd = PAL_SOCKET(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) {
        return FTP_ERR_SOCKET_CREATE;
    }

    struct sockaddr_in dst;
    memset(&dst, 0, sizeof(dst));
    dst.sin_family = AF_INET;
    dst.sin_port = PAL_HTONS(53);
    (void)PAL_INET_PTON(AF_INET, "8.8.8.8", &dst.sin_addr);

    if (PAL_CONNECT(fd, (struct sockaddr *)&dst, sizeof(dst)) < 0) {
        PAL_CLOSE(fd);
        return FTP_ERR_SOCKET_CREATE;
    }

    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));

    if (getsockname(fd, (struct sockaddr *)&local, &local_len) < 0) {
        PAL_CLOSE(fd);
        return FTP_ERR_INVALID_PARAM;
    }

    PAL_CLOSE(fd);
    return pal_sockaddr_to_ip(&local, buffer, size);
}
