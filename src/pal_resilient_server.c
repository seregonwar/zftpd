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

#include "pal_resilient_server.h"
#include "ftp_log.h"
#include "pal_notification.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>


int pal_resilient_accept(int *listen_fd,
                         const struct sockaddr_in *listen_addr,
                         struct sockaddr *client_addr,
                         socklen_t *addr_len,
                         atomic_int *running_flag)
{
    while (atomic_load(running_flag) != 0) {
        int fd = *listen_fd;
        
        if (fd < 0) {
            ftp_log_line(FTP_LOG_WARN, "[resilient] Listen socket is offline. Attempting to recreate socket...");
            
            fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) {
                ftp_log_line(FTP_LOG_ERROR, "[resilient] Socket creation failed. Retrying in 3 seconds...");
                sleep(3);
                continue;
            }

            if (pal_socket_set_reuseaddr(fd) != FTP_OK) {
                ftp_log_line(FTP_LOG_ERROR, "[resilient] setsockopt (SO_REUSEADDR) failed. Retrying in 3 seconds...");
                PAL_CLOSE(fd);
                sleep(3);
                continue;
            }

            if (PAL_BIND(fd, (const struct sockaddr*)listen_addr, sizeof(*listen_addr)) < 0) {
                ftp_log_line(FTP_LOG_ERROR, "[resilient] Socket bind failed. Retrying in 3 seconds...");
                PAL_CLOSE(fd);
                sleep(3);
                continue;
            }

            if (PAL_LISTEN(fd, FTP_LISTEN_BACKLOG) < 0) {
                ftp_log_line(FTP_LOG_ERROR, "[resilient] Socket listen failed. Retrying in 3 seconds...");
                PAL_CLOSE(fd);
                sleep(3);
                continue;
            }

            *listen_fd = fd;
            ftp_log_line(FTP_LOG_INFO, "[resilient] Socket recreated and listening successfully.");

            char msg[128];
            char display_ip[32] = "0.0.0.0";
            if (pal_network_get_primary_ip(display_ip, sizeof(display_ip)) == FTP_OK) {
                (void)snprintf(msg, sizeof(msg), "zftpd: Server resumed on %s:%u",
                               display_ip, (unsigned)ntohs(listen_addr->sin_port));
            } else {
                (void)snprintf(msg, sizeof(msg), "zftpd: Server resumed");
            }
            pal_notification_send(msg);
        }

        int client_fd = PAL_ACCEPT(fd, client_addr, addr_len);
        if (client_fd >= 0) {
            return client_fd;
        }

        if (atomic_load(running_flag) == 0) {
            break;
        }

        int err = errno;
        if (err == EINTR || err == EAGAIN || err == EWOULDBLOCK) {
            continue;
        }

        char log_msg[128];
        snprintf(log_msg, sizeof(log_msg), "[resilient] Fatal socket error in accept (errno=%d: %s). Dismantling socket.", err, strerror(err));
        ftp_log_line(FTP_LOG_ERROR, log_msg);

        PAL_CLOSE(fd);
        *listen_fd = -1;
    }
    return -1;
}
