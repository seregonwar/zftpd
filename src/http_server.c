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
 * @file http_server.c
 * @brief HTTP server — event-loop-driven, non-blocking connections
 *
 * ARCHITECTURE:
 *
 *   ┌────────────────────────────┐
 *   │  kqueue / epoll event loop │
 *   │                            │
 *   │  listen_fd ──► accept()    │
 *   │                  │         │
 *   │      ┌───────────┘         │
 *   │      ▼                     │
 *   │  client_fd  ──► read()     │
 *   │      ▼                     │
 *   │  parse HTTP request        │
 *   │      ▼                     │
 *   │  route to API / static     │
 *   │      ▼                     │
 *   │  send response headers     │
 *   │  send file (if download)   │
 *   │      ▼                     │
 *   │  close or keep-alive       │
 *   └────────────────────────────┘
 */

#include "http_server.h"
#include "ftp_config.h"
#include "http_api.h"
#include "http_config.h"
#if ENABLE_WEB_UPLOAD
#include "http_csrf.h"
#endif
#include "http_parser.h"
#include "http_response.h"
#include "pal_fileio.h"
#include "pal_network.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h> /* TCP_NODELAY */
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>


/*===========================================================================*
 * INTERNAL TYPES
 *===========================================================================*/

struct http_server {
  event_loop_t *loop;
  int listen_fd;
  uint16_t port;
  atomic_int connection_count;  /* Phase 4: thread-safe counter */
  char root_path[FTP_PATH_MAX]; /* filesystem confinement root */
};

typedef struct {
  http_server_t *server;
  int fd;
  char buffer[HTTP_REQUEST_BUFFER_SIZE];
  size_t buffer_used;
#if ENABLE_WEB_UPLOAD
  int upload_active;
  int upload_fd;
  size_t upload_remaining;
  /*
   * @field upload_chunk_buf
   * Heap-allocated read buffer for streaming uploads.
   * NULL when no upload is active.  Allocated to HTTP_UPLOAD_CHUNK_SIZE
   * at upload start; freed in http_connection_release().
   *
   * WHY: the connection's header buffer (conn->buffer) is only
   * HTTP_REQUEST_BUFFER_SIZE (8 KB).  Reading 8 KB per event-loop
   * iteration limits upload throughput to a few MB/s.  A dedicated
   * 256 KB buffer reduces the required syscall rate by 32× at the
   * same throughput target.
   *
   * ISOLATION: this field is only accessed inside ENABLE_WEB_UPLOAD
   * guards.  FTP paths, download paths, and internal copy are unaffected.
   *
   * @note Thread-safety: NOT thread-safe (single event-loop thread)
   * @note Must be freed (not closed) in http_connection_release()
   */
  uint8_t *upload_chunk_buf;
#endif
} http_connection_t;

static http_server_t g_http_server;
static atomic_int g_http_server_in_use = ATOMIC_VAR_INIT(0);
static http_connection_t g_http_connections[HTTP_MAX_CONNECTIONS];

static void http_connections_init(void) {
  for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
    g_http_connections[i].fd = -1;
    g_http_connections[i].server = NULL;
    g_http_connections[i].buffer_used = 0;
#if ENABLE_WEB_UPLOAD
    g_http_connections[i].upload_active = 0;
    g_http_connections[i].upload_fd = -1;
    g_http_connections[i].upload_remaining = 0;
    g_http_connections[i].upload_chunk_buf = NULL;
#endif
  }
}

static http_connection_t *http_connection_acquire(http_server_t *server,
                                                  int client_fd) {
  if ((server == NULL) || (client_fd < 0)) {
    return NULL;
  }
  for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
    if (g_http_connections[i].fd < 0) {
      http_connection_t *conn = &g_http_connections[i];
      memset(conn, 0, sizeof(*conn));
      conn->server = server;
      conn->fd = client_fd;
      conn->buffer_used = 0;
#if ENABLE_WEB_UPLOAD
      conn->upload_active = 0;
      conn->upload_fd = -1;
      conn->upload_remaining = 0;
#endif
      return conn;
    }
  }
  return NULL;
}

static void http_connection_release(http_connection_t *conn) {
  if (conn == NULL) {
    return;
  }
#if ENABLE_WEB_UPLOAD
  if (conn->upload_fd >= 0) {
    (void)pal_file_close(conn->upload_fd);
    conn->upload_fd = -1;
  }
  conn->upload_active = 0;
  conn->upload_remaining = 0;
  /* Free the upload read buffer if it was allocated */
  if (conn->upload_chunk_buf != NULL) {
    free(conn->upload_chunk_buf);
    conn->upload_chunk_buf = NULL;
  }
#endif
  conn->fd = -1;
  conn->server = NULL;
  conn->buffer_used = 0;
}

/*===========================================================================*
 * FORWARD DECLARATIONS
 *===========================================================================*/

static int http_accept_callback(int fd, uint32_t events, void *data);
static int http_client_callback(int fd, uint32_t events, void *data);
static int http_handle_request(http_connection_t *conn);
static void http_close_connection(http_connection_t *conn);

/*===========================================================================*
 * SET NON-BLOCKING
 *===========================================================================*/

static int set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    return -1;
  }
  return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

