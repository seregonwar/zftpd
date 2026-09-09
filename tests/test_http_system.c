#include "http_api.h"
#include "../src/http/http_api_internal.h"
#include <stdio.h>
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

static http_response_t *request(http_method_t method, const char *uri) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = method;
  (void)snprintf(req.uri, sizeof(req.uri), "%s", uri);
  return http_api_system_handle(&req);
}

int main(void) {
  char root[] = "/tmp/zftpd-http-system-XXXXXX";
  CHECK(mkdtemp(root) != NULL);
  http_api_set_root(root);

  char file[1024];
  (void)snprintf(file, sizeof(file), "%s/item.txt", root);
  FILE *f = fopen(file, "wb");
  CHECK(f != NULL);
  CHECK(fwrite("data", 1U, 4U, f) == 4U);
  fclose(f);

  http_response_t *resp = request(HTTP_METHOD_GET, "/api/status");
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/stats/ram");
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/stats/system");
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  char uri[2048];
  (void)snprintf(uri, sizeof(uri), "/api/stats?path=%s", root);
  resp = request(HTTP_METHOD_GET, uri);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/disk/info");
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  (void)snprintf(uri, sizeof(uri), "/api/disk/tree?path=%s", root);
  resp = request(HTTP_METHOD_GET, uri);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/status-extra");
  CHECK(resp == NULL);

  resp = request(HTTP_METHOD_GET, "/api/network/reset");
  CHECK(response_is(resp, 405));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/notify");
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);

  CHECK(unlink(file) == 0);
  CHECK(rmdir(root) == 0);
  http_api_set_root("/");
  puts("test_http_system: ok");
  return 0;
}
