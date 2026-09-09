#include "games_internal.h"
#include "../http_api_internal.h"
#include "ftp_config.h"
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef ENABLE_PKG_INSTALL
#define ENABLE_PKG_INSTALL 0
#endif

static http_response_t *api_games_installed(const http_request_t *request) {
  (void)request;

  enum { GAMES_BODY_CAP = 512U * 1024U };
  char *body = (char *)malloc(GAMES_BODY_CAP);
  if (body == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  size_t pos = 0U;
  if (http_api_buf_append_cstr(body, GAMES_BODY_CAP, &pos,
                      "{\"ok\":true,\"entries\":[") != 0) {
    free(body);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  int first = 1;
  size_t count_added = 0U;
  const char *bases[] = {"/user/app", "/system_ex/app", "/mnt/ext0/user/app",
                         NULL};
  for (size_t bi = 0; bases[bi] != NULL; bi++) {
    if (games_append_installed_entries(bases[bi], body, GAMES_BODY_CAP,
                                           &pos, &first,
                                           &count_added) != 0) {
      free(body);
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
    }
  }

  if (http_api_buf_append_cstr(body, GAMES_BODY_CAP, &pos, "]}") != 0) {
    free(body);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    free(body);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, body, pos) != 0) {
    free(body);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Body allocation failed");
  }
  return resp;
}

static http_response_t *api_games_icon(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_png_fallback_response();
  }

  char title_id[64] = {0};
  if (http_api_parse_query_param(query, "id", title_id, sizeof(title_id)) != 0) {
    title_id[0] = '\0';
  }
  if (title_id[0] != '\0') {
    for (size_t i = 0; title_id[i] != '\0'; i++) {
      unsigned char c = (unsigned char)title_id[i];
      if (!(isalnum(c) || c == '_' || c == '-')) {
        title_id[0] = '\0';
        break;
      }
      title_id[i] = (char)toupper(c);
    }
  }

  char path_hint[FTP_PATH_MAX] = {0};
  (void)http_api_parse_query_param(query, "path", path_hint, sizeof(path_hint));

  char icon_path[FTP_PATH_MAX] = {0};
  if (games_resolve_installed_icon((title_id[0] != '\0') ? title_id : NULL,
                                  (path_hint[0] != '\0') ? path_hint : NULL,
                                  icon_path, sizeof(icon_path)) != 0) {
    return http_api_png_fallback_response();
  }

  FILE *fp = fopen(icon_path, "rb");
  if (fp == NULL) {
    return http_api_png_fallback_response();
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return http_api_png_fallback_response();
  }
  long flen = ftell(fp);
  if (flen <= 0 || flen > (8 * 1024 * 1024)) {
    fclose(fp);
    return http_api_png_fallback_response();
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return http_api_png_fallback_response();
  }

  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) {
    fclose(fp);
    return http_api_png_fallback_response();
  }
  size_t got = fread(buf, 1, (size_t)flen, fp);
  fclose(fp);
  if (got != (size_t)flen) {
    free(buf);
    return http_api_png_fallback_response();
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, buf, got) != 0) {
    free(buf);
    http_response_destroy(resp);
    return http_api_png_fallback_response();
  }
  return resp;
}

