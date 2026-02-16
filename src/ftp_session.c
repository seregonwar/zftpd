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
 * @file ftp_session.c
 * @brief FTP session management implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 */

#include "ftp_session.h"
#include "ftp_protocol.h"
#include "ftp_path.h"
#include "ftp_log.h"
#include "pal_network.h"
#include "pal_fileio.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/time.h>
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
    (void)pal_socket_set_timeouts(session->ctrl_fd, FTP_CTRL_IO_TIMEOUT_MS, FTP_CTRL_IO_TIMEOUT_MS);
    
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
    
    size_t root_len = strlen(root_path);
    if (root_len >= sizeof(session->root_path)) {
        return FTP_ERR_PATH_TOO_LONG;
    }
    memcpy(session->root_path, root_path, root_len + 1U);
    memcpy(session->cwd, root_path, root_len + 1U);

    if (pal_path_exists(session->root_path) == 1) {
        char real_buf[FTP_PATH_MAX];
        if (realpath(session->root_path, real_buf) != NULL) {
            size_t n = strlen(real_buf);
            if (n < sizeof(session->root_path)) {
                memcpy(session->root_path, real_buf, n + 1U);
                memcpy(session->cwd, real_buf, n + 1U);
            }
        }
    }
    
    session->rename_from[0] = '\0';
    
    /* Authentication */
    session->auth_attempts = 0U;
    session->authenticated = 0U;
    session->user_ok = 0U;

    session->ctrl_rx_len = 0U;
    session->ctrl_rx_off = 0U;
    
    /* Session identification */
    session->session_id = session_id;
    
    /* Timing */
    session->connect_time = time(NULL);
    session->last_activity = session->connect_time;

    session->rl_tokens = 0U;
    session->rl_last_ns = 0U;
    
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

    ftp_log_session_event(session, "CONNECT", FTP_OK, 0U);
    
    /* Command processing loop */
    char cmd_buffer[FTP_CMD_BUFFER_SIZE];
    int should_quit = 0;
    
    while (!should_quit) {
        time_t now = time(NULL);
        if ((now != (time_t)-1) && (now > session->last_activity)) {
            if ((uint64_t)(now - session->last_activity) > (uint64_t)FTP_SESSION_TIMEOUT) {
                (void)ftp_session_send_reply(session, FTP_REPLY_421_SERVICE_UNAVAIL, "Idle timeout.");
                ftp_log_session_event(session, "IDLE_TIMEOUT", FTP_ERR_TIMEOUT, 0U);
                break;
            }
        }

        /* Read command line */
        ssize_t n = ftp_session_read_command(session, 
                                               cmd_buffer,
                                               sizeof(cmd_buffer));
        
        if (n == 0) {
            break;
        }
        if (n < 0) {
            if (n == (ssize_t)FTP_ERR_TIMEOUT) {
                continue;
            }
            if (n == (ssize_t)FTP_ERR_PROTOCOL) {
                continue;
            }
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

    ftp_log_session_event(session, "DISCONNECT", FTP_OK, 0U);
    
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
    ssize_t sent = pal_send_all(session->ctrl_fd, buffer, (size_t)len, 0);
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
        
        ssize_t sent = pal_send_all(session->ctrl_fd, buffer, (size_t)n, 0);
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
    
    ssize_t sent = pal_send_all(session->ctrl_fd, buffer, (size_t)n, 0);
    if (sent != n) {
        return FTP_ERR_SOCKET_SEND;
    }
    
    return FTP_OK;
}

/*===========================================================================*
 * DATA CONNECTION MANAGEMENT
 *===========================================================================*/

static int wait_fd_ready(int fd, int for_write, uint32_t timeout_ms)
{
    if (fd < 0) {
        errno = EBADF;
        return -1;
    }

    struct timeval tv;
    tv.tv_sec = (time_t)(timeout_ms / 1000U);
    tv.tv_usec = (suseconds_t)((timeout_ms % 1000U) * 1000U);

    fd_set rfds;
    fd_set wfds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);

    if (for_write != 0) {
        FD_SET(fd, &wfds);
    } else {
        FD_SET(fd, &rfds);
    }

    while (1) {
        int rc = select(fd + 1,
                        (for_write != 0) ? NULL : &rfds,
                        (for_write != 0) ? &wfds : NULL,
                        NULL,
                        &tv);
        if (rc == 0) {
            errno = ETIMEDOUT;
            return 0;
        }
        if (rc < 0) {
            if (errno == EINTR) {
                continue;
            }
        }
        return rc;
    }
}

