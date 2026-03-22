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
#include "ftp_server.h" /* ftp_server_context_t — for network reset endpoint */
#include "http_config.h"
#include "pal_fileio.h"
#include "pal_network.h"      /* pal_network_reset_ftp_stack() */
#include "pal_notification.h" /* pal_notification_send() — fallback notify */
#include "exfat_unpacker.h"  /* exFAT image parsing for game metadata */
#include "pkg_unpacker.h"    /* PKG archive parsing for game metadata */
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
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <dlfcn.h>

typedef enum {
    LNC_FLAG_NONE = 0,
    LNC_SKIP_LAUNCH_CHECK = 1,
    LNC_SKIP_SYSTEM_UPDATE_CHECK = 2,
    LNC_REBOOT_PATCH_INSTALL = 4,
    LNC_VR_MODE = 8,
    LNC_NON_VR_MODE = 16
} LncAppParamFlag;

typedef struct _LncAppParam {
    uint32_t sz;
    uint32_t user_id;
    uint32_t app_opt;
    uint64_t crash_report;
    LncAppParamFlag check_flag;
} LncAppParam;
#endif
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(__FreeBSD__)
#include <sys/mount.h> /* fstatfs, struct statfs — for sendfile safety check */
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
static http_response_t *api_dirsize(const http_request_t *request);
static http_response_t *api_download(const http_request_t *request);
static http_response_t *api_stats(const http_request_t *request);
static http_response_t *api_stats_ram(const http_request_t *request);
static http_response_t *api_stats_system(const http_request_t *request);
static http_response_t *api_disk_info(const http_request_t *request);
static http_response_t *api_disk_tree(const http_request_t *request);
static http_response_t *api_processes(const http_request_t *request);
static http_response_t *api_process_kill(const http_request_t *request);
static http_response_t *serve_static(const http_request_t *request);
static http_response_t *api_game_meta(const http_request_t *request);
static http_response_t *api_game_icon(const http_request_t *request);
static http_response_t *api_extract(const http_request_t *request);
static http_response_t *api_extract_progress(const http_request_t *request);
static http_response_t *api_extract_cancel(const http_request_t *request);
static http_response_t *api_dl_start(const http_request_t *request);
static http_response_t *api_dl_status(const http_request_t *request);
static http_response_t *api_dl_pause(const http_request_t *request);
static http_response_t *api_dl_cancel(const http_request_t *request);
#if ENABLE_WEB_UPLOAD
static http_response_t *api_create_file(const http_request_t *request);
static http_response_t *api_mkdir(const http_request_t *request);
static http_response_t *api_delete(const http_request_t *request);
static http_response_t *api_rename(const http_request_t *request);
static http_response_t *api_copy(const http_request_t *request);
static http_response_t *api_copy_progress(const http_request_t *request);
static http_response_t *api_copy_cancel(const http_request_t *request);
static http_response_t *api_copy_pause(const http_request_t *request);
#endif
static http_response_t *api_network_reset(const http_request_t *request);
static http_response_t *api_admin_fan(const http_request_t *request);
static http_response_t *api_admin_launch(const http_request_t *request);
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

#if defined(PLATFORM_PS5) || defined(PS5)
  /*
   * On PS5 always report the /user partition — that is what the system
   * Settings > Storage screen shows.  The "pick largest" heuristic used
   * below selects /mnt (full SSD) which is much larger and does not match
   * what the user expects.
   */
  (void)hint_path;
  uint64_t t = 0U, u = 0U, f = 0U;
  if (get_disk_stats_bytes("/user", &t, &u, &f) == 0) {
    *out_path = "/user";
    *total = t;
    *used = u;
    *free_b = f;
    return 0;
  }
  return -1;
#else
  /* Ordered: real user data mounts first, then root fallback.
   * macOS home and /Volumes entries come before PS4 paths. */
  const char *candidates[] = {
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
      "/Users",
      "/",
#elif defined(PLATFORM_PS4) || defined(PS4)
      "/user", "/data", "/system_data", "/mnt/usb0", "/mnt/usb1", "/",
#else
      "/home",
      "/",
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
#endif
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

/**
 * @brief Recursively sum the size of all regular files under a directory.
 *
 * Uses a shared context to enforce:
 *   - Time budget  (DIR_SIZE_TIMEOUT_MS) — bail out after ~200 ms
 *   - Entry limit  (DIR_SIZE_MAX_ENTRIES) — bail after 10 000 stat() calls
 *   - Depth limit  (DIR_SIZE_MAX_DEPTH)   — max 8 levels deep
 *
 * On slow USB/exFAT media with deeply nested trees the scan returns
 * a partial result instead of blocking the HTTP server for seconds.
 *
 *   ┌──────────────────────────────────────────────┐
 *   │  200 ms budget ──► partial=true, ~size       │
 *   │  10 000 entries ──► partial=true, ~size      │
 *   │  depth > 8      ──► skip subtree             │
 *   │  otherwise      ──► full scan, partial=false │
 *   └──────────────────────────────────────────────┘
 */
#define DIR_SIZE_MAX_DEPTH    8
#define DIR_SIZE_MAX_ENTRIES  10000
#define DIR_SIZE_TIMEOUT_MS   200

typedef struct {
  struct timeval deadline;  /* absolute wallclock deadline */
  uint32_t      entries;   /* stat() calls so far         */
  int           partial;   /* set to 1 if limits exceeded */
} dir_size_ctx_t;

/* Return 1 if the context limits have been exceeded. */
static int dir_size_exceeded(dir_size_ctx_t *ctx) {
  if (ctx->partial) {
    return 1;
  }
  if (ctx->entries >= DIR_SIZE_MAX_ENTRIES) {
    ctx->partial = 1;
    return 1;
  }
  /* Check clock every 64 entries to minimise gettimeofday overhead */
  if ((ctx->entries & 63U) == 0U) {
    struct timeval now;
    gettimeofday(&now, NULL);
    if ((now.tv_sec > ctx->deadline.tv_sec) ||
        (now.tv_sec == ctx->deadline.tv_sec &&
         now.tv_usec >= ctx->deadline.tv_usec)) {
      ctx->partial = 1;
      return 1;
    }
  }
  return 0;
}

static uint64_t dir_size_walk(const char *path, int depth, dir_size_ctx_t *ctx) {
  if ((path == NULL) || (depth > DIR_SIZE_MAX_DEPTH)) {
    return 0U;
  }
  if (dir_size_exceeded(ctx)) {
    return 0U;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    return 0U;
  }

  uint64_t total = 0U;

  for (;;) {
    if (dir_size_exceeded(ctx)) {
      break;
    }

    errno = 0;
    struct dirent *ent = readdir(dir);
    if (ent == NULL) {
      break;
    }
    if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
      continue;
    }

    char child[FTP_PATH_MAX];
    int n;
    if (strcmp(path, "/") == 0) {
      n = snprintf(child, sizeof(child), "/%s", ent->d_name);
    } else {
      n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    }
    if ((n < 0) || ((size_t)n >= sizeof(child))) {
      continue;
    }

    struct stat st;
    if (lstat(child, &st) != 0) {
      continue;
    }
    ctx->entries++;

    if (S_ISREG(st.st_mode)) {
      total += (uint64_t)st.st_blocks * 512U;
    } else if (S_ISDIR(st.st_mode)) {
      total += dir_size_walk(child, depth + 1, ctx);
    }
    /* skip symlinks, devices, etc. */
  }

  closedir(dir);
  return total;
}

uint64_t http_dir_size_recursive(const char *path, int depth) {
  dir_size_ctx_t ctx;
  gettimeofday(&ctx.deadline, NULL);
  ctx.deadline.tv_usec += DIR_SIZE_TIMEOUT_MS * 1000;
  if (ctx.deadline.tv_usec >= 1000000) {
    ctx.deadline.tv_sec  += ctx.deadline.tv_usec / 1000000;
    ctx.deadline.tv_usec  = ctx.deadline.tv_usec % 1000000;
  }
  ctx.entries = 0;
  ctx.partial = 0;

  return dir_size_walk(path, depth, &ctx);
}

/**
 * @brief Same as http_dir_size_recursive but also reports whether
 *        the scan was truncated by the time/entry budget.
 */
