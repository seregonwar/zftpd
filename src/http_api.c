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
#include "ftp_path.h"
#include "http_config.h"
#include "pal_fileio.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(__FreeBSD__)
#include <sys/mount.h>  /* fstatfs, struct statfs — for sendfile safety check */
/* PS4/PS5 libkernel exports _fstatfs, not fstatfs */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
extern int _fstatfs(int, struct statfs *);
#define http_fstatfs _fstatfs
#else
#define http_fstatfs fstatfs
#endif
#endif /* PLATFORM_PS4 || PLATFORM_PS5 || __FreeBSD__ */
#include <time.h>
#if defined(PLATFORM_LINUX) && __has_include(<sys/sysinfo.h>)
#define HAS_SYSINFO 1
#include <sys/sysinfo.h>
#endif
#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) ||                        \
    defined(PLATFORM_PS5) || defined(PS4) || defined(PS5) ||                   \
    defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif
#include <unistd.h>

/*===========================================================================*
 * EMBEDDED RESOURCES (defined in http_resources.c)
 *===========================================================================*/

extern const char *http_get_resource(const char *path, size_t *size);

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

void http_api_set_root(const char *root) {
  if ((root == NULL) || (root[0] == '\0')) {
    g_http_root[0] = '/';
    g_http_root[1] = '\0';
    return;
  }
  size_t len = strlen(root);
  if (len >= sizeof(g_http_root)) {
    len = sizeof(g_http_root) - 1U;
  }
  memcpy(g_http_root, root, len);
  g_http_root[len] = '\0';

  /* Strip trailing slash (unless root is exactly "/") */
  while (len > 1U && g_http_root[len - 1U] == '/') {
    g_http_root[--len] = '\0';
  }
}

const char *http_api_get_root(void) { return g_http_root; }

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

  /* Step 2: pre-realpath confinement check */
  if (ftp_path_is_within_root(normalized, root) != 1) {
    return -1;
  }

  /* Step 3: resolve symlinks */
  char real[FTP_PATH_MAX];
  if (realpath(normalized, real) != NULL) {
    /* Step 4: post-realpath re-check (anti symlink traversal) */
    if (ftp_path_is_within_root(real, root) != 1) {
      return -1;
    }
    size_t n = strlen(real);
    if ((n + 1U) > out_size) {
      return -1;
    }
    memcpy(out, real, n + 1U);
  } else {
    /*
     * Path doesn't exist yet (upload target, new directory).
     * Pre-realpath check already passed — use normalized.
     */
    size_t n = strlen(normalized);
    if ((n + 1U) > out_size) {
      return -1;
    }
    memcpy(out, normalized, n + 1U);
  }

  return 0;
}

/*===========================================================================*
 * FORWARD DECLARATIONS
 *===========================================================================*/

