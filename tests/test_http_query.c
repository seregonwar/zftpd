#include "http_api.h"
#include <dirent.h>
#include <stdio.h>
#include <string.h>

static int starts_with(const char *s, const char *prefix) {
  if ((s == NULL) || (prefix == NULL)) {
    return 0;
  }
  size_t n = strlen(prefix);
  return (strncmp(s, prefix, n) == 0) ? 1 : 0;
}

int main(void) {
  http_request_t req;
  memset(&req, 0, sizeof(req));
  req.method = HTTP_METHOD_GET;

  {
    (void)snprintf(req.uri, sizeof(req.uri), "/api/list?path=%%2F");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 2;
    }

    if ((resp->used == 0U) || (!starts_with(resp->data, "HTTP/1.1 200"))) {
      if (resp->stream_dir != NULL) {
        closedir((DIR *)resp->stream_dir);
        resp->stream_dir = NULL;
      }
      http_response_destroy(resp);
      return 3;
    }

    if (resp->stream_dir != NULL) {
      closedir((DIR *)resp->stream_dir);
      resp->stream_dir = NULL;
    }
    http_response_destroy(resp);
  }

  {
    (void)snprintf(req.uri, sizeof(req.uri), "/api/list?path=/");
    http_response_t *resp = http_api_handle(&req);
    if (resp == NULL) {
      return 4;
    }

    if ((resp->used == 0U) || (!starts_with(resp->data, "HTTP/1.1 200"))) {
      if (resp->stream_dir != NULL) {
        closedir((DIR *)resp->stream_dir);
        resp->stream_dir = NULL;
      }
      http_response_destroy(resp);
      return 5;
    }

    if (resp->stream_dir != NULL) {
      closedir((DIR *)resp->stream_dir);
      resp->stream_dir = NULL;
    }
    http_response_destroy(resp);
  }

  return 0;
}