static uint64_t http_dir_size_with_partial(const char *path, int *out_partial) {
  dir_size_ctx_t ctx;
  gettimeofday(&ctx.deadline, NULL);
  ctx.deadline.tv_usec += DIR_SIZE_TIMEOUT_MS * 1000;
  if (ctx.deadline.tv_usec >= 1000000) {
    ctx.deadline.tv_sec  += ctx.deadline.tv_usec / 1000000;
    ctx.deadline.tv_usec  = ctx.deadline.tv_usec % 1000000;
  }
  ctx.entries = 0;
  ctx.partial = 0;

  uint64_t sz = dir_size_walk(path, 0, &ctx);
  if (out_partial != NULL) {
    *out_partial = ctx.partial;
  }
  return sz;
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
#elif defined(PLATFORM_MACOS) || defined(__APPLE__) ||                         \
    defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||          \
    defined(PS5)
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

  /*  /api/dirsize?path=...  */
  if (strncmp(request->uri, "/api/dirsize", 12) == 0) {
    return api_dirsize(request);
  }

  /*  Download manager — /api/download/start, status, pause, cancel
   *
   *  IMPORTANT: These longer-prefix routes MUST come BEFORE the
   *  generic /api/download handler below, because strncmp matches
   *  left-to-right and "/api/download" (13 chars) is a prefix of
   *  "/api/download/start" (19 chars).
   *
   *  Route order:          Match example:
   *    /api/download/start   → api_dl_start()     ✓
   *    /api/download/status  → api_dl_status()    ✓
   *    /api/download/pause   → api_dl_pause()     ✓
   *    /api/download/cancel  → api_dl_cancel()    ✓
   *    /api/download?path=   → api_download()     ✓  (file download)
   */
  if (strncmp(request->uri, "/api/download/start", 19) == 0) {
    return api_dl_start(request);
  }
  if (strncmp(request->uri, "/api/download/status", 20) == 0) {
    return api_dl_status(request);
  }
  if (strncmp(request->uri, "/api/download/pause", 19) == 0) {
    return api_dl_pause(request);
  }
  if (strncmp(request->uri, "/api/download/cancel", 20) == 0) {
    return api_dl_cancel(request);
  }

  /*  /api/download?path=...  (file download — generic, MUST come after /start etc) */
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

  /*  POST /api/mkdir?path=...&name=...  */
  if (strncmp(request->uri, "/api/mkdir", 10) == 0) {
    return api_mkdir(request);
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
  if (strncmp(request->uri, "/api/copy_pause", 15) == 0) {
    return api_copy_pause(request);
  }
  if (strncmp(request->uri, "/api/copy", 9) == 0) {
    return api_copy(request);
  }
#endif

  /*  GET /api/game/meta?path=... — game metadata (title, icon base64) */
  if (strncmp(request->uri, "/api/game/meta", 14) == 0) {
    return api_game_meta(request);
  }

  /*  GET /api/game/icon?path=... — game cover art PNG */
  if (strncmp(request->uri, "/api/game/icon", 14) == 0) {
    return api_game_icon(request);
  }

  /*  POST /api/extract — archive extraction (libarchive) */
  if (strncmp(request->uri, "/api/extract_progress", 21) == 0) {
    return api_extract_progress(request);
  }
  if (strncmp(request->uri, "/api/extract_cancel", 19) == 0) {
    return api_extract_cancel(request);
  }
  if (strncmp(request->uri, "/api/extract", 12) == 0) {
    return api_extract(request);
  }

  /*  POST /api/network/reset — flush TCP buffer accounting (Fix #4) */
  if (strncmp(request->uri, "/api/network/reset", 18) == 0) {
    return api_network_reset(request);
  }

  /*  GET /api/admin/fan?threshold=... — set PS4/PS5 fan threshold */
  if (strncmp(request->uri, "/api/admin/fan", 14) == 0) {
    return api_admin_fan(request);
  }

  /*  GET /api/admin/launch?id=... — launch PS4/PS5 app by title ID */
  if (strncmp(request->uri, "/api/admin/launch", 17) == 0) {
    return api_admin_launch(request);
  }

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
 * GET /api/dirsize?path=<dir> — Recursive directory size
 *
 *  Returns the total size in bytes of all regular files under path.
 *  Called lazily by the frontend after the listing is already rendered,
 *  so it does not block the initial directory load.
 *
 *  RESPONSE: {"path":"/some/dir","size":123456789}
 *===========================================================================*/

static http_response_t *api_dirsize(const http_request_t *request) {
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

  struct stat st;
  if (stat(safe, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a directory");
  }

  int partial = 0;
  uint64_t sz = http_dir_size_with_partial(safe, &partial);

  char body[256];
  size_t pos = 0;
  size_t cap = sizeof(body);

  if (buf_append_cstr(body, cap, &pos, "{\"path\":\"") != 0 ||
      json_escape_append(body, cap, &pos, path) != 0 ||
      buf_append_cstr(body, cap, &pos, "\",\"size\":") != 0 ||
      buf_append_u64(body, cap, &pos, sz) != 0 ||
      buf_append_cstr(body, cap, &pos,
                      partial ? ",\"partial\":true}"
                              : ",\"partial\":false}") != 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
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
  /*
   * SAFETY: http_response_create() returns NULL when the response pool is
   * exhausted (HTTP_MAX_CONNECTIONS concurrent responses already in flight).
   * Without this check the subsequent struct-field assignments would
   * dereference a NULL pointer, causing SIGSEGV.  The open fd must be closed
   * here to prevent a file-descriptor leak — if we returned NULL without
   * closing it, the fd would be lost forever because no other code path holds
   * a reference to it.
   *
   * @pre  fd >= 0 and valid (opened above)
   * @post On NULL return: fd is closed, no resources are leaked
   */
  if (resp == NULL) {
    close(fd);
    return NULL; /* http_handle_request() will synthesise a 500 response */
  }
  http_response_add_header(resp, "Content-Type", "application/octet-stream");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");

  char disposition[512];
  snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"",
           basename);
  http_response_add_header(resp, "Content-Disposition", disposition);

  char len_str[32];
  snprintf(len_str, sizeof(len_str), "%lld", (long long)st.st_size);
  http_response_add_header(resp, "Content-Length", len_str);

  /*
   * Finalize headers (appends the blank \r\n line that separates headers
   * from the body).  Failure here means the response buffer is full —
   * destroy the response and close the fd rather than sending a malformed
   * HTTP message with missing header terminator.
   *
   * @post On failure: fd is closed, resp is freed, no resources are leaked
   */
  if (http_response_finalize(resp) != 0) {
    close(fd);
    http_response_destroy(resp);
    return NULL;
  }

  /* Store fd so http_server.c can stream the file content */
  resp->sendfile_fd = fd;
  resp->sendfile_offset = 0;
  resp->sendfile_count = (size_t)st.st_size;

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
      if ((strcmp(t, "ufs") == 0) || (strcmp(t, "ffs") == 0) ||
          (strcmp(t, "tmpfs") == 0) || (strcmp(t, "zfs") == 0)) {
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
 * POST /api/mkdir — Create directory
 *
 *   POST /api/mkdir?path=/parent&name=new_folder
 *   Returns: {"ok":true,"path":"/parent/new_folder","name":"new_folder"}
 *===========================================================================*/

static http_response_t *api_mkdir(const http_request_t *request) {
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
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid folder name");
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

  if (mkdir(safe_full, 0777) != 0 && errno != EEXIST) {
    char msg[128];
    snprintf(msg, sizeof(msg), "mkdir failed: %s", strerror(errno));
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, msg);
  }

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
          return error_json(
              HTTP_STATUS_500_INTERNAL_ERROR,
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

  /* POST-DELETE VERIFICATION: Ensure the path was actually deleted */
  struct stat verify_st;
  if (stat(safe, &verify_st) == 0) {
    /* Path still exists — delete operation failed silently */
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                      "Delete operation failed: path still exists (permission "
                      "denied or I/O error)");
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
  _Atomic int active;      /* 1 while copy thread is running  */
  _Atomic int done;        /* 1 when copy finished             */
  _Atomic int error;       /* 1 if copy failed                 */
  _Atomic int cancel;      /* 1 to request cancellation        */
  _Atomic int paused;      /* 1 to pause, 0 to resume          */
  _Atomic int error_code;  /* ftp_error_t value on failure     */
  _Atomic int error_errno; /* errno captured at failure point */
} copy_progress_t;

static copy_progress_t g_copy_progress = {0};

static int copy_progress_cb(uint64_t bytes_copied, void *user_data) {
  (void)user_data;
  atomic_store(&g_copy_progress.bytes_copied, bytes_copied);

  /* Pause: spin-wait in 100 ms increments while the flag is set.
   * Check cancel each iteration so the user can abort while paused. */
  while (atomic_load(&g_copy_progress.paused) != 0) {
    if (atomic_load(&g_copy_progress.cancel) != 0) {
      return -1;
    }
    usleep(100000); /* 100 ms */
  }

  /* Check cancellation flag — return -1 to abort copy */
  return (atomic_load(&g_copy_progress.cancel) != 0) ? -1 : 0;
}

/* Background copy thread */
typedef struct {
  char src[FTP_PATH_MAX];
  char dst[FTP_PATH_MAX];
  int *out_errno; /* points to g_copy_progress.error_errno storage (unused;
                     errno captured inside) */
} copy_thread_args_t;

static void *copy_thread_fn(void *arg) {
  copy_thread_args_t *a = (copy_thread_args_t *)arg;

  int saved_errno = 0;
  ftp_error_t rc = pal_file_copy_recursive_ex(
      a->src, a->dst, 1, copy_progress_cb, NULL, &saved_errno);
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

  int is_paused = atomic_load(&g_copy_progress.paused);

  char body[320];
  int len =
      snprintf(body, sizeof(body),
               "{\"active\":%s,\"done\":%s,\"error\":%s,\"paused\":%s,"
               "\"error_code\":%d,"
               "\"error_errno\":%d,"
               "\"bytes_copied\":%" PRIu64 ",\"total_bytes\":%" PRIu64 "}",
               active ? "true" : "false", done ? "true" : "false",
               err ? "true" : "false", is_paused ? "true" : "false", err_code,
               err_errno, copied, total);

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

/*  POST /api/copy_pause — toggle pause/resume  */
static http_response_t *api_copy_pause(const http_request_t *request) {
  (void)request;
  int cur = atomic_load(&g_copy_progress.paused);
  int next = (cur != 0) ? 0 : 1;
  atomic_store(&g_copy_progress.paused, next);

  char body[64];
  int len = snprintf(body, sizeof(body), "{\"ok\":true,\"paused\":%s}",
                     next ? "true" : "false");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, (size_t)len);
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
    atomic_store(&g_copy_progress.paused, 0);
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
  *used = 0;
  *cached = 0;
  *buffers = 0;
  *free_b = 0;
  *total = 0;

#if defined(HAS_SYSINFO)
  struct sysinfo si;
  if (sysinfo(&si) != 0) {
    return -1;
  }
  uint64_t unit = (uint64_t)si.mem_unit;
  *total = (uint64_t)si.totalram * unit;
  *free_b = (uint64_t)si.freeram * unit;
  *buffers = (uint64_t)si.bufferram * unit;
  *cached = 0; /* not in sysinfo; /proc/meminfo would give it */
  *used = (*total > *free_b + *buffers + *cached)
              ? (*total - *free_b - *buffers - *cached)
              : 0U;
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

  *free_b = (uint64_t)vmstat.free_count * (uint64_t)page_sz;
  *used =
      (uint64_t)(vmstat.active_count + vmstat.wire_count) * (uint64_t)page_sz;
  *cached = (uint64_t)vmstat.inactive_count * (uint64_t)page_sz;
  *buffers = (uint64_t)vmstat.speculative_count * (uint64_t)page_sz;
  return 0;
#elif defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||        \
    defined(PS5)
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
  sysctlbyname("vm.stats.vm.v_free_count", &v_free, &psz, NULL, 0);
  psz = sizeof(v_active);
  sysctlbyname("vm.stats.vm.v_active_count", &v_active, &psz, NULL, 0);
  psz = sizeof(v_inactive);
  sysctlbyname("vm.stats.vm.v_inactive_count", &v_inactive, &psz, NULL, 0);
  psz = sizeof(v_wire);
  sysctlbyname("vm.stats.vm.v_wire_count", &v_wire, &psz, NULL, 0);

  *free_b = (uint64_t)v_free * page_sz;
  *used = (uint64_t)(v_active + v_wire) * page_sz;
  *cached = (uint64_t)v_inactive * page_sz;
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
                     "{\"used\":%" PRIu64 ",\"cached\":%" PRIu64
                     ",\"buffers\":%" PRIu64 ",\"free\":%" PRIu64
                     ",\"total\":%" PRIu64 "}",
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
    pos += (size_t)snprintf(body + pos, cap - pos, "\"cpu_temp\":%" PRId32,
                            temp_c);
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
                          "{\"used\":%" PRIu64 ",\"free\":%" PRIu64
                          ",\"total\":%" PRIu64 ",\"path\":\"",
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
 *               "size": N, "children": [ { "name":..., "type":..., "size":...
 *}, ...] }
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
    pos +=
        (size_t)snprintf(body + pos, cap - pos,
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

  pos += (size_t)snprintf(body + pos, cap - pos, "],\"size\":%" PRIu64 "}",
                          dir_total);

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
#include <sys/proc.h>
#include <sys/sysctl.h>
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
          if (pid <= 0)
            continue;

          char name[MAXCOMLEN + 1];
          strncpy(name, kp->kp_proc.p_comm, sizeof(name) - 1);
          name[sizeof(name) - 1] = '\0';

          unsigned int uid = (unsigned int)kp->kp_eproc.e_ucred.cr_uid;

          /* p_stat: SSLEEP=1, SWAIT=2, SRUN=3, SIDL=4, SZOMB=5, SSTOP=6 */
          const char *status = "running";
          if (kp->kp_proc.p_stat == 1 || kp->kp_proc.p_stat == 2)
            status = "sleep";
          else if (kp->kp_proc.p_stat == 5)
            status = "zombie";

          /* RSS from e_vm — not always available, use 0 as fallback */
          uint64_t mem_mb = 0;

          int killable = (uid != 0) ? 1 : 0;

          if (!first && pos + 2 < cap) {
            body[pos++] = ',';
          }
          first = 0;

          pos += (size_t)snprintf(body + pos, cap - pos,
                                  "{\"pid\":%d,\"name\":\"", (int)pid);
          (void)json_escape_append(body, cap, &pos, name);
          pos += (size_t)snprintf(
              body + pos, cap - pos,
              "\",\"user\":\"%u\",\"cpu\":0.0,\"mem_mb\":%" PRIu64
              ",\"status\":\"%s\",\"killable\":%s}",
              uid, mem_mb, status, killable ? "true" : "false");

          if (pos + 512 >= cap)
            break;
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
        if (*c < '0' || *c > '9') {
          is_pid = 0;
          break;
        }
      }
      if (!is_pid || ent->d_name[0] == '\0')
        continue;
      pid = atoi(ent->d_name);
      if (pid <= 0)
        continue;

      /* /proc/<pid>/stat */
      char stat_path[64];
      snprintf(stat_path, sizeof(stat_path), "/proc/%d/stat", pid);
      FILE *f = fopen(stat_path, "r");
      if (!f)
        continue;

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
            sscanf(p + 2,
                   " %c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u "
                   "%*lu %*lu %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                   &state, &rss);
          }
        }
      }
      fclose(f);

      if (comm[0] == '\0')
        snprintf(comm, sizeof(comm), "pid%d", pid);

      uint64_t mem_mb =
          (uint64_t)((rss > 0 ? rss : 0) * 4096UL / (1024UL * 1024UL));
      const char *status_str = "running";
      if (state == 'S' || state == 'D')
        status_str = "sleep";
      else if (state == 'Z')
        status_str = "zombie";
      double cpu_pct = 0.0; /* snapshot only */
      int killable = (uid != 0) ? 1 : 0;

      if (!first && pos + 2 < cap) {
        body[pos++] = ',';
      }
      first = 0;

      pos += (size_t)snprintf(body + pos, cap - pos, "{\"pid\":%d,\"name\":\"",
                              pid);
      (void)json_escape_append(body, cap, &pos, comm);
      pos += (size_t)snprintf(
          body + pos, cap - pos,
          "\",\"user\":\"%u\",\"cpu\":%.1f,\"mem_mb\":%" PRIu64
          ",\"status\":\"%s\",\"killable\":%s}",
          uid, cpu_pct, mem_mb, status_str, killable ? "true" : "false");

      if (pos + 512 >= cap)
        break;
    }
    closedir(proc_dir);
  }

#else
  /* Unsupported platform — return empty array */
  (void)first;
#endif

  if (pos + 2 < cap) {
    body[pos++] = ']';
    body[pos] = '\0';
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
  if (!body || len == 0 || !out_pid)
    return -1;
  const char *p = strstr(body, "\"pid\"");
  if (!p)
    p = strstr(body, "\"pid\":");
  if (!p)
    return -1;
  p += 5; /* skip "pid" */
  while (*p == ':' || *p == ' ' || *p == '\t')
    p++;
  if (*p == '\0')
    return -1;
  int v = atoi(p);
  if (v <= 0)
    return -1;
  *out_pid = v;
  return 0;
}

static http_response_t *api_process_kill(const http_request_t *request) {
#if ENABLE_WEB_UPLOAD
  if (http_csrf_validate(request) != 0) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Invalid or missing CSRF token");
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
 * GAME METADATA — exFAT image parsing
 *
 *   GET /api/game/meta?path=<file>
 *     Extracts param.json + icon0.png from sce_sys/ inside an exFAT image.
 *     Returns JSON: { title_id, title_name, version, category, icon_base64 }
 *
 *   GET /api/game/icon?path=<file>
 *     Returns the raw PNG icon directly (Content-Type: image/png).
 *===========================================================================*/

/* Simple SFO string extraction */
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | ((uint16_t)p[1] << 8)); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)(p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24)); }

