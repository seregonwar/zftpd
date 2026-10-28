#include "../src/http/http_api_internal.h"
#include "../src/http/http_response_stream.h"
#include "http_api.h"
#include "http_response.h"
#include "pal_fileio.h"
#include "pal_limits.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); return 1; \
} } while (0)

static ssize_t collect_response(int fd, char *buf, size_t cap) {
  size_t used = 0U;
  while (used + 1U < cap) {
    ssize_t n = read(fd, buf + used, cap - used - 1U);
    if (n < 0) return -1;
    if (n == 0) break;
    used += (size_t)n;
  }
  buf[used] = '\0';
  return (ssize_t)used;
}
static const char *body_start(const char *response) {
  const char *p = strstr(response, "\r\n\r\n");
  return p == NULL ? NULL : p + 4;
}

static int stream_to_pair(http_response_t *resp, char *out, size_t cap) {
  int sv[2];
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
  int rc = http_response_stream_send(sv[0], resp, 1);
  CHECK(shutdown(sv[0], SHUT_WR) == 0);
  CHECK(collect_response(sv[1], out, cap) >= 0);
  close(sv[0]);
  close(sv[1]);
  return rc;
}

static int test_inline_body(void) {
  char out[4096];
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL);
  CHECK(http_response_set_body(resp, "abc", 3U) == 0);
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(body_start(out) != NULL && strcmp(body_start(out), "abc") == 0);
  http_response_destroy(resp);
  return 0;
}
static int test_memory_streams(void) {
  char out[4096];
  static const char ref_body[] = "reference-body";
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL);
  CHECK(http_response_set_body_ref(resp, ref_body, sizeof(ref_body) - 1U) == 0);
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strcmp(body_start(out), ref_body) == 0);
  http_response_destroy(resp);

  resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL);
  CHECK(http_response_set_body_splice(resp, "pre", 3U, "MID", 3U,
                                      "post", 4U) == 0);
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strcmp(body_start(out), "preMIDpost") == 0);
  http_response_destroy(resp);
  return 0;
}

typedef struct {
  int reads;
  int results;
  int result;
  int closes;
} stream_probe_t;

static ssize_t probe_read(void *ctx, void *buf, size_t len) {
  stream_probe_t *probe = (stream_probe_t *)ctx;
  if (probe->reads++ != 0) return 0;
  if (len < 3U) return -1;
  memcpy(buf, "abc", 3U);
  return 3;
}

static void probe_result(void *ctx, int result) {
  stream_probe_t *probe = (stream_probe_t *)ctx;
  probe->results++;
  probe->result = result;
}

static void probe_close(void *ctx) {
  ((stream_probe_t *)ctx)->closes++;
}

static int test_stream_result(void) {
  stream_probe_t probe = {0};
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL && http_response_finalize(resp) == 0);
  resp->stream_ctx = &probe;
  resp->stream_read = probe_read;
  resp->stream_result = probe_result;
  resp->stream_close = probe_close;
  char out[4096];
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strcmp(body_start(out), "abc") == 0);
  CHECK(probe.results == 1 && probe.result == 0);
  http_response_destroy(resp);
  CHECK(probe.results == 1 && probe.closes == 1);

  memset(&probe, 0, sizeof(probe));
  resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL);
  resp->stream_ctx = &probe;
  resp->stream_read = probe_read;
  resp->stream_result = probe_result;
  resp->stream_close = probe_close;
  http_response_destroy(resp);
  CHECK(probe.results == 1 && probe.result == -1 && probe.closes == 1);
  return 0;
}

static int test_sendfile_stream(void) {
  char path[] = "/tmp/zftpd-http-stream-XXXXXX";
  int fd = mkstemp(path);
  CHECK(fd >= 0);
  static const char payload[] = "sendfile-body";
  CHECK(write(fd, payload, sizeof(payload) - 1U) == (ssize_t)(sizeof(payload) - 1U));
  CHECK(lseek(fd, 0, SEEK_SET) == 0);
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  CHECK(resp != NULL);
  CHECK(http_response_finalize(resp) == 0);
  resp->sendfile_fd = fd;
  resp->sendfile_offset = 0;
  resp->sendfile_count = sizeof(payload) - 1U;

  char out[4096];
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strcmp(body_start(out), payload) == 0);
  CHECK(resp->sendfile_fd == -1);
  http_response_destroy(resp);
  unlink(path);
  return 0;
}

