/*
MIT License

Copyright (c) 2026 Seregon

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
/**
 * @file http_api.c
 * @brief REST API handlers for the Web File Explorer
 *
 * ENDPOINTS:
 *   GET /api/list?path=<dir>        Directory listing (JSON)
 *   GET /api/download?path=<file>   File download (binary)
 *   GET /                           Serve embedded index.html
 *   GET /style.css                  Serve embedded stylesheet
 *   GET /app.js                     Serve embedded JavaScript
 */

#include "http_api.h"
#include "http_config.h"
#include "pal_fileio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*===========================================================================*
 * EMBEDDED RESOURCES (defined in http_resources.c)
 *===========================================================================*/

extern const char *http_get_resource(const char *path, size_t *size);

/*===========================================================================*
 * FORWARD DECLARATIONS
 *===========================================================================*/

static http_response_t *api_list(const http_request_t *request);
static http_response_t *api_download(const http_request_t *request);
static http_response_t *serve_static(const http_request_t *request);
#if ENABLE_WEB_UPLOAD
static http_response_t *api_create_file(const http_request_t *request);
#endif
static http_response_t *error_json(http_status_t code, const char *message);

/*===========================================================================*
 * PATH SECURITY
 *
 *   ┌──────────────────────────────────────────────────┐
 *   │  BLOCKED PATTERNS            REASON              │
 *   │  ../                         traversal           │
 *   │  //                          double-slash trick  │
 *   │  /dev /proc /sys /kern       PS kernel crash     │
 *   └──────────────────────────────────────────────────┘
 *===========================================================================*/

/**
 * @brief Check for directory-traversal attacks
 *
 * Returns 1 if path is safe, 0 if it contains ".." components.
 */
