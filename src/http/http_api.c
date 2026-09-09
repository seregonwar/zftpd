/*
 * HTTP API composition root.
 * Domain handlers own their routes; this file only applies cross-cutting
 * policy and composes them in one place.
 */
#include "http_api.h"
#include "http_api_internal.h"
#include "games/games_internal.h"
#include "ftp_config.h"
#include "http_csrf.h"
#include <stddef.h>

typedef http_response_t *(*http_domain_handler_t)(const http_request_t *);

static const http_domain_handler_t k_api_domains[] = {
    http_api_transfer_handle,
    http_api_files_handle,
    http_api_system_handle,
    http_api_process_handle,
    http_games_metadata_handle,
    http_api_archive_handle,
    http_games_admin_handle,
};

static http_response_t *legacy_api_handle(const http_request_t *request) {
  if (http_api_route_is(request->uri, "/api/stream/status")) {
    return http_api_legacy_disabled_json(
        "{\"ok\":true,\"enabled\":false,\"status\":\"offline\"}");
  }
  if (http_api_route_is(request->uri, "/api/stream/start") ||
      http_api_route_is(request->uri, "/api/stream/stop") ||
      http_api_route_is(request->uri, "/api/stream")) {
    return http_api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Stream disabled\"}");
  }
  if (http_api_route_is(request->uri, "/api/admin/installed")) {
    return http_api_legacy_disabled_json(
        "{\"ok\":true,\"installed\":false}");
  }
  if (http_api_route_is(request->uri, "/api/admin/install")) {
    return http_api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Install API not available\"}");
  }
  return NULL;
}

http_response_t *http_api_handle(const http_request_t *request) {
  if (request == NULL) return NULL;

#if ENABLE_WEB_UPLOAD
  if (request->method == HTTP_METHOD_POST && http_csrf_validate(request) != 0) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                               "Invalid or missing CSRF token");
  }
#endif

  for (size_t i = 0; i < sizeof(k_api_domains) / sizeof(k_api_domains[0]); i++) {
    http_response_t *response = k_api_domains[i](request);
    if (response != NULL) return response;
  }

  http_response_t *legacy = legacy_api_handle(request);
  if (legacy != NULL) return legacy;
  return http_static_serve(request);
}
