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
 * @file ftp_session.h
 * @brief FTP session management and lifecycle
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 * DESIGN: Thread-per-client model with session pool
 * THREADING: Each session runs in dedicated thread
 * 
 */

#ifndef FTP_SESSION_H
#define FTP_SESSION_H

#include "ftp_types.h"

/*===========================================================================*
 * SESSION LIFECYCLE
 *===========================================================================*/

/**
 * @brief Initialize session structure
 * 
 * @param session     Session to initialize
 * @param ctrl_fd     Control socket descriptor
 * @param client_addr Client address information
 * @param session_id  Unique session identifier
 * @param root_path   Server root directory
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre ctrl_fd >= 0
 * @pre client_addr != NULL
 * @pre root_path != NULL
 * 
 * @post session->state == FTP_STATE_CONNECTED
 * @post session->ctrl_fd == ctrl_fd
 * @post All other file descriptors == -1
 */
ftp_error_t ftp_session_init(ftp_session_t *session,
                               int ctrl_fd,
                               const struct sockaddr_in *client_addr,
                               uint32_t session_id,
                               const char *root_path);

/**
 * @brief Cleanup session resources
 * 
 * @param session Session to cleanup
 * 
 * @pre session != NULL
 * 
 * @post All file descriptors closed
 * @post Session state set to FTP_STATE_TERMINATING
 * 
 * @note Thread-safety: Call only from session thread
 */
void ftp_session_cleanup(ftp_session_t *session);

/**
 * @brief Session thread entry point
 * 
 * WORKFLOW:
 * 1. Send greeting (220)
 * 2. Command processing loop
 * 3. Cleanup and exit
 * 
 * @param arg Session pointer (ftp_session_t*)
 * 
 * @return NULL
 * 
 * @pre arg != NULL
 * @pre arg points to valid ftp_session_t
 * 
 * @note This function runs in a separate thread
 */
void* ftp_session_thread(void *arg);

/*===========================================================================*
 * REPLY SENDING
 *===========================================================================*/

/**
 * @brief Send FTP reply to client
 * 
 * @param session Client session
 * @param code    FTP reply code
 * @param message Reply message (NULL for default)
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre session->ctrl_fd >= 0
 * 
 * @note Thread-safety: Safe (session not shared)
 * @note Blocks until entire reply sent
 */
ftp_error_t ftp_session_send_reply(ftp_session_t *session,
                                     ftp_reply_code_t code,
                                     const char *message);

/**
 * @brief Send multi-line reply to client
 * 
 * Format:
 *   123-First line
 *   Second line
 *   123 Last line
 * 
 * @param session Client session
 * @param code    FTP reply code
 * @param lines   Array of message lines
 * @param count   Number of lines
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre lines != NULL
 * @pre count > 0
 */
ftp_error_t ftp_session_send_multiline_reply(ftp_session_t *session,
                                               ftp_reply_code_t code,
                                               const char **lines,
                                               size_t count);

/*===========================================================================*
 * DATA CONNECTION MANAGEMENT
 *===========================================================================*/

/**
 * @brief Open data connection
 * 
 * Behavior depends on data_mode:
 * - FTP_DATA_MODE_ACTIVE: Connect to client
 * - FTP_DATA_MODE_PASSIVE: Accept from client
 * 
 * @param session Client session
 * 
 * @return FTP_OK on success, negative error code on failure
 * 
 * @pre session != NULL
 * @pre session->data_mode != FTP_DATA_MODE_NONE
 * 
 * @post On success: session->data_fd >= 0
 */
ftp_error_t ftp_session_open_data_connection(ftp_session_t *session);

/**
 * @brief Close data connection
 * 
 * @param session Client session
 * 
 * @pre session != NULL
 * 
 * @post session->data_fd == -1
 * @post session->pasv_fd == -1 (if passive mode)
 * @post session->data_mode == FTP_DATA_MODE_NONE
 */
void ftp_session_close_data_connection(ftp_session_t *session);

/**
 * @brief Send data via data connection
 * 
 * @param session Client session
 * @param buffer  Data to send
 * @param length  Number of bytes to send
 * 
 * @return Number of bytes sent, or negative error code
 * 
 * @pre session != NULL
 * @pre session->data_fd >= 0
 * @pre buffer != NULL
 * @pre length > 0
 * 
 * @note May send less than requested (non-blocking)
 */
ssize_t ftp_session_send_data(ftp_session_t *session,
                                const void *buffer,
                                size_t length);

/**
 * @brief Receive data via data connection
 * 
 * @param session Client session
 * @param buffer  Output buffer
 * @param length  Buffer size
 * 
 * @return Number of bytes received, or negative error code
 * 
 * @pre session != NULL
 * @pre session->data_fd >= 0
 * @pre buffer != NULL
 * @pre length > 0
 * 
 * @note May receive less than requested
 */
ssize_t ftp_session_recv_data(ftp_session_t *session,
                                void *buffer,
                                size_t length);

/*===========================================================================*
 * COMMAND PROCESSING
 *===========================================================================*/

/**
 * @brief Read command line from control connection
 * 
 * Reads until CRLF sequence (\r\n).
 * 
 * @param session Client session
 * @param buffer  Output buffer for command line
 * @param size    Size of output buffer
 * 
 * @return Number of bytes read, or negative error code
 * @retval >0 Command line read successfully
 * @retval 0  Connection closed by client
 * @retval <0 Error code
 * 
 * @pre session != NULL
 * @pre session->ctrl_fd >= 0
 * @pre buffer != NULL
 * @pre size >= FTP_CMD_BUFFER_SIZE
 * 
 * @post buffer contains null-terminated command line (CRLF removed)
 * 
 * @note Blocks until complete line received
 */
ssize_t ftp_session_read_command(ftp_session_t *session,
                                   char *buffer,
                                   size_t size);

/**
 * @brief Process FTP command
 * 
 * WORKFLOW:
 * 1. Parse command line
 * 2. Find command handler
 * 3. Validate arguments
 * 4. Execute command
 * 
 * @param session Client session
 * @param line    Command line
 * 
 * @return FTP_OK to continue, 1 to close session, negative on error
 * 
 * @pre session != NULL
 * @pre line != NULL
 */
int ftp_session_process_command(ftp_session_t *session, const char *line);

#endif /* FTP_SESSION_H */
