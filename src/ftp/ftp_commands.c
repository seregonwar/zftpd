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
 * @file ftp_commands.c
 * @brief FTP command handlers implementation
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 * PROTOCOL: RFC 959 (File Transfer Protocol)
 * EXTENSIONS: RFC 3659 (MLST, MLSD, SIZE, MDTM)
 *
 */

#include "ftp_commands.h"
#include "ftp_buffer_pool.h"
#include "ftp_crypto.h"
#include "ftp_instance.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include "pal_filesystem.h"
#include "pal_network.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__) ||                                                      \
    (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
#include <sys/mount.h>
#endif
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <sys/mount.h>
extern int _fstatfs(int, struct statfs *);
#endif
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#if defined(__linux__) || defined(__FreeBSD__)
#include <fcntl.h> /* posix_fadvise(POSIX_FADV_DONTNEED) */
#endif

/*===========================================================================*
 * FORWARD DECLARATIONS
 *===========================================================================*/

static ftp_error_t start_async_copy(ftp_session_t *session,
                                    const char *src_ftp_path,
                                    const char *dst_ftp_path, int is_move);






/*===========================================================================*
 * FILE MANAGEMENT
 *===========================================================================*/

/**
 * @brief DELE command - Delete file
 */
ftp_error_t cmd_DELE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_file_delete(resolved);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot delete file.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "File deleted.");
}

/**
 * @brief RMD command - Remove directory
 */
ftp_error_t cmd_RMD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_dir_remove(resolved);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot remove directory.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "Directory removed.");
}

/**
 * @brief MKD command - Make directory
 */
ftp_error_t cmd_MKD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_dir_create(resolved, DIR_PERM);

  if (err != FTP_OK) {
    if ((err == FTP_ERR_DIR_EXISTS) && (pal_path_is_directory(resolved) == 1)) {
      /*
       * +---------------------------------------------------------+
       * | CONCURRENCY HANDLING                                    |
       * | Directory was just created by another active thread.    |
       * | We treat this EEXIST as a success to prevent FileZilla  |
       * | from aborting the entire directory tree upload.         |
       * +---------------------------------------------------------+
       */
    } else {
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Cannot create directory.");
    }
  }

  char reply[FTP_REPLY_BUFFER_SIZE];
  size_t res_len = strlen(resolved);
  size_t max_path = sizeof(reply) - 14; /* "..." created. + NUL */
  if (res_len > max_path) { res_len = max_path; }
  int n = snprintf(reply, sizeof(reply), "\"%.*s\" created.",
                   (int)res_len, resolved);

  /* VULN-05 fix: check for truncation (same as cmd_PWD) */
  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED,
                                  "Directory created.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

/**
 * @brief RNFR command - Rename from
 */
ftp_error_t cmd_RNFR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Check if file exists */
  if (pal_path_exists(resolved) != 1) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  /* Store source path */
  size_t len = strlen(resolved);
  if (len >= sizeof(session->rename_from)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Path too long.");
  }

  memcpy(session->rename_from, resolved, len + 1U);

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "Ready for RNTO.");
}

/**
 * @brief RNTO command - Rename to
 */
ftp_error_t cmd_RNTO(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Check if RNFR was called */
  if (session->rename_from[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "RNFR required first.");
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->rename_from[0] = '\0';
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Perform rename */
  err = pal_file_rename(session->rename_from, resolved);

  if (err == FTP_ERR_CROSS_DEVICE) {
    ftp_error_t async_err =
        start_async_copy(session, session->rename_from, args, 1);
    /* Clear rename_from state */
    session->rename_from[0] = '\0';
    return async_err;
  }

  /* Clear rename_from */
  session->rename_from[0] = '\0';

  if (err != FTP_OK) {
    /*
     * Map ftp_error_t to a human-readable detail so the FTP client
     * (FileZilla, WinSCP, etc.) shows something actionable instead
     * of the opaque "Rename failed.".
     *
     *   550 Permission denied.
     *   550 Source not found.
     *   550 Path too long.
     *   ...
     */
    const char *detail;
    switch (err) {
    case FTP_ERR_NOT_FOUND:
      detail = "Source not found.";
      break;
    case FTP_ERR_PERMISSION:
      detail = "Permission denied.";
      break;
    case FTP_ERR_PATH_TOO_LONG:
      detail = "Path too long.";
      break;
    case FTP_ERR_OUT_OF_MEMORY:
      detail = "Out of memory.";
      break;
    case FTP_ERR_DIR_OPEN:
      detail = "Cannot open directory.";
      break;
    case FTP_ERR_FILE_OPEN:
      detail = "Cannot open file.";
      break;
    case FTP_ERR_FILE_READ:
      detail = "Read error during copy.";
      break;
    default: {
      static _Thread_local char buf[64];
      snprintf(buf, sizeof(buf), "Rename failed (err=%d).", (int)err);
      detail = buf;
      break;
    }
    }
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR, detail);
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "File renamed.");
}