static int is_safe_path(const char *path) {
  if (path == NULL) {
    return 0;
  }

  /* Must start with '/' */
  if (path[0] != '/') {
    return 0;
  }

  /* Search for ".." components */
  const char *p = path;
  while (*p != '\0') {
    if (p[0] == '.' && p[1] == '.') {
      /* ".." at start of path, or preceded by '/' */
      if (p == path || p[-1] == '/') {
        return 0;
      }
    }
    p++;
  }

  return 1;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||          \
    defined(PS5)
/**
 * @brief PS4/PS5 forbidden path blacklist
 *
 * Accessing these causes "Fatal trap 12: page fault" on unjailbroken kernels.
 */
static const char *forbidden_prefixes[] = {"/dev", "/proc", "/sys", "/kern",
                                           NULL};

static int is_ps_safe_path(const char *path) {
  for (size_t i = 0; forbidden_prefixes[i] != NULL; i++) {
    size_t len = strlen(forbidden_prefixes[i]);
    if (strncmp(path, forbidden_prefixes[i], len) == 0) {
      /* Exact match or followed by '/' */
      if (path[len] == '\0' || path[len] == '/') {
        return 0;
      }
    }
  }
  return 1;
}
#endif

/**
 * @brief Combined path validation
 */
static int validate_path(const char *path) {
  if (!is_safe_path(path)) {
    return 0;
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||          \
    defined(PS5)
  if (!is_ps_safe_path(path)) {
    return 0;
  }
#endif

  return 1;
}

/*===========================================================================*
 * JSON HELPERS
 *===========================================================================*/

/**
 * @brief Append a JSON-escaped string to buffer
 *
 * Escapes: " \ / \b \f \n \r \t and control chars
 */
static int json_escape_append(char *buf, size_t cap, size_t *pos,
                              const char *str) {
  size_t p = *pos;

  for (const char *s = str; *s != '\0'; s++) {
    unsigned char c = (unsigned char)*s;

    if (p + 6 >= cap) {
      return -1; /* would overflow */
    }

    switch (c) {
    case '"':
      buf[p++] = '\\';
      buf[p++] = '"';
      break;
    case '\\':
      buf[p++] = '\\';
      buf[p++] = '\\';
      break;
    case '\b':
      buf[p++] = '\\';
      buf[p++] = 'b';
      break;
    case '\f':
      buf[p++] = '\\';
      buf[p++] = 'f';
      break;
    case '\n':
      buf[p++] = '\\';
      buf[p++] = 'n';
      break;
    case '\r':
      buf[p++] = '\\';
      buf[p++] = 'r';
      break;
    case '\t':
      buf[p++] = '\\';
      buf[p++] = 't';
      break;
    default:
      if (c < 0x20) {
        p += (size_t)snprintf(buf + p, cap - p, "\\u%04x", c);
      } else {
        buf[p++] = (char)c;
      }
      break;
    }
  }

  *pos = p;
  return 0;
}

/*===========================================================================*
 * QUERY STRING PARSER
 *===========================================================================*/

/**
 * @brief Extract "path" parameter from query string
 *
 * Given "?path=/foo/bar&other=1", writes "/foo/bar" into out.
 */
static int parse_path_param(const char *query, char *out, size_t out_size) {
  if (query == NULL || out == NULL) {
    return -1;
  }
  if (out_size < 2U) {
    return -1;
  }

  const char *start = strstr(query, "path=");
  if (start == NULL) {
    return -1;
  }
  start += 5; /* skip "path=" */

  size_t in_pos = 0U;
  size_t out_pos = 0U;

  while ((start[in_pos] != '\0') && (start[in_pos] != '&') &&
         (out_pos < (out_size - 1U))) {
    unsigned char ch = (unsigned char)start[in_pos];

    if ((ch == '%') && (start[in_pos + 1] != '\0') &&
        (start[in_pos + 2] != '\0')) {
      unsigned char hi = (unsigned char)start[in_pos + 1];
      unsigned char lo = (unsigned char)start[in_pos + 2];

      unsigned int v_hi;
      unsigned int v_lo;

      if ((hi >= '0') && (hi <= '9')) {
        v_hi = (unsigned int)(hi - '0');
      } else if ((hi >= 'A') && (hi <= 'F')) {
        v_hi = 10U + (unsigned int)(hi - 'A');
      } else if ((hi >= 'a') && (hi <= 'f')) {
        v_hi = 10U + (unsigned int)(hi - 'a');
      } else {
        v_hi = 0xFFFFFFFFU;
      }

      if ((lo >= '0') && (lo <= '9')) {
        v_lo = (unsigned int)(lo - '0');
      } else if ((lo >= 'A') && (lo <= 'F')) {
        v_lo = 10U + (unsigned int)(lo - 'A');
      } else if ((lo >= 'a') && (lo <= 'f')) {
        v_lo = 10U + (unsigned int)(lo - 'a');
      } else {
        v_lo = 0xFFFFFFFFU;
      }

      if ((v_hi != 0xFFFFFFFFU) && (v_lo != 0xFFFFFFFFU)) {
        unsigned char decoded = (unsigned char)((v_hi << 4U) | v_lo);
        if (decoded == '\0') {
          return -1;
        }
        out[out_pos++] = (char)decoded;
        in_pos += 3U;
        continue;
      }
    }

    if (ch == '+') {
      out[out_pos++] = ' ';
    } else {
      out[out_pos++] = (char)ch;
    }
    in_pos++;
  }
  out[out_pos] = '\0';

  /* If empty, default to "/" */
  if (out[0] == '\0') {
    out[0] = '/';
    out[1] = '\0';
  }

  return 0;
}

#if ENABLE_WEB_UPLOAD
static int parse_name_param(const char *query, char *out, size_t out_size) {
  if (query == NULL || out == NULL) {
    return -1;
  }
  if (out_size < 2U) {
    return -1;
  }

  const char *start = strstr(query, "name=");
  if (start == NULL) {
    return -1;
  }
  start += 5; /* skip "name=" */

  size_t in_pos = 0U;
  size_t out_pos = 0U;

  while ((start[in_pos] != '\0') && (start[in_pos] != '&') &&
         (out_pos < (out_size - 1U))) {
    unsigned char ch = (unsigned char)start[in_pos];

    if ((ch == '%') && (start[in_pos + 1] != '\0') &&
        (start[in_pos + 2] != '\0')) {
      unsigned char hi = (unsigned char)start[in_pos + 1];
      unsigned char lo = (unsigned char)start[in_pos + 2];

      unsigned int v_hi;
      unsigned int v_lo;

      if ((hi >= '0') && (hi <= '9')) {
        v_hi = (unsigned int)(hi - '0');
      } else if ((hi >= 'A') && (hi <= 'F')) {
        v_hi = 10U + (unsigned int)(hi - 'A');
      } else if ((hi >= 'a') && (hi <= 'f')) {
        v_hi = 10U + (unsigned int)(hi - 'a');
      } else {
        v_hi = 0xFFFFFFFFU;
      }

      if ((lo >= '0') && (lo <= '9')) {
        v_lo = (unsigned int)(lo - '0');
      } else if ((lo >= 'A') && (lo <= 'F')) {
        v_lo = 10U + (unsigned int)(lo - 'A');
      } else if ((lo >= 'a') && (lo <= 'f')) {
        v_lo = 10U + (unsigned int)(lo - 'a');
      } else {
        v_lo = 0xFFFFFFFFU;
      }

      if ((v_hi != 0xFFFFFFFFU) && (v_lo != 0xFFFFFFFFU)) {
        unsigned char decoded = (unsigned char)((v_hi << 4U) | v_lo);
        if (decoded == '\0') {
          return -1;
        }
        out[out_pos++] = (char)decoded;
        in_pos += 3U;
        continue;
      }
    }

    if (ch == '+') {
      out[out_pos++] = ' ';
    } else {
      out[out_pos++] = (char)ch;
    }
    in_pos++;
  }
  out[out_pos] = '\0';
  if (out[0] == '\0') {
    return -1;
  }

  return 0;
}

static int is_safe_filename(const char *name) {
  if ((name == NULL) || (name[0] == '\0')) {
    return 0;
  }
  if (strstr(name, "..") != NULL) {
    return 0;
  }
  for (const char *p = name; *p != '\0'; p++) {
    if ((*p == '/') || (*p == '\\')) {
      return 0;
    }
  }
  return 1;
}
#endif

/*===========================================================================*
 * REQUEST ROUTER
 *===========================================================================*/

#include "http_csrf.h"

http_response_t *http_api_handle(const http_request_t *request) {
  if (request == NULL) {
    return NULL;
  }

#if ENABLE_WEB_UPLOAD
  /* CSRF Protection for mutating requests */
  if (request->method == HTTP_METHOD_POST) {
    if (http_csrf_validate(request) != 0) {
      return error_json(HTTP_STATUS_403_FORBIDDEN,
                        "Invalid or missing CSRF token");
    }
  }
#endif

  /*  /api/list?path=...  */
  if (strncmp(request->uri, "/api/list", 9) == 0) {
    return api_list(request);
  }

  /*  /api/download?path=...  */
  if (strncmp(request->uri, "/api/download", 13) == 0) {
    return api_download(request);
  }

#if ENABLE_WEB_UPLOAD
  /*  POST /api/create_file?path=...&name=...  */
  if (strncmp(request->uri, "/api/create_file", 16) == 0) {
    return api_create_file(request);
  }
#endif

  /*  Static resources (index.html, style.css, app.js)  */
  return serve_static(request);
}

/*===========================================================================*
 * GET /api/list — Directory Listing
 *
 *  RESPONSE:
 *  {
 *    "path": "/some/dir",
 *    "entries": [
 *      { "name": "file.txt", "type": "file",      "size": 1024 },
 *      { "name": "subdir",   "type": "directory",  "size": 0    }
 *    ]
 *  }
 *===========================================================================*/

static http_response_t *api_list(const http_request_t *request) {
  /* Extract ?path= */
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";

  if (query != NULL) {
    (void)parse_path_param(query, path, sizeof(path));
  }

  /* Security check */
  if (!validate_path(path)) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Directory not found");
  }

  /*
   * STREAMING JSON (Chunked Transfer Encoding)
   * Instead of building the whole JSON in memory (which can exceed 512KB),
   * we send the headers and the opening JSON, then let http_server.c
   * stream the entries one by one.
   */

  /* Build response headers */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Transfer-Encoding", "chunked");

  /* Prepare the opening JSON: {"path":"<escaped>","entries":[ */
  char prefix[2048];
  size_t pos = 0;
  size_t cap = sizeof(prefix);

  pos += (size_t)snprintf(prefix + pos, cap - pos, "{\"path\":\"");
  (void)json_escape_append(prefix, cap, &pos, path);
  pos += (size_t)snprintf(prefix + pos, cap - pos, "\",\"entries\":[");

  /* Finalize headers now (adds \r\n after headers) */
  http_response_finalize(resp);

  /* Now append the prefix as the first CHUNK */
  char chunk_header[32];
  int header_len = snprintf(chunk_header, sizeof(chunk_header), "%zx\r\n", pos);

  if (http_response_append_raw(resp, chunk_header, (size_t)header_len) < 0 ||
      http_response_append_raw(resp, prefix, pos) < 0 ||
      http_response_append_raw(resp, "\r\n", 2) < 0) {
    http_response_destroy(resp);
    closedir(dir);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  /* Set up streaming state */
  resp->stream_dir = dir;
  strncpy(resp->stream_path, path, sizeof(resp->stream_path) - 1);
  resp->stream_path[sizeof(resp->stream_path) - 1] = '\0';

  return resp;
}

/*===========================================================================*
 * GET /api/download — File Download
 *
 *  Reads the file and sends it with Content-Disposition: attachment.
 *  For large files, uses the sendfile_fd field so the server can
 *  stream with sendfile() / read+write loop.
 *===========================================================================*/

static http_response_t *api_download(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "";

  if (query != NULL) {
    (void)parse_path_param(query, path, sizeof(path));
  }

  if (path[0] == '\0') {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  if (!validate_path(path)) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  /* Open file */
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "File not found");
  }

  struct stat st;
  if (fstat(fd, &st) < 0 || S_ISDIR(st.st_mode)) {
    close(fd);
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a regular file");
  }

  /* Extract basename for Content-Disposition */
  const char *basename = strrchr(path, '/');
  basename = (basename != NULL) ? basename + 1 : path;

  /* Build response headers */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/octet-stream");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char disposition[512];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
           basename);
  http_response_add_header(resp, "Content-Disposition", disposition);

  char len_str[32];
  snprintf(len_str, sizeof(len_str), "%lld", (long long)st.st_size);
  http_response_add_header(resp, "Content-Length", len_str);

  /* Finalize headers (no body appended — file sent separately) */
  http_response_finalize(resp);

  /* Store fd so http_server.c can stream the file content */
  resp->sendfile_fd = fd;
  resp->sendfile_offset = 0;
  resp->sendfile_count = (size_t)st.st_size;

  return resp;
}