static int sfo_get_string(const uint8_t *sfo, size_t size, const char *req_key, char *out, size_t out_max) {
  if (!sfo || size < 20 || !req_key || !out || out_max == 0) return -1;
  /* 0x46535000 -> "\0PSF" in little-endian */
  if (le32(sfo) != 0x46535000) return -1;
  
  uint32_t key_table_ofs  = le32(sfo + 0x08);
  uint32_t data_table_ofs = le32(sfo + 0x0C);
  uint32_t count          = le32(sfo + 0x10);
  
  if (key_table_ofs >= size || data_table_ofs >= size) return -1;
  if (0x14 + count * 16 > size) return -1;
  
  for (uint32_t i = 0; i < count; i++) {
      const uint8_t *entry = sfo + 0x14 + i * 16;
      uint16_t key_ofs   = le16(entry + 0);
      uint16_t fmt       = le16(entry + 2);
      uint32_t data_len  = le32(entry + 4);
      uint32_t data_ofs  = le32(entry + 12);
      
      if (key_table_ofs + key_ofs >= size) return -1;
      const char *key = (const char *)(sfo + key_table_ofs + key_ofs);
      
      if (strcmp(key, req_key) == 0) {
          if (fmt == 0x0204 || fmt == 0x0004 || fmt == 0x0000 || fmt == 0x0404) {
              if (data_table_ofs + data_ofs + data_len <= size) {
                  snprintf(out, out_max, "%.*s", (int)(data_len > 0 ? data_len - 1 : 0), sfo + data_table_ofs + data_ofs);
                  return 0; /* success */
              }
          }
      }
  }
  return -1;
}