/*===========================================================================*
 * DATA CONNECTION
 *===========================================================================*/

/**
 * @brief PORT command - Active mode data connection
 */
ftp_error_t cmd_PORT(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Parse h1,h2,h3,h4,p1,p2 */
  unsigned int h1, h2, h3, h4, p1, p2;

  if (sscanf(args, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT format.");
  }

  /* Validate ranges */
  if ((h1 > 255U) || (h2 > 255U) || (h3 > 255U) || (h4 > 255U) || (p1 > 255U) ||
      (p2 > 255U)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT values.");
  }

  /* Build IP address */
  char ip[INET_ADDRSTRLEN];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", h1, h2, h3, h4);

  /* Build port */
  uint16_t port = (uint16_t)((p1 << 8) | p2);

  /* Debug: log PORT target */
  {
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "[DBG] PORT target: %s:%u", ip, (unsigned)port);
    ftp_log_line(FTP_LOG_INFO, dbg);
  }
  /* Create sockaddr */
  ftp_error_t err = pal_make_sockaddr(ip, port, &session->data_addr);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid address.");
  }

  /*
   * VULN-04 fix: validate PORT IP against control connection
   *
   *   RFC 2577 (FTP Security Considerations) recommends that
   *   servers verify the PORT IP matches the client's control
   *   connection IP to prevent SSRF (bounce attacks).
   *
   *   Compile with -DFTP_PORT_ALLOW_FOREIGN_IP=1 to disable
   *   this check for NAT environments (Android emulators).
   *
   *   control IP:  session->client_ip  (e.g. "192.168.1.50")
   *   PORT IP:     ip                  (e.g. "192.168.1.1")
   *   mismatch  -> 501 rejected
   */
#ifndef FTP_PORT_ALLOW_FOREIGN_IP
#define FTP_PORT_ALLOW_FOREIGN_IP 0
#endif

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

  /* Set mode to active */
  session->data_mode = FTP_DATA_MODE_ACTIVE;

  return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                "PORT command successful.");
}

/**
 * @brief PASV command - Passive mode data connection
 */
