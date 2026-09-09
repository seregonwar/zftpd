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
 * @file pal_resilient_server.c
 * @brief Rest-mode resilient accept loop
 */

#include "pal_resilient_server.h"
#include "ftp_log.h"
#include "pal_notification.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#ifndef ENOTSOCK
#define ENOTSOCK EBADF
#endif

/* Probe at most once every 5 seconds while the listen FD looks valid. */
#define PAL_LISTEN_PROBE_INTERVAL_SEC 5

int pal_errno_is_listener_lost(int err) {
  return (err == EBADF) || (err == ENOTSOCK) || (err == EINVAL);
}

unsigned pal_restmode_backoff_ms(unsigned attempt) {
  static const unsigned k_ms[] = {500U, 1000U, 2000U, 4000U, 8000U, 10000U};
  if (attempt >= (unsigned)(sizeof(k_ms) / sizeof(k_ms[0]))) {
    return 10000U;
  }
  return k_ms[attempt];
}

int pal_restmode_sleep_ms(unsigned ms, atomic_int *running_flag) {
  unsigned remaining = ms;
  while (remaining > 0U) {
    if ((running_flag != NULL) && (atomic_load(running_flag) == 0)) {
      return -1;
    }
    unsigned slice = remaining > 100U ? 100U : remaining;
    struct timespec ts;
    ts.tv_sec = (time_t)(slice / 1000U);
    ts.tv_nsec = (long)((slice % 1000U) * 1000000UL);
    (void)nanosleep(&ts, NULL);
    remaining -= slice;
  }
  return 0;
}

int pal_listen_fd_alive(int fd) {
  int type = 0;
  socklen_t len = sizeof(type);

  if (fd < 0) {
    return 0;
  }

  if (getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) != 0) {
    return 0;
  }
  if (type != SOCK_STREAM) {
    return 0;
  }

#ifdef SO_ACCEPTCONN
  {
    int accepting = 0;
    len = sizeof(accepting);
    if (getsockopt(fd, SOL_SOCKET, SO_ACCEPTCONN, &accepting, &len) == 0) {
      return accepting != 0;
    }
  }
#endif

  /* SO_ACCEPTCONN unavailable or failed — SO_TYPE success is best-effort. */
  return 1;
}

static int recreate_listen_socket(int *listen_fd,
                                  const struct sockaddr_in *listen_addr,
                                  unsigned *attempt,
                                  atomic_int *running_flag) {
  char log_msg[160];

  while ((running_flag == NULL) || (atomic_load(running_flag) != 0)) {
    ftp_log_line(FTP_LOG_WARN,
                 "[restmode] Listen socket offline — recreating...");

    /* After a few failures, poke the network stack (no-op on most hosts). */
    if (*attempt > 0U && ((*attempt % 3U) == 0U)) {
      (void)pal_network_reinit();
    }

    int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
      unsigned delay = pal_restmode_backoff_ms(*attempt);
      (void)snprintf(log_msg, sizeof(log_msg),
                     "[restmode] socket() failed (errno=%d). Retry in %u ms",
                     errno, delay);
      ftp_log_line(FTP_LOG_ERROR, log_msg);
      if (pal_restmode_sleep_ms(delay, running_flag) != 0) {
        return -1;
      }
      (*attempt)++;
      continue;
    }

    if (pal_socket_set_reuseaddr(fd) != FTP_OK) {
      PAL_CLOSE(fd);
      unsigned delay = pal_restmode_backoff_ms(*attempt);
      ftp_log_line(FTP_LOG_ERROR,
                   "[restmode] SO_REUSEADDR failed — retrying");
      if (pal_restmode_sleep_ms(delay, running_flag) != 0) {
        return -1;
      }
      (*attempt)++;
      continue;
    }

    if (PAL_BIND(fd, (const struct sockaddr *)listen_addr,
                 sizeof(*listen_addr)) < 0) {
      int err = errno;
      PAL_CLOSE(fd);
      unsigned delay = pal_restmode_backoff_ms(*attempt);
      (void)snprintf(log_msg, sizeof(log_msg),
                     "[restmode] bind() failed (errno=%d: %s). Retry in %u ms",
                     err, strerror(err), delay);
      ftp_log_line(FTP_LOG_ERROR, log_msg);
      if (pal_restmode_sleep_ms(delay, running_flag) != 0) {
        return -1;
      }
      (*attempt)++;
      continue;
    }

    if (PAL_LISTEN(fd, FTP_LISTEN_BACKLOG) < 0) {
      int err = errno;
      PAL_CLOSE(fd);
      unsigned delay = pal_restmode_backoff_ms(*attempt);
      (void)snprintf(log_msg, sizeof(log_msg),
                     "[restmode] listen() failed (errno=%d: %s). Retry in %u ms",
                     err, strerror(err), delay);
      ftp_log_line(FTP_LOG_ERROR, log_msg);
      if (pal_restmode_sleep_ms(delay, running_flag) != 0) {
        return -1;
      }
      (*attempt)++;
      continue;
    }

    *listen_fd = fd;
    *attempt = 0U;
    ftp_log_line(FTP_LOG_INFO,
                 "[restmode] Listen socket recreated successfully");

    {
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
    return 0;
  }
  return -1;
}

int pal_resilient_accept(int *listen_fd,
                         const struct sockaddr_in *listen_addr,
                         struct sockaddr *client_addr,
                         socklen_t *addr_len,
                         atomic_int *running_flag) {
  unsigned recreate_attempt = 0U;
  time_t last_probe = 0;

  if ((listen_fd == NULL) || (listen_addr == NULL)) {
    return -1;
  }

  while ((running_flag == NULL) || (atomic_load(running_flag) != 0)) {
    int fd = *listen_fd;

    if (fd < 0) {
      if (recreate_listen_socket(listen_fd, listen_addr, &recreate_attempt,
                                 running_flag) != 0) {
        break;
      }
      fd = *listen_fd;
      last_probe = time(NULL);
    } else {
      /* Stale-FD probe: FD may survive Rest Mode while the stack is dead. */
      time_t now = time(NULL);
      if ((now != (time_t)-1) &&
          ((last_probe == 0) ||
           ((now > last_probe) &&
            ((uint64_t)(now - last_probe) >=
             (uint64_t)PAL_LISTEN_PROBE_INTERVAL_SEC)))) {
        last_probe = now;
        if (!pal_listen_fd_alive(fd)) {
          ftp_log_line(FTP_LOG_WARN,
                       "[restmode] Listen FD failed liveness probe — "
                       "dismantling");
          PAL_CLOSE(fd);
          *listen_fd = -1;
          continue;
        }
      }
    }

    int client_fd = PAL_ACCEPT(fd, client_addr, addr_len);
    if (client_fd >= 0) {
      recreate_attempt = 0U;
      return client_fd;
    }

    if ((running_flag != NULL) && (atomic_load(running_flag) == 0)) {
      break;
    }

    int err = errno;
    if ((err == EINTR) || (err == EAGAIN) || (err == EWOULDBLOCK)) {
      continue;
    }

    {
      char log_msg[160];
      (void)snprintf(log_msg, sizeof(log_msg),
                     "[restmode] accept() failed (errno=%d: %s)%s — "
                     "dismantling listen socket",
                     err, strerror(err),
                     pal_errno_is_listener_lost(err) ? " [listener lost]" : "");
      ftp_log_line(FTP_LOG_ERROR, log_msg);
    }

    PAL_CLOSE(fd);
    *listen_fd = -1;
  }
  return -1;
}