/* Simple JSON string extraction: get value for "key":"value" */
static int json_get_string(const char *json, const char *key,
                           char *out, size_t out_size) {
  if (!json || !key || !out || out_size == 0) return -1;
  out[0] = '\0';

  char needle[128];
  int nlen = snprintf(needle, sizeof(needle), "\"%s\"", key);
  if (nlen < 0 || (size_t)nlen >= sizeof(needle)) return -1;

  const char *pos = strstr(json, needle);
  if (!pos) return -1;
  pos += (size_t)nlen;

  /* Skip whitespace and colon */
  while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == ':') pos++;
  if (*pos != '"') return -1;
  pos++; /* skip opening quote */

  size_t i = 0;
  while (*pos && *pos != '"' && i < out_size - 1) {
    if (*pos == '\\' && *(pos + 1)) {
      pos++;
      if (*pos == 'n') out[i++] = '\n';
      else if (*pos == 't') out[i++] = '\t';
      else out[i++] = *pos;
    } else {
      out[i++] = *pos;
    }
    pos++;
  }
  out[i] = '\0';
  return 0;
}

#define GAME_META_MAX_ENTRIES   256
#define GAME_META_SCE_ENTRIES    64
#define GAME_META_MAX_PARAM  (256 * 1024)
#define GAME_META_MAX_ICON   (2 * 1024 * 1024)

static http_response_t *api_game_meta(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query) (void)parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  char title_id[64]    = "";
  char title_name[256] = "";
  char version[64]     = "";
  char category[64]    = "";
  char content_id[48]  = "";
  uint8_t *icon_data   = NULL;
  size_t   icon_size   = 0;

  /* 1. Try PKG archive first */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe) == PKG_OK) {
    fprintf(stderr, "[PKG] Successfully opened %s (entries: %u)\n", safe, pkg_ctx.header.entry_count);
    
    /* Always grab content_id from PKG header */
    snprintf(content_id, sizeof(content_id), "%.36s", pkg_ctx.header.content_id);

    const pkg_entry_t *sfo_entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PARAM_SFO);
    if (sfo_entry && sfo_entry->size > 0 && sfo_entry->size <= 65536) {
      uint8_t *sfo_data = (uint8_t *)malloc((size_t)sfo_entry->size);
      if (sfo_data) {
        if (pkg_extract_to_buffer(&pkg_ctx, sfo_entry, sfo_data, (size_t)sfo_entry->size) > 0) {
          sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE_ID", title_id, sizeof(title_id));
          sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE", title_name, sizeof(title_name));
          sfo_get_string(sfo_data, (size_t)sfo_entry->size, "APP_VER", version, sizeof(version));
          sfo_get_string(sfo_data, (size_t)sfo_entry->size, "CATEGORY", category, sizeof(category));
          /* Also try CONTENT_ID from SFO (more authoritative than PKG header) */
          {
            char sfo_cid[48] = "";
            sfo_get_string(sfo_data, (size_t)sfo_entry->size, "CONTENT_ID", sfo_cid, sizeof(sfo_cid));
            if (sfo_cid[0]) {
              strncpy(content_id, sfo_cid, sizeof(content_id) - 1);
              content_id[sizeof(content_id) - 1] = '\0';
            }
          }
        }
        free(sfo_data);
      }
    }

    if (!title_id[0]) {
      snprintf(title_id, sizeof(title_id), "%.36s", pkg_ctx.header.content_id);
    }
    if (!title_name[0]) {
      snprintf(title_name, sizeof(title_name), "%.36s", pkg_ctx.header.content_id);
    }

    const pkg_entry_t *entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_ICON0_PNG);
    if (!entry) {
      entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PIC0_PNG); /* fallback */
    }

    if (entry) {
      if (pkg_entry_is_encrypted(entry)) {
        fprintf(stderr, "[PKG] Icon entry is encrypted! Cannot extract.\n");
      } else if (entry->size > GAME_META_MAX_ICON) {
        fprintf(stderr, "[PKG] Icon too large: %u\n", entry->size);
      } else {
         icon_size = (size_t)entry->size;
         icon_data = (uint8_t *)malloc(icon_size);
         if (icon_data) {
            ssize_t got = pkg_extract_to_buffer(&pkg_ctx, entry, icon_data, icon_size);
            if (got > 0) {
              icon_size = (size_t)got;
              fprintf(stderr, "[PKG] Icon successfully extracted (%zu bytes)\n", icon_size);
            } else { 
              free(icon_data); icon_data = NULL; icon_size = 0; 
              fprintf(stderr, "[PKG] Failed to extract icon (err: %zd)\n", got);
            }
         }
      }
    } else {
      fprintf(stderr, "[PKG] Could not find any icon entry in PKG\n");
    }
    pkg_cleanup(&pkg_ctx);
  } else {
    /* 2. Fall back to exFAT image */
    exfat_context_t ctx;
    if (exfat_init(&ctx, safe) != 0) {
      return error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
    }

    /* Scan root directory for sce_sys */
    exfat_file_info_t root_entries[GAME_META_MAX_ENTRIES];
    int root_count = exfat_read_directory(&ctx,
        ctx.boot_sector.root_dir_first_cluster,
        root_entries, GAME_META_MAX_ENTRIES);

    for (int i = 0; i < root_count; i++) {
      if (!root_entries[i].is_directory) continue;
      if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) continue;

      /* Read sce_sys contents */
      exfat_file_info_t sce_entries[GAME_META_SCE_ENTRIES];
      int sce_count = exfat_read_directory(&ctx,
          root_entries[i].first_cluster,
          sce_entries, GAME_META_SCE_ENTRIES);

      for (int j = 0; j < sce_count; j++) {
        if (sce_entries[j].is_directory) continue;

        /* param.json */
        if (strcasecmp(sce_entries[j].filename, "param.json") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= GAME_META_MAX_PARAM) {
          size_t plen = (size_t)sce_entries[j].data_length;
          uint8_t *pbuf = (uint8_t *)malloc(plen + 1);
          if (pbuf) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], pbuf, plen);
            if (got > 0) {
              pbuf[got] = '\0';
              json_get_string((char *)pbuf, "titleId", title_id, sizeof(title_id));
              if (!title_id[0])
                json_get_string((char *)pbuf, "title_id", title_id, sizeof(title_id));
              json_get_string((char *)pbuf, "titleName", title_name, sizeof(title_name));
              json_get_string((char *)pbuf, "contentVersion", version, sizeof(version));
              if (!version[0])
                json_get_string((char *)pbuf, "appVer", version, sizeof(version));
              json_get_string((char *)pbuf, "category", category, sizeof(category));
            }
            free(pbuf);
          }
        }

        /* icon0.png */
        if (strcasecmp(sce_entries[j].filename, "icon0.png") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= GAME_META_MAX_ICON) {
          icon_size = (size_t)sce_entries[j].data_length;
          icon_data = (uint8_t *)malloc(icon_size);
          if (icon_data) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j],
                                                   icon_data, icon_size);
            if (got > 0) icon_size = (size_t)got;
            else { free(icon_data); icon_data = NULL; icon_size = 0; }
          }
        }
      }
      break; /* found sce_sys */
    }

    exfat_cleanup(&ctx);
  }

  /* Build JSON response */
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "max-age=3600");

  size_t body_cap = 1024;
  char *body = (char *)malloc(body_cap);
  if (!body) {
    if (icon_data) free(icon_data);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  int blen = snprintf(body, body_cap,
      "{\"title_id\":\"%s\",\"title_name\":\"%s\",\"version\":\"%s\","
      "\"category\":\"%s\",\"content_id\":\"%s\",\"has_icon\":%s}",
      title_id, title_name, version, category, content_id,
      (icon_data && icon_size > 0) ? "true" : "false");

  http_response_set_body(resp, body, (size_t)blen);

  free(body);
  if (icon_data) free(icon_data);
  return resp;
}