ftp_error_t cmd_PASV(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (session->pasv_fd >= 0) {
    PAL_CLOSE(session->pasv_fd);
    session->pasv_fd = -1;
  }

  /* Create passive listener socket */
  int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot create socket.");
  }

  /* Enable address reuse */
  (void)pal_socket_set_reuseaddr(fd);

  /*
   * Set SO_RCVBUF on the LISTENING socket BEFORE bind/listen.
   *
   * On FreeBSD/PS4/PS5 the kernel copies the listening socket's receive
   * buffer size into each accepted connection during the 3-way handshake.
   * Setting SO_RCVBUF on the accepted socket after accept() is too late:
   * the kernel caps post-connect increases to kern.ipc.maxsockbuf (~1 MB
   * on OrbisOS), which is why STOR transfers stall after exactly 1 MB.
   * Setting it here propagates the full 4 MB to every accepted data socket.
   */
  {
    int rcvbuf = (int)FTP_TCP_RCVBUF;
    (void)PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  /*
   * SO_SNDBUF is intentionally NOT set on the listening socket.
   *
   * DESIGN RATIONALE — auto-tuning vs. explicit SNDBUF:
   *
   *   On both Linux (tcp_wmem) and FreeBSD (net.inet.tcp.sendbuf_auto),
   *   calling setsockopt(SO_SNDBUF) explicitly on any socket — even
   *   pre-bind — marks that socket as "manually sized" and DISABLES
   *   the kernel's TCP send-buffer auto-tuning for it.
   *
   *   The HTTP server never sets SO_SNDBUF and relies on auto-tuning;
   *   it achieves full link speed at any internet RTT because the kernel
   *   grows the buffer to exactly BDP = RTT × bandwidth.
   *
   *   A previous version of this code set SO_SNDBUF = FTP_TCP_DATA_SNDBUF
   *   (4 MB) here, hoping to bypass kern.ipc.maxsockbuf via the 3-way
   *   handshake inheritance trick.  On OrbisOS the kernel still capped the
   *   effective buffer (≈ 512 KB–1 MB) and, critically, disabled
   *   auto-tuning — leaving FTP stuck at ≈ 30 Mbps while HTTP with
   *   auto-tuning reached 80 Mbps on the same link.
   *
   *   SO_RCVBUF (STOR/uploads) cannot use auto-tuning because the kernel
   *   does not auto-grow the receive buffer on FreeBSD; the explicit value
   *   is required there to prevent zero-window stalls.  The send buffer
   *   (RETR/downloads) has no such constraint — leave it for auto-tuning.
   */

  /* Resolve the local IP from the control connection so the PASV
   * listener is bound to the specific interface — not INADDR_ANY.
   * On OrbisOS/FreeBSD (PS4/PS5), binding to INADDR_ANY causes
   * inbound SYNs to be silently dropped when the kernel routes the
   * incoming connection via a specific interface that doesn't match
   * the wildcard binding, even though the 227 reply advertises the
   * correct IP.  Binding to the exact local address fixes this. */
  uint32_t ip = 0U;
  {
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));
    if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr *)&local,
                        &local_len) == 0) {
      ip = PAL_NTOHL(local.sin_addr.s_addr);
    }
  }
  if (ip == 0U) {
    char ip_str[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK) {
      struct in_addr ia;
      if (PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
        ip = PAL_NTOHL(ia.s_addr);
      }
    }
  }

  /* Bind to the resolved local IP, ephemeral port (port 0 = auto-assign) */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = (ip != 0U) ? PAL_HTONL(ip) : PAL_HTONL(INADDR_ANY);
  addr.sin_port = 0; /* Auto-assign port */

  if (PAL_BIND(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Bind failed.");
  }

  /* Listen */
  if (PAL_LISTEN(fd, 1) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Listen failed.");
  }

  /* Get assigned port */
  struct sockaddr_in pasv_addr;
  socklen_t addr_len = sizeof(pasv_addr);
  if (PAL_GETSOCKNAME(fd, (struct sockaddr *)&pasv_addr, &addr_len) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot get socket name.");
  }

  session->pasv_fd = fd;
  session->data_mode = FTP_DATA_MODE_PASSIVE;

  /* Format reply: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
  if (ip == 0U) {
    ip = PAL_NTOHL(pasv_addr.sin_addr.s_addr);
  }
  uint16_t port = PAL_NTOHS(pasv_addr.sin_port);

  unsigned int h1 = (ip >> 24) & 0xFFU;
  unsigned int h2 = (ip >> 16) & 0xFFU;
  unsigned int h3 = (ip >> 8) & 0xFFU;
  unsigned int h4 = ip & 0xFFU;
  unsigned int p1 = (port >> 8) & 0xFFU;
  unsigned int p2 = port & 0xFFU;

  char reply[FTP_REPLY_BUFFER_SIZE];
  snprintf(reply, sizeof(reply), "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
           h1, h2, h3, h4, p1, p2);

  return ftp_session_send_reply(session, FTP_REPLY_227_PASV_MODE, reply);
}

/*---------------------------------------------------------------------------*
 * EPSV  (RFC 2428 — Extended Passive Mode)
 *
 *   Client:  EPSV
 *   Server:  229 Entering Extended Passive Mode (|||port|)
 *
 *   WinSCP and many IPv6-aware clients try EPSV first.
 *   Without it they fall back to PORT which often fails behind NAT.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_EPSV(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Reuse PASV socket setup */
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

  /* Set SO_RCVBUF on the listener — same rationale as cmd_PASV */
  {
    int rcvbuf = (int)FTP_TCP_RCVBUF;
    (void)PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  /* SO_SNDBUF intentionally NOT set — see cmd_PASV comment on auto-tuning. */

  /* Bind to the local IP of the control connection — not INADDR_ANY.
   * Same rationale as cmd_PASV: on OrbisOS/FreeBSD, INADDR_ANY causes
   * inbound SYNs to be silently dropped. */
  uint32_t epsv_ip = 0U;
  {
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));
    if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr *)&local,
                        &local_len) == 0) {
      epsv_ip = PAL_NTOHL(local.sin_addr.s_addr);
    }
  }
  if (epsv_ip == 0U) {
    char ip_str[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK) {
      struct in_addr ia;
      if (PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
        epsv_ip = PAL_NTOHL(ia.s_addr);
      }
    }
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = (epsv_ip != 0U) ? PAL_HTONL(epsv_ip) : PAL_HTONL(INADDR_ANY);
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

  struct sockaddr_in pasv_addr;
  socklen_t addr_len = sizeof(pasv_addr);
  if (PAL_GETSOCKNAME(fd, (struct sockaddr *)&pasv_addr, &addr_len) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot get socket name.");
  }

  session->pasv_fd = fd;
  session->data_mode = FTP_DATA_MODE_PASSIVE;

  /*
   * RFC 2428: 229 Entering Extended Passive Mode (|||port|)
   *
   * The triple-pipe delimiter is protocol-agnostic (works for IPv4 + IPv6).
   * The client already knows the server IP from the control connection.
   */
  uint16_t port = PAL_NTOHS(pasv_addr.sin_port);
  char reply[FTP_REPLY_BUFFER_SIZE];
  snprintf(reply, sizeof(reply), "Entering Extended Passive Mode (|||%u|).",
           (unsigned)port);

  return ftp_session_send_reply(session, FTP_REPLY_229_EPSV_MODE, reply);
}

