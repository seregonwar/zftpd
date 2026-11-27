#include "http_server_internal.h"
#if ENABLE_WEB_UPLOAD

#include "ftp_path.h"
#include "http_api_internal.h"
#include "http_csrf.h"
#include "http_response.h"
#include "pal_fileio.h"
#include "pal_network.h"
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int upload_send_json(http_connection_t *conn, http_status_t status,
                            const char *body) {
  http_response_t *resp = http_response_create(status);
  if (resp == NULL) return -1;
  (void)http_response_add_header(resp, "Content-Type", "application/json");
  (void)http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  if (body != NULL) (void)http_response_set_body(resp, body, strlen(body));
  int rc = 0;
  if (resp->used > 0U && pal_send_all(conn->fd, resp->data, resp->used, 0) < 0)
    rc = -1;
  http_response_destroy(resp);
  return rc;
}

void http_upload_reset(http_connection_t *conn) {
  if (conn == NULL) return;
  if (conn->upload.fd >= 0) {
    (void)pal_file_close(conn->upload.fd);
    /* Still open means the body never arrived in full (client aborted,
     * connection dropped, write error): drop the truncated file. */
    if (conn->upload.path[0] != '\0') (void)unlink(conn->upload.path);
  }
  free(conn->upload.chunk_buffer);
  memset(&conn->upload, 0, sizeof(conn->upload));
  conn->upload.fd = -1;
}

int http_upload_is_active(const http_connection_t *conn) {
  return conn != NULL && conn->upload.active != 0;
}

int http_upload_matches(const http_request_head_t *head) {
  return head != NULL && head->method == HTTP_METHOD_POST &&
         http_api_route_is(head->uri, "/api/upload");
}

static int upload_finish(http_connection_t *conn) {
  if (conn->upload.fd >= 0) {
    (void)pal_file_close(conn->upload.fd);
    conn->upload.fd = -1;
  }
  conn->upload.active = 0;
  conn->upload.remaining = 0U;
  (void)upload_send_json(conn, HTTP_STATUS_200_OK, "{\"ok\":true}");
  return -1;
}

int http_upload_continue(http_connection_t *conn) {
  if (!http_upload_is_active(conn)) return -1;
  uint8_t *buffer = conn->upload.chunk_buffer != NULL
                        ? conn->upload.chunk_buffer
                        : (uint8_t *)conn->buffer;
  size_t capacity = conn->upload.chunk_buffer != NULL
                        ? (size_t)HTTP_UPLOAD_CHUNK_SIZE
                        : sizeof(conn->buffer);

  ssize_t n;
  do {
    n = read(conn->fd, buffer, capacity);
  } while (n < 0 && errno == EINTR);
  if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
  if (n <= 0) return -1;

  size_t count = (size_t)n;
  if (count > conn->upload.remaining) count = conn->upload.remaining;
  if (count > 0U &&
      pal_file_write_all(conn->upload.fd, buffer, count) != (ssize_t)count)
    return -1;

  conn->upload.remaining -= count;
  return conn->upload.remaining == 0U ? upload_finish(conn) : 0;
}

int http_upload_start(http_connection_t *conn, const http_request_head_t *head) {
  if (conn == NULL || head == NULL || head->content_length == 0U) {
    if (conn != NULL)
      (void)upload_send_json(conn, HTTP_STATUS_400_BAD_REQUEST,
                             "{\"error\":\"Missing Content-Length\"}");
    return -1;
  }

  http_request_t request;
  if (http_parse_request(conn->buffer, head->header_length, &request) < 0 ||
      http_csrf_validate(&request) != 0) {
    (void)upload_send_json(conn, HTTP_STATUS_403_FORBIDDEN,
                           "{\"error\":\"Invalid or missing CSRF token\"}");
    return -1;
  }

  char dir_path[FTP_PATH_MAX];
  char file_name[FTP_PATH_MAX];
  char requested[FTP_PATH_MAX];
  char safe[FTP_PATH_MAX];
  char parent[FTP_PATH_MAX];
  if (http_api_parse_path_param(head->uri, dir_path, sizeof(dir_path)) != 0 ||
      http_api_parse_name_param(head->uri, file_name, sizeof(file_name)) != 0 ||
      !http_api_is_safe_filename(file_name) ||
      ftp_path_join(dir_path, file_name, requested, sizeof(requested)) != FTP_OK ||
      !http_api_validate_path(requested, safe, sizeof(safe))) {
    (void)upload_send_json(conn, HTTP_STATUS_400_BAD_REQUEST,
                           "{\"error\":\"Invalid upload path\"}");
    return -1;
  }

  if (ftp_path_dirname(safe, parent, sizeof(parent)) != FTP_OK ||
      pal_dir_create_recursive(parent, DIR_PERM) != FTP_OK) {
    (void)upload_send_json(conn, HTTP_STATUS_500_INTERNAL_ERROR,
                           "{\"error\":\"Failed to create directory\"}");
    return -1;
  }

  int fd = pal_file_open(safe, O_WRONLY | O_CREAT | O_TRUNC, FILE_PERM);
  if (fd < 0) {
    (void)upload_send_json(conn, HTTP_STATUS_500_INTERNAL_ERROR,
                           "{\"error\":\"Failed to open upload target\"}");
    return -1;
  }

  conn->upload.fd = fd;
  memcpy(conn->upload.path, safe, strlen(safe) + 1U);
  conn->upload.active = 1;
  conn->upload.remaining = head->content_length;
  conn->upload.chunk_buffer = (uint8_t *)malloc(HTTP_UPLOAD_CHUNK_SIZE);

  size_t buffered = conn->buffer_used > head->header_length
                        ? conn->buffer_used - head->header_length
                        : 0U;
  if (buffered > conn->upload.remaining) buffered = conn->upload.remaining;
  if (buffered > 0U &&
      pal_file_write_all(fd, conn->buffer + head->header_length, buffered) !=
          (ssize_t)buffered)
    return -1;

  conn->upload.remaining -= buffered;
  conn->buffer_used = 0U;
  conn->buffer[0] = '\0';
  return conn->upload.remaining == 0U ? upload_finish(conn) : 0;
}

#endif