static int http_parse_basic_request(const char *buf, char *method,
                                    size_t method_cap, char *uri,
                                    size_t uri_cap, size_t *header_len,
                                    size_t *content_length) {
  if ((buf == NULL) || (method == NULL) || (uri == NULL) ||
      (header_len == NULL) || (content_length == NULL)) {
    return -1;
  }

  const char *end = strstr(buf, "\r\n\r\n");
  if (end == NULL) {
    return -1;
  }
  *header_len = (size_t)(end - buf) + 4U;

  const char *line_end = strstr(buf, "\r\n");
  if (line_end == NULL) {
    return -1;
  }

  const char *sp1 = strchr(buf, ' ');
  if ((sp1 == NULL) || (sp1 >= line_end)) {
    return -1;
  }
  const char *sp2 = strchr(sp1 + 1, ' ');
  if ((sp2 == NULL) || (sp2 >= line_end)) {
    return -1;
  }

  size_t mlen = (size_t)(sp1 - buf);
  if ((mlen == 0U) || (mlen >= method_cap)) {
    return -1;
  }
  memcpy(method, buf, mlen);
  method[mlen] = '\0';

  size_t ulen = (size_t)(sp2 - (sp1 + 1));
  if ((ulen == 0U) || (ulen >= uri_cap)) {
    return -1;
  }
  memcpy(uri, sp1 + 1, ulen);
  uri[ulen] = '\0';

  *content_length = 0U;
  const char *p = line_end + 2;
  while ((p < end) && (p[0] != '\0')) {
    const char *eol = strstr(p, "\r\n");
    if ((eol == NULL) || (eol > end)) {
      break;
    }
    if (eol == p) {
      break;
    }

    if (strncasecmp(p, "Content-Length:", 15) == 0) {
      const char *v = p + 15;
      while ((*v == ' ') || (*v == '\t')) {
        v++;
      }
      unsigned long long cl = strtoull(v, NULL, 10);
      *content_length = (size_t)cl;
    }

    p = eol + 2;
  }

  return 0;
}

#if ENABLE_WEB_UPLOAD

/**
 * @brief Recursively create directories for a given path.
 *
 * Creates all intermediate directories in the path if they don't exist.
 * Returns 0 on success, -1 on error.
 */
static int mkdir_recursive(const char *path) {
  if (path == NULL || path[0] == '\0') {
    return -1;
  }

  char buf[1024];
  size_t len = strlen(path);
  if (len >= sizeof(buf)) {
    return -1;
  }
  strcpy(buf, path);

  for (size_t i = 1; i < len; i++) {
    if (buf[i] == '/') {
      buf[i] = '\0';
      struct stat st;
      if (stat(buf, &st) != 0) {
        if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
          return -1;
        }
      }
      buf[i] = '/';
    }
  }

  struct stat st;
  if (stat(buf, &st) != 0) {
    if (mkdir(buf, 0777) != 0 && errno != EEXIST) {
      return -1;
    }
  }

  return 0;
}

static int url_decode_component(const char *in, char *out, size_t out_cap) {
  if ((in == NULL) || (out == NULL) || (out_cap < 2U)) {
    return -1;
  }

  size_t in_pos = 0U;
  size_t out_pos = 0U;

  while ((in[in_pos] != '\0') && (in[in_pos] != '&') &&
         (out_pos < (out_cap - 1U))) {
    unsigned char ch = (unsigned char)in[in_pos];
    if ((ch == '%') && (in[in_pos + 1] != '\0') && (in[in_pos + 2] != '\0')) {
      unsigned char hi = (unsigned char)in[in_pos + 1];
      unsigned char lo = (unsigned char)in[in_pos + 2];
      unsigned int v_hi;
      unsigned int v_lo;

      if ((hi >= '0') && (hi <= '9')) {
        v_hi = (unsigned int)(hi - '0');
      } else if ((hi >= 'A') && (hi <= 'F')) {
        v_hi = 10U + (unsigned int)(hi - 'A');
      } else if ((hi >= 'a') && (hi <= 'f')) {
        v_hi = 10U + (unsigned int)(hi - 'a');
      } else {
        v_hi = 0xFFFFFFFFU;
      }

      if ((lo >= '0') && (lo <= '9')) {
        v_lo = (unsigned int)(lo - '0');
      } else if ((lo >= 'A') && (lo <= 'F')) {
        v_lo = 10U + (unsigned int)(lo - 'A');
      } else if ((lo >= 'a') && (lo <= 'f')) {
        v_lo = 10U + (unsigned int)(lo - 'a');
      } else {
        v_lo = 0xFFFFFFFFU;
      }

      if ((v_hi != 0xFFFFFFFFU) && (v_lo != 0xFFFFFFFFU)) {
        unsigned char decoded = (unsigned char)((v_hi << 4U) | v_lo);
        if (decoded == '\0') {
          return -1;
        }
        out[out_pos++] = (char)decoded;
        in_pos += 3U;
        continue;
      }
    }

    if (ch == '+') {
      out[out_pos++] = ' ';
    } else {
      out[out_pos++] = (char)ch;
    }
    in_pos++;
  }

  out[out_pos] = '\0';
  return 0;
}