/*---------------------------------------------------------------------------*
 * OPTS  (RFC 2389 — Feature Negotiation)
 *
 *   Client:  OPTS UTF8 ON
 *   Server:  200 UTF8 mode enabled.
 *
 *   Almost every modern client sends "OPTS UTF8 ON" right after FEAT.
 *   Without this command, they get 500 Unknown Command → may disconnect.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_OPTS(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "OPTS requires an argument.");
  }

  /* Case-insensitive check for "UTF8 ON" / "UTF8" / "utf8 on" */
  char upper[64];
  size_t len = strlen(args);
  if (len >= sizeof(upper)) {
    len = sizeof(upper) - 1U;
  }
  for (size_t i = 0U; i < len; i++) {
    upper[i] = (char)toupper((unsigned char)args[i]);
  }
  upper[len] = '\0';

  if ((len >= 4U) && (strncmp(upper, "UTF8", 4) == 0) &&
      (len == 4U || strcmp(upper + 4, " ON") == 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "UTF8 mode enabled.");
  }

  /*
   *  OPTS MLST type*;size*;modify*;
   *  Some clients send this to negotiate MLST facts.
   *  Accept it silently.
   */
  if (strncmp(upper, "MLST", 4) == 0) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK, "MLST OPTS set.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                "Option not recognized.");
}

