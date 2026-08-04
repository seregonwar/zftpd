#include "http_api.h"
#include <stdio.h>
#include <string.h>

static int starts_with(const char *s, const char *prefix) {
  if ((s == NULL) || (prefix == NULL)) {
    return 0;
  }
  size_t n = strlen(prefix);
  return (strncmp(s, prefix, n) == 0) ? 1 : 0;
}

static int body_contains(const http_response_t *resp, const char *needle) {
  if ((resp == NULL) || (needle == NULL)) {
    return 0;
  }
  return (strstr(resp->data, needle) != NULL) ? 1 : 0;
}

int main(void) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = HTTP_METHOD_GET;

  /* Happy path */
  {
    (void)snprintf(req.uri, sizeof(req.uri),
                   "/api/notify?text=Hello%%20World&icon=icon_system");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 2;
    }
    if (!starts_with(resp->data, "HTTP/1.1 200") ||
        !body_contains(resp, "\"ok\":true") ||
        !body_contains(resp, "Hello World") ||
        !body_contains(resp, "icon_system")) {
      http_response_destroy(resp);
      return 3;
    }
    http_response_destroy(resp);
  }

  /* Missing text */
  {
    (void)snprintf(req.uri, sizeof(req.uri), "/api/notify");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 4;
    }
    if (!starts_with(resp->data, "HTTP/1.1 400")) {
      http_response_destroy(resp);
      return 5;
    }
    http_response_destroy(resp);
  }

  /* Invalid icon */
  {
    (void)snprintf(req.uri, sizeof(req.uri),
                   "/api/notify?text=Hi&icon=bad/icon");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 6;
    }
    if (!starts_with(resp->data, "HTTP/1.1 400")) {
      http_response_destroy(resp);
      return 7;
    }
    http_response_destroy(resp);
  }

  /* Control characters rejected */
  {
    (void)snprintf(req.uri, sizeof(req.uri), "/api/notify?text=Hi%%0Athere");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 8;
    }
    if (!starts_with(resp->data, "HTTP/1.1 400")) {
      http_response_destroy(resp);
      return 9;
    }
    http_response_destroy(resp);
  }

  /* Wrong method */
  {
    req.method = HTTP_METHOD_HEAD;
    (void)snprintf(req.uri, sizeof(req.uri), "/api/notify?text=Hi");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 10;
    }
    if (!starts_with(resp->data, "HTTP/1.1 405")) {
      http_response_destroy(resp);
      return 11;
    }
    http_response_destroy(resp);
  }

  return 0;
}
