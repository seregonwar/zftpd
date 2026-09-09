#include "http_api.h"
#include "../src/http/http_api_internal.h"
#include <stdio.h>
#include <string.h>

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
                                char *body, size_t body_len) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = method;
  (void)snprintf(req.uri, sizeof(req.uri), "%s", uri);
  req.body = body;
  req.body_length = body_len;
  return http_api_process_handle(&req);
}

int main(void) {
  http_response_t *resp =
      request(HTTP_METHOD_GET, "/api/processes", NULL, 0U);
  CHECK(response_is(resp, 200));
  http_response_destroy(resp);

  resp = request(HTTP_METHOD_GET, "/api/processes-extra", NULL, 0U);
  CHECK(resp == NULL);

  resp = request(HTTP_METHOD_GET, "/api/process/kill", NULL, 0U);
  CHECK(response_is(resp, 405));
  http_response_destroy(resp);

  char pid_one[] = "{\"pid\":1}";
  resp = request(HTTP_METHOD_POST, "/api/process/kill",
                 pid_one, sizeof(pid_one) - 1U);
  CHECK(response_is(resp, 403));
  http_response_destroy(resp);

  char overflow[] = "{\"pid\":999999999999999999999}";
  resp = request(HTTP_METHOD_POST, "/api/process/kill",
                 overflow, sizeof(overflow) - 1U);
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);

  char truncated[7] = {'{','"','p','i','d','"',':'};
  resp = request(HTTP_METHOD_POST, "/api/process/kill",
                 truncated, sizeof(truncated));
  CHECK(response_is(resp, 400));
  http_response_destroy(resp);

  puts("test_http_process: ok");
  return 0;
}