/*---------------------------------------------------------------------------*
 * SITE  (RFC 959 — Site-Specific Commands)
 *
 *   Client:  SITE CHMOD 755 somefile.txt
 *   Server:  200 CHMOD command successful.
 *
 *   FileZilla / WinSCP send SITE CHMOD after uploads and from the
 *   "File permissions" dialog. We apply chmod(2) on the resolved path.
 *   Filesystems that lack Unix permission bits (some console mounts)
 *   may return an error — we surface that instead of faking success.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_SITE(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL || args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE requires a command.");
  }

  /* Match leading verb case-insensitively (CHMOD / chmod / Chmod). */
  char verb[8];
  size_t vi = 0U;
  while ((args[vi] != '\0') && (args[vi] != ' ') && (args[vi] != '\t') &&
         (vi + 1U < sizeof(verb))) {
    verb[vi] = (char)toupper((unsigned char)args[vi]);
    vi++;
  }
  verb[vi] = '\0';

  if (strcmp(verb, "CHMOD") != 0) {
    return ftp_session_send_reply(session, FTP_REPLY_502_NOT_IMPLEMENTED,
                                  "SITE command not supported.");
  }

  /* Skip verb + whitespace → "CHMOD <mode> <path>" */
  const char *p = args + vi;
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  if (*p == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE CHMOD requires mode and path.");
  }

  char *end = NULL;
  errno = 0;
  unsigned long mode_ul = strtoul(p, &end, 8);
  if ((end == p) || (errno == ERANGE) || (mode_ul > 07777UL)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid CHMOD mode (octal 0-7777).");
  }

  while ((*end == ' ') || (*end == '\t')) {
    end++;
  }
  if (*end == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE CHMOD requires a path.");
  }

  /* Strip optional quotes used by some clients around the path. */
  const char *path_arg = end;
  size_t path_len = strlen(path_arg);
  char path_buf[FTP_PATH_MAX];
  if ((path_len >= 2U) &&
      (((path_arg[0] == '"') && (path_arg[path_len - 1U] == '"')) ||
       ((path_arg[0] == '\'') && (path_arg[path_len - 1U] == '\'')))) {
    if (path_len - 2U >= sizeof(path_buf)) {
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Path too long.");
    }
    memcpy(path_buf, path_arg + 1, path_len - 2U);
    path_buf[path_len - 2U] = '\0';
    path_arg = path_buf;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  mode_t mode = (mode_t)(mode_ul & 07777UL);
  err = pal_file_chmod(resolved, mode);
  if (err == FTP_ERR_NOT_FOUND) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }
  if (err == FTP_ERR_PERMISSION) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Permission denied or CHMOD unsupported "
                                  "on this filesystem.");
  }
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot change permissions.");
  }

  /*
   * Some mounts accept chmod() but ignore bits (no Unix ACL). Verify when
   * possible and warn the client if the effective mode differs.
   */
  struct stat st;
  if (pal_file_stat(resolved, &st) == FTP_OK) {
    mode_t applied = (mode_t)(st.st_mode & 07777);
    if (applied != mode) {
      char msg[96];
      (void)snprintf(msg, sizeof(msg),
                     "CHMOD accepted but filesystem reports %04o "
                     "(requested %04o).",
                     (unsigned)applied, (unsigned)mode);
      return ftp_session_send_reply(session, FTP_REPLY_200_OK, msg);
    }
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                "CHMOD command successful.");
}

/*---------------------------------------------------------------------------*
 * CLNT  (Client Identification)
 *
 *   Client:  CLNT SuperFTP/1.0
 *   Server:  200 Noted.
 *
 *   Android apps (File Manager+, SuperFTP) send CLNT to identify
 *   themselves before USER/PASS. Without it they get 500 Unknown
 *   Command and disconnect immediately.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_CLNT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, "Noted.");
}

/*===========================================================================*
 * INFORMATION
 *===========================================================================*/

/**
 * @brief SIZE command - Return file size
 */
ftp_error_t cmd_SIZE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  vfs_stat_t st;
  err = vfs_stat(resolved, &st);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  char reply[64];
  snprintf(reply, sizeof(reply), "%llu", (unsigned long long)st.size);

  return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief MDTM command - Return modification time
 */
ftp_error_t cmd_MDTM(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  struct stat st;
  err = pal_file_stat(resolved, &st);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  /* Format: YYYYMMDDhhmmss */
  struct tm tm_time;
  gmtime_r(&st.st_mtime, &tm_time);

  char reply[32];
  snprintf(reply, sizeof(reply), "%04d%02d%02d%02d%02d%02d",
           tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
           tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);

  return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief STAT command - Status
 */
ftp_error_t cmd_STAT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  {
    char msg[128];
    (void)snprintf(msg, sizeof(msg),
                   "Server status OK. Instance %016llx",
                   (unsigned long long)ftp_daemon_instance_id());
    return ftp_session_send_reply(session, FTP_REPLY_211_SYSTEM_STATUS, msg);
  }
}

