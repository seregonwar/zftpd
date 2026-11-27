#ifndef ZFTPD_HTTP_SERVER_INTERNAL_H
#define ZFTPD_HTTP_SERVER_INTERNAL_H

#include "ftp_config.h"
#include "http_config.h"
#include "http_parser.h"
#include "http_server.h"
#include "http_response.h"
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>

typedef struct {
  int active;
  int fd;
  size_t remaining;
  uint8_t *chunk_buffer;
  /* Target file, so an upload cut short is removed instead of left half
   * written: a partial file keeps occupying storage the user cannot see. */
  char path[FTP_PATH_MAX];
} http_upload_state_t;

typedef struct http_connection {
  http_server_t *server;
  int fd;
  char buffer[HTTP_REQUEST_BUFFER_SIZE];
  size_t buffer_used;
#if ENABLE_WEB_UPLOAD
  http_upload_state_t upload;
#endif
} http_connection_t;

struct http_server {
  event_loop_t *loop;
  int listen_fd;
  uint16_t port;
  atomic_int connection_count;
  char bind_addr[128];
  struct sockaddr_storage listen_addr;
  socklen_t listen_addr_len;
  int af;
  int wake_r;
  int wake_w;
  atomic_int pending_listen_fd;
  atomic_int recreating;
  atomic_int alive;
};

http_connection_t *http_server_connection_acquire(http_server_t *server,
                                                  int client_fd);
void http_server_connection_release(http_connection_t *conn);
int http_server_client_callback(int fd, uint32_t events, void *data);
int http_listener_start(http_server_t *server, const char *bind_addr);
void http_listener_stop(http_server_t *server);

#if ENABLE_WEB_UPLOAD
void http_upload_reset(http_connection_t *conn);
int http_upload_is_active(const http_connection_t *conn);
int http_upload_matches(const http_request_head_t *head);
int http_upload_start(http_connection_t *conn, const http_request_head_t *head);
int http_upload_continue(http_connection_t *conn);
#endif

#endif