static http_response_t *api_game_icon(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query) (void)parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  uint8_t *icon_data = NULL;
  size_t   icon_size = 0;

  /* 1. Try PKG archive first */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe) == PKG_OK) {
    fprintf(stderr, "[PKG-ICON] Successfully opened %s\n", safe);
    const pkg_entry_t *entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_ICON0_PNG);
    if (!entry) {
      entry = pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PIC0_PNG); /* fallback */
    }

    if (entry) {
      if (pkg_entry_is_encrypted(entry)) {
        fprintf(stderr, "[PKG-ICON] Icon entry is encrypted! Cannot extract.\n");
      } else if (entry->size > GAME_META_MAX_ICON) {
        fprintf(stderr, "[PKG-ICON] Icon too large: %u\n", entry->size);
      } else {
         icon_size = (size_t)entry->size;
         icon_data = (uint8_t *)malloc(icon_size);
         if (icon_data) {
            ssize_t got = pkg_extract_to_buffer(&pkg_ctx, entry, icon_data, icon_size);
            if (got > 0) icon_size = (size_t)got;
            else { free(icon_data); icon_data = NULL; icon_size = 0; }
         }
      }
    }
    pkg_cleanup(&pkg_ctx);
  } else {
    /* 2. Fall back to exFAT image */
    exfat_context_t ctx;
    if (exfat_init(&ctx, safe) != 0) {
      return error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
    }

    exfat_file_info_t root_entries[GAME_META_MAX_ENTRIES];
    int root_count = exfat_read_directory(&ctx,
        ctx.boot_sector.root_dir_first_cluster,
        root_entries, GAME_META_MAX_ENTRIES);

    for (int i = 0; i < root_count; i++) {
      if (!root_entries[i].is_directory) continue;
      if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) continue;

      exfat_file_info_t sce_entries[GAME_META_SCE_ENTRIES];
      int sce_count = exfat_read_directory(&ctx,
          root_entries[i].first_cluster,
          sce_entries, GAME_META_SCE_ENTRIES);

      for (int j = 0; j < sce_count; j++) {
        if (sce_entries[j].is_directory) continue;
        if (strcasecmp(sce_entries[j].filename, "icon0.png") != 0) continue;
        if (sce_entries[j].data_length == 0 ||
            sce_entries[j].data_length > GAME_META_MAX_ICON) continue;

        icon_size = (size_t)sce_entries[j].data_length;
        icon_data = (uint8_t *)malloc(icon_size);
        if (icon_data) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j],
                                                 icon_data, icon_size);
          if (got > 0) icon_size = (size_t)got;
          else { free(icon_data); icon_data = NULL; icon_size = 0; }
        }
        break;
      }
      break;
    }

    exfat_cleanup(&ctx);
  }

  if (!icon_data || icon_size == 0) {
    return error_json(HTTP_STATUS_404_NOT_FOUND, "Icon not found in image");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "max-age=86400");
  
  if (http_response_set_body_owned(resp, icon_data, icon_size) != 0) {
    free(icon_data);
    http_response_destroy(resp);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to send icon");
  }
  return resp;
}

/*===========================================================================*
 * ARCHIVE EXTRACTION (Phase 5 — libarchive)
 *
 *   POST /api/extract?path=<archive>&dst=<dir>
 *     Extracts an archive to the destination directory using libarchive.
 *     Runs in a background thread with progress tracking.
 *
 *   GET  /api/extract_progress
 *     Returns extraction progress: { done, bytes_extracted, total_bytes, error }
 *
 *   POST /api/extract_cancel
 *     Cancels the active extraction.
 *
 * NOTE: libarchive must be linked (-larchive) for this to compile.
 *       When ENABLE_LIBARCHIVE is not defined, these return stub responses.
 *===========================================================================*/

/* Extraction state (single active extraction at a time) */
static struct {
  volatile int active;
  volatile int done;
  volatile int cancelled;
  volatile int error;
  volatile uint64_t bytes_extracted;
  volatile uint64_t total_bytes;
  char archive_path[1024];
  char dest_path[1024];
  char error_msg[256];
} g_extract = {0};

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>
#include <pthread.h>

static void *extract_thread(void *arg) {
  (void)arg;
  struct archive *a = archive_read_new();
  struct archive *ext = archive_write_disk_new();

  archive_read_support_format_all(a);
  archive_read_support_filter_all(a);
  archive_write_disk_set_options(ext,
      ARCHIVE_EXTRACT_TIME | ARCHIVE_EXTRACT_PERM | ARCHIVE_EXTRACT_ACL |
      ARCHIVE_EXTRACT_FFLAGS);
  archive_write_disk_set_standard_lookup(ext);

  if (archive_read_open_filename(a, g_extract.archive_path, 65536) != ARCHIVE_OK) {
    snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
             "Cannot open: %s", archive_error_string(a));
    g_extract.error = 1;
    g_extract.done = 1;
    g_extract.active = 0;
    archive_read_free(a);
    archive_write_free(ext);
    return NULL;
  }

  struct archive_entry *entry;
  while (!g_extract.cancelled) {
    int r = archive_read_next_header(a, &entry);
    if (r == ARCHIVE_EOF) break;
    if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
      snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
               "Read error: %s", archive_error_string(a));
      g_extract.error = 1;
      break;
    }

    /* Rewrite entry path to dest directory */
    const char *name = archive_entry_pathname(entry);
    char fullpath[2048];
    if (g_extract.dest_path[strlen(g_extract.dest_path) - 1] == '/') {
      snprintf(fullpath, sizeof(fullpath), "%s%s", g_extract.dest_path, name);
    } else {
      snprintf(fullpath, sizeof(fullpath), "%s/%s", g_extract.dest_path, name);
    }
    archive_entry_set_pathname(entry, fullpath);

    r = archive_write_header(ext, entry);
    if (r != ARCHIVE_OK) {
      /* Skip this entry on write error but continue */
      continue;
    }

    /* Copy data blocks */
    if (archive_entry_size(entry) > 0) {
      const void *buff;
      size_t size;
      int64_t offset;
      while (!g_extract.cancelled) {
        r = archive_read_data_block(a, &buff, &size, &offset);
        if (r == ARCHIVE_EOF) break;
        if (r != ARCHIVE_OK) break;
        archive_write_data_block(ext, buff, size, offset);
        g_extract.bytes_extracted += size;
      }
    }
    archive_write_finish_entry(ext);
  }

  archive_read_close(a);
  archive_read_free(a);
  archive_write_close(ext);
  archive_write_free(ext);

  if (g_extract.cancelled) {
    snprintf(g_extract.error_msg, sizeof(g_extract.error_msg), "Cancelled");
  }
  g_extract.done = 1;
  g_extract.active = 0;
  return NULL;
}
#endif /* ENABLE_LIBARCHIVE */

static http_response_t *api_extract(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  if (g_extract.active) {
    return error_json(HTTP_STATUS_409_CONFLICT, "Extraction already in progress");
  }

  const char *query = strchr(request->uri, '?');
  char path[1024] = "";
  char dst[1024] = "/";
  if (query) {
    (void)parse_path_param(query, path, sizeof(path));
    /* Parse dst param with URL decoding (%XX → byte) */
    const char *dp = strstr(query, "dst=");
    if (dp) {
      dp += 4;
      size_t ri = 0, wi = 0;
      while (dp[ri] && dp[ri] != '&' && wi < sizeof(dst) - 1) {
        if (dp[ri] == '%' && dp[ri + 1] && dp[ri + 2]) {
          unsigned char hi = (unsigned char)dp[ri + 1];
          unsigned char lo = (unsigned char)dp[ri + 2];
          unsigned int vh = (hi >= '0' && hi <= '9') ? hi - '0' :
                            (hi >= 'A' && hi <= 'F') ? 10 + hi - 'A' :
                            (hi >= 'a' && hi <= 'f') ? 10 + hi - 'a' : 0xFF;
          unsigned int vl = (lo >= '0' && lo <= '9') ? lo - '0' :
                            (lo >= 'A' && lo <= 'F') ? 10 + lo - 'A' :
                            (lo >= 'a' && lo <= 'f') ? 10 + lo - 'a' : 0xFF;
          if (vh <= 0xF && vl <= 0xF) {
            dst[wi++] = (char)((vh << 4) | vl);
            ri += 3;
            continue;
          }
        }
        dst[wi++] = dp[ri++];
      }
      dst[wi] = '\0';
    }
  }

  if (!path[0]) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe_path[FTP_PATH_MAX];
  char safe_dst[FTP_PATH_MAX];
  if (!validate_path(path, safe_path, sizeof(safe_path)) ||
      !validate_path(dst, safe_dst, sizeof(safe_dst))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
  /* Set up extraction state */
  memset(&g_extract, 0, sizeof(g_extract));
  strncpy(g_extract.archive_path, safe_path, sizeof(g_extract.archive_path) - 1);
  strncpy(g_extract.dest_path, safe_dst, sizeof(g_extract.dest_path) - 1);
  g_extract.active = 1;

  /* Get archive size for progress tracking */
  struct stat st;
  if (stat(safe_path, &st) == 0) {
    g_extract.total_bytes = (uint64_t)st.st_size;
  }

  /* Start extraction thread */
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, extract_thread, NULL) != 0) {
    g_extract.active = 0;
    pthread_attr_destroy(&attr);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to start extraction thread");
  }
  pthread_attr_destroy(&attr);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true,\"message\":\"Extraction started\"}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