static http_response_t *api_list(const http_request_t *request);
static http_response_t *api_download(const http_request_t *request);
static http_response_t *api_stats(const http_request_t *request);
static http_response_t *api_stats_ram(const http_request_t *request);
static http_response_t *api_stats_system(const http_request_t *request);
static http_response_t *api_disk_info(const http_request_t *request);
static http_response_t *api_disk_tree(const http_request_t *request);
static http_response_t *api_processes(const http_request_t *request);
static http_response_t *api_process_kill(const http_request_t *request);
static http_response_t *serve_static(const http_request_t *request);
#if ENABLE_WEB_UPLOAD
static http_response_t *api_create_file(const http_request_t *request);
static http_response_t *api_delete(const http_request_t *request);
static http_response_t *api_rename(const http_request_t *request);
static http_response_t *api_copy(const http_request_t *request);
static http_response_t *api_copy_progress(const http_request_t *request);
static http_response_t *api_copy_cancel(const http_request_t *request);
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
static int validate_path(const char *path, char *safe, size_t safe_size) {
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

static int buf_append_bytes(char *buf, size_t cap, size_t *pos,
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

static int buf_append_cstr(char *buf, size_t cap, size_t *pos,
                           const char *str) {
  if (str == NULL) {
    return -1;
  }
  return buf_append_bytes(buf, cap, pos, str, strlen(str));
}

static int buf_append_u64(char *buf, size_t cap, size_t *pos, uint64_t v) {
  char tmp[32];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu64, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

static int buf_append_u32(char *buf, size_t cap, size_t *pos, uint32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRIu32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

static int buf_append_i32(char *buf, size_t cap, size_t *pos, int32_t v) {
  char tmp[16];
  int n = snprintf(tmp, sizeof(tmp), "%" PRId32, v);
  if ((n < 0) || ((size_t)n >= sizeof(tmp))) {
    return -1;
  }
  return buf_append_bytes(buf, cap, pos, tmp, (size_t)n);
}

static int u64_mul_checked(uint64_t a, uint64_t b, uint64_t *out) {
  if (out == NULL) {
    return -1;
  }
  if ((a != 0U) && (b > (UINT64_MAX / a))) {
    *out = UINT64_MAX;
    return -1;
  }
  *out = a * b;
  return 0;
}

static int get_disk_stats_bytes(const char *path, uint64_t *total,
                                uint64_t *used, uint64_t *free_b) {
  if ((path == NULL) || (total == NULL) || (used == NULL) || (free_b == NULL)) {
    return -1;
  }

  struct statvfs s;
  if (statvfs(path, &s) != 0) {
    return -1;
  }

  uint64_t fr = (uint64_t)((s.f_frsize != 0U) ? s.f_frsize : s.f_bsize);
  uint64_t total_bytes = 0U;
  uint64_t free_bytes = 0U;

  if (u64_mul_checked(fr, (uint64_t)s.f_blocks, &total_bytes) != 0) {
    return -1;
  }
  if (u64_mul_checked(fr, (uint64_t)s.f_bavail, &free_bytes) != 0) {
    return -1;
  }

  uint64_t used_bytes =
      (total_bytes >= free_bytes) ? (total_bytes - free_bytes) : total_bytes;

  *total = total_bytes;
  *free_b = free_bytes;
  *used = used_bytes;
  return 0;
}

static int get_best_disk_stats(const char *hint_path, const char **out_path,
                               uint64_t *total, uint64_t *used,
                               uint64_t *free_b) {
  if ((out_path == NULL) || (total == NULL) || (used == NULL) ||
      (free_b == NULL)) {
    return -1;
  }

  /* Ordered: real user data mounts first, then root fallback.
   * macOS home and /Volumes entries come before PS4/PS5 paths. */
  const char *candidates[] = {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
      "/Users", "/",
#elif defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
      "/user", "/data", "/system_data", "/mnt/usb0", "/mnt/usb1", "/",
#else
      "/home", "/",
#endif
      NULL,
  };

  const char *best = NULL;
  uint64_t best_total = 0U;
  uint64_t best_used = 0U;
  uint64_t best_free = 0U;

  if (hint_path != NULL) {
    uint64_t t = 0U, u = 0U, f = 0U;
    if (get_disk_stats_bytes(hint_path, &t, &u, &f) == 0) {
      best = hint_path;
      best_total = t;
      best_used = u;
      best_free = f;
    }
  }

  for (size_t i = 0U; candidates[i] != NULL; i++) {
    uint64_t t = 0U, u = 0U, f = 0U;
    if (get_disk_stats_bytes(candidates[i], &t, &u, &f) != 0) {
      continue;
    }
    if (t > best_total) {
      best = candidates[i];
      best_total = t;
      best_used = u;
      best_free = f;
    }
  }

  if (best == NULL) {
    return -1;
  }

  *out_path = best;
  *total = best_total;
  *used = best_used;
  *free_b = best_free;
  return 0;
}

static int count_dir_items(const char *path, uint32_t *out_count) {
  if ((path == NULL) || (out_count == NULL)) {
    return -1;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    return -1;
  }

  uint32_t count = 0U;
  for (;;) {
    errno = 0;
    struct dirent *ent = readdir(dir);
    if (ent == NULL) {
      if (errno != 0) {
        closedir(dir);
        return -1;
      }
      break;
    }
    if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
      continue;
    }
    if (count == UINT32_MAX) {
      closedir(dir);
      return -1;
    }
    count++;
  }

  closedir(dir);
  *out_count = count;
  return 0;
}

static int get_boot_epoch_seconds(uint64_t *out_epoch) {
  if (out_epoch == NULL) {
    return -1;
  }

#if defined(HAS_SYSINFO)
  struct sysinfo info;
  if (sysinfo(&info) != 0) {
    return -1;
  }
  time_t now = time(NULL);
  if (now < 0) {
    return -1;
  }
  uint64_t now_u = (uint64_t)now;
  uint64_t up_u = (uint64_t)info.uptime;
  *out_epoch = (now_u >= up_u) ? (now_u - up_u) : 0U;
  return 0;
#elif defined(PLATFORM_MACOS) || defined(__APPLE__) || defined(PLATFORM_PS4) \
    || defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
  struct timeval bt;
  size_t sz = sizeof(bt);
  if (sysctlbyname("kern.boottime", &bt, &sz, NULL, 0) != 0) {
    return -1;
  }
  if (sz < sizeof(bt)) {
    return -1;
  }
  if (bt.tv_sec < 0) {
    return -1;
  }
  *out_epoch = (uint64_t)bt.tv_sec;
  return 0;
#else
  (void)out_epoch;
  return -1;
#endif
}

static int get_cpu_temp_c(int32_t *out_c) {
  if (out_c == NULL) {
    return -1;
  }

#if defined(PLATFORM_PS4) || defined(PS4)
  __attribute__((weak)) int32_t sceKernelGetCpuTemperature(
      uint64_t *temperature);
  if (sceKernelGetCpuTemperature != NULL) {
    uint64_t raw = 0U;
    int32_t rc = sceKernelGetCpuTemperature(&raw);
    if (rc == 0) {
      if ((raw >= 20U) && (raw <= 110U)) {
        *out_c = (int32_t)raw;
        return 0;
      }
    }
  }
#endif

#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) ||                        \
    defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
  const char *names[] = {
      "dev.cpu.0.temperature",
      "dev.cpu.0.coretemp.temperature",
      "dev.cpu.0.temp",
      "dev.amdtemp.0.temperature",
      "dev.amdtemp.0.core0.sensor0",
      "dev.thermal.0.temperature",
      "hw.acpi.thermal.tz0.temperature",
      "hw.temperature",
      NULL,
  };

  for (size_t i = 0U; names[i] != NULL; i++) {
    int v = 0;
    size_t sz = sizeof(v);
    if (sysctlbyname(names[i], &v, &sz, NULL, 0) != 0) {
      continue;
    }
    if (sz != sizeof(v)) {
      continue;
    }

    int32_t c = 0;
    if (v > 1000) {
      int32_t dk = (int32_t)v;
      c = (dk - 2731 + 5) / 10;
    } else {
      c = (int32_t)v;
    }
    if ((c < -40) || (c > 200)) {
      continue;
    }
    *out_c = c;
    return 0;
  }

  return -1;
#else
  (void)out_c;
  return -1;
#endif
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

  /*  /api/stats/ram  */
  if (strncmp(request->uri, "/api/stats/ram", 14) == 0) {
    return api_stats_ram(request);
  }

  /*  /api/stats/system  */
  if (strncmp(request->uri, "/api/stats/system", 17) == 0) {
    return api_stats_system(request);
  }

  /*  /api/stats?path=... (legacy widget)  */
  if (strncmp(request->uri, "/api/stats", 10) == 0) {
    return api_stats(request);
  }

  /*  /api/disk/info  */
  if (strncmp(request->uri, "/api/disk/info", 14) == 0) {
    return api_disk_info(request);
  }

  /*  /api/disk/tree?path=...  */
  if (strncmp(request->uri, "/api/disk/tree", 14) == 0) {
    return api_disk_tree(request);
  }

  /*  POST /api/process/kill  */
  if (strncmp(request->uri, "/api/process/kill", 17) == 0) {
    return api_process_kill(request);
  }

  /*  GET /api/processes  */
  if (strncmp(request->uri, "/api/processes", 14) == 0) {
    return api_processes(request);
  }

#if ENABLE_WEB_UPLOAD
  /*  POST /api/create_file?path=...&name=...  */
  if (strncmp(request->uri, "/api/create_file", 16) == 0) {
    return api_create_file(request);
  }

  /*  POST /api/delete?path=...  */
  if (strncmp(request->uri, "/api/delete", 11) == 0) {
    return api_delete(request);
  }

  /*  POST /api/rename?path=...&name=...  */
  if (strncmp(request->uri, "/api/rename", 11) == 0) {
    return api_rename(request);
  }

  /*  POST /api/copy?src=...&dst=...  */
  if (strncmp(request->uri, "/api/copy_progress", 18) == 0) {
    return api_copy_progress(request);
  }
  if (strncmp(request->uri, "/api/copy_cancel", 16) == 0) {
    return api_copy_cancel(request);
  }
  if (strncmp(request->uri, "/api/copy", 9) == 0) {
    return api_copy(request);
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

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  DIR *dir = opendir(safe);
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

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Path traversal attempt detected");
  }

  /* Open file */
  int fd = open(safe, O_RDONLY);
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
  resp->sendfile_fd     = fd;
  resp->sendfile_offset = 0;
  resp->sendfile_count  = (size_t)st.st_size;

  /*
   * SENDFILE SAFETY CHECK — must happen before http_server.c touches the fd.
   *
   * On PS5/PS4 (FreeBSD), calling sendfile(2) on vnodes backed by certain
   * filesystems causes an IMMEDIATE KERNEL PANIC:
   *
   *   exfatfs  — USB drives formatted exFAT: the kernel exFAT vnode does not
   *               implement vm_pager_ops, so sendfile() dereferences a null
   *               function pointer.
   *   msdosfs  — FAT32 USB drives: same broken pager ops.
   *   nullfs   — bind-mount: inherits the pager of the origin vnode.  If the
   *               origin is exFAT, the nullfs vnode also KPs.
   *   pfsmnt   — PlayStation FS mount (/user/av_contents, game data mounts):
   *               sendfile() sends corrupt/incomplete data.
   *   pfs      — raw PFS on internal SSD (/data, /user):
   *               same broken pager as pfsmnt.
   *
   * CRITICAL: on these filesystems errno is NEVER set — the kernel triple-
   * faults before returning to userspace.  Our EINVAL fallback in
   * pal_sendfile() cannot help because execution never reaches it.
   *
   * The fix: detect the filesystem type on the open fd with fstatfs() and set
   * sendfile_safe = 0.  http_server.c will then use pread()+send_all() for
   * the entire transfer, bypassing sendfile(2) entirely.
   *
   * On Linux and macOS sendfile() is always safe; sendfile_safe = 1.
   * On FreeBSD/PS5/PS4 default to 0 (unsafe) and only enable for filesystems
   * known to be safe (ufs, tmpfs, zfs, ffs — internal NVMe on PS5 via
   * the native FFS layer if ever used).
   */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(__FreeBSD__)
  {
    int sf_safe = 0; /* conservative default: assume unsafe */
    struct statfs sfs;
    if (http_fstatfs(fd, &sfs) == 0) {
      const char *t = sfs.f_fstypename;
      /*
       * Whitelist: filesystems known to work correctly with sendfile(2).
       * Everything not on this list is treated as unsafe.
       *
       * ufs/ffs  — standard FreeBSD FFS (unlikely on PS5 but correct)
       * tmpfs    — memory-backed (safe, though uncommon for large files)
       * zfs      — ZFS (safe on standard FreeBSD)
       *
       * NOT whitelisted (KP or corrupt data):
       *   exfatfs, msdosfs, nullfs, pfsmnt, pfs
       */
      if ((strcmp(t, "ufs")   == 0) ||
          (strcmp(t, "ffs")   == 0) ||
          (strcmp(t, "tmpfs") == 0) ||
          (strcmp(t, "zfs")   == 0)) {
        sf_safe = 1;
      }
    }
    /* fstatfs failure: stay with 0 (unsafe) — tolerate the perf hit */
    resp->sendfile_safe = sf_safe;
  }
#else
  /* Linux / macOS: sendfile() is always safe */
  resp->sendfile_safe = 1;
#endif

  return resp;
}

static http_response_t *api_stats(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";

  if (query != NULL) {
    (void)parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  uint64_t disk_total = 0U;
  uint64_t disk_used = 0U;
  uint64_t disk_free = 0U;
  const char *disk_path = NULL;
  int disk_ok = get_best_disk_stats(path, &disk_path, &disk_total, &disk_used,
                                    &disk_free);

  uint32_t items = 0U;
  int items_ok = count_dir_items(path, &items);

  uint64_t boot_epoch = 0U;
  int boot_ok = get_boot_epoch_seconds(&boot_epoch);

  int32_t temp_c = 0;
  int temp_ok = get_cpu_temp_c(&temp_c);

  char body[1024];
  size_t pos = 0U;
  size_t cap = sizeof(body);

  if (buf_append_cstr(body, cap, &pos, "{\"path\":\"") != 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  if (json_escape_append(body, cap, &pos, path) != 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  if (buf_append_cstr(body, cap, &pos, "\"") != 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  if (disk_ok == 0) {
    if (buf_append_cstr(body, cap, &pos, ",\"disk_used\":") != 0 ||
        buf_append_u64(body, cap, &pos, disk_used) != 0 ||
        buf_append_cstr(body, cap, &pos, ",\"disk_total\":") != 0 ||
        buf_append_u64(body, cap, &pos, disk_total) != 0 ||
        buf_append_cstr(body, cap, &pos, ",\"disk_free\":") != 0 ||
        buf_append_u64(body, cap, &pos, disk_free) != 0 ||
        buf_append_cstr(body, cap, &pos, ",\"disk_path\":\"") != 0 ||
        json_escape_append(body, cap, &pos,
                           (disk_path != NULL) ? disk_path : "") != 0 ||
        buf_append_cstr(body, cap, &pos, "\"") != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (buf_append_cstr(body, cap, &pos,
                        ",\"disk_used\":null,\"disk_total\":null,"
                        "\"disk_free\":null,\"disk_path\":null") != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (temp_ok == 0) {
    if (buf_append_cstr(body, cap, &pos, ",\"cpu_temp\":") != 0 ||
        buf_append_i32(body, cap, &pos, temp_c) != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (buf_append_cstr(body, cap, &pos, ",\"cpu_temp\":null") != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (boot_ok == 0) {
    if (buf_append_cstr(body, cap, &pos, ",\"uptime\":") != 0 ||
        buf_append_u64(body, cap, &pos, boot_epoch) != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (buf_append_cstr(body, cap, &pos, ",\"uptime\":null") != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (items_ok == 0) {
    if (buf_append_cstr(body, cap, &pos, ",\"items_in_dir\":") != 0 ||
        buf_append_u32(body, cap, &pos, items) != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (buf_append_cstr(body, cap, &pos, ",\"items_in_dir\":null") != 0) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (buf_append_cstr(body, cap, &pos, "}") != 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, pos);
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
  char safe_dir[FTP_PATH_MAX];
  if (!validate_path(dir_path, safe_dir, sizeof(safe_dir))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  char full[FTP_PATH_MAX];
  if (strcmp(safe_dir, "/") == 0) {
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    (void)snprintf(full, sizeof(full), "%s/%s", safe_dir, name);
  }

  char safe_full[FTP_PATH_MAX];
  if (!validate_path(full, safe_full, sizeof(safe_full))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  int fd = pal_file_open(safe_full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
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
  int len =
      snprintf(body, sizeof(body),
               "{\"ok\":true,\"path\":\"%s\",\"name\":\"%s\"}", full, name);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*===========================================================================*
 * POST /api/delete — Delete file or empty directory
 *
 *   ┌─────────────────────────────────────────────┐
 *   │  POST /api/delete?path=/some/file.txt       │
 *   │                                             │
 *   │  file   -> pal_file_delete(path)            │
 *   │  dir    -> pal_dir_remove(path)  (empty)    │
 *   │  result -> {"ok":true}                      │
 *   └─────────────────────────────────────────────┘
 *===========================================================================*/

static http_response_t *api_delete(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[1024] = "";
  if (parse_path_param(query, path, sizeof(path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  /* Refuse to delete the root itself */
  if (strcmp(safe, g_http_root) == 0) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot delete root");
  }

  struct stat st;
  if (stat(safe, &st) != 0) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  ftp_error_t rc;
  if (S_ISDIR(st.st_mode)) {
    /*
     * DIRECTORY DELETE
     *
     * Standard rmdir(2) fails with ENOTEMPTY if the directory has any
     * contents — including hidden system files (e.g. PFS metadata on
     * /data, exFAT recycle-bin entries on USB) that the user cannot see
     * from a normal listing.  This caused "random" delete failures because
     * some directories appeared empty in the UI but were not at the kernel
     * level.
     *
     * Strategy:
     *   1. Try rmdir() first — fast, safe, and correct for truly empty dirs.
     *   2. If that returns ENOTEMPTY and the caller passed ?recursive=1,
     *      fall back to pal_dir_remove_recursive() (depth-first unlink tree).
     *   3. Without ?recursive=1 on a non-empty dir: return 409 Conflict
     *      with a clear message so the web UI can prompt for confirmation
     *      rather than silently succeeding or giving a generic 500.
     *
     * SAFETY: recursive delete is opt-in — the client must explicitly send
     * ?recursive=1.  A plain POST /api/delete?path=X on a non-empty dir
     * returns 409 instead of deleting everything silently.
     *
     * @note pal_dir_remove_recursive() is the same depth-first cleanup
     *       used in the rollback path of pal_copy_cross_device_r_ex, so
     *       its error handling (unlink failures on locked files, etc.) is
     *       already well-exercised.
     */
    rc = pal_dir_remove(safe); /* try rmdir first */

    if (rc != FTP_OK) {
      /* Check if the failure was ENOTEMPTY (or our mapped error code) */
      const char *recursive_flag = strstr(query, "recursive=1");
      if (recursive_flag != NULL) {
        /* Caller explicitly requested recursive delete — proceed */
        rc = pal_dir_remove_recursive_pub(safe);
        if (rc != FTP_OK) {
          return error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                            "Recursive delete failed (permission denied or I/O error)");
        }
      } else {
        /*
         * Return 409 Conflict — the directory is not empty and the
         * caller did not ask for recursive deletion.
         *
         * The web UI should catch this and either:
         *   (a) Show a confirmation dialog ("Delete all contents?") then
         *       retry with ?recursive=1, or
         *   (b) Tell the user to empty the folder first.
         */
        return error_json(HTTP_STATUS_409_CONFLICT,
                          "Directory is not empty. Use recursive=1 to force.");
      }
    }
  } else {
    rc = pal_file_delete(safe);
    if (rc != FTP_OK) {
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                        "Failed to delete file");
    }
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*===========================================================================*
 * POST /api/rename — Rename file or directory in-place
 *
 *   ┌─────────────────────────────────────────────────┐
 *   │  POST /api/rename?path=/dir/old.txt&name=new    │
 *   │                                                 │
 *   │  old  = validate_path(path)                     │
 *   │  new  = parent(old) + '/' + name                │
 *   │  pal_file_rename(old, new)                      │
 *   │  result -> {"ok":true,"path":"/dir/new"}        │
 *   └─────────────────────────────────────────────────┘
 *===========================================================================*/

static http_response_t *api_rename(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[1024] = "";
  char name[256];
  if (parse_path_param(query, path, sizeof(path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (parse_name_param(query, name, sizeof(name)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid name");
  }
  if (!is_safe_filename(name)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid file name");
  }

  /* Validate old path */
  char safe_old[FTP_PATH_MAX];
  if (!validate_path(path, safe_old, sizeof(safe_old))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  /* Check old exists */
  if (pal_path_exists(safe_old) != 1) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Path not found");
  }

  /*
   * Build new path:  parent(safe_old) + '/' + name
   *
   *   /data/files/old.txt  ->  /data/files/  (parent)
   *   parent + "new.txt"   ->  /data/files/new.txt
   */
  char new_path[FTP_PATH_MAX];
  const char *last_slash = strrchr(safe_old, '/');
  if (last_slash == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Internal path error");
  }
  size_t parent_len = (size_t)(last_slash - safe_old);
  if (parent_len == 0U) {
    /* file is directly under root "/" */
    (void)snprintf(new_path, sizeof(new_path), "/%s", name);
  } else {
    (void)snprintf(new_path, sizeof(new_path), "%.*s/%s", (int)parent_len,
                   safe_old, name);
  }

  /* Validate new path stays within root */
  char safe_new[FTP_PATH_MAX];
  if (!validate_path(new_path, safe_new, sizeof(safe_new))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Destination forbidden");
  }

  ftp_error_t rc = pal_file_rename(safe_old, safe_new);
  if (rc != FTP_OK) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Rename failed");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char body[512];
  int len =
      snprintf(body, sizeof(body), "{\"ok\":true,\"path\":\"%s\"}", new_path);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*===========================================================================*
 * COPY PROGRESS TRACKING  (background pthread)
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │           Browser            Server                  │
 *   │    POST /api/copy ──────────►  spawn pthread         │
 *   │    ◄── {ok:true,async:true}       │                  │
 *   │                                   │ copy thread      │
 *   │    GET /api/copy_progress ◄────── atomic counters    │
 *   │    (polled every 500ms)            │                  │
 *   │                                   ▼                  │
 *   │    GET /api/copy_progress ──► done=true              │
 *   └──────────────────────────────────────────────────────┘
 *===========================================================================*/

#include <pthread.h>

typedef struct {
  _Atomic uint64_t bytes_copied;
  _Atomic uint64_t total_bytes;
  _Atomic int active;     /* 1 while copy thread is running  */
  _Atomic int done;       /* 1 when copy finished             */
  _Atomic int error;      /* 1 if copy failed                 */
  _Atomic int cancel;     /* 1 to request cancellation        */
  _Atomic int error_code; /* ftp_error_t value on failure     */
  _Atomic int error_errno; /* errno captured at failure point */
} copy_progress_t;

static copy_progress_t g_copy_progress = {0};

static int copy_progress_cb(uint64_t bytes_copied, void *user_data) {
  (void)user_data;
  atomic_store(&g_copy_progress.bytes_copied, bytes_copied);
  /* Check cancellation flag — return -1 to abort copy */
  return (atomic_load(&g_copy_progress.cancel) != 0) ? -1 : 0;
}

/* Background copy thread */
typedef struct {
  char src[FTP_PATH_MAX];
  char dst[FTP_PATH_MAX];
  int *out_errno; /* points to g_copy_progress.error_errno storage (unused; errno captured inside) */
} copy_thread_args_t;

static void *copy_thread_fn(void *arg) {
  copy_thread_args_t *a = (copy_thread_args_t *)arg;

  int saved_errno = 0;
  ftp_error_t rc =
      pal_file_copy_recursive_ex(a->src, a->dst, 1, copy_progress_cb,
                                 NULL, &saved_errno);
  if ((rc != FTP_OK) || (atomic_load(&g_copy_progress.cancel) != 0)) {
    atomic_store(&g_copy_progress.error, 1);
    atomic_store(&g_copy_progress.error_code, (int)rc);
    atomic_store(&g_copy_progress.error_errno, saved_errno);
  }
  atomic_store(&g_copy_progress.active, 0);
  atomic_store(&g_copy_progress.done, 1);

  free(a);
  return NULL;
}

/*  GET /api/copy_progress  */
static http_response_t *api_copy_progress(const http_request_t *request) {
  (void)request;

  uint64_t copied = atomic_load(&g_copy_progress.bytes_copied);
  uint64_t total = atomic_load(&g_copy_progress.total_bytes);
  int active = atomic_load(&g_copy_progress.active);
  int done = atomic_load(&g_copy_progress.done);
  int err = atomic_load(&g_copy_progress.error);
  int err_code = atomic_load(&g_copy_progress.error_code);
  int err_errno = atomic_load(&g_copy_progress.error_errno);

  char body[320];
  int len =
      snprintf(body, sizeof(body),
               "{\"active\":%s,\"done\":%s,\"error\":%s,\"error_code\":%d,"
               "\"error_errno\":%d,"
               "\"bytes_copied\":%" PRIu64 ",\"total_bytes\":%" PRIu64 "}",
               active ? "true" : "false", done ? "true" : "false",
               err ? "true" : "false", err_code, err_errno, copied, total);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*  POST /api/copy_cancel  */
static http_response_t *api_copy_cancel(const http_request_t *request) {
  (void)request;
  atomic_store(&g_copy_progress.cancel, 1);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*===========================================================================*
 * POST /api/copy — Server-side file/directory copy
 *
 *   ┌──────────────────────────────────────────────────────┐
 *   │  POST /api/copy?path=/src/file&dst=/dest/folder      │
 *   │                                                      │
 *   │  src  = validate_path(path)                          │
 *   │  dst  = validate_path(dst) + '/' + basename(src)     │
 *   │  pal_file_copy_recursive_ex(src, dst, keep_src=1)    │
 *   │  result -> {"ok":true}                               │
 *   └──────────────────────────────────────────────────────┘
 *===========================================================================*/

static int parse_dst_param(const char *query, char *out, size_t out_size) {
  if (query == NULL || out == NULL) {
    return -1;
  }
  if (out_size < 2U) {
    return -1;
  }

  const char *start = strstr(query, "dst=");
  if (start == NULL) {
    return -1;
  }
  start += 4; /* skip "dst=" */

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
    out[0] = '/';
    out[1] = '\0';
  }

  return 0;
}

static http_response_t *api_copy(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST for this endpoint");
  }

  /* Reject if a copy is already in progress */
  if (atomic_load(&g_copy_progress.active) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "A copy operation is already in progress");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char src_path[1024] = "";
  char dst_dir[1024] = "";
  if (parse_path_param(query, src_path, sizeof(src_path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid path");
  }
  if (parse_dst_param(query, dst_dir, sizeof(dst_dir)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or invalid dst parameter");
  }

  /* Validate source */
  char safe_src[FTP_PATH_MAX];
  if (!validate_path(src_path, safe_src, sizeof(safe_src))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Source path forbidden");
  }
  if (pal_path_exists(safe_src) != 1) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Source not found");
  }

  /* Validate destination directory */
  char safe_dst_dir[FTP_PATH_MAX];
  if (!validate_path(dst_dir, safe_dst_dir, sizeof(safe_dst_dir))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Destination path forbidden");
  }
  if (pal_path_is_directory(safe_dst_dir) != 1) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Destination is not a directory");
  }

  /*
   * Build full destination:  dst_dir + '/' + basename(src)
   *
   *   src = /data/files/readme.txt
   *   dst = /mnt/usb0/backup
   *   ->    /mnt/usb0/backup/readme.txt
   */
  const char *base = strrchr(safe_src, '/');
  base = (base != NULL) ? base + 1 : safe_src;

  char full_dst[FTP_PATH_MAX];
  if (strcmp(safe_dst_dir, "/") == 0) {
    (void)snprintf(full_dst, sizeof(full_dst), "/%s", base);
  } else {
    (void)snprintf(full_dst, sizeof(full_dst), "%s/%s", safe_dst_dir, base);
  }

  /* Re-validate the composed destination */
  char safe_final[FTP_PATH_MAX];
  if (!validate_path(full_dst, safe_final, sizeof(safe_final))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Final destination forbidden");
  }

  /*
   * Compute total size for progress UI.
   * For a single file use stat(). For directories an exact total
   * would require a full recursive scan — instead the browser
   * passes an estimate via ?totalsize= from the original listing.
   */
  {
    struct stat copy_st;
    uint64_t total_est = 0U;
    if (stat(safe_src, &copy_st) == 0) {
      total_est = (uint64_t)copy_st.st_size;
    }
    /* Client may provide totalsize hint for dirs */
    const char *ts_str = strstr(query, "totalsize=");
    if (ts_str != NULL) {
      uint64_t ts_val = (uint64_t)strtoull(ts_str + 10, NULL, 10);
      if (ts_val > 0U) {
        total_est = ts_val;
      }
    }
    atomic_store(&g_copy_progress.bytes_copied, 0U);
    atomic_store(&g_copy_progress.total_bytes, total_est);
    atomic_store(&g_copy_progress.active, 1);
    atomic_store(&g_copy_progress.done, 0);
    atomic_store(&g_copy_progress.error, 0);
    atomic_store(&g_copy_progress.error_code, 0);
    atomic_store(&g_copy_progress.error_errno, 0);
    atomic_store(&g_copy_progress.cancel, 0);
  }

  /* Spawn background copy thread so event loop stays responsive */
  copy_thread_args_t *args =
      (copy_thread_args_t *)malloc(sizeof(copy_thread_args_t));
  if (args == NULL) {
    atomic_store(&g_copy_progress.active, 0);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  (void)strncpy(args->src, safe_src, sizeof(args->src) - 1U);
  args->src[sizeof(args->src) - 1U] = '\0';
  (void)strncpy(args->dst, safe_final, sizeof(args->dst) - 1U);
  args->dst[sizeof(args->dst) - 1U] = '\0';

  pthread_t tid;
  if (pthread_create(&tid, NULL, copy_thread_fn, args) != 0) {
    free(args);
    atomic_store(&g_copy_progress.active, 0);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                      "Failed to start copy thread");
  }
  (void)pthread_detach(tid);

  /* Return immediately -- client polls /api/copy_progress for status */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  const char *body = "{\"ok\":true,\"async\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}
#endif

/*===========================================================================*
 * GET /api/stats/ram  — RAM usage
 *
 *  RESPONSE: { "used": N, "cached": N, "buffers": N, "free": N, "total": N }
 *===========================================================================*/

static int get_ram_stats(uint64_t *used, uint64_t *cached, uint64_t *buffers,
                         uint64_t *free_b, uint64_t *total) {
  if (!used || !cached || !buffers || !free_b || !total) {
    return -1;
  }
  *used = 0; *cached = 0; *buffers = 0; *free_b = 0; *total = 0;

#if defined(HAS_SYSINFO)
  struct sysinfo si;
  if (sysinfo(&si) != 0) {
    return -1;
  }
  uint64_t unit = (uint64_t)si.mem_unit;
  *total   = (uint64_t)si.totalram   * unit;
  *free_b  = (uint64_t)si.freeram    * unit;
  *buffers = (uint64_t)si.bufferram  * unit;
  *cached  = 0; /* not in sysinfo; /proc/meminfo would give it */
  *used    = (*total > *free_b + *buffers + *cached)
               ? (*total - *free_b - *buffers - *cached) : 0U;
  /* Try /proc/meminfo for Cached */
  FILE *fp = fopen("/proc/meminfo", "r");
  if (fp) {
    char line[128];
    while (fgets(line, sizeof(line), fp)) {
      uint64_t v = 0;
      if (sscanf(line, "Cached: %" SCNu64, &v) == 1) {
        *cached = v * 1024U;
      } else if (sscanf(line, "MemAvailable: %" SCNu64, &v) == 1) {
        /* recalculate used from MemAvailable */
        uint64_t avail = v * 1024U;
        *used = (*total > avail) ? (*total - avail) : 0U;
      }
    }
    fclose(fp);
  }
  return 0;
#elif defined(PLATFORM_MACOS) || defined(__APPLE__)
  /* Total physical memory */
  uint64_t mem_total = 0;
  size_t sz = sizeof(mem_total);
  if (sysctlbyname("hw.memsize", &mem_total, &sz, NULL, 0) != 0) {
    return -1;
  }
  *total = mem_total;

  /* Page size */
  vm_size_t page_sz = 0;
  if (host_page_size(mach_host_self(), &page_sz) != KERN_SUCCESS) {
    page_sz = 4096;
  }

  /* VM stats via host_statistics64 — same source as vm_stat(1) */
  vm_statistics64_data_t vmstat;
  mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
  if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
                        (host_info64_t)&vmstat, &count) != KERN_SUCCESS) {
    return -1;
  }

  *free_b  = (uint64_t)vmstat.free_count        * (uint64_t)page_sz;
  *used    = (uint64_t)(vmstat.active_count +
                        vmstat.wire_count)       * (uint64_t)page_sz;
  *cached  = (uint64_t)vmstat.inactive_count     * (uint64_t)page_sz;
  *buffers = (uint64_t)vmstat.speculative_count  * (uint64_t)page_sz;
  return 0;
#elif defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
  /* PS4/PS5 FreeBSD-derived kernel */
  uint64_t physmem = 0;
  size_t psz = sizeof(physmem);
  sysctlbyname("hw.physmem", &physmem, &psz, NULL, 0);
  *total = physmem;

  uint32_t page_sz32 = 16384;
  psz = sizeof(page_sz32);
  sysctlbyname("hw.pagesize", &page_sz32, &psz, NULL, 0);
  uint64_t page_sz = (uint64_t)page_sz32;

  uint32_t v_free = 0, v_active = 0, v_inactive = 0, v_wire = 0;
  psz = sizeof(v_free);
  sysctlbyname("vm.stats.vm.v_free_count",     &v_free,     &psz, NULL, 0);
  psz = sizeof(v_active);
  sysctlbyname("vm.stats.vm.v_active_count",   &v_active,   &psz, NULL, 0);
  psz = sizeof(v_inactive);
  sysctlbyname("vm.stats.vm.v_inactive_count", &v_inactive, &psz, NULL, 0);
  psz = sizeof(v_wire);
  sysctlbyname("vm.stats.vm.v_wire_count",     &v_wire,     &psz, NULL, 0);

  *free_b  = (uint64_t)v_free     * page_sz;
  *used    = (uint64_t)(v_active + v_wire) * page_sz;
  *cached  = (uint64_t)v_inactive * page_sz;
  *buffers = 0;
  return 0;
#else
  return -1;
#endif
}

static http_response_t *api_stats_ram(const http_request_t *request) {
  (void)request;
  uint64_t used = 0, cached = 0, buffers = 0, free_b = 0, total = 0;
  get_ram_stats(&used, &cached, &buffers, &free_b, &total);

  char body[256];
  int len = snprintf(body, sizeof(body),
    "{\"used\":%" PRIu64 ",\"cached\":%" PRIu64 ",\"buffers\":%" PRIu64
    ",\"free\":%" PRIu64 ",\"total\":%" PRIu64 "}",
    used, cached, buffers, free_b, total);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

/*===========================================================================*
 * GET /api/stats/system  — CPU temp, uptime, boot time
 *
 *  RESPONSE: { "cpu_temp": N|null, "uptime_seconds": N|null,
 *               "boot_epoch": N|null }
 *===========================================================================*/

static http_response_t *api_stats_system(const http_request_t *request) {
  (void)request;

  int32_t temp_c = 0;
  int temp_ok = get_cpu_temp_c(&temp_c);

  uint64_t boot_epoch = 0;
  int boot_ok = get_boot_epoch_seconds(&boot_epoch);

  uint64_t uptime_sec = 0;
  if (boot_ok == 0) {
    time_t now = time(NULL);
    if (now > 0 && (uint64_t)now >= boot_epoch) {
      uptime_sec = (uint64_t)now - boot_epoch;
    }
  }

  char body[512];
  size_t pos = 0;
  size_t cap = sizeof(body);

  pos += (size_t)snprintf(body + pos, cap - pos, "{");
  if (temp_ok == 0) {
    pos += (size_t)snprintf(body + pos, cap - pos,
                            "\"cpu_temp\":%" PRId32, temp_c);
  } else {
    pos += (size_t)snprintf(body + pos, cap - pos, "\"cpu_temp\":null");
  }
  if (boot_ok == 0) {
    pos += (size_t)snprintf(body + pos, cap - pos,
                            ",\"uptime_seconds\":%" PRIu64
                            ",\"boot_epoch\":%" PRIu64,
                            uptime_sec, boot_epoch);
  } else {
    pos += (size_t)snprintf(body + pos, cap - pos,
                            ",\"uptime_seconds\":null,\"boot_epoch\":null");
  }
  pos += (size_t)snprintf(body + pos, cap - pos, "}");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
  return resp;
}

/*===========================================================================*
 * GET /api/disk/info  — Disk usage for largest mounted volume
 *
 *  RESPONSE: { "used": N, "free": N, "total": N, "path": "..." }
 *===========================================================================*/

static http_response_t *api_disk_info(const http_request_t *request) {
  (void)request;

  uint64_t total = 0, used = 0, free_b = 0;
  const char *disk_path = NULL;
  get_best_disk_stats(g_http_root, &disk_path, &total, &used, &free_b);

  char body[256];
  size_t pos = 0;
  size_t cap = sizeof(body);
  pos += (size_t)snprintf(body + pos, cap - pos,
    "{\"used\":%" PRIu64 ",\"free\":%" PRIu64 ",\"total\":%" PRIu64 ",\"path\":\"",
    used, free_b, total);
  (void)json_escape_append(body, cap, &pos, disk_path ? disk_path : "/");
  pos += (size_t)snprintf(body + pos, cap - pos, "\"}");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
  return resp;
}

/*===========================================================================*
 * GET /api/disk/tree?path=X  — Directory tree (1 level deep, with sizes)
 *
 *  RESPONSE: { "name": "dirname", "type": "directory",
 *               "size": N, "children": [ { "name":..., "type":..., "size":... }, ...] }
 *===========================================================================*/

#define DISK_TREE_MAX_CHILDREN 512

static http_response_t *api_disk_tree(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query != NULL) {
    (void)parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  DIR *dir = opendir(safe);
  if (dir == NULL) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Directory not found");
  }

  /* Allocate a generous output buffer — tree JSON can be large */
  size_t cap = 512 * 1024; /* 512 KB */
  char *body = (char *)malloc(cap);
  if (body == NULL) {
    closedir(dir);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  size_t pos = 0;

  /* Header: root node name */
  const char *dirname = strrchr(safe, '/');
  dirname = (dirname && dirname[1] != '\0') ? dirname + 1 : safe;

  pos += (size_t)snprintf(body + pos, cap - pos, "{\"name\":\"");
  (void)json_escape_append(body, cap, &pos, dirname);
  pos += (size_t)snprintf(body + pos, cap - pos,
                          "\",\"type\":\"directory\",\"children\":[");

  uint64_t dir_total = 0;
  int first = 1;
  int count = 0;

  for (;;) {
    errno = 0;
    struct dirent *ent = readdir(dir);
    if (ent == NULL) {
      break;
    }
    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
      continue;
    }
    if (count >= DISK_TREE_MAX_CHILDREN) {
      break;
    }

    /* Build full child path */
    char child[FTP_PATH_MAX];
    if (strcmp(safe, "/") == 0) {
      (void)snprintf(child, sizeof(child), "/%s", ent->d_name);
    } else {
      (void)snprintf(child, sizeof(child), "%s/%s", safe, ent->d_name);
    }

    struct stat st;
    if (stat(child, &st) != 0) {
      continue;
    }

    uint64_t sz = (uint64_t)st.st_size;
    const char *type = S_ISDIR(st.st_mode) ? "directory" : "file";

    /* For directories, use statvfs block count as size approximation */
    if (S_ISDIR(st.st_mode)) {
      /* Use du-style: st_blocks * 512 */
      sz = (uint64_t)st.st_blocks * 512U;
    }

    dir_total += sz;

    if (!first) {
      if (pos + 1 < cap) {
        body[pos++] = ',';
      }
    }
    first = 0;

    /* Append child entry */
    size_t name_start = pos;
    pos += (size_t)snprintf(body + pos, cap - pos, "{\"name\":\"");
    (void)json_escape_append(body, cap, &pos, ent->d_name);
    pos += (size_t)snprintf(body + pos, cap - pos,
                            "\",\"type\":\"%s\",\"size\":%" PRIu64 "}", type, sz);

    /* Safety: if we are getting close to buffer limit, stop */
    if (pos + 256 >= cap) {
      /* Truncate last entry and break */
      pos = name_start;
      if (pos > 0 && body[pos - 1] == ',') {
        pos--;
      }
      break;
    }
    count++;
  }
  closedir(dir);

  pos += (size_t)snprintf(body + pos, cap - pos,
                          "],\"size\":%" PRIu64 "}", dir_total);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, body, pos) != 0) {
    free(body);
  }
  return resp;
}

/*===========================================================================*
 * GET /api/processes  — Process list
 *
 *  RESPONSE: [ { "pid": N, "name": "...", "user": "...",
 *                "cpu": F, "mem_mb": N, "status": "...",
 *                "killable": bool }, ... ]
 *===========================================================================*/

#include <signal.h>

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
#include <sys/sysctl.h>
#include <sys/proc.h>
#endif

static http_response_t *api_processes(const http_request_t *request) {
  (void)request;

  size_t cap = 256 * 1024; /* 256 KB */
  char *body = (char *)malloc(cap);
  if (body == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  size_t pos = 0;
  pos += (size_t)snprintf(body + pos, cap - pos, "[");
  int first = 1;

#if defined(PLATFORM_MACOS) || defined(__APPLE__)
  /* --- macOS: use KERN_PROC sysctl (no entitlements required) --- */
  int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0};
  size_t buf_size = 0;
  /* First call: get required size */
  if (sysctl(mib, 4, NULL, &buf_size, NULL, 0) == 0 && buf_size > 0) {
    /* Over-allocate slightly to handle races */
    buf_size += buf_size / 8;
    struct kinfo_proc *procs = (struct kinfo_proc *)malloc(buf_size);
    if (procs) {
      if (sysctl(mib, 4, procs, &buf_size, NULL, 0) == 0) {
        int n = (int)(buf_size / sizeof(struct kinfo_proc));
        for (int i = 0; i < n; i++) {
          struct kinfo_proc *kp = &procs[i];
          pid_t pid = kp->kp_proc.p_pid;
          if (pid <= 0) continue;

          char name[MAXCOMLEN + 1];
          strncpy(name, kp->kp_proc.p_comm, sizeof(name) - 1);
          name[sizeof(name) - 1] = '\0';

          unsigned int uid = (unsigned int)kp->kp_eproc.e_ucred.cr_uid;

          /* p_stat: SSLEEP=1, SWAIT=2, SRUN=3, SIDL=4, SZOMB=5, SSTOP=6 */
          const char *status = "running";
          if (kp->kp_proc.p_stat == 1 || kp->kp_proc.p_stat == 2) status = "sleep";
          else if (kp->kp_proc.p_stat == 5) status = "zombie";

          /* RSS from e_vm — not always available, use 0 as fallback */
          uint64_t mem_mb = 0;

          int killable = (uid != 0) ? 1 : 0;

          if (!first && pos + 2 < cap) { body[pos++] = ','; }
          first = 0;

          pos += (size_t)snprintf(body + pos, cap - pos,
            "{\"pid\":%d,\"name\":\"", (int)pid);
          (void)json_escape_append(body, cap, &pos, name);
          pos += (size_t)snprintf(body + pos, cap - pos,
            "\",\"user\":\"%u\",\"cpu\":0.0,\"mem_mb\":%" PRIu64
            ",\"status\":\"%s\",\"killable\":%s}",
            uid, mem_mb, status,
            killable ? "true" : "false");

          if (pos + 512 >= cap) break;
        }
      }
      free(procs);
    }
  }

#elif defined(HAS_SYSINFO)
  /* --- Linux: parse /proc --- */
  DIR *proc_dir = opendir("/proc");
  if (proc_dir) {
    struct dirent *ent;
    while ((ent = readdir(proc_dir)) != NULL) {
      /* Only numeric entries are PIDs */
      int pid = 0;
      int is_pid = 1;
      for (const char *c = ent->d_name; *c; c++) {
        if (*c < '0' || *c > '9') { is_pid = 0; break; }
      }
      if (!is_pid || ent->d_name[0] == '\0') continue;
      pid = atoi(ent->d_name);
      if (pid <= 0) continue;

      /* /proc/<pid>/stat */
      char stat_path[64];
      snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
      FILE *f = fopen(stat_path, "r");
      if (!f) continue;

      char comm[256] = "";
      char state = '?';
      long rss = 0;
      unsigned int uid = 0;

      /* Read comm from /proc/<pid>/status for cleaner name */
      char status_path[64];
      snprintf(status_path, sizeof(status_path), "/proc/%d/status", pid);
      FILE *sf = fopen(status_path, "r");
      if (sf) {
        char line[256];
        while (fgets(line, sizeof(line), sf)) {
          if (strncmp(line, "Name:", 5) == 0) {
            sscanf(line + 5, " %255s", comm);
          } else if (strncmp(line, "Uid:", 4) == 0) {
            sscanf(line + 4, " %u", &uid);
          }
        }
        fclose(sf);
      }

      /* Read utime/stime/rss from stat */
      {
        char tmp[2048];
        if (fgets(tmp, sizeof(tmp), f)) {
          /* format: pid (comm) state ppid ... utime stime ... rss */
          char *p = strrchr(tmp, ')');
          if (p) {
            sscanf(p + 2, " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                          "%*lu %*lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                   &state, &rss);
          }
        }
      }
      fclose(f);

      if (comm[0] == '\0') snprintf(comm, sizeof(comm), "pid%d", pid);

      uint64_t mem_mb = (uint64_t)((rss > 0 ? rss : 0) * 4096UL / (1024UL * 1024UL));
      const char *status_str = "running";
      if (state == 'S' || state == 'D') status_str = "sleep";
      else if (state == 'Z') status_str = "zombie";
      double cpu_pct = 0.0; /* snapshot only */
      int killable = (uid != 0) ? 1 : 0;

      if (!first && pos + 2 < cap) { body[pos++] = ','; }
      first = 0;

      pos += (size_t)snprintf(body + pos, cap - pos,
        "{\"pid\":%d,\"name\":\"", pid);
      (void)json_escape_append(body, cap, &pos, comm);
      pos += (size_t)snprintf(body + pos, cap - pos,
        "\",\"user\":\"%u\",\"cpu\":%.1f,\"mem_mb\":%" PRIu64
        ",\"status\":\"%s\",\"killable\":%s}",
        uid, cpu_pct, mem_mb, status_str,
        killable ? "true" : "false");

      if (pos + 512 >= cap) break;
    }
    closedir(proc_dir);
  }

#else
  /* Unsupported platform — return empty array */
  (void)first;
#endif

  if (pos + 2 < cap) {
    body[pos++] = ']';
    body[pos]   = '\0';
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, body, pos) != 0) {
    free(body);
  }
  return resp;
}

/*===========================================================================*
 * POST /api/process/kill  — Send SIGTERM to a process
 *
 *  REQUEST BODY: { "pid": N }
 *  RESPONSE:     { "success": true } | { "error": "..." }
 *===========================================================================*/

static int parse_pid_from_body(const char *body, size_t len, int *out_pid) {
  if (!body || len == 0 || !out_pid) return -1;
  const char *p = strstr(body, "\"pid\"");
  if (!p) p = strstr(body, "\"pid\":");
  if (!p) return -1;
  p += 5; /* skip "pid" */
  while (*p == ':' || *p == ' ' || *p == '\t') p++;
  if (*p == '\0') return -1;
  int v = atoi(p);
  if (v <= 0) return -1;
  *out_pid = v;
  return 0;
}

static http_response_t *api_process_kill(const http_request_t *request) {
#if ENABLE_WEB_UPLOAD
  if (http_csrf_validate(request) != 0) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Invalid or missing CSRF token");
  }
#endif
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  int pid = 0;
  if (parse_pid_from_body(request->body, request->body_length, &pid) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid pid");
  }

  /* Safety: never kill PID 1 or negative PIDs */
  if (pid <= 1) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot kill system process");
  }

  if (kill((pid_t)pid, SIGTERM) != 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "kill failed: %s", strerror(errno));
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, msg);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[64];
  int len = snprintf(body, sizeof(body), "{\"success\":true,\"pid\":%d}", pid);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

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
        if (http_response_set_body_splice(
                resp, content, prefix_len, meta_tag, strlen(meta_tag),
                found + strlen(placeholder), suffix_len) == 0) {
          return resp;
        }
      }
    }
#endif
  } else if (strstr(path, ".css") != NULL) {
    http_response_add_header(resp, "Content-Type", "text/css; charset=utf-8");
  } else if (strstr(path, ".js") != NULL) {
    http_response_add_header(resp, "Content-Type",
                             "application/javascript; charset=utf-8");
  } else if (strstr(path, ".png") != NULL) {
    http_response_add_header(resp, "Content-Type", "image/png");
  } else {
    http_response_add_header(resp, "Content-Type", "application/octet-stream");
  }

  if (http_response_set_body(resp, content, size) != 0) {
    (void)http_response_set_body_ref(resp, content, size);
  }
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
