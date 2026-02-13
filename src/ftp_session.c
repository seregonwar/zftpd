/**
 * @file ftp_session.c
 * @brief FTP session management implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2025-02-13
 * 
 * SAFETY CLASSIFICATION: Embedded systems, production-grade
 * STANDARDS: MISRA C:2012, CERT C, ISO C11
 */

#include "ftp_session.h"
#include "ftp_protocol.h"
#include "ftp_path.h"
#include "pal_network.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>

/*===========================================================================*
 * SESSION LIFECYCLE
 *===========================================================================*/

/**
 * @brief Initialize session structure
 */
ftp_error_t ftp_session_init(ftp_session_t *session,
                               int ctrl_fd,
                               const struct sockaddr_in *client_addr,
                               uint32_t session_id,
                               const char *root_path)
{
    /* Validate parameters */
    if ((session == NULL) || (client_addr == NULL) || (root_path == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (ctrl_fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Zero-initialize structure */
    memset(session, 0, sizeof(*session));
    
    /* Control connection */
    session->ctrl_fd = ctrl_fd;
    memcpy(&session->ctrl_addr, client_addr, sizeof(session->ctrl_addr));
    
    /* Data connection (initially closed) */
    session->data_fd = -1;
    session->pasv_fd = -1;
    session->data_mode = FTP_DATA_MODE_NONE;
    
    /* Session state */
    atomic_store(&session->state, FTP_STATE_CONNECTED);
    
    /* Transfer parameters (defaults) */
    session->transfer_type = FTP_TYPE_BINARY;
    session->transfer_mode = FTP_MODE_STREAM;
    session->file_structure = FTP_STRU_FILE;
    session->restart_offset = 0;
    
    /* File system state (start at root) */
    size_t root_len = strlen(root_path);
    if (root_len >= sizeof(session->cwd)) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    memcpy(session->cwd, root_path, root_len + 1U);
    
    session->rename_from[0] = '\0';
    
    /* Authentication */
    session->auth_attempts = 0U;
    session->authenticated = 0U;
    
    /* Session identification */
    session->session_id = session_id;
    
    /* Timing */
    session->connect_time = time(NULL);
    session->last_activity = session->connect_time;
    
    /* Client IP address (text) */
    ftp_error_t err = pal_sockaddr_to_ip(client_addr, 
                                          session->client_ip,
                                          sizeof(session->client_ip));
    if (err != FTP_OK) {
        /* Non-fatal: use placeholder */
        strncpy(session->client_ip, "unknown", sizeof(session->client_ip) - 1U);
        session->client_ip[sizeof(session->client_ip) - 1U] = '\0';
    }
    
    session->client_port = pal_sockaddr_get_port(client_addr);
    
    /* Initialize statistics */
    atomic_store(&session->stats.bytes_sent, 0U);
    atomic_store(&session->stats.bytes_received, 0U);
    atomic_store(&session->stats.files_sent, 0U);
    atomic_store(&session->stats.files_received, 0U);
    atomic_store(&session->stats.commands_processed, 0U);
    atomic_store(&session->stats.errors, 0U);
    
    return FTP_OK;
}

/**
 * @brief Cleanup session resources
 */
void ftp_session_cleanup(ftp_session_t *session)
{
    if (session == NULL) {
        return;
    }
    
    /* Set state to terminating */
    atomic_store(&session->state, FTP_STATE_TERMINATING);
    
    /* Close data connection */
    ftp_session_close_data_connection(session);
    
    /* Close control connection */
    if (session->ctrl_fd >= 0) {
        PAL_CLOSE(session->ctrl_fd);
        session->ctrl_fd = -1;
    }
}

/**
 * @brief Session thread entry point
 */
void* ftp_session_thread(void *arg)
{
    ftp_session_t *session = (ftp_session_t*)arg;
    
    if (session == NULL) {
        return NULL;
    }
    
    /* Send greeting */
    ftp_session_send_reply(session, FTP_REPLY_220_SERVICE_READY, NULL);
    
    /* Command processing loop */
    char cmd_buffer[FTP_CMD_BUFFER_SIZE];
    int should_quit = 0;
    
    while (!should_quit) {
        /* Read command line */
        ssize_t n = ftp_session_read_command(session, 
                                               cmd_buffer,
                                               sizeof(cmd_buffer));
        
        if (n <= 0) {
            /* Connection closed or error */
            break;
        }
        
        /* Update activity timestamp */
        session->last_activity = time(NULL);
        
        /* Process command */
        int result = ftp_session_process_command(session, cmd_buffer);
        
        if (result == 1) {
            /* QUIT command: graceful exit */
            should_quit = 1;
        } else if (result < 0) {
            /* Error: terminate session */
            break;
        }
        
        /* Increment command counter */
        atomic_fetch_add(&session->stats.commands_processed, 1U);
    }
    
    /* Cleanup and exit */
    ftp_session_cleanup(session);
    
    return NULL;
}

/*===========================================================================*
 * REPLY SENDING
 *===========================================================================*/

/**
 * @brief Send FTP reply to client
 */
ftp_error_t ftp_session_send_reply(ftp_session_t *session,
                                     ftp_reply_code_t code,
                                     const char *message)
{
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->ctrl_fd < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    /* Format reply */
    char buffer[FTP_REPLY_BUFFER_SIZE];
    ssize_t len = ftp_format_reply(code, message, buffer, sizeof(buffer));
    
    if (len < 0) {
        return (ftp_error_t)len;
    }
    
    /* Send reply */
    ssize_t sent = PAL_SEND(session->ctrl_fd, buffer, (size_t)len, 0);
    
    if (sent != len) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/**
 * @brief Send multi-line reply
 */
ftp_error_t ftp_session_send_multiline_reply(ftp_session_t *session,
                                               ftp_reply_code_t code,
                                               const char **lines,
                                               size_t count)
{
    if ((session == NULL) || (lines == NULL) || (count == 0U)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->ctrl_fd < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    char buffer[FTP_REPLY_BUFFER_SIZE];
    
    /* Send all lines except last with '-' */
    for (size_t i = 0U; i < (count - 1U); i++) {
        int n = snprintf(buffer, sizeof(buffer), "%d-%s\r\n",
                         (int)code, lines[i]);
        
        if ((n < 0) || ((size_t)n >= sizeof(buffer))) {
            return FTP_ERR_INVALID_PARAM;
        }
        
        ssize_t sent = PAL_SEND(session->ctrl_fd, buffer, (size_t)n, 0);
        if (sent != n) {
            return FTP_ERR_SOCKET_SEND;
        }
    }
    
    /* Send last line with ' ' */
    int n = snprintf(buffer, sizeof(buffer), "%d %s\r\n",
                     (int)code, lines[count - 1U]);
    
    if ((n < 0) || ((size_t)n >= sizeof(buffer))) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    ssize_t sent = PAL_SEND(session->ctrl_fd, buffer, (size_t)n, 0);
    if (sent != n) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/*===========================================================================*
 * DATA CONNECTION MANAGEMENT
 *===========================================================================*/

/**
 * @brief Open data connection
 */
ftp_error_t ftp_session_open_data_connection(ftp_session_t *session)
{
    if (session == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->data_mode == FTP_DATA_MODE_NONE) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->data_mode == FTP_DATA_MODE_ACTIVE) {
        /* Active mode: Connect to client */
        int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) {
            return FTP_ERR_SOCKET_CREATE;
        }
        
        if (PAL_CONNECT(fd, (struct sockaddr*)&session->data_addr,
                        sizeof(session->data_addr)) < 0) {
            PAL_CLOSE(fd);
            return FTP_ERR_SOCKET_SEND;
        }
        
        session->data_fd = fd;
        
    } else if (session->data_mode == FTP_DATA_MODE_PASSIVE) {
        /* Passive mode: Accept from client */
        if (session->pasv_fd < 0) {
            return FTP_ERR_INVALID_PARAM;
        }
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int fd = PAL_ACCEPT(session->pasv_fd,
                            (struct sockaddr*)&client_addr,
                            &addr_len);
        
        if (fd < 0) {
            return FTP_ERR_SOCKET_ACCEPT;
        }
        
        session->data_fd = fd;
        
        /* Close passive listener (one connection only) */
        PAL_CLOSE(session->pasv_fd);
        session->pasv_fd = -1;
    }
    
    /* Configure socket for optimal performance */
    (void)pal_socket_configure(session->data_fd);
    
    return FTP_OK;
}

/**
 * @brief Close data connection
 */
void ftp_session_close_data_connection(ftp_session_t *session)
{
    if (session == NULL) {
        return;
    }
    
    if (session->data_fd >= 0) {
        PAL_CLOSE(session->data_fd);
        session->data_fd = -1;
    }
    
    if (session->pasv_fd >= 0) {
        PAL_CLOSE(session->pasv_fd);
        session->pasv_fd = -1;
    }
    
    session->data_mode = FTP_DATA_MODE_NONE;
    session->restart_offset = 0;
}

/**
 * @brief Send data via data connection
 */
ssize_t ftp_session_send_data(ftp_session_t *session,
                                const void *buffer,
                                size_t length)
{
    if ((session == NULL) || (buffer == NULL) || (length == 0U)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->data_fd < 0) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    ssize_t sent = PAL_SEND(session->data_fd, buffer, length, 0);
    
    if (sent > 0) {
        atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)sent);
    }
    
    return sent;
}

/**
 * @brief Receive data via data connection
 */
ssize_t ftp_session_recv_data(ftp_session_t *session,
                                void *buffer,
                                size_t length)
{
    if ((session == NULL) || (buffer == NULL) || (length == 0U)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->data_fd < 0) {
        return FTP_ERR_SOCKET_RECV;
    }
    
    ssize_t received = PAL_RECV(session->data_fd, buffer, length, 0);
    
    if (received > 0) {
        atomic_fetch_add(&session->stats.bytes_received, (uint64_t)received);
    }
    
    return received;
}

/*===========================================================================*
 * COMMAND PROCESSING
 *===========================================================================*/

/**
 * @brief Read command line from control connection
 */
ssize_t ftp_session_read_command(ftp_session_t *session,
                                   char *buffer,
                                   size_t size)
{
    if ((session == NULL) || (buffer == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (session->ctrl_fd < 0) {
        return FTP_ERR_SOCKET_RECV;
    }
    
    if (size < FTP_CMD_BUFFER_SIZE) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Read until CRLF */
    size_t pos = 0U;
    int found_cr = 0;
    
    while (pos < (size - 1U)) {
        char c;
        ssize_t n = PAL_RECV(session->ctrl_fd, &c, 1U, 0);
        
        if (n <= 0) {
            if (n == 0) {
                /* Connection closed */
                return 0;
            }
            
            if (errno == EINTR) {
                continue; /* Interrupted, retry */
            }
            
            return FTP_ERR_SOCKET_RECV;
        }
        
        if (c == '\r') {
            found_cr = 1;
        } else if ((c == '\n') && found_cr) {
            /* Found CRLF: command complete */
            buffer[pos] = '\0';
            return (ssize_t)pos;
        } else {
            buffer[pos] = c;
            pos++;
            found_cr = 0;
        }
    }
    
    /* Buffer full without CRLF */
    return FTP_ERR_PROTOCOL;
}

/**
 * @brief Process FTP command
 */
int ftp_session_process_command(ftp_session_t *session, const char *line)
{
    if ((session == NULL) || (line == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Parse command line */
    char command[64];
    char args[FTP_CMD_BUFFER_SIZE];
    
    ftp_error_t err = ftp_parse_command_line(line, command, args,
                                              sizeof(command), sizeof(args));
    
    if (err != FTP_OK) {
        ftp_session_send_reply(session, FTP_REPLY_500_SYNTAX_ERROR, NULL);
        return 0;
    }
    
    /* Find command handler */
    const ftp_command_entry_t *cmd = ftp_find_command(command);
    
    if (cmd == NULL) {
        ftp_session_send_reply(session, FTP_REPLY_500_SYNTAX_ERROR,
                               "Unknown command.");
        return 0;
    }
    
    /* Validate arguments */
    const char *cmd_args = (args[0] != '\0') ? args : NULL;
    err = ftp_validate_command_args(cmd, cmd_args);
    
    if (err != FTP_OK) {
        ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS, NULL);
        return 0;
    }
    
    /* Execute command */
    err = cmd->handler(session, cmd_args);
    
    if (err != FTP_OK) {
        /* Command failed */
        atomic_fetch_add(&session->stats.errors, 1U);
        
        /* Most command handlers send their own error replies */
        /* This is a fallback for unhandled errors */
        if (err == FTP_ERR_NOT_FOUND) {
            ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR, NULL);
        } else if (err == FTP_ERR_PERMISSION) {
            ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                   "Permission denied.");
        }
    }
    
    /* Check if QUIT command */
    if (strcmp(command, "QUIT") == 0) {
        return 1; /* Signal to close session */
    }
    
    return 0; /* Continue processing */
}