static int get_query_param(const char *uri, const char *key, char *out,
                           size_t out_cap) {
  if ((uri == NULL) || (key == NULL) || (out == NULL) || (out_cap < 2U)) {
    return -1;
  }

  const char *q = strchr(uri, '?');
  if (q == NULL) {
    return -1;
  }
  q++;

  char pattern[64];
  (void)snprintf(pattern, sizeof(pattern), "%s=", key);
  const char *p = strstr(q, pattern);
  if (p == NULL) {
    return -1;
  }
  p += strlen(pattern);

  if (url_decode_component(p, out, out_cap) != 0) {
    return -1;
  }
  if (out[0] == '\0') {
    return -1;
  }
  return 0;
}

static int is_safe_path_local(const char *path) {
  if (path == NULL || path[0] != '/') {
    return 0;
  }
  if (strstr(path, "//") != NULL) {
    return 0;
  }
  const char *p = path;
  while (*p != '\0') {
    if ((p[0] == '.') && (p[1] == '.')) {
      if ((p == path) || (p[-1] == '/')) {
        return 0;
      }
    }
    p++;
  }
  return 1;
}

static int is_safe_filename_local(const char *name) {
  if ((name == NULL) || (name[0] == '\0')) {
    return 0;
  }
  if (strstr(name, "..") != NULL) {
    return 0;
  }
  for (const char *p = name; *p != '\0'; p++) {
    if ((*p == '/') || (*p == '\\')) {
      return 0;
    }
  }
  return 1;
}
#endif

/*===========================================================================*
 * CREATE / DESTROY
 *===========================================================================*/

http_server_t *http_server_create(event_loop_t *loop, uint16_t port,
                                  const char *root_path) {
  if ((loop == NULL) || (root_path == NULL)) {
    return NULL;
  }

  if (atomic_load(&g_http_server_in_use) != 0) {
    return NULL;
  }

  memset(&g_http_server, 0, sizeof(g_http_server));
  g_http_server.listen_fd = -1;
  g_http_server.loop = loop;
  g_http_server.port = port;
  atomic_store(&g_http_server.connection_count, 0);

  /* Store root path for filesystem confinement */
  size_t rlen = strlen(root_path);
  if (rlen >= sizeof(g_http_server.root_path)) {
    return NULL;
  }
  memcpy(g_http_server.root_path, root_path, rlen + 1U);

  /* Propagate root to API layer */
  http_api_set_root(root_path);

  http_connections_init();

  /* Create TCP listen socket */
  g_http_server.listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_http_server.listen_fd < 0) {
    return NULL;
  }

  int reuse = 1;
  setsockopt(g_http_server.listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
             sizeof(reuse));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  if (bind(g_http_server.listen_fd, (struct sockaddr *)&addr, sizeof(addr)) <
      0) {
    close(g_http_server.listen_fd);
    g_http_server.listen_fd = -1;
    return NULL;
  }

  if (listen(g_http_server.listen_fd, 128) < 0) {
    close(g_http_server.listen_fd);
    g_http_server.listen_fd = -1;
    return NULL;
  }

  /* Non-blocking accept */
  (void)set_nonblocking(g_http_server.listen_fd);

  /* Register with event loop */
  if (event_loop_add(loop, g_http_server.listen_fd, EVENT_READ,
                     http_accept_callback, &g_http_server) != 0) {
    close(g_http_server.listen_fd);
    g_http_server.listen_fd = -1;
    return NULL;
  }

  atomic_store(&g_http_server_in_use, 1);
  return &g_http_server;
}

void http_server_destroy(http_server_t *server) {
  if (server != NULL) {
    if (server == &g_http_server) {
      for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
        if (g_http_connections[i].fd >= 0) {
          http_close_connection(&g_http_connections[i]);
        }
      }
      if (server->listen_fd >= 0) {
        event_loop_remove(server->loop, server->listen_fd);
        close(server->listen_fd);
        server->listen_fd = -1;
      }
      atomic_store(&g_http_server_in_use, 0);
    }
  }
}

/*===========================================================================*
 * ACCEPT CALLBACK — new client connecting
 *===========================================================================*/

