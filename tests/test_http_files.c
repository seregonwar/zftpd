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

  resp = request(HTTP_METHOD_GET, "/api/listing?path=/", NULL);
  CHECK(resp == NULL);
  resp = request(HTTP_METHOD_GET, "/api/download/start", NULL);
  CHECK(resp == NULL);

  (void)snprintf(uri, sizeof(uri), "/api/mkdir?path=%s&name=sub", root);
  resp = request(HTTP_METHOD_POST, uri, NULL);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char sub[1024];
  (void)snprintf(sub, sizeof(sub), "%s/sub", root);
  struct stat st;
  CHECK(stat(sub, &st) == 0 && S_ISDIR(st.st_mode));

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

  CHECK(unlink(hello) == 0);
  CHECK(rmdir(root) == 0);
  http_api_set_root("/");
  puts("test_http_files: ok");
  return 0;
}