static uint64_t monotonic_ns(void)
{
#if defined(CLOCK_MONOTONIC)
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        return ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
    }
#endif
    struct timeval tv;
    if (gettimeofday(&tv, NULL) == 0) {
        return ((uint64_t)tv.tv_sec * 1000000000ULL) + ((uint64_t)tv.tv_usec * 1000ULL);
    }
    return 0U;
}

static void rate_limit(ftp_session_t *session, size_t bytes)
{
    if ((session == NULL) || (bytes == 0U)) {
        return;
    }

    uint64_t rate = (uint64_t)FTP_TRANSFER_RATE_LIMIT_BPS;
    if (rate == 0U) {
        return;
    }

    uint64_t burst = (uint64_t)FTP_TRANSFER_RATE_BURST_BYTES;
    uint64_t cap = (burst != 0U) ? burst : rate;

    uint64_t now = monotonic_ns();
    if (now == 0U) {
        return;
    }

    if (session->rl_last_ns == 0U) {
        session->rl_last_ns = now;
        session->rl_tokens = cap;
    } else if (now > session->rl_last_ns) {
        uint64_t elapsed = now - session->rl_last_ns;
        uint64_t add = (elapsed * rate) / 1000000000ULL;
        session->rl_last_ns = now;
        if (add > 0U) {
            uint64_t t = session->rl_tokens + add;
            session->rl_tokens = (t > cap) ? cap : t;
        }
    }

    uint64_t need = (uint64_t)bytes;
    while (session->rl_tokens < need) {
        uint64_t missing = need - session->rl_tokens;
        uint64_t wait_ns = (missing * 1000000000ULL + rate - 1U) / rate;
        uint64_t wait_us = (wait_ns + 999ULL) / 1000ULL;
        if (wait_us > 500000ULL) {
            wait_us = 500000ULL;
        }
        usleep((useconds_t)wait_us);

        now = monotonic_ns();
        if ((now != 0U) && (now > session->rl_last_ns)) {
            uint64_t elapsed = now - session->rl_last_ns;
            uint64_t add = (elapsed * rate) / 1000000000ULL;
            session->rl_last_ns = now;
            if (add > 0U) {
                uint64_t t = session->rl_tokens + add;
                session->rl_tokens = (t > cap) ? cap : t;
            }
        } else {
            break;
        }
    }

    if (session->rl_tokens >= need) {
        session->rl_tokens -= need;
    } else {
        session->rl_tokens = 0U;
    }
}

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

        (void)pal_socket_set_nonblocking(fd);

        if (PAL_CONNECT(fd, (struct sockaddr*)&session->data_addr,
                        sizeof(session->data_addr)) < 0) {
            if (errno != EINPROGRESS) {
                PAL_CLOSE(fd);
                return FTP_ERR_SOCKET_SEND;
            }
        }

        int ready = wait_fd_ready(fd, 1, FTP_DATA_CONNECT_TIMEOUT_MS);
        if (ready <= 0) {
            PAL_CLOSE(fd);
            return FTP_ERR_TIMEOUT;
        }

        int so_error = 0;
        socklen_t so_len = (socklen_t)sizeof(so_error);
        if (PAL_GETSOCKOPT(fd, SOL_SOCKET, SO_ERROR, &so_error, &so_len) < 0) {
            PAL_CLOSE(fd);
            return FTP_ERR_SOCKET_SEND;
        }
        if (so_error != 0) {
            errno = so_error;
            PAL_CLOSE(fd);
            return FTP_ERR_SOCKET_SEND;
        }

        (void)pal_socket_set_blocking(fd);
        
        session->data_fd = fd;
        
    } else if (session->data_mode == FTP_DATA_MODE_PASSIVE) {
        /* Passive mode: Accept from client */
        if (session->pasv_fd < 0) {
            return FTP_ERR_INVALID_PARAM;
        }
        
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);
        
        int ready = wait_fd_ready(session->pasv_fd, 0, FTP_DATA_CONNECT_TIMEOUT_MS);
        if (ready <= 0) {
            PAL_CLOSE(session->pasv_fd);
            session->pasv_fd = -1;
            return FTP_ERR_TIMEOUT;
        }

        int fd = PAL_ACCEPT(session->pasv_fd, (struct sockaddr*)&client_addr, &addr_len);
        
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
    
    rate_limit(session, length);
    ssize_t sent = pal_send_all(session->data_fd, buffer, length, 0);
    
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
        rate_limit(session, (size_t)received);
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
    
    size_t out_len = 0U;
    int too_long = 0;

    while (1) {
        for (uint16_t i = session->ctrl_rx_off; i < session->ctrl_rx_len; i++) {
            char c = session->ctrl_rxbuf[i];

            if (too_long == 0) {
                if (out_len < (size - 1U)) {
                    buffer[out_len] = c;
                    out_len++;
                } else {
                    too_long = 1;
                }
            }

            if (c == '\n') {
                uint16_t line_end = (uint16_t)(i + 1U);
                uint16_t consume = line_end;

                session->ctrl_rx_off = consume;
                if (session->ctrl_rx_off >= session->ctrl_rx_len) {
                    session->ctrl_rx_off = 0U;
                    session->ctrl_rx_len = 0U;
                }

                if (too_long != 0) {
                    (void)ftp_session_send_reply(session, FTP_REPLY_500_SYNTAX_ERROR, "Command too long.");
                    return (ssize_t)FTP_ERR_PROTOCOL;
                }

                if ((out_len >= 2U) && (buffer[out_len - 2U] == '\r') && (buffer[out_len - 1U] == '\n')) {
                    out_len -= 2U;
                    buffer[out_len] = '\0';
                    return (ssize_t)out_len;
                }

                out_len = 0U;
            }
        }

        if (too_long != 0) {
            out_len = 0U;
        }

        if (session->ctrl_rx_off >= session->ctrl_rx_len) {
            session->ctrl_rx_off = 0U;
            session->ctrl_rx_len = 0U;
        }

        ssize_t n = PAL_RECV(session->ctrl_fd,
                             session->ctrl_rxbuf,
                             sizeof(session->ctrl_rxbuf),
                             0);
        if (n == 0) {
            return 0;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            if ((errno == EAGAIN) || (errno == EWOULDBLOCK)) {
                return (ssize_t)FTP_ERR_TIMEOUT;
            }
            return (ssize_t)FTP_ERR_SOCKET_RECV;
        }

        session->ctrl_rx_len = (uint16_t)n;
        session->ctrl_rx_off = 0U;
    }
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

    if (session->authenticated == 0U) {
        if ((strcmp(command, "USER") != 0) &&
            (strcmp(command, "PASS") != 0) &&
            (strcmp(command, "QUIT") != 0) &&
            (strcmp(command, "NOOP") != 0) &&
            (strcmp(command, "FEAT") != 0) &&
            (strcmp(command, "SYST") != 0)) {
            ftp_session_send_reply(session, FTP_REPLY_530_NOT_LOGGED_IN,
                                   "Please login with USER and PASS.");
            return 0;
        }
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

    if (FTP_LOG_COMMANDS != 0) {
        ftp_log_session_cmd(session, command, err);
    }
    
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