static int http_accept_callback(int fd, uint32_t events, void *data) {
  http_server_t *server = (http_server_t *)data;
  (void)events;

  struct sockaddr_in client_addr;
  socklen_t addr_len = sizeof(client_addr);

  int client_fd = accept(fd, (struct sockaddr *)&client_addr, &addr_len);
  if (client_fd < 0) {
    return 0; /* EAGAIN or error, keep listening */
  }

  /*
   * SOCKET TUNING FOR UPLOAD THROUGHPUT
   *
   * TCP_NODELAY
   *   Disable Nagle's algorithm on the server's outgoing path.
   *   Nagle batches small writes, adding up to 200 ms of latency for
   *   ACKs and HTTP response headers.  Disabling it ensures the 200-byte
   *   "HTTP/1.1 200 OK" response after upload completes is sent immediately
   *   rather than waiting for the kernel to accumulate more data.
   *   Has no effect on incoming data (upload body direction).
   *
   * SO_RCVBUF
   *   Hint the kernel to allocate a 2 MB receive buffer for this socket.
   *   A larger receive buffer allows the kernel to acknowledge incoming
   *   data in larger batches, keeping the sender's congestion window open
   *   between event-loop wakeups.  Without this, the default receive buffer
   *   (~87 KB on Linux, ~256 KB on FreeBSD) can be drained faster than the
   *   event loop wakes up, forcing the remote sender to pause.
   *
   *   IMPORTANT: setsockopt(SO_RCVBUF) is a hint.  The kernel caps it at
   *   net.core.rmem_max (Linux) or kern.ipc.maxsockbuf (FreeBSD/PS5) and
   *   silently ignores requests above the system maximum.  Setting it here
   *   is therefore always safe — worst case it has no effect.
   *
   * @note These options apply to ALL HTTP client connections, not just
   *       uploads.  TCP_NODELAY is universally beneficial for request/
   *       response HTTP.  The SO_RCVBUF hint is harmless for short API
   *       requests (the kernel won't actually allocate the full buffer
   *       until data arrives).
   *
   * @note Thread-safety: called only in the accept callback (single
   *       event-loop thread).
   */
  {
    int nodelay = 1;
    (void)setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY,
                     &nodelay, sizeof(nodelay));

    int rcvbuf = (int)HTTP_UPLOAD_RCVBUF_SIZE;
    (void)setsockopt(client_fd, SOL_SOCKET, SO_RCVBUF,
                     &rcvbuf, sizeof(rcvbuf));
  }

  /* Connection limit */
  if (atomic_load(&server->connection_count) >= HTTP_MAX_CONNECTIONS) {
    close(client_fd);
    return 0;
  }

  http_connection_t *conn = http_connection_acquire(server, client_fd);
  if (conn == NULL) {
    close(client_fd);
    return 0;
  }

  (void)atomic_fetch_add(&server->connection_count, 1);

  /* Register for read events */
  event_loop_add(server->loop, client_fd, EVENT_READ, http_client_callback,
                 conn);

  return 0;
}

/*===========================================================================*
 * CLIENT CALLBACK — data available on client socket
 *===========================================================================*/

