/**
 * @file ftp_server.h
 * @brief FTP server main loop and session management
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * ARCHITECTURE: Single listener thread + thread-per-client sessions
 * CONCURRENCY: Fixed session pool (FTP_MAX_SESSIONS)
 * 
 * SAFETY CLASSIFICATION: Embedded systems, production-grade
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#ifndef FTP_SERVER_H
#define FTP_SERVER_H

#include "ftp_types.h"

/*===========================================================================*
 * SERVER LIFECYCLE
 *===========================================================================*/

/**
 * @brief Initialize FTP server
 * 
 * @param ctx       Server context to initialize
 * @param bind_ip   IP address to bind to ("0.0.0.0" for all interfaces)
 * @param port      Port number to listen on
 * @param root_path Server root directory path
 * 
 * @return FTP_OK on success, negative error code on failure
 * @retval FTP_OK Server initialized successfully
 * @retval FTP_ERR_INVALID_PARAM Invalid parameters
 * @retval FTP_ERR_SOCKET_CREATE Socket creation failed
 * @retval FTP_ERR_SOCKET_BIND Bind failed (port in use?)
 * @retval FTP_ERR_SOCKET_LISTEN Listen failed
 * 
 * @pre ctx != NULL
 * @pre bind_ip != NULL
 * @pre port > 0
 * @pre root_path != NULL
 * 
 * @post ctx->listen_fd >= 0
 * @post ctx->running == 0 (call ftp_server_start to begin)
 * @post All sessions initialized
 */
ftp_error_t ftp_server_init(ftp_server_context_t *ctx,
                              const char *bind_ip,
                              uint16_t port,
                              const char *root_path);

/**
 * @brief Start FTP server (begin accepting connections)
 * 
 * Creates accept thread and begins processing connections.
 * 
 * @param ctx Server context
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre ctx != NULL
 * @pre ctx->listen_fd >= 0
 * 
 * @post ctx->running == 1
 * @post Accept thread is running
 * 
 * @note Non-blocking: returns immediately
 */
ftp_error_t ftp_server_start(ftp_server_context_t *ctx);

/**
 * @brief Stop FTP server (graceful shutdown)
 * 
 * Signals server to stop accepting connections and waits
 * for all active sessions to complete.
 * 
 * @param ctx Server context
 * 
 * @pre ctx != NULL
 * 
 * @post ctx->running == 0
 * @post All sessions terminated
 * 
 * @note Blocking: waits for all threads to exit
 */
void ftp_server_stop(ftp_server_context_t *ctx);

/**
 * @brief Cleanup server resources
 * 
 * @param ctx Server context
 * 
 * @pre ctx != NULL
 * @pre Server stopped (running == 0)
 * 
 * @post All resources freed
 * @post Listen socket closed
 */
void ftp_server_cleanup(ftp_server_context_t *ctx);

/*===========================================================================*
 * SERVER CONTROL
 *===========================================================================*/

/**
 * @brief Check if server is running
 * 
 * @param ctx Server context
 * 
 * @return 1 if running, 0 if not
 * 
 * @pre ctx != NULL
 */
int ftp_server_is_running(const ftp_server_context_t *ctx);

/**
 * @brief Get number of active sessions
 * 
 * @param ctx Server context
 * 
 * @return Number of active sessions
 * 
 * @pre ctx != NULL
 */
uint32_t ftp_server_get_active_sessions(const ftp_server_context_t *ctx);

/**
 * @brief Get server statistics
 * 
 * @param ctx       Server context
 * @param total_conn     Output: Total connections
 * @param bytes_sent     Output: Total bytes sent
 * @param bytes_received Output: Total bytes received
 * 
 * @pre ctx != NULL
 * @pre At least one output parameter != NULL
 */
void ftp_server_get_stats(const ftp_server_context_t *ctx,
                            uint64_t *total_conn,
                            uint64_t *bytes_sent,
                            uint64_t *bytes_received);

#endif /* FTP_SERVER_H */