static http_response_t *api_games_repair_visibility(const http_request_t *request) {
  char body[2048];
  size_t pos = 0U;
  int first = 1;
  size_t count_added = 0U;
  int repaired_titles = 0;
  int repaired_tables = 0;
  int repaired_rows = 0;
  int sqlite_available = 0;

  char requested_id[32] = {0};
  const char *query = strchr(request->uri, '?');
  if (query != NULL) {
    (void)http_api_parse_query_param(query, "id", requested_id, sizeof(requested_id));
    for (size_t i = 0; requested_id[i] != '\0'; i++) {
      requested_id[i] = (char)toupper((unsigned char)requested_id[i]);
    }
  }

  if (http_api_buf_append_cstr(body, sizeof(body), &pos,
                      "{\"ok\":true,\"message\":\"Visibility reindex completed\",\"scanned\":[") !=
      0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  const char *bases[] = {"/user/app", "/system_ex/app", "/mnt/ext0/user/app",
                         "/user/appmeta", "/system_data/priv/appmeta", NULL};

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (requested_id[0] != '\0') {
    int valid = 1;
    for (size_t i = 0; requested_id[i] != '\0'; i++) {
      unsigned char c = (unsigned char)requested_id[i];
      if (!(isalnum(c) || c == '_' || c == '-')) {
        valid = 0;
        break;
      }
    }
    if (valid) {
      int touched_tables = 0;
      int touched_rows = 0;
      int r =
          games_psx_repair_appdb_visibility(requested_id, &touched_tables,
                                                &touched_rows);
      if (r == 0) {
        sqlite_available = 1;
        repaired_titles = 1;
        repaired_tables += touched_tables;
        repaired_rows += touched_rows;
      }
    }
  } else {
    const char *repair_bases[] = {"/user/app", "/mnt/ext0/user/app",
                                  "/system_ex/app", NULL};
    for (size_t bi = 0; repair_bases[bi] != NULL; bi++) {
      DIR *d = opendir(repair_bases[bi]);
      if (d == NULL) {
        continue;
      }

      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
          continue;
        }

        int valid = 1;
        size_t len = strlen(e->d_name);
        if (len < 9U || len > 31U) {
          valid = 0;
        }
        for (size_t i = 0; valid && i < len; i++) {
          unsigned char c = (unsigned char)e->d_name[i];
          if (!(isalnum(c) || c == '_' || c == '-')) {
            valid = 0;
            break;
          }
        }
        if (!valid) {
          continue;
        }

        char title_id[32];
        memset(title_id, 0, sizeof(title_id));
        for (size_t i = 0; i < len && i < (sizeof(title_id) - 1U); i++) {
          title_id[i] = (char)toupper((unsigned char)e->d_name[i]);
        }

        int touched_tables = 0;
        int touched_rows = 0;
        int r =
            games_psx_repair_appdb_visibility(title_id, &touched_tables,
                                                  &touched_rows);
        if (r == 0) {
          sqlite_available = 1;
          repaired_titles++;
          repaired_tables += touched_tables;
          repaired_rows += touched_rows;
        }
      }
      closedir(d);
    }
  }
#endif

  for (size_t i = 0; bases[i] != NULL; i++) {
    if (!first) {
      (void)http_api_buf_append_cstr(body, sizeof(body), &pos, ",");
    }
    first = 0;
    (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "\"");
    (void)http_api_json_escape_append(body, sizeof(body), &pos, bases[i]);
    (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "\"");

    DIR *d = opendir(bases[i]);
    if (d != NULL) {
      struct dirent *e;
      while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') {
          continue;
        }
        count_added++;
      }
      closedir(d);
    }
  }

  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "],\"items_seen\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%zu", count_added);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)http_api_buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, ",\"hints\":[");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos,
                        "\"Use Refresh Installed in Games tab\",");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos,
                        "\"If titles still missing, restart shell/console\"");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "],\"sqlite_repair\":{");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "\"available\":");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos,
                        sqlite_available ? "true" : "false");
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, ",\"titles\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_titles);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)http_api_buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, ",\"tables\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_tables);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)http_api_buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, ",\"rows\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_rows);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)http_api_buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)http_api_buf_append_cstr(body, sizeof(body), &pos, "}}");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

static http_response_t *api_games_install_status(const http_request_t *request) {
  (void)request;
  games_install_snapshot_t state;
  (void)games_install_state_refresh(&state);

  char body[768];
  int n = snprintf(
      body, sizeof(body),
      "{\"ok\":true,\"active\":%s,\"task_id\":%d,\"progress\":%d,\"error\":%d,\"length\":%lu,\"transferred\":%lu,\"title_id\":\"%s\",\"path\":\"%s\"}",
      state.active ? "true" : "false", state.task_id, state.last_percent,
      state.last_error, state.last_length, state.last_transferred, state.title_id,
      state.path);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, (size_t)n);
  return resp;
}

static http_response_t *api_games_uninstall(const http_request_t *request) {
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char title_id[64] = {0};
  if (http_api_parse_query_param(query, "id", title_id, sizeof(title_id)) != 0 ||
      !games_is_valid_title_id(title_id)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid title id");
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int rc = -1;
  if (games_psx_uninstall(title_id, &rc) != 0) {
    return http_api_status_json_200(0, "Uninstall API unavailable", -1);
  }
  if (rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Uninstall failed: 0x%08X", (unsigned)rc);
    return http_api_status_json_200(0, msg, rc);
  }

  char body[192];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":true,\"message\":\"Uninstalled\",\"id\":\"%s\"}",
                   title_id);
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_set_body(resp, body, (size_t)n);
  return resp;
#else
  (void)title_id;
  return http_api_status_json_200(0, "Uninstall only available on PS4/PS5", -1);
#endif
}