#else
  return error_json(HTTP_STATUS_500_INTERNAL_ERROR,
                    "libarchive not available — compile with ENABLE_LIBARCHIVE=1");
#endif
}

static http_response_t *api_extract_progress(const http_request_t *request) {
  (void)request;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  char body[256];
  int len = snprintf(body, sizeof(body),
      "{\"active\":%s,\"done\":%s,\"cancelled\":%s,\"error\":%s,"
      "\"bytes_extracted\":%" PRIu64 ",\"total_bytes\":%" PRIu64
      ",\"error_msg\":\"%s\"}",
      g_extract.active ? "true" : "false",
      g_extract.done ? "true" : "false",
      g_extract.cancelled ? "true" : "false",
      g_extract.error ? "true" : "false",
      (uint64_t)g_extract.bytes_extracted,
      (uint64_t)g_extract.total_bytes,
      g_extract.error_msg);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

static http_response_t *api_extract_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }
  g_extract.cancelled = 1;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*===========================================================================*
 * DOWNLOAD MANAGER (Phase 6)
 *
 *   POST /api/download/start   { "url": "...", "dst": "/path/" }
 *     Starts a background HTTP download to the console filesystem.
 *
 *   GET  /api/download/status
 *     Returns all active downloads with progress.
 *
 *   POST /api/download/pause   { "id": N }
 *   POST /api/download/cancel  { "id": N }
 *
 * NOTE: Requires a socket-based HTTP client. On PS5 this uses the
 *       kernel's socket API directly. On desktop, libcurl can be used.
 *       When neither is available, returns stub responses.
 *===========================================================================*/

#define DL_MAX_ACTIVE 4
#define DL_URL_MAX    2048
#define DL_READ_BUF   (256 * 1024)

/* Download entry state */
typedef struct {
  int         active;
  int         done;
  int         paused;
  int         error;
  int         id;
  char        url[DL_URL_MAX];
  char        dst_path[1024];
  char        filename[256];
  char        error_msg[256];
  uint64_t    total_size;
  uint64_t    downloaded;
  double      speed;         /* bytes/sec */
  time_t      start_time;
} dl_entry_t;

static dl_entry_t g_downloads[DL_MAX_ACTIVE];
static int g_dl_next_id = 1;

static dl_entry_t *dl_find_slot(void) {
  for (int i = 0; i < DL_MAX_ACTIVE; i++) {
    if (!g_downloads[i].active && g_downloads[i].done == 0) return &g_downloads[i];
  }
  /* Reuse a completed slot */
  for (int i = 0; i < DL_MAX_ACTIVE; i++) {
    if (g_downloads[i].done) {
      memset(&g_downloads[i], 0, sizeof(dl_entry_t));
      return &g_downloads[i];
    }
  }
  return NULL;
}

static dl_entry_t *dl_find_by_id(int id) {
  for (int i = 0; i < DL_MAX_ACTIVE; i++) {
    if (g_downloads[i].id == id) return &g_downloads[i];
  }
  return NULL;
}

/* Extract filename from URL (last path component) */
static void dl_extract_filename(const char *url, char *out, size_t out_size) {
  if (!url || !out || out_size == 0) return;
  const char *last_slash = strrchr(url, '/');
  const char *name = last_slash ? last_slash + 1 : url;
  /* Strip query string */
  const char *qmark = strchr(name, '?');
  size_t len = qmark ? (size_t)(qmark - name) : strlen(name);
  if (len == 0 || len >= out_size) {
    snprintf(out, out_size, "download_%d", g_dl_next_id);
    return;
  }
  memcpy(out, name, len);
  out[len] = '\0';
}

#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include "pal_curl.h"
#else
#include <curl/curl.h>
#endif
#include <pthread.h>

struct dl_write_ctx {
  dl_entry_t *dl;
  int fd;
};

static size_t dl_curl_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
  struct dl_write_ctx *ctx = (struct dl_write_ctx *)userdata;
  size_t total = size * nmemb;
  if (ctx->dl->paused) return CURL_WRITEFUNC_PAUSE;
  ssize_t w = write(ctx->fd, ptr, total);
  if (w <= 0) return 0;
  ctx->dl->downloaded += (uint64_t)w;
  return (size_t)w;
}

static void *dl_thread(void *arg) {
  dl_entry_t *dl = (dl_entry_t *)arg;
  char filepath[2048];
  snprintf(filepath, sizeof(filepath), "%s/%s", dl->dst_path, dl->filename);

  int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg), "Cannot create file: %s", strerror(errno));
    dl->error = 1;
    dl->done = 1;
    dl->active = 0;
    return NULL;
  }

  CURL *curl = curl_easy_init();
  if (!curl) {
    close(fd);
    snprintf(dl->error_msg, sizeof(dl->error_msg), "curl_easy_init failed");
    dl->error = 1;
    dl->done = 1;
    dl->active = 0;
    return NULL;
  }

  struct dl_write_ctx wctx = { dl, fd };
  curl_easy_setopt(curl, CURLOPT_URL, dl->url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, dl_curl_write);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wctx);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

  CURLcode res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    snprintf(dl->error_msg, sizeof(dl->error_msg), "curl: %s", curl_easy_strerror(res));
    dl->error = 1;
  }

  double total_size = 0;
  curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD, &total_size);
  if (total_size > 0) dl->total_size = (uint64_t)total_size;

  double speed = 0;
  curl_easy_getinfo(curl, CURLINFO_SPEED_DOWNLOAD, &speed);
  dl->speed = speed;

  curl_easy_cleanup(curl);
  close(fd);
  dl->done = 1;
  dl->active = 0;
  return NULL;
}
#endif /* ENABLE_LIBCURL */

static http_response_t *api_dl_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  /* Parse JSON body: {"url":"...","dst":"..."} — simple extraction */
  const char *body_data = request->body;
  size_t body_len = request->body_length;
  char url[DL_URL_MAX] = "";
  char dst[1024] = "/";

  if (body_data && body_len > 0) {
    /* Simple JSON extraction for "url" and "dst" */
    const char *u = strstr(body_data, "\"url\"");
    if (u) {
      u = strchr(u + 5, '"');
      if (u) {
        u++;
        size_t i = 0;
        while (*u && *u != '"' && i < sizeof(url) - 1) {
          if (*u == '\\' && *(u + 1)) { u++; }
          url[i++] = *u++;
        }
        url[i] = '\0';
      }
    }
    const char *d = strstr(body_data, "\"dst\"");
    if (d) {
      d = strchr(d + 5, '"');
      if (d) {
        d++;
        size_t i = 0;
        while (*d && *d != '"' && i < sizeof(dst) - 1) {
          if (*d == '\\' && *(d + 1)) { d++; }
          dst[i++] = *d++;
        }
        dst[i] = '\0';
      }
    }
  }

  if (!url[0]) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing url parameter");
  }

  char safe_dst[FTP_PATH_MAX];
  if (!validate_path(dst, safe_dst, sizeof(safe_dst))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Invalid destination path");
  }

  dl_entry_t *dl = dl_find_slot();
  if (!dl) {
    return error_json(HTTP_STATUS_409_CONFLICT, "Max concurrent downloads reached");
  }

  memset(dl, 0, sizeof(dl_entry_t));
  dl->id = g_dl_next_id++;
  strncpy(dl->url, url, sizeof(dl->url) - 1);
  strncpy(dl->dst_path, safe_dst, sizeof(dl->dst_path) - 1);
  dl_extract_filename(url, dl->filename, sizeof(dl->filename));
  dl->start_time = time(NULL);
  dl->active = 1;

#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, dl_thread, dl) != 0) {
    dl->active = 0;
    pthread_attr_destroy(&attr);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to start download thread");
  }
  pthread_attr_destroy(&attr);
#else
  /* Without libcurl, mark as error immediately */
  snprintf(dl->error_msg, sizeof(dl->error_msg),
           "Download not available — compile with ENABLE_LIBCURL=1");
  dl->error = 1;
  dl->done = 1;
  dl->active = 0;