/**
 * @brief SYST command - System type
 */
ftp_error_t cmd_SYST(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_215_SYSTEM_TYPE, NULL);
}

/**
 * @brief FEAT command - Feature list
 */
ftp_error_t cmd_FEAT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /*
   * FEAT reply (RFC 2389)
   *
   *  211-Extensions supported:
   *   SIZE
   *   MDTM
   *   REST STREAM
   *   APPE
   *   UTF8
   *   XZFTPD INSTANCE <hex>
   *  211 End
   */
  char feat_instance[48];
  (void)snprintf(feat_instance, sizeof(feat_instance),
                 " XZFTPD INSTANCE %016llx",
                 (unsigned long long)ftp_daemon_instance_id());

  const char *features[] = {"Extensions supported:",
#if FTP_ENABLE_SIZE
                            " SIZE",
#endif
#if FTP_ENABLE_MDTM
                            " MDTM",
#endif
#if FTP_ENABLE_REST
                            " REST STREAM",
#endif
                            " APPE",
                            " EPSV",
#if FTP_ENABLE_UTF8
                            " UTF8",
#endif
#if FTP_ENABLE_MLST
                            " MLSD",
                            " MLST type*;size*;modify*;unix.mode*;",
#endif
#if FTP_ENABLE_CRYPTO
                            " XCRYPT",
#endif
                            " CPFR",
                            " CPTO",
                            " COPY",
                            " SITE CHMOD",
                            feat_instance,
                            "End"};

  return ftp_session_send_multiline_reply(
      session, FTP_REPLY_211_SYSTEM_STATUS, features,
      sizeof(features) / sizeof(features[0]));
}

/**
 * @brief HELP command - Help information
 */
ftp_error_t cmd_HELP(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  const char *lines[] = {"Supported commands:",
                         " USER PASS QUIT NOOP CWD CDUP PWD",
                         " LIST NLST MLSD MLST",
                         " RETR STOR APPE REST",
                         " DELE RMD MKD RNFR RNTO",
                         " PORT PASV SIZE MDTM STAT",
                         " SYST FEAT HELP TYPE MODE STRU",
                         " SITE CHMOD",
                         "End"};

  return ftp_session_send_multiline_reply(session, FTP_REPLY_214_HELP, lines,
                                          sizeof(lines) / sizeof(lines[0]));
}

/*===========================================================================*
 * ASYNC BACKGROUND COPY
 *===========================================================================*/

typedef struct {
  ftp_session_t *session;
  char src_path[FTP_PATH_MAX];
  char dst_path[FTP_PATH_MAX];
  int is_move;
} ftp_copy_task_t;

static void *ftp_copy_thread_func(void *arg) {
  ftp_copy_task_t *task = (ftp_copy_task_t *)arg;
  ftp_session_t *session = task->session;

  ftp_log_line(FTP_LOG_INFO, "[COPY] Background task started");

  /* Use pal_file_copy_recursive_ex so the OS errno is captured and visible
   * in the failure log.  Previously pal_file_copy_recursive was called with
   * no errno output, causing all error logs to show errno=0. */
  int copy_errno = 0;
  ftp_error_t err = pal_file_copy_recursive_ex(task->src_path, task->dst_path,
                                               !task->is_move, NULL, NULL,
                                               &copy_errno);

  pthread_mutex_lock(&session->copy_mutex);
  session->copy_in_progress = 0;
  pthread_mutex_unlock(&session->copy_mutex);

  if (err == FTP_OK) {
    ftp_log_line(FTP_LOG_INFO, "[COPY] Background task completed successfully");
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "[COPY] Background task failed: err=%d errno=%d",
             (int)err, copy_errno);
    ftp_log_line(FTP_LOG_WARN, msg);
  }

  free(task);
  return NULL;
}