static http_response_t *api_games_install(const http_request_t *request) {
#if !ENABLE_PKG_INSTALL
  (void)request;
  return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                    "PKG installation is disabled for this build");
#else
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[FTP_PATH_MAX] = {0};
  if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  if (!games_has_pkg_extension(safe)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Install supports only PKG/FPKG files");
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int install_rc = -1;
  int task_id = -1;
  char title_id[64] = {0};
  if (games_psx_install_bgft(safe, "Remote PKG Install", title_id,
                           sizeof(title_id), &task_id,
                           &install_rc) == 0) {
    if (install_rc == 0 && task_id >= 0) {
      games_install_state_begin(task_id, title_id[0] ? title_id : "", safe);
    }
  } else if (games_psx_install_path(safe, title_id, sizeof(title_id),
                                  &install_rc) != 0) {
    return http_api_status_json_200(0, "Install API unavailable", -1);
  }
  if (install_rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Install failed: 0x%08X",
                   (unsigned)install_rc);
    return http_api_status_json_200(0, msg, install_rc);
  }

    char body[512];
  int n = snprintf(
      body, sizeof(body),
      "{\"ok\":true,\"message\":\"Install started\",\"title_id\":\"%s\",\"path\":\"%s\",\"task_id\":%d,\"task_based\":%s}",
      title_id[0] ? title_id : "", safe, task_id,
      (task_id >= 0) ? "true" : "false");
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_set_body(resp, body, (size_t)n);
  return resp;
#else
  (void)safe;
  return http_api_status_json_200(0, "Install only available on PS4/PS5", -1);
#endif
#endif
}

static http_response_t *api_games_reinstall(const http_request_t *request) {
#if !ENABLE_PKG_INSTALL
  (void)request;
  return http_api_error_json(HTTP_STATUS_409_CONFLICT,
                    "PKG installation is disabled for this build");
#else
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[FTP_PATH_MAX] = {0};
  if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  if (!games_has_pkg_extension(safe)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Reinstall supports only PKG/FPKG files");
  }

  char title_id[64] = {0};
  (void)games_extract_title_id_from_image(safe, title_id, sizeof(title_id));

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int uninstall_rc = -1;
  if (title_id[0] != '\0') {
    if (games_psx_uninstall(title_id, &uninstall_rc) != 0) {
      uninstall_rc = -1;
    }
  }

  int install_rc = -1;
  int task_id = -1;
  char install_title[64] = {0};
  if (games_psx_install_bgft(safe, "Remote PKG Reinstall", install_title,
                           sizeof(install_title), &task_id,
                           &install_rc) == 0) {
    if (install_rc == 0 && task_id >= 0) {
      games_install_state_begin(task_id, install_title[0] ? install_title : title_id, safe);
    }
  } else if (games_psx_install_path(safe, install_title, sizeof(install_title),
                                  &install_rc) != 0) {
    return http_api_status_json_200(0, "Install API unavailable", -1);
  }
  if (install_rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Reinstall failed: 0x%08X",
                   (unsigned)install_rc);
    return http_api_status_json_200(0, msg, install_rc);
  }

    char body[576];
  int n = snprintf(
      body, sizeof(body),
      "{\"ok\":true,\"message\":\"Reinstall started\",\"title_id\":\"%s\",\"uninstall_rc\":%d,\"task_id\":%d,\"task_based\":%s}",
      install_title[0] ? install_title : title_id, uninstall_rc, task_id,
      (task_id >= 0) ? "true" : "false");
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_set_body(resp, body, (size_t)n);
  return resp;
#else
  return http_api_status_json_200(0, "Reinstall only available on PS4/PS5", -1);
#endif
#endif
}


http_response_t *http_games_admin_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/admin/games/installed")) return api_games_installed(request);
  if (http_api_route_is(request->uri, "/api/admin/games/icon")) return api_games_icon(request);
  if (http_api_route_is(request->uri, "/api/admin/games/repair_visibility")) return api_games_repair_visibility(request);
  if (http_api_route_is(request->uri, "/api/admin/games/uninstall")) return api_games_uninstall(request);
  if (http_api_route_is(request->uri, "/api/admin/games/install_status")) return api_games_install_status(request);
  if (http_api_route_is(request->uri, "/api/admin/games/install")) return api_games_install(request);
  if (http_api_route_is(request->uri, "/api/admin/games/reinstall")) return api_games_reinstall(request);
  if (http_api_route_is(request->uri, "/api/admin/launch")) return http_games_launch(request);
  return NULL;
}
