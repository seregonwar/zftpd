#include "http_api.h"
#include "../src/http/http_api_internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(x) do { if (!(x)) { \
  fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
  return 1; \
} } while (0)

static int response_is(http_response_t *resp, int code) {
  if (resp == NULL || resp->used == 0U) return 0;
  char expected[32];
  (void)snprintf(expected, sizeof(expected), "HTTP/1.1 %d", code);
  return strncmp(resp->data, expected, strlen(expected)) == 0;
}

static http_response_t *request(http_method_t method, const char *uri,
                                char *body) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = method;
  (void)snprintf(req.uri, sizeof(req.uri), "%s", uri);
  req.body = body;
  req.body_length = body != NULL ? strlen(body) : 0U;
  return http_api_files_handle(&req);
}

static int write_text(const char *path, const char *text) {
  FILE *f = fopen(path, "wb");
  if (f == NULL) return -1;
  size_t n = strlen(text);
  int ok = fwrite(text, 1U, n, f) == n ? 0 : -1;
  fclose(f);
  return ok;
}

int main(void) {
  char root[] = "/tmp/zftpd-http-files-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  http_api_set_root(root);

  char hello[1024];
  (void)snprintf(hello, sizeof(hello), "%s/hello.txt", root);
  CHECK(write_text(hello, "hello") == 0);

  char uri[2048];
  (void)snprintf(uri, sizeof(uri), "/api/list?path=%s", root);
  http_response_t *resp = request(HTTP_METHOD_GET, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/dirsize?path=%s", root);
  resp = request(HTTP_METHOD_GET, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/file/get?path=%s", hello);
  resp = request(HTTP_METHOD_GET, uri, NULL);
  CHECK(response_is(resp, 200));
  CHECK(resp->sendfile_fd >= 0);
  CHECK(resp->sendfile_count == 5U);
  http_response_destroy(resp);

  http_request_t range_req;
  memset(&range_req, 0, sizeof(range_req));
  range_req.method = HTTP_METHOD_GET;
  (void)snprintf(range_req.uri, sizeof(range_req.uri), "%s", uri);
  char range_header[] = "Range";
  char middle[] = "bytes=1-3";
  range_req.headers[0] = (http_header_t){range_header, middle};
  range_req.num_headers = 1;
  resp = http_api_files_handle(&range_req);
  CHECK(response_is(resp, 206));
  CHECK(strstr(resp->data, "Content-Range: bytes 1-3/5\r\n") != NULL);
  CHECK(strstr(resp->data, "Content-Length: 3\r\n") != NULL);
  CHECK(resp->sendfile_offset == 1 && resp->sendfile_count == 3U);
  http_response_destroy(resp);

  char suffix[] = "bytes=-2";
  range_req.headers[0].value = suffix;
  resp = http_api_files_handle(&range_req);
  CHECK(response_is(resp, 206));
  CHECK(resp->sendfile_offset == 3 && resp->sendfile_count == 2U);
  http_response_destroy(resp);

  char beyond[] = "bytes=5-";
  range_req.headers[0].value = beyond;
  resp = http_api_files_handle(&range_req);
  CHECK(response_is(resp, 416));
  CHECK(strstr(resp->data, "Content-Range: bytes */5\r\n") != NULL);
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/listing?path=/", NULL);
  CHECK(resp == NULL);
  resp = request(HTTP_METHOD_GET, "/api/download/start", NULL);
  CHECK(resp == NULL);
  http_request_t transfer_req;
  memset(&transfer_req, 0, sizeof(transfer_req));
  transfer_req.method = HTTP_METHOD_GET;
  (void)snprintf(transfer_req.uri, sizeof(transfer_req.uri),
                 "/api/download/delete");
  resp = http_api_transfer_handle(&transfer_req);
  CHECK(response_is(resp, 405));
  http_response_destroy(resp);
  (void)snprintf(transfer_req.uri, sizeof(transfer_req.uri),
                 "/api/download/retry");
  resp = http_api_transfer_handle(&transfer_req);
  CHECK(response_is(resp, 405));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/mkdir?path=%s&name=sub", root);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char sub[1024];
  (void)snprintf(sub, sizeof(sub), "%s/sub", root);
  struct stat st;
  CHECK(stat(sub, &st) == 0 && S_ISDIR(st.st_mode));

  char collision[1024];
  (void)snprintf(collision, sizeof(collision), "%s/collision", root);
  CHECK(write_text(collision, "file") == 0);
  (void)snprintf(uri, sizeof(uri), "/api/mkdir?path=%s&name=collision", root);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 409));
  http_response_destroy(resp);
  CHECK(lstat(collision, &st) == 0 && S_ISREG(st.st_mode));

  (void)snprintf(uri, sizeof(uri),
                 "/api/create_file?path=%s&name=data.txt", sub);
  char payload[] = "abc";
  resp = request(HTTP_METHOD_POST, uri, payload);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char data[1024];
  (void)snprintf(data, sizeof(data), "%s/data.txt", sub);
  CHECK(stat(data, &st) == 0 && st.st_size == 3);

  (void)snprintf(uri, sizeof(uri),
                 "/api/rename?path=%s&name=renamed.txt", data);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char renamed[1024];
  (void)snprintf(renamed, sizeof(renamed), "%s/renamed.txt", sub);
  CHECK(stat(renamed, &st) == 0);

  /* chmod: the reply reports the mode the mount actually kept. */
  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s&mode=755", renamed);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  CHECK(strstr(resp->data, "\"mode\":\"755\"") != NULL);
  CHECK(strstr(resp->data, "\"supported\":true") != NULL);
  http_response_destroy(resp);
  CHECK(stat(renamed, &st) == 0 && (st.st_mode & 0777U) == 0755U);

  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s&mode=999", renamed);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s&mode=1777", renamed);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s&mode=u+x", renamed);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);

  /* Read-only and missing targets. */
  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s&mode=644", renamed);
  resp = request(HTTP_METHOD_GET, uri, NULL);
  CHECK(response_is(resp, 405));
  http_response_destroy(resp);
  (void)snprintf(uri, sizeof(uri), "/api/chmod?path=%s/missing&mode=644", root);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 404));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s", renamed);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);
  CHECK(stat(renamed, &st) != 0 && errno == ENOENT);

  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s", sub);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);
  CHECK(stat(sub, &st) != 0);

  char guarddir[1024], guardfile[1024];
  (void)snprintf(guarddir, sizeof(guarddir), "%s/guard", root);
  (void)snprintf(guardfile, sizeof(guardfile), "%s/keep.txt", guarddir);
  CHECK(mkdir(guarddir, 0700) == 0);
  CHECK(write_text(guardfile, "keep") == 0);
  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s&notrecursive=1", guarddir);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 409));
  http_response_destroy(resp);
  CHECK(lstat(guardfile, &st) == 0);
  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s&recursive=10", guarddir);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 409));
  http_response_destroy(resp);
  CHECK(lstat(guardfile, &st) == 0);

  char targetdir[1024], targetfile[1024], dirlink[1024];
  (void)snprintf(targetdir, sizeof(targetdir), "%s/targetdir", root);
  (void)snprintf(targetfile, sizeof(targetfile), "%s/keep.txt", targetdir);
  (void)snprintf(dirlink, sizeof(dirlink), "%s/dirlink", root);
  CHECK(mkdir(targetdir, 0700) == 0);
  CHECK(write_text(targetfile, "target") == 0);
  CHECK(symlink(targetdir, dirlink) == 0);
  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s", dirlink);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);
  CHECK(lstat(dirlink, &st) != 0 && errno == ENOENT);
  CHECK(lstat(targetfile, &st) == 0 && S_ISREG(st.st_mode));

  (void)snprintf(uri, sizeof(uri), "/api/mkdir?path=%s&name=copydst", root);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char copydst[1024], copied[1024];
  (void)snprintf(copydst, sizeof(copydst), "%s/copydst", root);
  (void)snprintf(copied, sizeof(copied), "%s/hello.txt", copydst);
  (void)snprintf(uri, sizeof(uri), "/api/copy?path=%s&dst=%s", hello, copydst);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  for (int i = 0; i < 100 && stat(copied, &st) != 0; i++) usleep(10000);
  CHECK(stat(copied, &st) == 0 && st.st_size == 5);
  resp = request(HTTP_METHOD_GET, "/api/copy_progress", NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/delete?path=%s&recursive=1", copydst);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  CHECK(unlink(collision) == 0);
  CHECK(unlink(guardfile) == 0);
  CHECK(rmdir(guarddir) == 0);
  CHECK(unlink(targetfile) == 0);
  CHECK(rmdir(targetdir) == 0);
  CHECK(unlink(hello) == 0);
  CHECK(rmdir(root) == 0);
  http_api_set_root("/");
  puts("test_http_files: ok");
  return 0;
}