static int http_client_callback(int fd, uint32_t events, void *data) {
  http_connection_t *conn = (http_connection_t *)data;
  (void)fd;

  /* Connection closed or error */
  if (events & (EVENT_CLOSE | EVENT_ERROR)) {
    http_close_connection(conn);
    return -1;
  }

  if (events & EVENT_READ) {
#if ENABLE_WEB_UPLOAD
    if (conn->upload_active != 0) {
      /*
       * UPLOAD STREAMING READ
       *
       * Use the pre-allocated upload_chunk_buf (HTTP_UPLOAD_CHUNK_SIZE =
       * 256 KB) instead of conn->buffer (HTTP_REQUEST_BUFFER_SIZE = 8 KB).
       *
       * WHY: reading 8 KB per event-loop iteration caps throughput because
       * each iteration requires a kqueue/epoll round-trip (typically
       * 5-20 µs).  At 8 KB × 50 000 wakeups/s the ceiling is ~400 MB/s in
       * theory, but in practice kernel scheduling overhead and interrupt
       * coalescing bring the real ceiling much lower (measured ~5 MB/s).
       *
       * At 256 KB the required syscall rate for 113 MB/s drops to ~440/s,
       * well within the event-loop budget.  Each read() drains a much
       * larger window of TCP receive-buffer data in one call, so the kernel
       * spends far more time in DMA and far less in context switches.
       *
       * FALLBACK: if malloc failed during upload initialisation,
       * upload_chunk_buf is NULL and we fall back to conn->buffer (8 KB).
       * This preserves correctness at the cost of performance.
       *
       * ISOLATION: only active when upload_active != 0.  All other HTTP
       * paths (headers, download, API) continue to use conn->buffer.
       * FTP and internal copy are completely unaffected.
       *
       * @pre  conn->upload_chunk_buf allocated at upload start (or NULL)
       * @pre  conn->upload_remaining > 0
       * @post conn->upload_remaining decremented by bytes written to file
       */
      uint8_t *rd_buf  = (conn->upload_chunk_buf != NULL)
                             ? conn->upload_chunk_buf
                             : (uint8_t *)conn->buffer;
      size_t   rd_cap  = (conn->upload_chunk_buf != NULL)
                             ? (size_t)HTTP_UPLOAD_CHUNK_SIZE
                             : sizeof(conn->buffer);

      ssize_t n = read(conn->fd, rd_buf, rd_cap);
      if (n <= 0) {
        http_close_connection(conn);
        return -1;
      }

      size_t got = (size_t)n;
      if (got > conn->upload_remaining) {
        got = conn->upload_remaining;
      }

      if ((got > 0U) && (conn->upload_fd >= 0)) {
        /*
         * DIRECT WRITE — bypass pal_file_write_all()
         *
         * pal_file_write_all() subdivides writes into PAL_FILE_WRITE_CHUNK_MAX
         * (128 KB on PS5, 64 KB on PS4).  That limit exists for FTP STOR to
         * prevent TCP recv-buffer stalls while the kernel is busy with a long
         * write: the FTP STOR loop reads from the socket and writes to disk
         * serially, so a slow 4 MB write would stall socket reads long enough
         * to fill the TCP window and trigger a client inactivity timeout.
         *
         * HTTP upload is different:
         *   - We already have the full chunk (up to 256 KB) in rd_buf.
         *   - The socket read and the disk write are NOT interleaved inside a
         *     single loop iteration, so TCP flow control is not a concern.
         *   - Writing 256 KB in a single write() call instead of two 128 KB
         *     calls halves the number of PFS AES-XTS context setups per MB.
         *     This is the primary reason /data uploads were capped at ~25 MB/s
         *     while USB (no encryption) reached 80 MB/s.
         *
         * We reproduce the same direct-write loop used by pal_file_copy_atomic
         * (which bypasses pal_file_write_all for identical reasons) and handle
         * the PS4/PS5 PFS silent-ENOSPC (write() == 0) quirk.
         *
         * @pre  rd_buf[0..got-1] contains valid data to write
         * @pre  conn->upload_fd is a valid open writable file descriptor
         */
        const uint8_t *wr_p   = rd_buf;
        size_t         wr_rem = got;
        int            wr_ok  = 1;

        while (wr_rem > 0U) {
          ssize_t w = write(conn->upload_fd, wr_p, wr_rem);
          if (w > 0) {
            wr_p   += (size_t)w;
            wr_rem -= (size_t)w;
            continue;
          }
          if ((w < 0) && (errno == EINTR)) {
            continue;
          }
          /* w == 0: PS4/PS5 PFS silent ENOSPC */
          wr_ok = 0;
          break;
        }

        if (wr_ok == 0) {
          http_close_connection(conn);
          return -1;
        }
      }

      conn->upload_remaining -= got;
      if (conn->upload_remaining == 0U) {
        if (conn->upload_fd >= 0) {
          (void)pal_file_close(conn->upload_fd);
          conn->upload_fd = -1;
        }

        http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
        if (resp != NULL) {
          http_response_add_header(resp, "Content-Type", "application/json");
          http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
          const char *body = "{\"ok\":true}";
          http_response_set_body(resp, body, strlen(body));
          if (resp->used > 0) {
            size_t total = 0;
            size_t remain = resp->used;
            while (remain > 0) {
              ssize_t sent = write(conn->fd, resp->data + total, remain);
              if (sent <= 0) {
                break;
              }
              total += (size_t)sent;
              remain -= (size_t)sent;
            }
          }
          http_response_destroy(resp);
        }

        http_close_connection(conn);
        return -1;
      }

      return 0;
    }
#endif

    size_t remaining = sizeof(conn->buffer) - conn->buffer_used - 1;
    if (remaining == 0) {
      /* Buffer full without complete request — drop */
      http_close_connection(conn);
      return -1;
    }

    ssize_t n = read(conn->fd, conn->buffer + conn->buffer_used, remaining);

    if (n <= 0) {
      http_close_connection(conn);
      return -1;
    }

    conn->buffer_used += (size_t)n;
    conn->buffer[conn->buffer_used] = '\0';

    /* Check for complete HTTP request (headers end with \r\n\r\n) */
    const char *end = strstr(conn->buffer, "\r\n\r\n");
    if (end != NULL) {
      char method[8];
      char uri[HTTP_URI_MAX_LENGTH];
      size_t header_len = 0U;
      size_t content_length = 0U;

      if (http_parse_basic_request(conn->buffer, method, sizeof(method), uri,
                                   sizeof(uri), &header_len,
                                   &content_length) != 0) {
        http_close_connection(conn);
        return -1;
      }

#if ENABLE_WEB_UPLOAD
      if ((strcmp(method, "POST") == 0) &&
          (strncmp(uri, "/api/upload", 11) == 0)) {
        http_request_t up_req;
        if ((http_parse_request(conn->buffer, conn->buffer_used, &up_req) <
             0) ||
            (http_csrf_validate(&up_req) != 0)) {
          http_response_t *resp =
              http_response_create(HTTP_STATUS_403_FORBIDDEN);
          if (resp != NULL) {
            http_response_add_header(resp, "Content-Type", "application/json");
            http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
            const char *body = "{\"error\":\"Invalid or missing CSRF token\"}";
            http_response_set_body(resp, body, strlen(body));
            if (resp->used > 0) {
              (void)write(conn->fd, resp->data, resp->used);
            }
            http_response_destroy(resp);
          }
          http_close_connection(conn);
          return -1;
        }

        if (content_length == 0U) {
          http_response_t *resp =
              http_response_create(HTTP_STATUS_400_BAD_REQUEST);
          if (resp != NULL) {
            const char *msg = "Missing Content-Length";
            http_response_set_body(resp, msg, strlen(msg));
            if (resp->used > 0) {
              (void)write(conn->fd, resp->data, resp->used);
            }
            http_response_destroy(resp);
          }
          http_close_connection(conn);
          return -1;
        }

        char dir_path[1024];
        char file_name[256];
        if ((get_query_param(uri, "path", dir_path, sizeof(dir_path)) != 0) ||
            (get_query_param(uri, "name", file_name, sizeof(file_name)) != 0)) {
          http_close_connection(conn);
          return -1;
        }
        if (!is_safe_path_local(dir_path) ||
            !is_safe_filename_local(file_name)) {
          http_close_connection(conn);
          return -1;
        }

        char full[1024];
        if (strcmp(dir_path, "/") == 0) {
          (void)snprintf(full, sizeof(full), "/%s", file_name);
        } else {
          (void)snprintf(full, sizeof(full), "%s/%s", dir_path, file_name);
        }
        if (!is_safe_path_local(full)) {
          http_close_connection(conn);
          return -1;
        }

        /*
         * VULN-02 fix: confine upload path to the HTTP root
         *
         *   is_safe_path_local()  blocks ".." and "//"
         *   http_api_get_root()   confines to server root directory
         */
        {
          const char *http_root = http_api_get_root();
          if (http_root[0] != '\0') {
            size_t rlen = strlen(http_root);
            /* root "/" allows everything */
            if (!(rlen == 1U && http_root[0] == '/')) {
              if (strncmp(full, http_root, rlen) != 0 ||
                  (full[rlen] != '/' && full[rlen] != '\0')) {
                http_close_connection(conn);
                return -1;
              }
            }
          }
        }

        /* Create intermediate directories if needed (for folder uploads) */
        char dir_buf[1024];
        const char *last_slash = strrchr(full, '/');
        if (last_slash != NULL && last_slash != full) {
          size_t dir_len = (size_t)(last_slash - full);
          if (dir_len < sizeof(dir_buf)) {
            strncpy(dir_buf, full, dir_len);
            dir_buf[dir_len] = '\0';
            (void)mkdir_recursive(dir_buf);
          }
        }

        int out_fd = pal_file_open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out_fd < 0) {
          http_close_connection(conn);
          return -1;
        }
        conn->upload_fd = out_fd;
        conn->upload_active = 1;

        /*
         * Allocate the large upload read buffer (HTTP_UPLOAD_CHUNK_SIZE =
         * 256 KB).  If malloc fails we fall back to conn->buffer (8 KB) —
         * correctness is preserved, only throughput is affected.
         *
         * The buffer is freed in http_connection_release() regardless of
         * how the connection terminates (success, error, or timeout).
         */
        if (conn->upload_chunk_buf == NULL) {
          conn->upload_chunk_buf = (uint8_t *)malloc(HTTP_UPLOAD_CHUNK_SIZE);
          /* malloc failure is non-fatal: fallback path uses conn->buffer */
        }

        size_t in_buf = 0U;
        if (conn->buffer_used > header_len) {
          in_buf = conn->buffer_used - header_len;
          if (in_buf > content_length) {
            in_buf = content_length;
          }
        }

        if (in_buf > 0U) {
          if (pal_file_write_all(out_fd, conn->buffer + header_len, in_buf) <
              0) {
            http_close_connection(conn);
            return -1;
          }
        }

        conn->upload_remaining = content_length - in_buf;
        conn->buffer_used = 0;
        conn->buffer[0] = '\0';

        if (conn->upload_remaining == 0U) {
          (void)pal_file_close(conn->upload_fd);
          conn->upload_fd = -1;
          conn->upload_active = 0;
          http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
          if (resp != NULL) {
            http_response_add_header(resp, "Content-Type", "application/json");
            http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
            const char *body = "{\"ok\":true}";
            http_response_set_body(resp, body, strlen(body));
            if (resp->used > 0) {
              (void)write(conn->fd, resp->data, resp->used);
            }
            http_response_destroy(resp);
          }
          http_close_connection(conn);
          return -1;
        }

        return 0;
      }
#endif

      if (content_length > 0U) {
        /*
         * OVERFLOW-SAFE size check.
         *
         * WHY: a malicious client can send
         *   Content-Length: 18446744073709551615  (SIZE_MAX on 64-bit)
         * Without the pre-addition guard, (header_len + SIZE_MAX) wraps to
         * (header_len - 1) via unsigned overflow, which is LESS than the
         * buffer limit.  Both the "too large" rejection and the "wait for
         * more data" guard would then pass silently, dispatching a request
         * to the handler with a fabricated body pointer.
         *
         * Fix: reject if content_length alone already exceeds the available
         * space before performing the addition.  This makes overflow
         * impossible because after the guard content_length <=
         * (sizeof(conn->buffer) - 1), and header_len is always < that same
         * limit (we have already confirmed \r\n\r\n fits inside the buffer).
         *
         * @pre  header_len > 0 (guaranteed by the strstr("\r\n\r\n") check)
         * @post content_length + header_len <= SIZE_MAX (no wrap possible)
         */
        const size_t buf_limit = sizeof(conn->buffer) - 1U;
        if (content_length > buf_limit ||
            (header_len + content_length) > buf_limit) {
          http_response_t *resp =
              http_response_create(HTTP_STATUS_400_BAD_REQUEST);
          if (resp != NULL) {
            const char *msg = "Request too large";
            http_response_set_body(resp, msg, strlen(msg));
            if (resp->used > 0) {
              (void)write(conn->fd, resp->data, resp->used);
            }
            http_response_destroy(resp);
          }
          http_close_connection(conn);
          return -1;
        }

        if (conn->buffer_used < (header_len + content_length)) {
          return 0;
        }
      }

      (void)http_handle_request(conn);
      http_close_connection(conn);
      return -1;
    }
  }

  return 0;
}

