/* URL transfer REST endpoints; protocol work lives in src/transfer/. */
#include "http_api_internal.h"
#include "transfer/transfer_manager.h"
#include "ftp_config.h"

#include <inttypes.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/*===========================================================================*
 * DOWNLOAD / TRANSFER API
 *
 * Transport logic lives in src/transfer/.  The HTTP API is intentionally
 * limited to request validation, destination confinement and JSON mapping.
 *===========================================================================*/

static int dl_is_directory_path(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int dl_try_destination(const char *candidate, char *safe,
                              size_t safe_size) {
  if (candidate == NULL || candidate[0] == '\0') return 0;
  if (!http_api_validate_path(candidate, safe, safe_size)) return 0;
  return dl_is_directory_path(safe);
}

static int dl_normalize_destination(const char *requested, char *safe,
                                    size_t safe_size) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (requested == NULL || requested[0] == '\0' || strcmp(requested, "/") == 0) {
    static const char *const defaults[] = {"/data", "/mnt/ext1", "/mnt/usb0",
                                            "/mnt/usb1", NULL};
    for (size_t i = 0U; defaults[i] != NULL; i++) {
      if (dl_try_destination(defaults[i], safe, safe_size)) return 1;
    }
  }
#endif
  if (dl_try_destination(requested, safe, safe_size)) return 1;
  if (http_api_get_root()[0] != '\0' && strcmp(http_api_get_root(), "/") != 0 &&
      dl_try_destination(http_api_get_root(), safe, safe_size)) return 1;
  return 0;
}

static int json_body_string(const http_request_t *request, const char *key,
                            char *out, size_t out_size) {
  if (request == NULL || key == NULL || out == NULL || out_size == 0U ||
      request->body == NULL || request->body_length == 0U) return 0;
  out[0] = '\0';
  char needle[64];
  int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (nn <= 0 || (size_t)nn >= sizeof(needle)) return 0;
  const char *p = strstr(request->body, needle);
  if (p == NULL) return 0;
  p = strchr(p + (size_t)nn, ':');
  if (p == NULL) return 0;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
  if (*p++ != '"') return 0;
  size_t pos = 0U;
  while (*p != '\0' && *p != '"' && pos + 1U < out_size) {
    if (*p == '\\' && p[1] != '\0') {
      p++;
      if (*p == 'n') out[pos++] = '\n';
      else if (*p == 'r') out[pos++] = '\r';
      else if (*p == 't') out[pos++] = '\t';
      else out[pos++] = *p;
      p++;
      continue;
    }
    out[pos++] = *p++;
  }
  out[pos] = '\0';
  return *p == '"';
}

static int json_body_int(const http_request_t *request, const char *key,
                         int *out) {
  if (request == NULL || key == NULL || out == NULL || request->body == NULL)
    return 0;
  char needle[64];
  int nn = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (nn <= 0 || (size_t)nn >= sizeof(needle)) return 0;
  const char *p = strstr(request->body, needle);
  if (p == NULL || (p = strchr(p + (size_t)nn, ':')) == NULL) return 0;
  p++;
  while (*p == ' ' || *p == '\t') p++;
  char *endptr = NULL;
  long value = strtol(p, &endptr, 10);
  if (endptr == p || value <= 0L || value > INT_MAX) return 0;
  *out = (int)value;
  return 1;
}