static ftp_error_t start_async_copy(ftp_session_t *session,
                                    const char *src_ftp_path,
                                    const char *dst_ftp_path, int is_move) {
  pthread_mutex_lock(&session->copy_mutex);
  if (session->copy_in_progress) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_450_FILE_UNAVAILABLE,
                                  "Operation already in progress.");
  }

  /* Validate paths */
  char src_resolved[FTP_PATH_MAX];
  char dst_resolved[FTP_PATH_MAX];
  if (ftp_path_resolve(session, src_ftp_path, src_resolved,
                       sizeof(src_resolved)) != FTP_OK ||
      ftp_path_resolve(session, dst_ftp_path, dst_resolved,
                       sizeof(dst_resolved)) != FTP_OK) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  if (strcmp(src_resolved, dst_resolved) == 0) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_553_FILENAME_INVALID,
                                  "Source and destination are the same.");
  }

  ftp_copy_task_t *task = malloc(sizeof(ftp_copy_task_t));
  if (task == NULL) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Memory allocation failed.");
  }

  task->session = session;
  strncpy(task->src_path, src_resolved, sizeof(task->src_path) - 1);
  task->src_path[sizeof(task->src_path) - 1] = '\0';
  strncpy(task->dst_path, dst_resolved, sizeof(task->dst_path) - 1);
  task->dst_path[sizeof(task->dst_path) - 1] = '\0';
  task->is_move = is_move;

  session->copy_in_progress = 1;

  if (session->copy_thread_valid) {
    pthread_join(session->copy_thread, NULL);
  }

  if (pthread_create(&session->copy_thread, NULL, ftp_copy_thread_func, task) !=
      0) {
    session->copy_in_progress = 0;
    session->copy_thread_valid = 0;
    pthread_mutex_unlock(&session->copy_mutex);
    free(task);
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Failed to create background thread.");
  }

  session->copy_thread_valid = 1;
  pthread_mutex_unlock(&session->copy_mutex);

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                is_move ? "Move started in background."
                                        : "Copy started in background.");
}

ftp_error_t cmd_CPFR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: CPFR <path>");
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid source path.");
  }

  if (!pal_path_exists(resolved)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Source does not exist.");
  }

  strncpy(session->copy_from, args, sizeof(session->copy_from) - 1);
  session->copy_from[sizeof(session->copy_from) - 1] = '\0';

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "File exists, ready for destination name.");
}

ftp_error_t cmd_CPTO(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: CPTO <path>");
  }

  if (session->copy_from[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "Bad sequence of commands (use CPFR first).");
  }

  ftp_error_t result = start_async_copy(session, session->copy_from, args, 0);

  /* Clear the copy_from state regardless of success to prevent reuse */
  session->copy_from[0] = '\0';

  return result;
}

ftp_error_t cmd_COPY(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char src_arg[FTP_PATH_MAX];
  char dst_arg[FTP_PATH_MAX];

  const char *space = strchr(args, ' ');
  if (space == NULL) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: COPY <src> <dst>");
  }

  size_t src_len = (size_t)(space - args);
  if (src_len >= sizeof(src_arg)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Paths too long.");
  }

  strncpy(src_arg, args, src_len);
  src_arg[src_len] = '\0';

  const char *dst_start = space + 1;
  while (*dst_start == ' ')
    dst_start++; /* skip extra spaces */

  if (*dst_start == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: COPY <src> <dst>");
  }

  strncpy(dst_arg, dst_start, sizeof(dst_arg) - 1);
  dst_arg[sizeof(dst_arg) - 1] = '\0';

  return start_async_copy(session, src_arg, dst_arg, 0);
}

/*===========================================================================*
 * TRANSFER PARAMETERS
 *===========================================================================*/

/**
 * @brief TYPE command - Set transfer type
 */
ftp_error_t cmd_TYPE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'A') || (args[0] == 'a')) {
    session->transfer_type = FTP_TYPE_ASCII;
  } else if ((args[0] == 'I') || (args[0] == 'i')) {
    session->transfer_type = FTP_TYPE_BINARY;
  } else {
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                  "Type not supported.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, "Type set.");
}