/*===========================================================================*
 * HANDLE REQUEST — parse, route, respond
 *===========================================================================*/

static int http_handle_request(http_connection_t *conn) {
  http_request_t request;
  if (http_parse_request(conn->buffer, conn->buffer_used, &request) < 0) {
    return -1;
  }

  http_response_t *response = http_api_handle(&request);
  if (response == NULL) {
    response = http_response_create(HTTP_STATUS_500_INTERNAL_ERROR);
    if (response == NULL) {
      return -1;
    }
    const char *msg = "Internal Server Error";
    http_response_set_body(response, msg, strlen(msg));
  }

  /* Send response headers (+ body if no sendfile) */
  if (response->used > 0) {
    if (pal_send_all(conn->fd, response->data, response->used, 0) < 0) {
      http_response_destroy(response);
      return -1;
    }
  }

  /*
   *  ┌────────────────────────────────────────────────────┐
   *  │  MEMORY BODY PATH — stream embedded static assets   │
   *  └────────────────────────────────────────────────────┘
   */
  if ((response->mem_seg_count > 0U) &&
      (response->mem_seg_index < response->mem_seg_count)) {
    while (response->mem_seg_index < response->mem_seg_count) {
      const void *seg = response->mem_segs[response->mem_seg_index];
      size_t seg_len = response->mem_lens[response->mem_seg_index];
      if ((seg == NULL) || (seg_len == 0U)) {
        response->mem_seg_index++;
        response->mem_seg_sent = 0U;
        continue;
      }
      if (response->mem_seg_sent >= seg_len) {
        response->mem_seg_index++;
        response->mem_seg_sent = 0U;
        continue;
      }
      const unsigned char *p = (const unsigned char *)seg;
      const unsigned char *start = p + response->mem_seg_sent;
      size_t remaining = seg_len - response->mem_seg_sent;
      if (pal_send_all(conn->fd, start, remaining, 0) < 0) {
        http_response_destroy(response);
        return -1;
      }
      response->mem_seg_sent = seg_len;
    }
  }

  if ((response->mem_body != NULL) &&
      (response->mem_sent < response->mem_length)) {
    const unsigned char *p = (const unsigned char *)response->mem_body;
    const unsigned char *start = p + response->mem_sent;
    size_t remaining = response->mem_length - response->mem_sent;
    if (pal_send_all(conn->fd, start, remaining, 0) < 0) {
      http_response_destroy(response);
      return -1;
    }
    response->mem_sent = response->mem_length;
  }

  /*
   *  ┌────────────────────────────────────────┐
   *  │  SENDFILE PATH — stream file content   │
   *  │  Used by /api/download                 │
   *  └────────────────────────────────────────┘
   */
  /*
   *  ┌────────────────────────────────────────┐
   *  │  SENDFILE PATH — stream file content   │
   *  │  Used by /api/download                 │
   *  └────────────────────────────────────────┘
   */
  if (response->sendfile_fd >= 0) {
    /*
     * FILE DOWNLOAD — two paths based on filesystem safety
     *
     * PATH A: sendfile_safe == 1  (Linux, macOS, FreeBSD ufs/zfs)
     *   pal_sendfile() → zero-copy DMA from page-cache to NIC.
     *
     * PATH B: sendfile_safe == 0  (PS5/PS4 exFAT, PFS, nullfs, msdosfs)
     *   pread() + pal_send_all() — explicit userspace copy.
     *
     *   WHY: On PS5/PS4 (FreeBSD), calling sendfile(2) on exFAT, msdosfs,
     *   nullfs, pfsmnt or pfs vnodes dereferences a null pager function
     *   pointer inside the kernel and causes an IMMEDIATE KERNEL PANIC.
     *   errno is never set — execution never returns to userspace.
     *   The EINVAL fallback in pal_sendfile() therefore never executes.
     *   The ONLY safe solution is to never call sendfile() on these FSes.
     *
     *   api_download() (http_api.c) sets sendfile_safe via fstatfs() on
     *   the open fd before returning the http_response_t to us.
     */
    off_t  sf_offset    = (off_t)response->sendfile_offset;
    size_t sf_remaining = response->sendfile_count;

    if (response->sendfile_safe) {
      /* PATH A: zero-copy sendfile */
      while (sf_remaining > 0U) {
        size_t chunk = sf_remaining;
        if (chunk > (size_t)HTTP_SENDFILE_CHUNK_SIZE) {
          chunk = (size_t)HTTP_SENDFILE_CHUNK_SIZE;
        }
        ssize_t sent = pal_sendfile(conn->fd, response->sendfile_fd,
                                    &sf_offset, chunk);
        if (sent < 0) { break; }
        if (sent == 0) {
          if (errno == EINTR) { continue; }
          break;
        }
        sf_remaining -= (size_t)sent;
      }

    } else {
      /* PATH B: pread + send_all (PS5/PS4 safe) */
#ifndef HTTP_DOWNLOAD_PREAD_CHUNK
#define HTTP_DOWNLOAD_PREAD_CHUNK (512U * 1024U)
#endif
      /*
       * Heap-allocate the read buffer — 512 KB on the stack would
       * overflow the event-loop thread's stack on PS5.
       * On malloc failure, fall back to a small on-stack buffer so the
       * transfer degrades in speed rather than aborting.
       */
      uint8_t  dl_stack[4096];
      uint8_t *dl_buf = (uint8_t *)malloc(HTTP_DOWNLOAD_PREAD_CHUNK);
      size_t   dl_cap = (dl_buf != NULL)
                            ? (size_t)HTTP_DOWNLOAD_PREAD_CHUNK
                            : sizeof(dl_stack);
      if (dl_buf == NULL) {
        dl_buf = dl_stack;
      }

      int dl_err = 0;
      while (sf_remaining > 0U) {
        size_t  want = (sf_remaining < dl_cap) ? sf_remaining : dl_cap;
        ssize_t nr   = pread(response->sendfile_fd, dl_buf, want,
                             sf_offset);
        if (nr <= 0) {
          if ((nr < 0) && (errno == EINTR)) { continue; }
          dl_err = 1;
          break;
        }
        if (pal_send_all(conn->fd, dl_buf, (size_t)nr, 0) < 0) {
          dl_err = 1;
          break;
        }
        sf_offset    += (off_t)nr;
        sf_remaining -= (size_t)nr;
      }
      (void)dl_err;

      if (dl_buf != dl_stack) {
        free(dl_buf);
      }
    }

    close(response->sendfile_fd);
    response->sendfile_fd = -1;
  }

  /*
   *  ┌─────────────────────────────────────────────────────────┐
   *  │  CHUNKED STREAMING — for large directories (/api/list)  │
   *  └─────────────────────────────────────────────────────────┘
   */
  if (response->stream_dir != NULL) {
    DIR *dir = (DIR *)response->stream_dir;
    struct dirent *entry;
    char buffer[4096];
    int first = 1;

    /*
     * Iterate over directory entries and send them as chunks.
     * We don't need a huge buffer; we just send one entry at a time.
     */
    while ((entry = readdir(dir)) != NULL) {
      if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
        continue;
      }
      if ((strcmp(response->stream_path, "/") == 0) &&
          ((strcmp(entry->d_name, "dev") == 0) ||
           (strcmp(entry->d_name, "proc") == 0) ||
           (strcmp(entry->d_name, "sys") == 0) ||
           (strcmp(entry->d_name, "kern") == 0))) {
        continue;
      }

      char fullpath[2048];
      snprintf(fullpath, sizeof(fullpath), "%s/%s", response->stream_path,
               entry->d_name);

      struct stat st;
      if (stat(fullpath, &st) < 0) {
        continue;
      }

      /* Format JSON entry: ,{"name":"...","type":"...","size":...} */
      size_t pos = 0;
      if (!first) {
        buffer[pos++] = ',';
      }
      first = 0;

      pos +=
          (size_t)snprintf(buffer + pos, sizeof(buffer) - pos, "{\"name\":\"");

      /* Simple escaping for now — relying on snprintf not to overflow */
      for (const char *p = entry->d_name; *p && pos < sizeof(buffer) - 64;
           p++) {
        if (*p == '"' || *p == '\\') {
          buffer[pos++] = '\\';
        }
        buffer[pos++] = *p;
      }

      long long entry_size = S_ISDIR(st.st_mode)
                             ? (long long)st.st_blocks * 512LL
                             : (long long)st.st_size;
      pos += (size_t)snprintf(buffer + pos, sizeof(buffer) - pos,
                              "\",\"type\":\"%s\",\"size\":%lld}",
                              S_ISDIR(st.st_mode) ? "directory" : "file",
                              entry_size);

      /* Send chunk: <hex-len>\r\n<data>\r\n */
      char chunk_header[32];
      int header_len =
          snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", pos);

      if (pal_send_all(conn->fd, chunk_header, (size_t)header_len, 0) < 0)
        break;
      if (pal_send_all(conn->fd, buffer, pos, 0) < 0)
        break;
      if (pal_send_all(conn->fd, "\r\n", 2, 0) < 0)
        break;
    }

    closedir(dir);
    response->stream_dir = NULL;

    /* Send closing JSON: ]} */
    const char *closer = "]}";
    size_t closer_len = strlen(closer);
    char chunk_header[32];
    int header_len =
        snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", closer_len);
    (void)pal_send_all(conn->fd, chunk_header, (size_t)header_len, 0);
    (void)pal_send_all(conn->fd, closer, closer_len, 0);
    (void)pal_send_all(conn->fd, "\r\n", 2, 0);

    /* End of stream: 0\r\n\r\n */
    (void)pal_send_all(conn->fd, "0\r\n\r\n", 5, 0);
  }

  http_response_destroy(response);
  return 0;
}

/*===========================================================================*
 * CLOSE CONNECTION — cleanup resources
 *===========================================================================*/

static void http_close_connection(http_connection_t *conn) {
  if (conn == NULL) {
    return;
  }

  http_server_t *server = conn->server;
  int fd = conn->fd;

  if ((server != NULL) && (fd >= 0)) {
    event_loop_remove(server->loop, fd);
    if (atomic_load(&server->connection_count) > 0) {
      (void)atomic_fetch_sub(&server->connection_count, 1);
    }
  }

  if (fd >= 0) {
    close(fd);
  }
  http_connection_release(conn);
}
