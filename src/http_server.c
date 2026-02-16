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
#include "http_api.h"
#include "http_config.h"
#if ENABLE_WEB_UPLOAD
#include "http_csrf.h"
#endif
#include "http_parser.h"
#include "http_response.h"
#include "pal_fileio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_sendfile_chunk[HTTP_SENDFILE_CHUNK_SIZE];

/*===========================================================================*
 * INTERNAL TYPES
 *===========================================================================*/

struct http_server {
  event_loop_t *loop;
  int listen_fd;
  uint16_t port;
  int connection_count;
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
#endif
} http_connection_t;

static http_server_t g_http_server;
static int g_http_server_in_use = 0;
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

http_server_t *http_server_create(event_loop_t *loop, uint16_t port) {
  if (loop == NULL) {
    return NULL;
  }

  if (g_http_server_in_use != 0) {
    return NULL;
  }

  memset(&g_http_server, 0, sizeof(g_http_server));
  g_http_server.listen_fd = -1;
  g_http_server.loop = loop;
  g_http_server.port = port;
  g_http_server.connection_count = 0;
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

  g_http_server_in_use = 1;
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
      g_http_server_in_use = 0;
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

  /* Connection limit */
  if (server->connection_count >= HTTP_MAX_CONNECTIONS) {
    close(client_fd);
    return 0;
  }

  http_connection_t *conn = http_connection_acquire(server, client_fd);
  if (conn == NULL) {
    close(client_fd);
    return 0;
  }

  server->connection_count++;

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
      ssize_t n = read(conn->fd, conn->buffer, sizeof(conn->buffer));
      if (n <= 0) {
        http_close_connection(conn);
        return -1;
      }

      size_t got = (size_t)n;
      if (got > conn->upload_remaining) {
        got = conn->upload_remaining;
      }

      if ((got > 0U) && (conn->upload_fd >= 0)) {
        if (pal_file_write_all(conn->upload_fd, conn->buffer, got) < 0) {
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
      if ((strcmp(method, "POST") == 0) && (strncmp(uri, "/api/upload", 11) == 0)) {
        http_request_t up_req;
        if ((http_parse_request(conn->buffer, conn->buffer_used, &up_req) < 0) ||
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
        if (!is_safe_path_local(dir_path) || !is_safe_filename_local(file_name)) {
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

        int out_fd = pal_file_open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (out_fd < 0) {
          http_close_connection(conn);
          return -1;
        }
        conn->upload_fd = out_fd;
        conn->upload_active = 1;

        size_t in_buf = 0U;
        if (conn->buffer_used > header_len) {
          in_buf = conn->buffer_used - header_len;
          if (in_buf > content_length) {
            in_buf = content_length;
          }
        }

        if (in_buf > 0U) {
          if (pal_file_write_all(out_fd, conn->buffer + header_len, in_buf) < 0) {
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
        if ((header_len + content_length) > (sizeof(conn->buffer) - 1U)) {
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
    size_t total = 0;
    size_t remain = response->used;

    while (remain > 0) {
      ssize_t sent = write(conn->fd, response->data + total, remain);
      if (sent <= 0) {
        break;
      }
      total += (size_t)sent;
      remain -= (size_t)sent;
    }
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
    size_t remaining = response->sendfile_count;

    while (remaining > 0) {
      size_t to_read = remaining;
      if (to_read > sizeof(g_sendfile_chunk)) {
        to_read = sizeof(g_sendfile_chunk);
      }

      ssize_t nread = read(response->sendfile_fd, g_sendfile_chunk, to_read);
      if (nread <= 0) {
        break;
      }

      size_t written = 0;
      while (written < (size_t)nread) {
        ssize_t nsent =
            write(conn->fd, g_sendfile_chunk + written, (size_t)nread - written);
        if (nsent <= 0) {
          goto done_sendfile;
        }
        written += (size_t)nsent;
      }

      remaining -= (size_t)nread;
    }
  done_sendfile:
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

      pos += (size_t)snprintf(buffer + pos, sizeof(buffer) - pos,
                              "\",\"type\":\"%s\",\"size\":%lld}",
                              S_ISDIR(st.st_mode) ? "directory" : "file",
                              (long long)st.st_size);

      /* Send chunk: <hex-len>\r\n<data>\r\n */
      char chunk_header[32];
      int header_len =
          snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", pos);

      if (write(conn->fd, chunk_header, (size_t)header_len) <= 0)
        break;
      if (write(conn->fd, buffer, pos) <= 0)
        break;
      if (write(conn->fd, "\r\n", 2) <= 0)
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
    write(conn->fd, chunk_header, (size_t)header_len);
    write(conn->fd, closer, closer_len);
    write(conn->fd, "\r\n", 2);

    /* End of stream: 0\r\n\r\n */
    write(conn->fd, "0\r\n\r\n", 5);
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
    if (server->connection_count > 0) {
      server->connection_count--;
    }
  }

  if (fd >= 0) {
    close(fd);
  }
  http_connection_release(conn);
}