#if ENABLE_WEB_UPLOAD
static http_response_t *api_create_file(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char dir_path[1024] = "/";
  char name[256];

  if (parse_path_param(query, dir_path, sizeof(dir_path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (parse_name_param(query, name, sizeof(name)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid name");
  }
  if (!is_safe_filename(name)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid file name");
  }
  if (!validate_path(dir_path)) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  char full[1024];
  if (strcmp(dir_path, "/") == 0) {
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    (void)snprintf(full, sizeof(full), "%s/%s", dir_path, name);
  }
  if (!validate_path(full)) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  int fd = pal_file_open(full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to create file");
  }

  if ((request->body != NULL) && (request->body_length > 0U)) {
    if (pal_file_write_all(fd, request->body, request->body_length) < 0) {
      (void)pal_file_close(fd);
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to write file");
    }
  }

  (void)pal_file_close(fd);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len = snprintf(body, sizeof(body),
                     "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\"}", full,
                     name);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}
#endif

/*===========================================================================*
 * STATIC RESOURCE SERVING
 *===========================================================================*/

static http_response_t *serve_static(const http_request_t *request) {
  const char *path = request->uri;
  if (path[0] == '/') {
    path++;
  }
  if (path[0] == '\0') {
    path = "index.html";
  }

  size_t size = 0;
  const char *content = http_get_resource(path, &size);

  if (content == NULL) {
    http_response_t *resp = http_response_create(HTTP_STATUS_404_NOT_FOUND);
    const char *msg = "404 Not Found";
    http_response_set_body(resp, msg, strlen(msg));
    return resp;
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);

  /* MIME type detection & Token Injection */
  if (strstr(path, ".html") != NULL) {
    http_response_add_header(resp, "Content-Type", "text/html; charset=utf-8");

#if ENABLE_WEB_UPLOAD
    /* Inject CSRF token into HTML */
    if (strstr(path, "index.html") != NULL) {
      const char *token = http_csrf_get_token();
      char meta_tag[128];
      snprintf(meta_tag, sizeof(meta_tag),
               "<meta name=\"csrf-token\" content=\"%s\">", token);

      /* Identify placeholder: <!-- CSRF_TOKEN --> */
      const char *placeholder = "<!-- CSRF_TOKEN -->";
      const char *found = strstr(content, placeholder);

      if (found != NULL) {
        size_t prefix_len = (size_t)(found - content);
        size_t suffix_len = size - prefix_len - strlen(placeholder);
        size_t new_size = prefix_len + strlen(meta_tag) + suffix_len;

        char len_str[32];
        snprintf(len_str, sizeof(len_str), "%zu", new_size);
        if (http_response_add_header(resp, "Content-Length", len_str) != 0) {
          return resp;
        }
        if (http_response_finalize(resp) != 0) {
          return resp;
        }
        if (http_response_append_raw(resp, content, prefix_len) != 0) {
          return resp;
        }
        if (http_response_append_raw(resp, meta_tag, strlen(meta_tag)) != 0) {
          return resp;
        }
        if (http_response_append_raw(resp, found + strlen(placeholder),
                                     suffix_len) != 0) {
          return resp;
        }
        return resp;
      }
    }
#endif
  } else if (strstr(path, ".css") != NULL) {
    http_response_add_header(resp, "Content-Type", "text/css; charset=utf-8");
  } else if (strstr(path, ".js") != NULL) {
    http_response_add_header(resp, "Content-Type",
                             "application/javascript; charset=utf-8");
  } else {
    http_response_add_header(resp, "Content-Type", "application/octet-stream");
  }

  http_response_set_body(resp, content, size);
  return resp;
}

/*===========================================================================*
 * ERROR HELPERS
 *===========================================================================*/

static http_response_t *error_json(http_status_t code, const char *message) {
  http_response_t *resp = http_response_create(code);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len = snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);

  http_response_set_body(resp, body, (size_t)len);
  return resp;
}