#endif

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char rbody[256];
  int rlen = snprintf(rbody, sizeof(rbody),
      "{\"ok\":true,\"id\":%d,\"name\":\"%s\",\"size\":0}",
      dl->id, dl->filename);
  http_response_set_body(resp, rbody, (size_t)rlen);
  return resp;
}

static http_response_t *api_dl_status(const http_request_t *request) {
  (void)request;
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  /* Build JSON array of all download entries */
  char body[4096];
  int pos = 0;
  pos += snprintf(body + pos, sizeof(body) - (size_t)pos, "{\"downloads\":[");

  int first = 1;
  for (int i = 0; i < DL_MAX_ACTIVE; i++) {
    dl_entry_t *dl = &g_downloads[i];
    if (dl->id == 0) continue;
    if (!first) pos += snprintf(body + pos, sizeof(body) - (size_t)pos, ",");
    first = 0;

    int progress = 0;
    if (dl->total_size > 0) {
      progress = (int)(dl->downloaded * 100 / dl->total_size);
      if (progress > 100) progress = 100;
    }

    pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
        "{\"id\":%d,\"name\":\"%s\",\"url\":\"%.*s\","
        "\"progress\":%d,\"downloaded\":%" PRIu64 ",\"total_size\":%" PRIu64 ","
        "\"speed\":%.0f,\"done\":%s,\"error\":\"%s\",\"paused\":%s}",
        dl->id, dl->filename,
        (int)(sizeof(body) - (size_t)pos > 200 ? 100 : 40), dl->url,
        progress, (uint64_t)dl->downloaded, (uint64_t)dl->total_size,
        dl->speed, dl->done ? "true" : "false",
        dl->error ? dl->error_msg : "",
        dl->paused ? "true" : "false");
  }
  pos += snprintf(body + pos, sizeof(body) - (size_t)pos, "]}");

  http_response_set_body(resp, body, (size_t)pos);
  return resp;
}

static http_response_t *api_dl_pause(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }
  /* Parse {"id": N} from body */
  int id = 0;
  if (request->body && request->body_length > 0) {
    const char *idp = strstr(request->body, "\"id\"");
    if (idp) {
      idp += 4;
      while (*idp == ' ' || *idp == ':' || *idp == '\t') idp++;
      id = atoi(idp);
    }
  }

  dl_entry_t *dl = dl_find_by_id(id);
  if (!dl) return error_json(HTTP_STATUS_404_NOT_FOUND, "Download not found");

  dl->paused = !dl->paused;

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[64];
  int len = snprintf(body, sizeof(body), "{\"ok\":true,\"paused\":%s}",
                     dl->paused ? "true" : "false");
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}

static http_response_t *api_dl_cancel(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }
  int id = 0;
  if (request->body && request->body_length > 0) {
    const char *idp = strstr(request->body, "\"id\"");
    if (idp) {
      idp += 4;
      while (*idp == ' ' || *idp == ':' || *idp == '\t') idp++;
      id = atoi(idp);
    }
  }

  dl_entry_t *dl = dl_find_by_id(id);
  if (!dl) return error_json(HTTP_STATUS_404_NOT_FOUND, "Download not found");

  dl->error = 1;
  snprintf(dl->error_msg, sizeof(dl->error_msg), "Cancelled by user");
  dl->done = 1;
  dl->active = 0;

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*===========================================================================*
 * STATIC RESOURCE SERVING
 *===========================================================================*/

/*---------------------------------------------------------------------------*
 * MIME type lookup — maps file extension to Content-Type.
 * Covers all file types used by the modular web UI.
 *---------------------------------------------------------------------------*/
static const char *mime_for_ext(const char *path) {
  const char *dot = strrchr(path, '.');
  if (dot == NULL) return "application/octet-stream";
  dot++; /* skip the '.' */
  if (strcasecmp(dot, "html") == 0) return "text/html; charset=utf-8";
  if (strcasecmp(dot, "css")  == 0) return "text/css; charset=utf-8";
  if (strcasecmp(dot, "js")   == 0) return "application/javascript; charset=utf-8";
  if (strcasecmp(dot, "json") == 0) return "application/json; charset=utf-8";
  if (strcasecmp(dot, "png")  == 0) return "image/png";
  if (strcasecmp(dot, "jpg")  == 0) return "image/jpeg";
  if (strcasecmp(dot, "jpeg") == 0) return "image/jpeg";
  if (strcasecmp(dot, "gif")  == 0) return "image/gif";
  if (strcasecmp(dot, "svg")  == 0) return "image/svg+xml";
  if (strcasecmp(dot, "ico")  == 0) return "image/x-icon";
  if (strcasecmp(dot, "webp") == 0) return "image/webp";
  if (strcasecmp(dot, "woff") == 0) return "font/woff";
  if (strcasecmp(dot, "woff2")== 0) return "font/woff2";
  if (strcasecmp(dot, "ttf")  == 0) return "font/ttf";
  if (strcasecmp(dot, "map")  == 0) return "application/json";
  return "application/octet-stream";
}

/*---------------------------------------------------------------------------*
 * serve_static — read files from HTTP_WEB_ROOT on the filesystem.
 *
 * Replaces the previous embedded-resource approach (http_resources.c).
 * Files are read from disk at request time, which:
 *   1. Reduces payload binary size by ~10 MB
 *   2. Allows hot-reloading during development
 *   3. Supports the new modular CSS/JS file structure
 *
 * Path traversal is prevented by rejecting any URI containing "..".
 *---------------------------------------------------------------------------*/
