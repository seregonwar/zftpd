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

/** @file data_connection.c @brief FTP active and passive data-channel commands. */
#include "ftp_commands.h"
#include "ftp_log.h"
#include "ftp_session.h"
#include "pal_network.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef FTP_PORT_ALLOW_FOREIGN_IP
#define FTP_PORT_ALLOW_FOREIGN_IP 0
#endif

ftp_error_t cmd_PORT(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  unsigned int h1, h2, h3, h4, p1, p2;
  int consumed = 0;
  if (sscanf(args, "%u,%u,%u,%u,%u,%u%n", &h1, &h2, &h3, &h4, &p1, &p2,
             &consumed) != 6) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT format.");
  }
  while (args[consumed] == ' ' || args[consumed] == '\t') consumed++;
  if (args[consumed] != '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT format.");
  }

  if ((h1 > 255U) || (h2 > 255U) || (h3 > 255U) || (h4 > 255U) || (p1 > 255U) ||
      (p2 > 255U)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT values.");
  }

  char ip[INET_ADDRSTRLEN];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", h1, h2, h3, h4);

  uint16_t port = (uint16_t)((p1 << 8) | p2);

  ftp_error_t err = pal_make_sockaddr(ip, port, &session->data_addr);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid address.");
  }

  /* RFC 2577: block FTP bounce attacks by default. */
#if !FTP_PORT_ALLOW_FOREIGN_IP
  if (strcmp(ip, session->client_ip) != 0) {
    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "[SEC] PORT IP mismatch: client=%s port=%s (rejected)",
             session->client_ip, ip);
    ftp_log_line(FTP_LOG_INFO, dbg);
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "PORT address mismatch.");
  }
#endif

  session->data_mode = FTP_DATA_MODE_ACTIVE;

  return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                "PORT command successful.");
}

static ftp_error_t open_passive_listener(ftp_session_t *session,
                                         uint32_t *out_ip,
                                         uint16_t *out_port) {
  if (session == NULL || out_ip == NULL || out_port == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }
  if (session->pasv_fd >= 0) {
    PAL_CLOSE(session->pasv_fd);
    session->pasv_fd = -1;
  }

  int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot create socket.");
  }
  (void)pal_socket_set_reuseaddr(fd);

  /* FreeBSD/OrbisOS inherit SO_RCVBUF from the listener into accepted
   * sockets. Set it before bind/listen to prevent STOR zero-window stalls.
   * SO_SNDBUF remains unset so TCP send auto-tuning stays enabled. */
  int rcvbuf = (int)FTP_TCP_RCVBUF;
  (void)PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

  uint32_t ip = 0U;
  struct sockaddr_in local;
  socklen_t local_len = (socklen_t)sizeof(local);
  memset(&local, 0, sizeof(local));
  if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr *)&local,
                      &local_len) == 0) {
    ip = PAL_NTOHL(local.sin_addr.s_addr);
  }
  if (ip == 0U) {
    char ip_str[INET_ADDRSTRLEN];
    struct in_addr ia;
    if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK &&
        PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
      ip = PAL_NTOHL(ia.s_addr);
    }
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = (ip != 0U) ? PAL_HTONL(ip) : PAL_HTONL(INADDR_ANY);
  addr.sin_port = 0;
  if (PAL_BIND(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Bind failed.");
  }
  if (PAL_LISTEN(fd, 1) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Listen failed.");
  }

  struct sockaddr_in passive_addr;
  socklen_t addr_len = (socklen_t)sizeof(passive_addr);
  memset(&passive_addr, 0, sizeof(passive_addr));
  if (PAL_GETSOCKNAME(fd, (struct sockaddr *)&passive_addr, &addr_len) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot get socket name.");
  }

  session->pasv_fd = fd;
  session->data_mode = FTP_DATA_MODE_PASSIVE;
  *out_ip = (ip != 0U) ? ip : PAL_NTOHL(passive_addr.sin_addr.s_addr);
  *out_port = PAL_NTOHS(passive_addr.sin_port);
  return FTP_OK;
}

ftp_error_t cmd_PASV(ftp_session_t *session, const char *args) {
  (void)args;
  if (session == NULL) return FTP_ERR_INVALID_PARAM;

  uint32_t ip = 0U;
  uint16_t port = 0U;
  ftp_error_t err = open_passive_listener(session, &ip, &port);
  if (err != FTP_OK) return err;

  char reply[FTP_REPLY_BUFFER_SIZE];
  (void)snprintf(reply, sizeof(reply),
                 "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
                 (ip >> 24) & 0xFFU, (ip >> 16) & 0xFFU,
                 (ip >> 8) & 0xFFU, ip & 0xFFU,
                 (port >> 8) & 0xFFU, port & 0xFFU);
  return ftp_session_send_reply(session, FTP_REPLY_227_PASV_MODE, reply);
}

ftp_error_t cmd_EPSV(ftp_session_t *session, const char *args) {
  (void)args;
  if (session == NULL) return FTP_ERR_INVALID_PARAM;

  uint32_t ip = 0U;
  uint16_t port = 0U;
  ftp_error_t err = open_passive_listener(session, &ip, &port);
  if (err != FTP_OK) return err;
  (void)ip;

  char reply[FTP_REPLY_BUFFER_SIZE];
  (void)snprintf(reply, sizeof(reply),
                 "Entering Extended Passive Mode (|||%u|).",
                 (unsigned)port);
  return ftp_session_send_reply(session, FTP_REPLY_229_EPSV_MODE, reply);
}