http_response_t *http_api_transfer_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");

  char url[TRANSFER_URL_MAX];
  char dst[TRANSFER_PATH_MAX];
  if (!json_body_string(request, "url", url, sizeof(url)) || url[0] == '\0')
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing url parameter");
  if (!json_body_string(request, "dst", dst, sizeof(dst))) dst[0] = '\0';

  char reason[TRANSFER_ERROR_MAX];
  if (!transfer_url_supported(url, reason, sizeof(reason)))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, reason);

  char safe_dst[FTP_PATH_MAX];
  if (!dl_normalize_destination(dst, safe_dst, sizeof(safe_dst)))
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Invalid or read-only destination path");

  int id = 0;
  char name[TRANSFER_NAME_MAX];
  char error[TRANSFER_ERROR_MAX];
  if (transfer_start(url, safe_dst, &id, name, sizeof(name), error,
                     sizeof(error)) != 0)
    return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                      error[0] != '\0' ? error : "Failed to start transfer");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char esc_name[TRANSFER_NAME_MAX * 2U];
  size_t esc_pos = 0U;
  esc_name[0] = '\0';
  (void)http_api_json_escape_append(esc_name, sizeof(esc_name), &esc_pos, name);
  esc_name[(esc_pos < sizeof(esc_name)) ? esc_pos : sizeof(esc_name) - 1U] = '\0';
  char body[768];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"size\":0}",
                     id, esc_name);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_transfer_status(const http_request_t *request) {
  (void)request;
  transfer_snapshot_t snaps[TRANSFER_MAX_ACTIVE];
  size_t count = transfer_snapshot_all(snaps, TRANSFER_MAX_ACTIVE);
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  char body[8192];
  size_t pos = 0U;
  int n = snprintf(body, sizeof(body), "{\"downloads\":[");
  if (n < 0) return resp;
  pos = (size_t)n;
  for (size_t i = 0U; i < count && pos < sizeof(body); i++) {
    transfer_snapshot_t *s = &snaps[i];
    char esc_name[512], esc_url[4096], esc_error[512];
    size_t ep = 0U;
    esc_name[0] = esc_url[0] = esc_error[0] = '\0';
    (void)http_api_json_escape_append(esc_name, sizeof(esc_name), &ep, s->filename);
    esc_name[ep < sizeof(esc_name) ? ep : sizeof(esc_name) - 1U] = '\0';
    ep = 0U; (void)http_api_json_escape_append(esc_url, sizeof(esc_url), &ep, s->url);
    esc_url[ep < sizeof(esc_url) ? ep : sizeof(esc_url) - 1U] = '\0';
    ep = 0U; (void)http_api_json_escape_append(esc_error, sizeof(esc_error), &ep,
                                      s->error ? s->error_msg : "");
    esc_error[ep < sizeof(esc_error) ? ep : sizeof(esc_error) - 1U] = '\0';
    unsigned progress = s->total_size > 0U
        ? (unsigned)((s->downloaded * 100U) / s->total_size)
        : (s->done && !s->error ? 100U : 0U);
    if (progress > 100U) progress = 100U;
    n = snprintf(body + pos, sizeof(body) - pos,
        "%s{\"id\":%d,\"name\":\"%s\",\"url\":\"%s\",\"progress\":%u,"
        "\"downloaded\":%" PRIu64 ",\"total_size\":%" PRIu64 ",\"speed\":%.0f,"
        "\"done\":%s,\"error\":\"%s\",\"paused\":%s}",
        i == 0U ? "" : ",", s->id, esc_name, esc_url, progress,
        s->downloaded, s->total_size, s->speed, s->done ? "true" : "false",
        esc_error, s->paused ? "true" : "false");
    if (n < 0 || (size_t)n >= sizeof(body) - pos) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                        "Transfer status response too large");
    }
    pos += (size_t)n;
  }
  n = snprintf(body + pos, sizeof(body) - pos, "]}");
  if (n < 0 || (size_t)n >= sizeof(body) - pos)
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                      "Transfer status response too large");
  pos += (size_t)n;
  http_response_set_body(resp, body, pos);
  return resp;
}

http_response_t *http_api_transfer_pause(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0, paused = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_toggle_pause(id, &paused) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Transfer not found");
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[64];
  int len = snprintf(body, sizeof(body), "{\"ok\":true,\"paused\":%s}",
                     paused ? "true" : "false");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

http_response_t *http_api_transfer_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST)
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  int id = 0;
  if (!json_body_int(request, "id", &id))
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing download id");
  if (transfer_cancel(id) != 0)
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Transfer not found");
  return http_api_status_json_200(1, "Transfer cancellation requested", id);
}


http_response_t *http_api_transfer_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/download/start")) return http_api_transfer_start(request);
  if (http_api_route_is(request->uri, "/api/download/status")) return http_api_transfer_status(request);
  if (http_api_route_is(request->uri, "/api/download/pause")) return http_api_transfer_pause(request);
  if (http_api_route_is(request->uri, "/api/download/cancel")) return http_api_transfer_cancel(request);
  return NULL;
}