static http_response_t *serve_static(const http_request_t *request) {
  const char *path = request->uri;
  if (path[0] == '/') {
    path++;
  }
  if (path[0] == '\0') {
    path = "index.html";
  }

  /* ── Strip query string (e.g. "?v=3" cache busting) ──
   *
   *  "css/base.css?v=3"  →  "css/base.css"
   *                 ^── stop here                        */
  char clean_path[1024];
  {
    const char *qmark = strchr(path, '?');
    size_t plen = qmark ? (size_t)(qmark - path) : strlen(path);
    if (plen >= sizeof(clean_path)) plen = sizeof(clean_path) - 1;
    memcpy(clean_path, path, plen);
    clean_path[plen] = '\0';
  }
  path = clean_path;

  /* Block path traversal */
  if (strstr(path, "..") != NULL) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  /* Build full filesystem path: HTTP_WEB_ROOT + relative path */
  char fspath[1024];
  int n = snprintf(fspath, sizeof(fspath), "%s%s", HTTP_WEB_ROOT, path);
  if (n < 0 || (size_t)n >= sizeof(fspath)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Path too long");
  }

  /* Open and stat the file */
  struct stat st;
  if (stat(fspath, &st) != 0 || !S_ISREG(st.st_mode)) {
    /* Fallback: try embedded resources (backward compat during transition) */
    size_t esize = 0;
    const char *econtent = http_get_resource(path, &esize);
    if (econtent != NULL) {
      http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
      http_response_add_header(resp, "Content-Type", mime_for_ext(path));
      if (http_response_set_body(resp, econtent, esize) != 0) {
        (void)http_response_set_body_ref(resp, econtent, esize);
      }
      return resp;
    }
    http_response_t *resp = http_response_create(HTTP_STATUS_404_NOT_FOUND);
    const char *msg = "404 Not Found";
    http_response_set_body(resp, msg, strlen(msg));
    return resp;
  }

  size_t size = (size_t)st.st_size;

  /* Read file into memory */
  FILE *fp = fopen(fspath, "rb");
  if (fp == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Cannot open file");
  }
  char *content = (char *)malloc(size + 1);
  if (content == NULL) {
    fclose(fp);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  size_t got = fread(content, 1, size, fp);
  fclose(fp);
  content[got] = '\0';

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", mime_for_ext(path));
  http_response_add_header(resp, "Cache-Control", "no-cache");

#if ENABLE_WEB_UPLOAD
  /* Inject CSRF token into HTML */
  if (strstr(path, "index.html") != NULL) {
    const char *token = http_csrf_get_token();
    char meta_tag[128];
    snprintf(meta_tag, sizeof(meta_tag),
             "<meta name=\"csrf-token\" content=\"%s\">", token);

    const char *placeholder = "<!-- CSRF_TOKEN -->";
    const char *found = strstr(content, placeholder);

    if (found != NULL) {
      size_t prefix_len = (size_t)(found - content);
      size_t suffix_len = got - prefix_len - strlen(placeholder);
      if (http_response_set_body_splice(
              resp, content, prefix_len, meta_tag, strlen(meta_tag),
              found + strlen(placeholder), suffix_len) == 0) {
        free(content);
        return resp;
      }
    }
  }
#endif

  /*
   * Two paths for sending file content:
   *
   *   SMALL FILE  (<= ~7 KB): set_body copies into resp->data inline
   *   LARGE FILE  (> ~7 KB):  set_body_owned transfers ownership of the
   *                            malloc'd buffer; http_handle_request()
   *                            streams it via the mem_body path after
   *                            sending headers.
   *
   *   ┌──────────────────────────────────────────────────┐
   *   │  resp->data (8 KB)   │  mem_body (heap, any sz)  │
   *   │  [headers + body]    │  [large body streamed]    │
   *   └──────────────────────────────────────────────────┘
   */
  if (http_response_set_body(resp, content, got) == 0) {
    /* Small file — fully contained in resp->data */
    free(content);
    return resp;
  }
  /* Large file — transfer ownership of malloc'd content */
  if (http_response_set_body_owned(resp, content, got) == 0) {
    /* content ownership transferred, do NOT free */
    return resp;
  }
  /* Both paths failed (should not happen) */
  http_response_destroy(resp);
  free(content);
  return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Body allocation failed");
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

/*===========================================================================*
 * POST /api/network/reset — TCP buffer reset (Fix #4, #3, #7)
 *
 * ROOT CAUSE (repeated from pal_network.c for API-layer context):
 *
 *   After transfers to the internal SSD or M.2, OrbisOS kernel socket buffer
 *   accounting becomes inflated.  New data connections receive smaller-than-
 *   configured buffers, causing the 450 MB/s → 250 MB/s degradation.
 *   The manual workaround (disable/enable PS5 networking) triggers a full NIC
 *   buffer reallocation cycle.  This endpoint replicates that cycle at the
 *   application level by writing 0 then the target value to SO_SNDBUF /
 *   SO_RCVBUF on each idle session socket.
 *
 * RESPONSE:
 *   200  {"ok":true,"message":"Network stack reset (N sessions)"}
 *   200  {"ok":false,"message":"..."} — with PAL notification as fallback
 *   405  If method is not POST
 *
 * SIDE EFFECTS:
 *   - Closes orphaned data sockets (data_fd on non-TRANSFERRING sessions)
 *   - Sends a PS4/PS5 notification if the reset succeeds or fails
 *===========================================================================*/

/**
 * @brief POST /api/network/reset
 *
 * @note Thread-safety: Runs in the HTTP event loop thread.
 *       pal_network_reset_ftp_stack() is safe to call from any thread
 *       provided no session is in the middle of accept().
 */
static http_response_t *api_network_reset(const http_request_t *request) {
  if (request == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Null request");
  }

  /* Only POST is accepted */
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST /api/network/reset");
  }

  char body[128];
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "OOM");
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  if (g_ftp_server_ctx == NULL) {
    /*
     * HTTP server running without an attached FTP context (unlikely in
     * production, but handle it gracefully).  Send a PAL notification so
     * the user knows what happened, then return a soft failure.
     */
    pal_notification_send("zftpd: network reset unavailable (no FTP ctx)");
    (void)snprintf(body, sizeof(body),
                   "{\"ok\":false,\"message\":\"FTP context not attached\"}");
    http_response_set_body(resp, body, strlen(body));
    return resp;
  }

  int rc =
      pal_network_reset_ftp_stack(g_ftp_server_ctx->sessions, FTP_MAX_SESSIONS);

  if (rc == 0) {
    pal_notification_send("zftpd: network stack reset OK");
    (void)snprintf(
        body, sizeof(body),
        "{\"ok\":true,\"message\":\"Network stack reset (%u sessions)\"}",
        (unsigned)FTP_MAX_SESSIONS);
  } else {
    /*
     * Partial failure (invalid args) — fall back to notification so the
     * user is still informed, even if the UI reset failed.
     */
    pal_notification_send("zftpd: network reset partial failure");
    (void)snprintf(body, sizeof(body),
                   "{\"ok\":false,\"message\":\"Reset partial — check logs\"}");
  }

  http_response_set_body(resp, body, strlen(body));
  return resp;
}

/*===========================================================================*
 * GET /api/admin/fan?threshold=...
 * Sets the fan threshold on PS4/PS5 using /dev/icc_fan ioctl 0xC01C8F07.
 *===========================================================================*/
static http_response_t *api_admin_fan(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  if (!query) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing threshold parameter");
  }

  int threshold = 0;
  if (sscanf(query, "?threshold=%d", &threshold) != 1) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid threshold parameter format");
  }

  /* Clamp to safe operating values */
  if (threshold < 40) { threshold = 40; }
  if (threshold > 90) { threshold = 90; }

#ifndef _WIN32
  int fd = open("/dev/icc_fan", O_RDONLY, 0);
  if (fd < 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to open /dev/icc_fan (Unsupported OS)");
  }

  char data[10] = {0x00, 0x00, 0x00, 0x00, 0x00, (char)threshold, 0x00, 0x00, 0x00, 0x00};
  int ret = ioctl(fd, 0xC01C8F07, data);
  close(fd);

  if (ret < 0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Fan control ioctl failed");
  }
#else
  /* Mock for development environments */
  (void)threshold;
#endif

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK, "application/json");
  if (!resp) return NULL;
  
  char body[128];
  int blen = snprintf(body, sizeof(body), "{\"status\":\"ok\",\"threshold\":%d}", threshold);
  http_response_set_body(resp, body, (size_t)blen);
  return resp;
}

/*===========================================================================*
 * GET /api/admin/launch — Game Launcher Endpoint
 *===========================================================================*/
static http_response_t *api_admin_launch(const http_request_t *request) {
    char title_id[64] = {0};
    if (http_get_query_param(request, "id", title_id, sizeof(title_id)) < 0) {
        return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing 'id' parameter");
    }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    /*
     * We dynamically load libSceLncUtil.sprx, libSceUserService.sprx,
     * and libSceSystemService.sprx. Since this is a payload, these are
     * not statically linked and must be resolved at runtime using POSIX dlopen.
     */
    void *sysService = dlopen("/system/common/lib/libSceSystemService.sprx", RTLD_NOW | RTLD_GLOBAL);
    void *userService = dlopen("/system/common/lib/libSceUserService.sprx", RTLD_NOW | RTLD_GLOBAL);
    void *lncUtil = dlopen("/system/common/lib/libSceLncUtil.sprx", RTLD_NOW | RTLD_GLOBAL);

    if (sysService && userService && lncUtil) {
        int (*f_sceUserServiceGetForegroundUser)(uint32_t *) = 
            (int (*)(uint32_t *))dlsym(userService, "sceUserServiceGetForegroundUser");
            
        uint32_t (*f_sceLncUtilLaunchApp)(const char*, const char**, LncAppParam*) = 
            (uint32_t (*)(const char*, const char**, LncAppParam*))dlsym(lncUtil, "sceLncUtilLaunchApp");

        if (f_sceUserServiceGetForegroundUser && f_sceLncUtilLaunchApp) {
            uint32_t userId = 0;
            if (f_sceUserServiceGetForegroundUser(&userId) < 0) {
                return error_json(HTTP_STATUS_500_INTERNAL_SERVER_ERROR, "Failed to get Foreground User");
            }

            LncAppParam param;
            memset(&param, 0, sizeof(param));
            param.sz = sizeof(LncAppParam);
            param.user_id = userId;
            param.app_opt = 0;
            param.crash_report = 0;
            param.check_flag = LNC_SKIP_SYSTEM_UPDATE_CHECK;

            /* LncUtil uses Title ID like CUSAXXXXX */
            uint32_t res = f_sceLncUtilLaunchApp(title_id, NULL, &param);
            
            char msg[128];
            if (res == 0 || res == 0x8094000c) { /* 0x8094000c means ALREADY_RUNNING */
                (void)snprintf(msg, sizeof(msg), "{\"status\": \"ok\", \"message\": \"Game %s launched successfully!\"}", title_id);
                http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
                http_response_add_header(resp, "Content-Type", "application/json");
                http_response_set_body(resp, (const uint8_t *)msg, strlen(msg));
                pal_notification_send("Game Launch executed.");
                return resp;
            } else {
                (void)snprintf(msg, sizeof(msg), "Game Launch failed with error: 0x%08X", res);
                return error_json(HTTP_STATUS_500_INTERNAL_SERVER_ERROR, msg);
            }
        } else {
            return error_json(HTTP_STATUS_500_INTERNAL_SERVER_ERROR, "Failed to resolve Launch API symbols");
        }
    } else {
        return error_json(HTTP_STATUS_500_INTERNAL_SERVER_ERROR, "Failed to dynamically load Sony SPRX modules");
    }
#else
    /* Mock fallback for local tests */
    char debug_msg[128];
    (void)snprintf(debug_msg, sizeof(debug_msg), "{\"status\": \"ok\", \"message\": \"Mock Launch %s initiated\"}", title_id);
    http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
    http_response_add_header(resp, "Content-Type", "application/json");
    http_response_set_body(resp, (const uint8_t *)debug_msg, strlen(debug_msg));
    return resp;
#endif
}
