/* Shared implementation details for zhttp API modules. */
#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "ftp_path.h"
#include "ftp_server.h"
#include "http_response.h"

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*===========================================================================*
 * ROOT PATH CONFINEMENT
 *
 *   FTP side:   ftp_path_resolve() -> ftp_path_normalize() ->
 *               realpath() -> ftp_path_is_within_root()
 *   HTTP side:  http_validate_and_confine() reuses the same primitives.
 *
 *   Root is stored in http_server.root_path and propagated here via
 *   http_api_set_root() during http_server_create().
 *===========================================================================*/

static char g_http_root[FTP_PATH_MAX] = "/";

/*
 * Pointer to the FTP server context.
 *
 * Set once by http_api_set_server_ctx() during server startup.
 * Used by the /api/network/reset endpoint (Fix #4) to reach the session pool
 * and call pal_network_reset_ftp_stack().
 *
 * NULL if not set (e.g. HTTP server started standalone without FTP).
 * Access is single-threaded from the HTTP event loop — no lock needed.
 */
static ftp_server_context_t *g_ftp_server_ctx = NULL;

/**
 * @brief Set the FTP server context for the HTTP API layer.
 *
 * Must be called after ftp_server_init() and before http_server_create().
 *
 * @param ctx  Pointer to the initialized FTP server context, or NULL to clear.
 */
void http_api_set_server_ctx(ftp_server_context_t *ctx) {
  g_ftp_server_ctx = ctx;
}

void http_api_set_root(const char *root) {
  const char *source = root;
  char canonical[FTP_PATH_MAX];

  if (root == NULL || root[0] == '\0') source = "/";
  if (realpath(source, canonical) != NULL) {
    source = canonical;
  } else if (ftp_path_normalize(source, canonical, sizeof(canonical)) == FTP_OK) {
    source = canonical;
  } else {
    source = "/";
  }

  size_t len = strlen(source);
  if (len >= sizeof(g_http_root)) len = sizeof(g_http_root) - 1U;
  memcpy(g_http_root, source, len);
  g_http_root[len] = '\0';
  while (len > 1U && g_http_root[len - 1U] == '/')
    g_http_root[--len] = '\0';
}

const char *http_api_get_root(void) { return g_http_root; }

ftp_server_context_t *http_api_server_ctx(void) { return g_ftp_server_ctx; }

/**
 * @brief Validate and confine an HTTP path to the server root
 *
 * Reuses the same path security primitives as the FTP core:
 *
 *   Step 1: ftp_path_normalize()       - resolve .., ., //
 *   Step 2: ftp_path_is_within_root()  - pre-realpath confinement
 *   Step 3: realpath()                 - resolve symlinks
 *   Step 4: ftp_path_is_within_root()  - post-realpath re-check
 *
 * @param[in]  input     Raw path from URL (already URL-decoded)
 * @param[in]  root      Root directory (absolute)
 * @param[out] out       Buffer for the canonical confined path
 * @param[in]  out_size  Size of out (>= FTP_PATH_MAX)
 *
 * @return 0 on success, -1 if path escapes root
 *
 * @pre input != NULL, root != NULL, out != NULL
 * @post On success, ftp_path_is_within_root(out, root) == 1
 */
static int http_validate_and_confine(const char *input, const char *root,
                                     char *out, size_t out_size) {
  if ((input == NULL) || (root == NULL) || (out == NULL)) {
    return -1;
  }

  /* Step 1: normalize (resolve .., ., //) */
  char normalized[FTP_PATH_MAX];
  if (ftp_path_normalize(input, normalized, sizeof(normalized)) != FTP_OK) {
    return -1;
  }

  /* Existing paths are checked in canonical form first. This matters on
   * platforms where an absolute alias such as /tmp resolves elsewhere. */
  char real[FTP_PATH_MAX];
  if (realpath(normalized, real) != NULL) {
    if (ftp_path_is_within_root(real, root) != 1) return -1;
    size_t n = strlen(real);
    if (n + 1U > out_size) return -1;
    memcpy(out, real, n + 1U);
    return 0;
  }

  /* Non-existing targets cannot be canonicalized yet. Their normalized
   * path must already be rooted below the canonical server root. */
  if (ftp_path_is_within_root(normalized, root) != 1) return -1;
  size_t n = strlen(normalized);
  if (n + 1U > out_size) return -1;
  memcpy(out, normalized, n + 1U);

  return 0;
}

