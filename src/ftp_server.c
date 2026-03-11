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
 * @brief Stop FTP server (graceful shutdown with bounded wait)
 *
 * SHUTDOWN SEQUENCE (order matters):
 *
 *  1. Clear the running flag so the accept thread exits its loop check.
 *
 *  2. Close listen_fd immediately.
 *     WHY: PAL_ACCEPT() is a blocking syscall.  The accept thread only checks
 *     ctx->running AFTER accept() returns.  Without closing the socket, the
 *     thread blocks forever — even though running == 0.  Closing the fd
 *     makes accept() return EBADF/EINVAL immediately, allowing the thread to
 *     observe running == 0 and exit.
 *
 *  3. Force shutdown(SHUT_RDWR) on every active session's ctrl_fd.
 *     WHY: Session threads block inside recv() waiting for the next FTP
 *     command.  They only check for server shutdown when recv() returns.
 *     shutdown() injects an EOF into the socket without closing the fd,
 *     making recv() return 0 so the session loop exits cleanly.
 *     (close() would race with the session thread which also closes ctrl_fd
 *     in ftp_session_cleanup().)
 *
 *  4. Wait up to SERVER_STOP_TIMEOUT_MS for active_sessions to reach 0.
 *     WHY: Each session thread calls ftp_server_release_session() at exit,
 *     which decrements active_sessions.  The timeout is a safety net: if a
 *     session thread is stuck (e.g. blocked in a long PFS write), we do not
 *     hang the entire PS5 shutdown sequence — the kernel will collect the
 *     remaining resources via SIGKILL/forced unmount anyway, but an orderly
 *     decrement is strongly preferred.
 */
void ftp_server_stop(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    /* Step 1 — signal stop */
    atomic_store(&ctx->running, 0);

    /* Step 2 — unblock PAL_ACCEPT() in the accept thread */
    if (ctx->listen_fd >= 0) {
        PAL_CLOSE(ctx->listen_fd);
        ctx->listen_fd = -1; /* ftp_server_cleanup() guards against double-close */
    }

    /* Step 3 — interrupt blocking recv() in each session thread */
    pthread_mutex_lock(&ctx->session_lock);
    for (size_t i = 0U; i < FTP_MAX_SESSIONS; i++) {
        int state = atomic_load(&ctx->sessions[i].state);
        if ((state == FTP_STATE_CONNECTED)    ||
            (state == FTP_STATE_AUTHENTICATED)||
            (state == FTP_STATE_TRANSFERRING)) {
            int cfd = ctx->sessions[i].ctrl_fd;
            if (cfd >= 0) {
                /*
                 * shutdown() injects EOF without closing the fd.
                 * The session thread's recv() returns 0 → loop exits →
                 * ftp_session_cleanup() closes the fd normally.
                 * Using close() here would race with ftp_session_cleanup().
                 */
                (void)shutdown(cfd, SHUT_RDWR);
            }
        }
    }
    pthread_mutex_unlock(&ctx->session_lock);

    /* Step 4 — wait for sessions to drain, with a hard timeout */
#define SERVER_STOP_TIMEOUT_MS 5000U
#define SERVER_STOP_POLL_MS    100U
    uint32_t waited_ms = 0U;
    while ((atomic_load(&ctx->active_sessions) > 0U) &&
           (waited_ms < SERVER_STOP_TIMEOUT_MS)) {
        usleep(SERVER_STOP_POLL_MS * 1000U);
        waited_ms += SERVER_STOP_POLL_MS;
    }

#undef SERVER_STOP_TIMEOUT_MS
#undef SERVER_STOP_POLL_MS
}

/**
 * @brief Cleanup server resources
 *
 * @note listen_fd may already be closed by ftp_server_stop() — guard
 *       against double-close with the >= 0 check.
 */
void ftp_server_cleanup(ftp_server_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    
    /* Close listen socket if not already closed by ftp_server_stop() */
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

        /*
         * Store the back-pointer to the owning server context.
         *
         * WHY: The session thread calls ftp_server_release_session() on exit
         * to decrement active_sessions and mark the slot as free.  Without
         * this pointer the thread cannot reach the server context.
         *
         * Written here — before pthread_create() — so it is visible to the
         * new thread without any additional synchronisation (pthread_create
         * acts as a memory barrier).
         */
        session->server_ctx = ctx;
        
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
 * SESSION POOL MANAGEMENT
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
 * @brief Free session back to pool (internal error-path helper).
 *
 * Used ONLY in server_accept_thread for early error paths — BEFORE
 * pthread_create() succeeds and BEFORE active_sessions is incremented.
 *
 * WHY NO DECREMENT:
 *   active_sessions is incremented at line:
 *       atomic_fetch_add(&ctx->active_sessions, 1U);
 *   which occurs AFTER pthread_create() returns 0.  All calls to
 *   free_session() happen in error branches that execute BEFORE that
 *   line (ftp_session_init failure, pthread_create failure).
 *   Decrementing here would underflow the counter from 0 to UINT_FAST32_MAX,
 *   which would permanently prevent ftp_server_stop() from exiting even
 *   after all real sessions have finished.
 *
 *   The only correct place to decrement active_sessions is
 *   ftp_server_release_session(), which is called from inside
 *   ftp_session_thread() — i.e. after the thread (and the increment) exist.
 *
 * @note Thread-safety: Protected by ctx->session_lock
 */
static void free_session(ftp_server_context_t *ctx, ftp_session_t *session)
{
    if ((ctx == NULL) || (session == NULL)) {
        return;
    }
    
    pthread_mutex_lock(&ctx->session_lock);
    
    /* Reset slot state so it can be reused — no counter decrement here */
    atomic_store(&session->state, FTP_STATE_INIT);
    
    pthread_mutex_unlock(&ctx->session_lock);
}

/**
 * @brief Release a session slot back to the server pool (public API).
 *
 * Called by ftp_session_thread() as its very last action, after
 * ftp_session_cleanup() has closed all file descriptors.
 *
 * INVARIANT: active_sessions was already incremented (in server_accept_thread,
 * after pthread_create() returned 0) before this thread started running.
 * This function performs the matching decrement.  free_session() (the internal
 * error-path helper) deliberately does NOT decrement — it is only called from
 * code paths that execute BEFORE the increment.
 *
 * @pre Called only from the session's own thread
 * @pre ftp_session_cleanup() already called (all FDs closed)
 * @pre ctx->active_sessions > 0
 */
void ftp_server_release_session(ftp_server_context_t *ctx,
                                ftp_session_t *session)
{
    if ((ctx == NULL) || (session == NULL)) {
        return;
    }

    /*
     * NULL out the back-pointer before releasing the slot.
     * Prevents stale pointer access if the slot is immediately reused.
     */
    session->server_ctx = NULL;

    /* Reset slot state and decrement the active-session counter */
    pthread_mutex_lock(&ctx->session_lock);
    atomic_store(&session->state, FTP_STATE_INIT);
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