/**
 * @brief MODE command - Set transfer mode
 */
ftp_error_t cmd_MODE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'S') || (args[0] == 's')) {
    session->transfer_mode = FTP_MODE_STREAM;
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "Mode set to Stream.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                "Only Stream mode supported.");
}

/**
 * @brief STRU command - Set file structure
 */
ftp_error_t cmd_STRU(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'F') || (args[0] == 'f')) {
    session->file_structure = FTP_STRU_FILE;
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "Structure set to File.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                "Only File structure supported.");
}

/*===========================================================================*
 * ENCRYPTION (ChaCha20)
 *
 *  AUTH XCRYPT handshake:
 *
 *    Client                          Server
 *    ──────                          ──────
 *    AUTH XCRYPT ──────────────────►
 *                ◄────────────────── 234 XCRYPT <24-hex-nonce>
 *
 *    Both sides derive:
 *      session_key = ChaCha20_KDF(PSK, nonce)
 *
 *    All subsequent traffic is XORed with ChaCha20 keystream.
 *
 *===========================================================================*/

#if FTP_ENABLE_CRYPTO

/**
 * @brief Convert nibble (0-15) to hex character
 */
static char nibble_to_hex(uint8_t n) {
  return (n < 10U) ? (char)('0' + n) : (char)('a' + (n - 10U));
}

/**
 * @brief Generate cryptographic random nonce from /dev/urandom or fallback
 */
static int generate_nonce(uint8_t *buf, size_t len) {
  /*
   * /dev/urandom is available on Linux, macOS.
   * Falls back to time-based PRNG if unavailable.
   */
  int fd = pal_file_open("/dev/urandom", O_RDONLY, 0);
  if (fd >= 0) {
    ssize_t n = pal_file_read(fd, buf, len);
    pal_file_close(fd);
    if (n == (ssize_t)len) {
      return 0;
    }
  }

  /* Fallback: time-based seed (weaker but functional) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t seed = ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
  for (size_t i = 0U; i < len; i++) {
    seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    buf[i] = (uint8_t)(seed >> 33U);
  }
  return 0;
}

ftp_error_t cmd_AUTH(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Only XCRYPT mechanism is supported */
  if ((strcmp(args, "XCRYPT") != 0) && (strcmp(args, "xcrypt") != 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                  "Unsupported AUTH mechanism.");
  }

  /* Already encrypted? */
  if (session->crypto.active != 0U) {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "Already encrypted.");
  }

  /* Generate 12-byte random nonce */
  uint8_t nonce[12];
  (void)generate_nonce(nonce, sizeof(nonce));

  /* Derive session key from PSK + nonce */
  static const uint8_t psk[32] = FTP_CRYPTO_PSK;
  uint8_t session_key[32];
  ftp_crypto_derive_key(psk, nonce, session_key);

  /* Format nonce as hex string for reply */
  char hex_nonce[25]; /* 24 hex chars + NUL */
  for (size_t i = 0U; i < 12U; i++) {
    hex_nonce[i * 2U] = nibble_to_hex((nonce[i] >> 4U) & 0x0FU);
    hex_nonce[(i * 2U) + 1U] = nibble_to_hex(nonce[i] & 0x0FU);
  }
  hex_nonce[24] = '\0';

  /* Reply: 234 XCRYPT <nonce-hex> */
  char reply_msg[64];
  (void)snprintf(reply_msg, sizeof(reply_msg), "XCRYPT %s", hex_nonce);
  ftp_error_t err =
      ftp_session_send_reply(session, FTP_REPLY_234_AUTH_OK, reply_msg);

  if (err == FTP_OK) {
    /* Activate encryption on this session */
    ftp_crypto_init(&session->crypto, session_key, nonce);
    ftp_log_session_event(session, "CRYPTO_ON", FTP_OK, 0U);
  }

  /* Scrub key material from stack */
  volatile uint8_t *vk = (volatile uint8_t *)session_key;
  for (size_t i = 0U; i < sizeof(session_key); i++) {
    vk[i] = 0U;
  }

  return err;
}

#endif /* FTP_ENABLE_CRYPTO */