static int write_text(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0) return -1;
  size_t len = strlen(text);
  int ok = write(fd, text, len) == (ssize_t)len ? 0 : -1;
  close(fd);
  return ok;
}
static int test_directory_stream_is_pinned(void) {
  char root[] = "/tmp/zftpd-stream-root-XXXXXX";
  char outside[] = "/tmp/zftpd-stream-out-XXXXXX";
  CHECK(mkdtemp(root) != NULL && mkdtemp(outside) != NULL);
  char inside[512], alias[512], in_file[512], out_file[512], quoted[512];
  CHECK(snprintf(inside, sizeof(inside), "%s/inside", root) > 0);
  CHECK(snprintf(alias, sizeof(alias), "%s/alias", root) > 0);
  CHECK(snprintf(in_file, sizeof(in_file), "%s/same.txt", inside) > 0);
  CHECK(snprintf(out_file, sizeof(out_file), "%s/same.txt", outside) > 0);
  CHECK(snprintf(quoted, sizeof(quoted), "%s/q\"uote.txt", inside) > 0);
  CHECK(mkdir(inside, 0700) == 0);
  CHECK(write_text(in_file, "abc") == 0);
  CHECK(write_text(out_file, "outside-data") == 0);
  CHECK(write_text(quoted, "x") == 0);
  CHECK(symlink(inside, alias) == 0);

  http_api_set_root(root);
  http_request_t req = {0};
  req.method = HTTP_METHOD_GET;
  CHECK(snprintf(req.uri, sizeof(req.uri), "/api/list?path=%s", alias) > 0);
  http_response_t *resp = http_api_handle(&req);
  CHECK(resp != NULL && resp->stream_dir != NULL);
  /* realpath() needs a PATH_MAX buffer; a smaller one aborts under fortify. */
  char inside_real[PAL_PATH_MAX];
  CHECK(realpath(inside, inside_real) != NULL);
  CHECK(strcmp(resp->stream_path, inside_real) == 0);
  CHECK(unlink(alias) == 0 && symlink(outside, alias) == 0);

  char out[16384];
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strstr(out, "\\\"name\\\":\\\"same.txt\\\"") == NULL);
  CHECK(strstr(out, "\"name\":\"same.txt\"") != NULL);
  CHECK(strstr(out, "\"size\":3,") != NULL);
  CHECK(strstr(out, "\"mode\":") != NULL);
  CHECK(strstr(out, "\"size\":12}") == NULL);
  CHECK(strstr(out, "q\\\"uote.txt") != NULL);
  http_response_destroy(resp);
  http_api_set_root("/");

  unlink(alias);
  unlink(in_file);
  unlink(out_file);
  unlink(quoted);
  rmdir(inside);
  rmdir(root);
  rmdir(outside);
  return 0;
}

static int test_long_directory_path(void) {
  char root[] = "/tmp/zftpd-stream-long-XXXXXX";
  CHECK(mkdtemp(root) != NULL);

  char path[PAL_PATH_MAX];
  CHECK(snprintf(path, sizeof(path), "%s", root) > 0);
  const size_t target = PAL_PATH_MAX > 1200U ? 1100U : PAL_PATH_MAX - 128U;
  for (unsigned i = 0U; strlen(path) <= target; i++) {
    char component[96];
    CHECK(snprintf(component, sizeof(component),
                   "segment-%02u-abcdefghijklmnopqrstuvwxyz0123456789", i) > 0);
    size_t used = strlen(path);
    size_t need = strlen(component) + 1U;
    CHECK(used + need + 1U < sizeof(path));
    path[used] = '/';
    memcpy(path + used + 1U, component, need);
    CHECK(mkdir(path, 0700) == 0);
  }
  CHECK(sizeof(((http_response_t *)0)->stream_path) == PAL_PATH_MAX);
  if (PAL_PATH_MAX > 1200U) CHECK(strlen(path) > 1024U);

  char file_path[PAL_PATH_MAX];
  CHECK(snprintf(file_path, sizeof(file_path), "%s/deep.txt", path) > 0);
  CHECK(write_text(file_path, "deep") == 0);
  http_api_set_root(root);

  http_request_t req = {0};
  req.method = HTTP_METHOD_GET;
  CHECK(snprintf(req.uri, sizeof(req.uri), "/api/list?path=%s", path) > 0);
  http_response_t *resp = http_api_handle(&req);
  CHECK(resp != NULL && resp->stream_dir != NULL);
  char expected[PAL_PATH_MAX];
  CHECK(realpath(path, expected) != NULL);
  CHECK(strlen(resp->stream_path) == strlen(expected));
  if (PAL_PATH_MAX > 1200U) CHECK(strlen(resp->stream_path) > 1024U);
  CHECK(strcmp(resp->stream_path, expected) == 0);

  char out[16384];
  CHECK(stream_to_pair(resp, out, sizeof(out)) == 0);
  CHECK(strstr(out, "\"name\":\"deep.txt\"") != NULL);
  http_response_destroy(resp);
  http_api_set_root("/");
  CHECK(pal_dir_remove_recursive_pub(root) == FTP_OK);
  return 0;
}

int main(void) {
  CHECK(test_inline_body() == 0);
  CHECK(test_memory_streams() == 0);
  CHECK(test_stream_result() == 0);
  CHECK(test_sendfile_stream() == 0);
  CHECK(test_directory_stream_is_pinned() == 0);
  CHECK(test_long_directory_path() == 0);
  puts("test_http_response_stream: ok");
  return 0;
}
