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
 * @file ftp_server.c
 * @brief FTP server main loop and session management implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 */

#include "ftp_server.h"
#include "ftp_session.h"
#include "pal_network.h"
#include <string.h>
#include <unistd.h>
#include <signal.h>

/* Forward declarations for internal functions */
static void* server_accept_thread(void *arg);
static ftp_session_t* allocate_session(ftp_server_context_t *ctx);
static void free_session(ftp_server_context_t *ctx, ftp_session_t *session);

/*===========================================================================*
 * SERVER LIFECYCLE
 *===========================================================================*/

/**
 * @brief Initialize FTP server
 */
ftp_error_t ftp_server_init(ftp_server_context_t *ctx,
                              const char *bind_ip,
                              uint16_t port,
                              const char *root_path)
{
    /* Validate parameters */
    if ((ctx == NULL) || (bind_ip == NULL) || (root_path == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (port == 0U) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Zero-initialize context */
    memset(ctx, 0, sizeof(*ctx));
    
    /* Initialize network subsystem */
    ftp_error_t err = pal_network_init();
    if (err != FTP_OK) {
        return err;
    }
    
    /* Create listen socket */
    int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return FTP_ERR_SOCKET_CREATE;
    }
    
    /* Enable address reuse */
    err = pal_socket_set_reuseaddr(fd);
    if (err != FTP_OK) {
        PAL_CLOSE(fd);
        return err;
    }
    
    /* Build bind address */
    err = pal_make_sockaddr(bind_ip, port, &ctx->listen_addr);
    if (err != FTP_OK) {
        PAL_CLOSE(fd);
        return err;
    }
    
    /* Bind socket */
    if (PAL_BIND(fd, (struct sockaddr*)&ctx->listen_addr,
                 sizeof(ctx->listen_addr)) < 0) {
        PAL_CLOSE(fd);
        return FTP_ERR_SOCKET_BIND;
    }
    
    /* Listen for connections */
    if (PAL_LISTEN(fd, FTP_LISTEN_BACKLOG) < 0) {
        PAL_CLOSE(fd);
        return FTP_ERR_SOCKET_LISTEN;
    }
    
    ctx->listen_fd = fd;
    ctx->port = port;
    
    /* Store root path */
    size_t root_len = strlen(root_path);
    if (root_len >= sizeof(ctx->root_path)) {
        PAL_CLOSE(fd);
        return FTP_ERR_PATH_TOO_LONG;
    }
    memcpy(ctx->root_path, root_path, root_len + 1U);
    
    /* Initialize server state */
    atomic_store(&ctx->running, 0);
    atomic_store(&ctx->active_sessions, 0U);
    
    /* Initialize session pool */
    for (size_t i = 0U; i < FTP_MAX_SESSIONS; i++) {
        memset(&ctx->sessions[i], 0, sizeof(ctx->sessions[i]));
        ctx->sessions[i].ctrl_fd = -1;
        ctx->sessions[i].data_fd = -1;
        ctx->sessions[i].pasv_fd = -1;
    }
    
    /* Initialize session lock */
    if (pthread_mutex_init(&ctx->session_lock, NULL) != 0) {
        PAL_CLOSE(fd);
        return FTP_ERR_THREAD_CREATE;
    }
    
    /* Initialize statistics */
    atomic_store(&ctx->stats.total_connections, 0U);
    atomic_store(&ctx->stats.total_bytes_sent, 0U);
    atomic_store(&ctx->stats.total_bytes_received, 0U);
    atomic_store(&ctx->stats.total_errors, 0U);
    
    return FTP_OK;
}

/**
 * @brief Start FTP server
 */
ftp_error_t ftp_server_start(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (ctx->listen_fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Set running flag */
    atomic_store(&ctx->running, 1);
    
    /* Create accept thread */
    pthread_t accept_thread;
    pthread_attr_t attr;
    int attr_ok = (pthread_attr_init(&attr) == 0);
    if (attr_ok != 0) {
        (void)pthread_attr_setstacksize(&attr, (size_t)FTP_THREAD_STACK_SIZE);
    }
    
    if (pthread_create(&accept_thread, (attr_ok != 0) ? &attr : NULL, server_accept_thread, ctx) != 0) {
        if (attr_ok != 0) {
            (void)pthread_attr_destroy(&attr);
        }
        atomic_store(&ctx->running, 0);
        return FTP_ERR_THREAD_CREATE;
    }
    
    if (attr_ok != 0) {
        (void)pthread_attr_destroy(&attr);
    }
    
    /* Detach thread (we don't need to join it) */
    pthread_detach(accept_thread);
    
    return FTP_OK;
}

/**
 * @brief Stop FTP server
 */
void ftp_server_stop(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    
    /* Signal server to stop */
    atomic_store(&ctx->running, 0);
    
    /* Wait for all sessions to complete */
    while (atomic_load(&ctx->active_sessions) > 0U) {
        usleep(100000); /* 100ms */
    }
}

/**
 * @brief Cleanup server resources
 */
void ftp_server_cleanup(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    
    /* Close listen socket */
    if (ctx->listen_fd >= 0) {
        PAL_CLOSE(ctx->listen_fd);
        ctx->listen_fd = -1;
    }
    
    /* Destroy session lock */
    pthread_mutex_destroy(&ctx->session_lock);
    
    /* Cleanup network subsystem */
    pal_network_fini();
}

/*===========================================================================*
 * ACCEPT THREAD
 *===========================================================================*/

/**
 * @brief Accept thread - handles incoming connections
 */
static void* server_accept_thread(void *arg)
{
    ftp_server_context_t *ctx = (ftp_server_context_t*)arg;
    
    if (ctx == NULL) {
        return NULL;
    }
    
    while (atomic_load(&ctx->running) != 0) {
        /* Accept new connection */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int client_fd = PAL_ACCEPT(ctx->listen_fd,
                                    (struct sockaddr*)&client_addr,
                                    &addr_len);
        
        if (client_fd < 0) {
            /* Accept failed - check if server is stopping */
            if (atomic_load(&ctx->running) == 0) {
                break;
            }
            
            continue; /* Try again */
        }
        
        /* Configure client socket */
        (void)pal_socket_configure(client_fd);
        
        /* Allocate session */
        ftp_session_t *session = allocate_session(ctx);
        
        if (session == NULL) {
            /* No available sessions - reject connection */
            PAL_CLOSE(client_fd);
            atomic_fetch_add(&ctx->stats.total_errors, 1U);
            continue;
        }
        
        /* Initialize session */
        static atomic_uint_fast32_t session_counter = ATOMIC_VAR_INIT(0);
        uint32_t session_id = atomic_fetch_add(&session_counter, 1U);
        
        ftp_error_t err = ftp_session_init(session, client_fd, &client_addr,
                                            session_id, ctx->root_path);
        
        if (err != FTP_OK) {
            PAL_CLOSE(client_fd);
            free_session(ctx, session);
            atomic_fetch_add(&ctx->stats.total_errors, 1U);
            continue;
        }
        
        /* Create session thread */
        pthread_attr_t sess_attr;
        int sess_attr_ok = (pthread_attr_init(&sess_attr) == 0);
        if (sess_attr_ok != 0) {
            (void)pthread_attr_setstacksize(&sess_attr, (size_t)FTP_THREAD_STACK_SIZE);
        }
        
        if (pthread_create(&session->thread, (sess_attr_ok != 0) ? &sess_attr : NULL,
                           ftp_session_thread, session) != 0) {
            if (sess_attr_ok != 0) {
                (void)pthread_attr_destroy(&sess_attr);
            }
            PAL_CLOSE(client_fd);
            free_session(ctx, session);
            atomic_fetch_add(&ctx->stats.total_errors, 1U);
            continue;
        }
        
        if (sess_attr_ok != 0) {
            (void)pthread_attr_destroy(&sess_attr);
        }
        
        /* Detach thread */
        pthread_detach(session->thread);
        
        /* Update statistics */
        atomic_fetch_add(&ctx->stats.total_connections, 1U);
        atomic_fetch_add(&ctx->active_sessions, 1U);
    }
    
    return NULL;
}

/*===========================================================================*
 * SESSION MANAGEMENT
 *===========================================================================*/

/**
 * @brief Allocate session from pool
 */
static ftp_session_t* allocate_session(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return NULL;
    }
    
    pthread_mutex_lock(&ctx->session_lock);
    
    /* Find free session slot */
    ftp_session_t *session = NULL;
    
    for (size_t i = 0U; i < FTP_MAX_SESSIONS; i++) {
        int state = atomic_load(&ctx->sessions[i].state);
        
        if ((state == FTP_STATE_INIT) || (state == FTP_STATE_TERMINATING)) {
            /* Available slot */
            session = &ctx->sessions[i];
            atomic_store(&session->state, FTP_STATE_CONNECTED);
            break;
        }
    }
    
    pthread_mutex_unlock(&ctx->session_lock);
    
    return session;
}

/**
 * @brief Free session back to pool
 */
static void free_session(ftp_server_context_t *ctx, ftp_session_t *session)
{
    if ((ctx == NULL) || (session == NULL)) {
        return;
    }
    
    pthread_mutex_lock(&ctx->session_lock);
    
    /* Mark session as free */
    atomic_store(&session->state, FTP_STATE_INIT);
    
    /* Update active count */
    atomic_fetch_sub(&ctx->active_sessions, 1U);
    
    pthread_mutex_unlock(&ctx->session_lock);
}

/*===========================================================================*
 * SERVER CONTROL
 *===========================================================================*/

/**
 * @brief Check if server is running
 */
int ftp_server_is_running(const ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return 0;
    }
    
    return atomic_load(&ctx->running);
}

/**
 * @brief Get active session count
 */
uint32_t ftp_server_get_active_sessions(const ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return 0U;
    }
    
    return (uint32_t)atomic_load(&ctx->active_sessions);
}

/**
 * @brief Get server statistics
 */
void ftp_server_get_stats(const ftp_server_context_t *ctx,
                            uint64_t *total_conn,
                            uint64_t *bytes_sent,
                            uint64_t *bytes_received)
{
    if (ctx == NULL) {
        return;
    }
    
    if (total_conn != NULL) {
        *total_conn = atomic_load(&ctx->stats.total_connections);
    }
    
    if (bytes_sent != NULL) {
        /* Sum all session statistics */
        uint64_t total = 0U;
        for (size_t i = 0U; i < FTP_MAX_SESSIONS; i++) {
            total += atomic_load(&ctx->sessions[i].stats.bytes_sent);
        }
        *bytes_sent = total;
    }
    
    if (bytes_received != NULL) {
        /* Sum all session statistics */
        uint64_t total = 0U;
        for (size_t i = 0U; i < FTP_MAX_SESSIONS; i++) {
            total += atomic_load(&ctx->sessions[i].stats.bytes_received);
        }
        *bytes_received = total;
    }
}