/*===========================================================================*
 * PATH SECURITY
 *
 *   ┌──────────────────────────────────────────────────┐
 *   │  BLOCKED PATTERNS            REASON              │
 *   │  ../                         traversal           │
 *   │  //                          double-slash trick  │
 *   │  /dev /proc /sys /kern       PS kernel crash     │
 *   │  outside g_http_root         VULN-01/02 fix      │
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
 *
 *   1. Reject traversal patterns ("..")
 *   2. Reject PS kernel-crash paths (/dev, /proc, ...)
 *   3. Confine to g_http_root via http_validate_and_confine()
 *
 * @param[in]  path  Raw input path
 * @param[out] safe  Canonical path confined to root (FTP_PATH_MAX)
 *
 * @return 1 if safe, 0 if rejected
 */
int http_api_validate_path(const char *path, char *safe, size_t safe_size) {
  if (!is_safe_path(path)) {
    return 0;
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||          \
    defined(PS5)
  if (!is_ps_safe_path(path)) {
    return 0;
  }
#endif

  /* Root confinement via ftp_path_normalize + ftp_path_is_within_root */
  if (http_validate_and_confine(path, g_http_root, safe, safe_size) != 0) {
    return 0;
  }

  return 1;
}

int http_api_buf_append_bytes(char *buf, size_t cap, size_t *pos,
                            const char *data, size_t len) {
  if ((buf == NULL) || (pos == NULL) || (data == NULL)) {
    return -1;
  }
  if (*pos > cap) {
    return -1;
  }
  if (len > (cap - *pos)) {
    return -1;
  }
  if (len > 0U) {
    memcpy(buf + *pos, data, len);
    *pos += len;
  }
  return 0;
}

int http_api_buf_append_cstr(char *buf, size_t cap, size_t *pos,
                           const char *str) {
  if (str == NULL) {
    return -1;
  }
  return http_api_buf_append_bytes(buf, cap, pos, str, strlen(str));
}

int http_api_buf_append_u64(char *buf, size_t cap, size_t *pos, uint64_t v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_api_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

int http_api_buf_append_u32(char *buf, size_t cap, size_t *pos, uint32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_api_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

int http_api_buf_append_i32(char *buf, size_t cap, size_t *pos, int32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRId32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return http_api_buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

/*===========================================================================*
 * JSON HELPERS
 *===========================================================================*/

/**
 * @brief Append a JSON-escaped string to buffer
 *
 * Escapes: " \ / \b \f \n \r \t and control chars
 */
int http_api_json_escape_append(char *buf, size_t cap, size_t *pos,
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

static int hex_nibble(unsigned char c) {
  if (c >= '0' && c <= '9') return (int)(c - '0');
  if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
  if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
  return -1;
}

static const char *query_value_start(const char *query, const char *key) {
  if (query == NULL || key == NULL || key[0] == '\0') return NULL;
  size_t key_len = strlen(key);
  for (const char *q = query; (q = strstr(q, key)) != NULL; q += key_len) {
    if ((q == query || q[-1] == '?' || q[-1] == '&') && q[key_len] == '=')
      return q + key_len + 1U;
  }
  return NULL;
}

static int decode_query_value(const char *start, char *out, size_t out_size,
                              int allow_empty) {
  if (start == NULL || out == NULL || out_size < 2U) return -1;
  size_t wi = 0U;
  for (size_t ri = 0U; start[ri] != '\0' && start[ri] != '&'; ri++) {
    unsigned char ch = (unsigned char)start[ri];
    if (wi + 1U >= out_size) return -1;
    if (ch == '%') {
      if (start[ri + 1U] == '\0' || start[ri + 2U] == '\0') return -1;
      int hi = hex_nibble((unsigned char)start[ri + 1U]);
      int lo = hex_nibble((unsigned char)start[ri + 2U]);
      if (hi < 0 || lo < 0) return -1;
      ch = (unsigned char)((hi << 4) | lo);
      if (ch == '\0') return -1;
      ri += 2U;
    } else if (ch == '+') {
      ch = ' ';
    }
    out[wi++] = (char)ch;
  }
  out[wi] = '\0';
  return (allow_empty || wi > 0U) ? 0 : -1;
}

int http_api_parse_query_param(const char *query, const char *key,
                               char *out, size_t out_size) {
  return decode_query_value(query_value_start(query, key), out, out_size, 0);
}

int http_api_parse_path_param(const char *query, char *out, size_t out_size) {
  const char *start = query_value_start(query, "path");
  if (decode_query_value(start, out, out_size, 1) != 0) return -1;
  if (out[0] == '\0') {
    out[0] = '/';
    out[1] = '\0';
  }
  return 0;
}

#if ENABLE_WEB_UPLOAD
int http_api_parse_name_param(const char *query, char *out, size_t out_size) {
  return http_api_parse_query_param(query, "name", out, out_size);
}

int http_api_is_safe_filename(const char *name) {
  if (name == NULL || name[0] == '\0' || strcmp(name, ".") == 0 ||
      strcmp(name, "..") == 0)
    return 0;
  for (const char *q = name; *q != '\0'; q++) {
    if (*q == '/' || *q == '\\' || (unsigned char)*q < 0x20U) return 0;
  }
  return 1;
}
#endif

/*===========================================================================*
 * ERROR HELPERS
 *===========================================================================*/

http_response_t *http_api_error_json(http_status_t code, const char *message) {
  if (message == NULL) message = "error";
  http_response_t *resp = http_response_create(code);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[768];
  size_t pos = 0U;
  if (http_api_buf_append_cstr(body, sizeof(body), &pos, "{\"error\":\"") != 0 ||
      http_api_json_escape_append(body, sizeof(body), &pos, message) != 0 ||
      http_api_buf_append_cstr(body, sizeof(body), &pos, "\"}") != 0) {
    static const char fallback[] = "{\"error\":\"Response too large\"}";
    http_response_set_body(resp, fallback, sizeof(fallback) - 1U);
    return resp;
  }
  http_response_set_body(resp, body, pos);
  return resp;
}

http_response_t *http_api_status_json_200(int ok, const char *message, int code) {
  if (message == NULL) message = ok ? "ok" : "error";
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  char body[768];
  size_t pos = 0U;
  char prefix[96];
  int n = snprintf(prefix, sizeof(prefix),
                   "{\"ok\":%s,\"status\":\"%s\",\"message\":\"",
                   ok ? "true" : "false", ok ? "ok" : "error");
  if (n < 0 || (size_t)n >= sizeof(prefix) ||
      http_api_buf_append_bytes(body, sizeof(body), &pos, prefix, (size_t)n) != 0 ||
      http_api_json_escape_append(body, sizeof(body), &pos, message) != 0) {
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
  }
  n = snprintf(body + pos, sizeof(body) - pos, "\",\"code\":%d}", code);
  if (n < 0 || (size_t)n >= sizeof(body) - pos) {
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
  }
  pos += (size_t)n;
  http_response_set_body(resp, body, pos);
  return resp;
}

http_response_t *http_api_png_fallback_response(void) {
  static const uint8_t k_png_1x1[] = {
      0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00,
      0x0D, 0x49, 0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
      0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89,
      0x00, 0x00, 0x00, 0x0D, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63,
      0x60, 0x60, 0x60, 0xF8, 0x0F, 0x00, 0x01, 0x04, 0x01, 0x00, 0x5F,
      0xE2, 0x26, 0x05, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44,
      0xAE, 0x42, 0x60, 0x82};

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "public, max-age=3600");
  http_response_set_body(resp, k_png_1x1, sizeof(k_png_1x1));
  return resp;
}

http_response_t *http_api_legacy_disabled_json(const char *json_body) {
  if (json_body == NULL) {
    json_body = "{\"ok\":false}";
  }
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, json_body, strlen(json_body));
  return resp;
}


int http_api_route_is(const char *uri, const char *path) {
  if (uri == NULL || path == NULL) return 0;
  size_t n = strlen(path);
  return strncmp(uri, path, n) == 0 && (uri[n] == '\0' || uri[n] == '?');
}
