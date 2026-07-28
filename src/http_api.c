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
 *   GET /api/status                 Daemon identity (rest-mode reconnect)
 *   GET /                           Serve embedded index.html
 *   GET /style.css                  Serve embedded stylesheet
 *   GET /app.js                     Serve embedded JavaScript
 */

#include "http_api.h"
#include "ftp_config.h"
#include "ftp_instance.h"
#include "ftp_path.h"
#include "ftp_server.h" /* ftp_server_context_t — for network reset endpoint */
#include "ftp_log.h"
#include "http_config.h"
#include "pal_fileio.h"
#include "pal_network.h"      /* pal_network_reset_ftp_stack() */
#include "pal_notification.h" /* pal_notification_send() — fallback notify */
#include "exfat_unpacker.h"  /* exFAT image parsing for game metadata */
#include "pkg_unpacker.h"    /* PKG archive parsing for game metadata */
#include "builtin_unzip.h"   /* built-in ZIP extractor (PS5 fallback) */
#include <dirent.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif

#define SCE_LNC_UTIL_ERROR_ALREADY_RUNNING 0x8094000CU
#define SCE_LNC_UTIL_ERROR_ALREADY_INITIALIZED 0x80940018U
#define SCE_LNC_UTIL_ERROR_INVALID_PARAM 0x80940005U
#define SCE_LNC_ERROR_APP_NOT_FOUND 0x80940031U
#define SCE_LNC_APP_ID_BIG_BASE 0x60000000U
#define SCE_LNC_APP_ID_TYPE_MASK 0xFF000000U

#ifndef ENABLE_PKG_INSTALL
#define ENABLE_PKG_INSTALL 0
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <dlfcn.h>

typedef struct sqlite3 sqlite3;

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

#define SCE_SYSMODULE_INTERNAL_SYS_CORE 0x80000004
#define SCE_SYSMODULE_INTERNAL_HTTP 0x8000000A
#define SCE_SYSMODULE_INTERNAL_SSL 0x8000000B
#define SCE_SYSMODULE_INTERNAL_SYSTEM_SERVICE 0x80000010
#define SCE_SYSMODULE_INTERNAL_USER_SERVICE 0x80000011
#define SCE_SYSMODULE_INTERNAL_NET 0x8000001C

static int psx_sysmodule_load_internal(unsigned int module_id, int *out_rc) {
  void *sysmodule =
      dlopen("/system/common/lib/libSceSysmodule.sprx", RTLD_NOW | RTLD_GLOBAL);
  if (!sysmodule) {
    if (out_rc)
      *out_rc = -1;
    return -1;
  }

  int (*f_sceSysmoduleLoadModuleInternal)(unsigned int) =
      (int (*)(unsigned int))dlsym(sysmodule,
                                   "sceSysmoduleLoadModuleInternal");
  if (!f_sceSysmoduleLoadModuleInternal) {
    dlclose(sysmodule);
    if (out_rc)
      *out_rc = -2;
    return -1;
  }

  int rc = f_sceSysmoduleLoadModuleInternal(module_id);
  dlclose(sysmodule);
  if (out_rc)
    *out_rc = rc;
  return 0;
}

typedef enum {
  BGFT_TASK_OPTION_NONE = 0x0,
  BGFT_TASK_OPTION_DELETE_AFTER_UPLOAD = 0x1,
  BGFT_TASK_OPTION_INVISIBLE = 0x2,
  BGFT_TASK_OPTION_ENABLE_PLAYGO = 0x4,
  BGFT_TASK_OPTION_FORCE_UPDATE = 0x8,
  BGFT_TASK_OPTION_REMOTE = 0x10,
  BGFT_TASK_OPTION_COPY_CRASH_REPORT_FILES = 0x20,
  BGFT_TASK_OPTION_DISABLE_INSERT_POPUP = 0x40,
  BGFT_TASK_OPTION_DISABLE_CDN_QUERY_PARAM = 0x10000,
} bgft_task_option_t;

typedef struct {
  int user_id;
  int entitlement_type;
  const char *id;
  const char *content_url;
  const char *content_ex_url;
  const char *content_name;
  const char *icon_path;
  const char *sku_id;
  bgft_task_option_t option;
  const char *playgo_scenario_id;
  const char *release_date;
  const char *package_type;
  const char *package_sub_type;
  unsigned long package_size;
} bgft_download_param;

typedef struct {
  bgft_download_param param;
  unsigned int slot;
} bgft_download_param_ex;

typedef struct {
  void *heap;
  size_t heapSize;
} bgft_init_params;

typedef struct {
  unsigned int bits;
  int error_result;
  unsigned long length;
  unsigned long transferred;
  unsigned long lengthTotal;
  unsigned long transferredTotal;
  unsigned int numIndex;
  unsigned int numTotal;
  unsigned int restSec;
  unsigned int restSecTotal;
  int preparingPercent;
  int localCopyPercent;
} SceBgftTaskProgress;
#endif
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

#include "http_resources.h"

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
static http_response_t *api_status(const http_request_t *request);
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
static http_response_t *api_games_installed(const http_request_t *request);
static http_response_t *api_games_install_status(const http_request_t *request);
static http_response_t *api_games_icon(const http_request_t *request);
static http_response_t *api_games_repair_visibility(const http_request_t *request);
static http_response_t *api_games_uninstall(const http_request_t *request);
static http_response_t *api_games_install(const http_request_t *request);
static http_response_t *api_games_reinstall(const http_request_t *request);
static http_response_t *api_legacy_disabled_json(const char *json_body);
static http_response_t *error_json(http_status_t code, const char *message);
static http_response_t *status_json_200(int ok, const char *message,
                                        int code);
static http_response_t *png_fallback_response(void);

static void launch_diag_log(const char *stage, const char *title_id, int code,
                            const char *detail) {
  char line[512];
  const char *s = stage ? stage : "unknown";
  const char *t = title_id ? title_id : "-";
  const char *d = detail ? detail : "";
  (void)snprintf(line, sizeof(line),
                 "[LAUNCH-DIAG] stage=%s title=%s code=0x%08X detail=%s", s,
                 t, (unsigned)code, d);
  fprintf(stderr, "%s\n", line);
  ftp_log_line((code == 0 ||
                (uint32_t)code == SCE_LNC_UTIL_ERROR_ALREADY_RUNNING ||
                (((uint32_t)code & SCE_LNC_APP_ID_TYPE_MASK) ==
                 SCE_LNC_APP_ID_BIG_BASE))
                   ? FTP_LOG_INFO
                   : FTP_LOG_ERROR,
               line);
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static int launch_result_is_success(uint32_t res) {
  return (res == 0U) || (res == SCE_LNC_UTIL_ERROR_ALREADY_RUNNING) ||
         ((res & SCE_LNC_APP_ID_TYPE_MASK) == SCE_LNC_APP_ID_BIG_BASE);
}

static int psx_user_id_is_valid(int32_t user_id) {
  return user_id >= 0;
}

#if defined(PLATFORM_PS4)
__attribute__((weak))
int sceKernelLoadStartModule(const char *, size_t, const void *, uint32_t,
                             void *, void *);
__attribute__((weak))
int sceKernelDlsym(int, const char *, void **);
__attribute__((weak))
int sceUserServiceGetForegroundUser(int32_t *);
__attribute__((weak))
int sceUserServiceInitialize(void *);
__attribute__((weak))
int sceUserServiceGetLoginUserIdList(void *);
__attribute__((weak))
int sceUserServiceGetInitialUser(int32_t *);
__attribute__((weak))
uint32_t sceLncUtilLaunchApp(const char *, const char **, LncAppParam *);
__attribute__((weak))
int sceSystemServiceLaunchApp(const char *, const char **, void *);
__attribute__((weak))
int sceSystemServiceLoadExec(const char *, const char **);
__attribute__((weak))
int sceLncUtilInitialize(void);
__attribute__((weak))
int sceLncUtilGetAppId(const char *);

static int ps4_load_prx_symbol(const char *module_path, const char *symbol,
                               void **out_symbol) {
  if (module_path == NULL || symbol == NULL || out_symbol == NULL) {
    return -1;
  }
  *out_symbol = NULL;

  int module_id = -1;
  if (sceKernelLoadStartModule != NULL) {
    module_id =
        sceKernelLoadStartModule(module_path, 0U, NULL, 0U, NULL, NULL);
  }

  if (module_id < 0) {
    module_id = -1;
    long load_rc = syscall(594, module_path, 0, &module_id, 0);
    if (load_rc < 0 || module_id < 0) {
      return (int)load_rc;
    }
  }

  if (sceKernelDlsym != NULL) {
    int dlsym_rc = sceKernelDlsym(module_id, symbol, out_symbol);
    if (dlsym_rc >= 0 && *out_symbol != NULL) {
      return 0;
    }
  }

  long dyn_rc = syscall(591, module_id, symbol, out_symbol);
  if (dyn_rc < 0 || *out_symbol == NULL) {
    return (int)dyn_rc;
  }
  return 0;
}
#endif
#endif

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
    int nmax_dw = (int)(sizeof(child) - 2 - strlen(ent->d_name));
    if (nmax_dw < 0) { continue; }
    int n;
    if (strcmp(path, "/") == 0) {
      n = snprintf(child, sizeof(child), "/%s", ent->d_name);
    } else {
      n = snprintf(child, sizeof(child), "%.*s/%s", nmax_dw, path, ent->d_name);
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
  {
    int64_t usec = (int64_t)ctx.deadline.tv_usec + (int64_t)DIR_SIZE_TIMEOUT_MS * 1000;
    ctx.deadline.tv_sec  += (time_t)(usec / 1000000);
    ctx.deadline.tv_usec  = (suseconds_t)(usec % 1000000);
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
  {
    int64_t usec = (int64_t)ctx.deadline.tv_usec + (int64_t)DIR_SIZE_TIMEOUT_MS * 1000;
    ctx.deadline.tv_sec  += (time_t)(usec / 1000000);
    ctx.deadline.tv_usec  = (suseconds_t)(usec % 1000000);
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

#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) ||                        \
    defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
static int normalize_temp_c_from_raw(int64_t raw, int32_t *out_c) {
  if (out_c == NULL) {
    return -1;
  }

  if ((raw >= -40) && (raw <= 200)) {
    *out_c = (int32_t)raw;
    return 0;
  }

  /* FreeBSD-style sysctl values are often deci-Kelvin. */
  if ((raw >= 2000) && (raw <= 5000)) {
    int64_t c = (raw - 2731 + 5) / 10;
    if ((c >= -40) && (c <= 200)) {
      *out_c = (int32_t)c;
      return 0;
    }
  }

  /* Some sensors report milli-Kelvin. */
  if ((raw >= 200000) && (raw <= 500000)) {
    int64_t c = (raw - 273150 + 500) / 1000;
    if ((c >= -40) && (c <= 200)) {
      *out_c = (int32_t)c;
      return 0;
    }
  }

  return -1;
}
#endif

static int get_cpu_temp_c(int32_t *out_c) {
  if (out_c == NULL) {
    return -1;
  }

#if defined(PLATFORM_PS5) || defined(PS5)
  /*
   * PS5 payload SDK exposes SoC temperature as sceKernelGetSocSensorTemperature.
   * Sensor 0 is the APU/SoC reading used by the SDK's hwinfo sample.
   */
  __attribute__((weak)) int sceKernelGetSocSensorTemperature(int sensor,
                                                            int *temperature);
  if (sceKernelGetSocSensorTemperature != NULL) {
    for (int sensor = 0; sensor < 4; sensor++) {
      int temp = 0;
      if (sceKernelGetSocSensorTemperature(sensor, &temp) == 0 &&
          normalize_temp_c_from_raw((int64_t)temp, out_c) == 0) {
        return 0;
      }
    }
  }
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) ||          \
    defined(PS5)
  __attribute__((weak)) int sceKernelGetCpuTemperature(int *temperature);
  if (sceKernelGetCpuTemperature != NULL) {
    int temp = 0;
    if (sceKernelGetCpuTemperature(&temp) == 0 &&
        normalize_temp_c_from_raw((int64_t)temp, out_c) == 0) {
      return 0;
    }
  }
#endif

#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) ||                        \
    defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
  const char *names[] = {
      "dev.cpu.0.temperature",
      "dev.cpu.0.coretemp.temperature",
      "dev.cpu.0.temp",
      "dev.cpu.0.sensor0.temperature",
      "dev.amdtemp.0.temperature",
      "dev.amdtemp.0.core0.sensor0",
      "dev.amdtemp.0.sensor0.temperature",
      "dev.ps5.apu.temperature",
      "dev.apu.0.temperature",
      "dev.apu.temperature",
      "dev.thermal.0.temperature",
      "dev.thermal.apu.temperature",
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

    if (normalize_temp_c_from_raw((int64_t)v, out_c) == 0) {
      return 0;
    }
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

static int parse_query_param(const char *query, const char *key,
                             char *out, size_t out_size) {
  if ((query == NULL) || (key == NULL) || (out == NULL) || (out_size < 2U)) {
    return -1;
  }

  size_t key_len = strlen(key);
  const char *p = query;
  while ((p = strstr(p, key)) != NULL) {
    if ((p == query || p[-1] == '?' || p[-1] == '&') &&
        (p[key_len] == '=')) {
      const char *start = p + key_len + 1U;
      size_t in_pos = 0U;
      size_t out_pos = 0U;

      while ((start[in_pos] != '\0') && (start[in_pos] != '&') &&
             (out_pos < (out_size - 1U))) {
        unsigned char ch = (unsigned char)start[in_pos];

        if ((ch == '%') && (start[in_pos + 1] != '\0') &&
            (start[in_pos + 2] != '\0')) {
          unsigned char hi = (unsigned char)start[in_pos + 1];
          unsigned char lo = (unsigned char)start[in_pos + 2];
          unsigned int v_hi = 0xFFFFFFFFU;
          unsigned int v_lo = 0xFFFFFFFFU;

          if ((hi >= '0') && (hi <= '9'))
            v_hi = (unsigned int)(hi - '0');
          else if ((hi >= 'A') && (hi <= 'F'))
            v_hi = 10U + (unsigned int)(hi - 'A');
          else if ((hi >= 'a') && (hi <= 'f'))
            v_hi = 10U + (unsigned int)(hi - 'a');

          if ((lo >= '0') && (lo <= '9'))
            v_lo = (unsigned int)(lo - '0');
          else if ((lo >= 'A') && (lo <= 'F'))
            v_lo = 10U + (unsigned int)(lo - 'A');
          else if ((lo >= 'a') && (lo <= 'f'))
            v_lo = 10U + (unsigned int)(lo - 'a');

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

        out[out_pos++] = (ch == '+') ? ' ' : (char)ch;
        in_pos++;
      }
      out[out_pos] = '\0';
      return (out_pos > 0U) ? 0 : -1;
    }
    p += key_len;
  }

  return -1;
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

  /*
   * Browser-safe alias for local file downloads. Some browsers/extensions
   * block URLs named "/api/download" as suspicious/insecure downloads.
   */
  if (strncmp(request->uri, "/api/file/get", 13) == 0) {
    return api_download(request);
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

  /*  /api/status — daemon identity for rest-mode reconnect */
  if (strncmp(request->uri, "/api/status", 11) == 0) {
    return api_status(request);
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

  /* Games management API */
  if (strncmp(request->uri, "/api/admin/games/installed",
              sizeof("/api/admin/games/installed") - 1U) == 0) {
    return api_games_installed(request);
  }
  if (strncmp(request->uri, "/api/admin/games/icon",
              sizeof("/api/admin/games/icon") - 1U) == 0) {
    return api_games_icon(request);
  }
  if (strncmp(request->uri, "/api/admin/games/repair_visibility",
              sizeof("/api/admin/games/repair_visibility") - 1U) == 0) {
    return api_games_repair_visibility(request);
  }
  if (strncmp(request->uri, "/api/admin/games/uninstall",
              sizeof("/api/admin/games/uninstall") - 1U) == 0) {
    return api_games_uninstall(request);
  }
  if (strncmp(request->uri, "/api/admin/games/install_status",
              sizeof("/api/admin/games/install_status") - 1U) == 0) {
    return api_games_install_status(request);
  }
  if (strncmp(request->uri, "/api/admin/games/install",
              sizeof("/api/admin/games/install") - 1U) == 0) {
    return api_games_install(request);
  }
  if (strncmp(request->uri, "/api/admin/games/reinstall",
              sizeof("/api/admin/games/reinstall") - 1U) == 0) {
    return api_games_reinstall(request);
  }

  /*  GET /api/admin/launch?id=... — launch PS4/PS5 app by title ID */
  if (strncmp(request->uri, "/api/admin/launch", 17) == 0) {
    return api_admin_launch(request);
  }

  /* Legacy frontend compatibility (old embedded UIs) */
  if (strncmp(request->uri, "/api/stream/status", 18) == 0) {
    return api_legacy_disabled_json(
        "{\"ok\":true,\"enabled\":false,\"status\":\"offline\"}");
  }
  if (strncmp(request->uri, "/api/stream/start", 17) == 0 ||
      strncmp(request->uri, "/api/stream/stop", 16) == 0 ||
      strncmp(request->uri, "/api/stream", 11) == 0) {
    return api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Stream disabled\"}");
  }
  if (strncmp(request->uri, "/api/admin/installed", 20) == 0) {
    return api_legacy_disabled_json(
        "{\"ok\":true,\"installed\":false}");
  }
  if (strncmp(request->uri, "/api/admin/install", 18) == 0) {
    return api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Install API not available\"}");
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
  /*
   * SENDFILE — zero-copy only, no fallback.
   *
   * Linux, macOS, and FreeBSD (including PS4/PS5 OrbisOS) all support
   * sendfile(2) as a zero-copy kernel-to-NIC DMA path.  The previous
   * per-filesystem whitelist was overly conservative and forced the
   * slower pread()+send_all() userspace-copy path for PFS and exFAT
   * on PS5, causing a 2-3× throughput regression vs v1.4.0.
   *
   * If a specific filesystem cannot support sendfile, the transfer
   * fails — there is no silent degradation to a userspace copy.
   */
  resp->sendfile_safe = 1;

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
    int room = (int)(sizeof(full) - 3 - strlen(name));
    if (room < 0) room = 0;
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    int room = (int)(sizeof(full) - 2 - strlen(name));
    if (room < 1) room = 1;
    (void)snprintf(full, sizeof(full), "%.*s/%s", room, safe_dir, name);
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
    int room_md = (int)(sizeof(full) - 3 - strlen(name));
    if (room_md < 0) room_md = 0;
    (void)snprintf(full, sizeof(full), "/%s", name);
  } else {
    int room_md = (int)(sizeof(full) - 2 - strlen(name));
    if (room_md < 1) room_md = 1;
    (void)snprintf(full, sizeof(full), "%.*s/%s", room_md, safe_dir, name);
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
    int room_rn = (int)(sizeof(new_path) - 3 - strlen(name));
    if (room_rn < 0) room_rn = 0;
    (void)snprintf(new_path, sizeof(new_path), "/%s", name);
  } else {
    int room_rn = (int)(sizeof(new_path) - 2 - strlen(name));
    int actual = (int)parent_len;
    if (room_rn < 1) room_rn = 1;
    if (actual > room_rn) actual = room_rn;
    (void)snprintf(new_path, sizeof(new_path), "%.*s/%s", actual,
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
    int room_cp = (int)(sizeof(full_dst) - 3 - strlen(base));
    if (room_cp < 0) room_cp = 0;
    (void)snprintf(full_dst, sizeof(full_dst), "/%.*s", room_cp, base);
  } else {
    size_t dirlen = strlen(safe_dst_dir);
    size_t baselen = strlen(base);
    size_t overhead = 2; /* '/' + NUL */
    if (dirlen + baselen + overhead > sizeof(full_dst)) {
      if (dirlen > sizeof(full_dst) - overhead) { dirlen = sizeof(full_dst) - overhead; }
      baselen = sizeof(full_dst) - dirlen - overhead;
    }
    (void)snprintf(full_dst, sizeof(full_dst), "%.*s/%.*s",
                   (int)dirlen, safe_dst_dir, (int)baselen, base);
  }

  /* Re-validate the composed destination */
  char safe_final[FTP_PATH_MAX];
  if (!validate_path(full_dst, safe_final, sizeof(safe_final))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Final destination forbidden");
  }

  /*
   * Compute total size for progress UI.
   * For a single file use stat(). For directories compute the real
   * recursive total so the progress bar is accurate.
   */
  {
    struct stat copy_st;
    uint64_t total_est = 0U;
    if (stat(safe_src, &copy_st) == 0) {
      if (S_ISDIR(copy_st.st_mode)) {
        int partial = 0;
        total_est = http_dir_size_with_partial(safe_src, &partial);
      } else {
        total_est = (uint64_t)copy_st.st_size;
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
 * GET /api/status — daemon identity for rest-mode reconnect
 *
 *  RESPONSE: {
 *    "ok": true,
 *    "version": "...",
 *    "instance_id": "0123...abcd",
 *    "start_monotonic_ns": N,
 *    "pid": N
 *  }
 *===========================================================================*/

static http_response_t *api_status(const http_request_t *request) {
  (void)request;

  uint64_t instance_id = ftp_daemon_instance_id();
  uint64_t start_ns = ftp_daemon_start_monotonic_ns();

  char body[320];
  size_t pos = 0;
  size_t cap = sizeof(body);

  pos += (size_t)snprintf(
      body + pos, cap - pos,
      "{\"ok\":true,\"version\":\"%s\",\"instance_id\":\"%016llx\","
      "\"start_monotonic_ns\":%" PRIu64 ",\"pid\":%d}",
      RELEASE_VERSION, (unsigned long long)instance_id, start_ns,
      (int)getpid());

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
      int max_dt = (int)(sizeof(child) - 2 - strlen(ent->d_name));
      if (max_dt < 0) {
        count++;
        continue;
      }
      (void)snprintf(child, sizeof(child), "%.*s/%s", max_dt, safe, ent->d_name);
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
                   "%*u %*u %*d %*d %*d %*d %*d %*d %*u %*u %ld",
                   &state, &rss);
          }
        }
      }
      fclose(f);

      if (comm[0] == '\0')
        snprintf(comm, sizeof(comm), "pid%d", pid);

      uint64_t mem_mb =
          (uint64_t)(rss > 0 ? rss : 0) * 4096UL / (1024UL * 1024UL);
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

static int title_id_from_content_id(const char *content_id,
                                    char *out, size_t out_size) {
  if ((content_id == NULL) || (out == NULL) || (out_size < 10U)) {
    return -1;
  }

  size_t n = strlen(content_id);
  for (size_t i = 0; (i + 9U) <= n; i++) {
    if (isalpha((unsigned char)content_id[i + 0]) &&
        isalpha((unsigned char)content_id[i + 1]) &&
        isalpha((unsigned char)content_id[i + 2]) &&
        isalpha((unsigned char)content_id[i + 3]) &&
        isdigit((unsigned char)content_id[i + 4]) &&
        isdigit((unsigned char)content_id[i + 5]) &&
        isdigit((unsigned char)content_id[i + 6]) &&
        isdigit((unsigned char)content_id[i + 7]) &&
        isdigit((unsigned char)content_id[i + 8])) {
      for (size_t j = 0; j < 9U; j++) {
        out[j] = (char)toupper((unsigned char)content_id[i + j]);
      }
      out[9] = '\0';
      return 0;
    }
  }

  return -1;
}

static int read_file_to_buffer(const char *path, uint8_t **out_data,
                               size_t *out_size, size_t max_size) {
  if ((path == NULL) || (out_data == NULL) || (out_size == NULL)) {
    return -1;
  }

  FILE *fp = fopen(path, "rb");
  if (fp == NULL) {
    return -1;
  }

  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return -1;
  }
  long flen = ftell(fp);
  if (flen <= 0 || (size_t)flen > max_size) {
    fclose(fp);
    return -1;
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return -1;
  }

  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) {
    fclose(fp);
    return -1;
  }

  size_t got = fread(buf, 1, (size_t)flen, fp);
  fclose(fp);
  if (got != (size_t)flen) {
    free(buf);
    return -1;
  }

  *out_data = buf;
  *out_size = got;
  return 0;
}

static int read_installed_game_sfo(const char *app_dir, char *title_id,
                                   size_t title_id_size, char *title_name,
                                   size_t title_name_size) {
  if ((app_dir == NULL) || (title_id == NULL) || (title_name == NULL)) {
    return -1;
  }

  char sfo_path[FTP_PATH_MAX];
  int n = snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", app_dir);
  if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
    n = snprintf(sfo_path, sizeof(sfo_path), "%s/param.sfo", app_dir);
    if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
      return -1;
    }
  }

  uint8_t *sfo = NULL;
  size_t sfo_size = 0U;
  if (read_file_to_buffer(sfo_path, &sfo, &sfo_size, 65536U) != 0) {
    return -1;
  }

  (void)sfo_get_string(sfo, sfo_size, "TITLE_ID", title_id, title_id_size);
  (void)sfo_get_string(sfo, sfo_size, "TITLE", title_name, title_name_size);
  if (title_name[0] == '\0') {
    (void)sfo_get_string(sfo, sfo_size, "TITLE_01", title_name,
                         title_name_size);
  }

  free(sfo);
  return 0;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static int read_installed_game_sfo_field(const char *app_dir,
                                         const char *key,
                                         char *out,
                                         size_t out_size) {
  if ((app_dir == NULL) || (key == NULL) || (out == NULL) || (out_size < 2U)) {
    return -1;
  }
  out[0] = '\0';

  char sfo_path[FTP_PATH_MAX];
  int n = snprintf(sfo_path, sizeof(sfo_path), "%s/sce_sys/param.sfo", app_dir);
  if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
    n = snprintf(sfo_path, sizeof(sfo_path), "%s/param.sfo", app_dir);
    if (n < 0 || (size_t)n >= sizeof(sfo_path) || access(sfo_path, R_OK) != 0) {
      return -1;
    }
  }

  uint8_t *sfo = NULL;
  size_t sfo_size = 0U;
  if (read_file_to_buffer(sfo_path, &sfo, &sfo_size, 65536U) != 0) {
    return -1;
  }

  int rc = sfo_get_string(sfo, sfo_size, key, out, out_size);
  free(sfo);
  return (rc == 0 && out[0] != '\0') ? 0 : -1;
}
#endif

static int resolve_installed_icon_path(const char *title_id,
                                       const char *app_dir,
                                       char *out_path,
                                       size_t out_size) {
  if (!out_path || out_size < 2U) {
    return -1;
  }
  out_path[0] = '\0';

  if (app_dir && app_dir[0] != '\0') {
    int n = snprintf(out_path, out_size, "%s/sce_sys/icon0.png", app_dir);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
    n = snprintf(out_path, out_size, "%s/icon0.png", app_dir);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
  }

  if (title_id && title_id[0] != '\0') {
    int n = snprintf(out_path, out_size, "/user/appmeta/%s/icon0.png", title_id);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
    n = snprintf(out_path, out_size, "/system_data/priv/appmeta/%s/icon0.png",
                 title_id);
    if (n > 0 && (size_t)n < out_size && access(out_path, R_OK) == 0) {
      return 0;
    }
  }

  return -1;
}

static int resolve_installed_app_dir_by_title(const char *title_id,
                                              char *out_path,
                                              size_t out_size) {
  if ((title_id == NULL) || (title_id[0] == '\0') || (out_path == NULL) ||
      (out_size < 2U)) {
    return -1;
  }
  out_path[0] = '\0';

  const char *bases[] = {
      "/user/app",
      "/mnt/ext0/user/app",
      "/system_ex/app",
      "/system/app",
      NULL,
  };

  for (size_t i = 0; bases[i] != NULL; i++) {
    int n = snprintf(out_path, out_size, "%s/%s", bases[i], title_id);
    if (n <= 0 || (size_t)n >= out_size) {
      continue;
    }

    struct stat st;
    if (stat(out_path, &st) == 0 && S_ISDIR(st.st_mode)) {
      return 0;
    }
  }

  out_path[0] = '\0';
  return -1;
}

static int extract_title_id_from_app_dir(const char *safe_path,
                                         char *title_id,
                                         size_t title_id_size) {
  if ((safe_path == NULL) || (title_id == NULL) || (title_id_size < 10U)) {
    return -1;
  }
  title_id[0] = '\0';

  struct stat st;
  if (stat(safe_path, &st) != 0 || !S_ISDIR(st.st_mode)) {
    return -1;
  }

  char title_name[128] = {0};
  if (read_installed_game_sfo(safe_path, title_id, title_id_size, title_name,
                              sizeof(title_name)) == 0 &&
      title_id[0] != '\0') {
    return 0;
  }

  const char *base = strrchr(safe_path, '/');
  if (base && base[1] != '\0') {
    base++;
    size_t len = strlen(base);
    if (len >= 9U && len < title_id_size) {
      int valid = 1;
      for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)base[i];
        if (!(isalnum(c) || c == '_' || c == '-')) {
          valid = 0;
          break;
        }
      }
      if (valid) {
        (void)snprintf(title_id, title_id_size, "%s", base);
        return 0;
      }
    }
  }

  return -1;
}

static int is_valid_title_id_for_uninstall(const char *title_id) {
  if (title_id == NULL) {
    return 0;
  }
  size_t len = strlen(title_id);
  if (len < 4U || len > 16U) {
    return 0;
  }
  for (size_t i = 0; i < len; i++) {
    unsigned char c = (unsigned char)title_id[i];
    if (!(isalnum(c) || c == '_' || c == '-')) {
      return 0;
    }
  }
  return 1;
}

#if ENABLE_PKG_INSTALL
static int has_pkg_extension(const char *path) {
  if (path == NULL) {
    return 0;
  }
  const char *dot = strrchr(path, '.');
  if (dot == NULL) {
    return 0;
  }
  dot++;
  return (strcasecmp(dot, "pkg") == 0 || strcasecmp(dot, "fpkg") == 0 ||
          strcasecmp(dot, "ffpkg") == 0);
}
#endif

typedef struct {
  int active;
  int task_id;
  int last_percent;
  int last_error;
  unsigned long last_length;
  unsigned long last_transferred;
  char title_id[64];
  char path[FTP_PATH_MAX];
} game_install_state_t;

static game_install_state_t g_game_install_state = {0};

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
typedef int (*fn_sceAppInstUtilInitialize_t)(void);
typedef int (*fn_sceAppInstUtilTerminate_t)(void);
typedef int (*fn_sceAppInstUtilAppUnInstall_t)(const char *);
typedef int (*fn_sceAppInstUtilAppInstallPkg_t)(const char *, void *);
typedef int (*fn_sceAppInstUtilGetTitleIdFromPkg_t)(const char *, char *, int *);
typedef int (*fn_sceBgftServiceInit_t)(bgft_init_params *);
typedef int (*fn_sceBgftServiceTerm_t)(void);
typedef int (*fn_sceBgftServiceIntDownloadRegisterTaskByStorageEx_t)(
    bgft_download_param_ex *, int *);
typedef int (*fn_sceBgftServiceDownloadStartTask_t)(int);
typedef int (*fn_sceBgftServiceDownloadGetProgress_t)(int, SceBgftTaskProgress *);

static int g_bgft_initialized = 0;
static uint8_t g_bgft_heap[1024 * 1024];

static int psx_bgft_resolve(
    void **out_bgft,
    fn_sceBgftServiceInit_t *out_init,
    fn_sceBgftServiceTerm_t *out_term,
    fn_sceBgftServiceIntDownloadRegisterTaskByStorageEx_t *out_register,
    fn_sceBgftServiceDownloadStartTask_t *out_start,
    fn_sceBgftServiceDownloadGetProgress_t *out_progress) {
  if (!out_bgft || !out_init || !out_term || !out_register || !out_start ||
      !out_progress) {
    return -1;
  }

  *out_bgft = dlopen("/system/common/lib/libSceBgft.sprx",
                     RTLD_NOW | RTLD_GLOBAL);
  if (!*out_bgft) {
    return -1;
  }

  *out_init =
      (fn_sceBgftServiceInit_t)dlsym(*out_bgft, "sceBgftServiceInit");
  *out_term =
      (fn_sceBgftServiceTerm_t)dlsym(*out_bgft, "sceBgftServiceTerm");
  *out_register = (fn_sceBgftServiceIntDownloadRegisterTaskByStorageEx_t)dlsym(
      *out_bgft, "sceBgftServiceIntDownloadRegisterTaskByStorageEx");
  *out_start = (fn_sceBgftServiceDownloadStartTask_t)dlsym(
      *out_bgft, "sceBgftServiceDownloadStartTask");
  *out_progress = (fn_sceBgftServiceDownloadGetProgress_t)dlsym(
      *out_bgft, "sceBgftServiceDownloadGetProgress");

  if (!*out_init || !*out_term || !*out_register || !*out_start ||
      !*out_progress) {
    dlclose(*out_bgft);
    *out_bgft = NULL;
    return -1;
  }

  return 0;
}

static int psx_bgft_ensure_initialized(fn_sceBgftServiceInit_t f_bgft_init) {
  if (!f_bgft_init) {
    return -1;
  }
  if (g_bgft_initialized) {
    return 0;
  }

  bgft_init_params params;
  memset(&params, 0, sizeof(params));
  params.heap = g_bgft_heap;
  params.heapSize = sizeof(g_bgft_heap);
  int rc = f_bgft_init(&params);
  if (rc == 0) {
    g_bgft_initialized = 1;
    return 0;
  }
  /* already initialized */
  if ((uint32_t)rc == 0x80990001U) {
    g_bgft_initialized = 1;
    return 0;
  }
  return rc;
}

#if ENABLE_PKG_INSTALL
static int psx_get_title_id_from_pkg(const char *pkg_path, char *out_title_id,
                                     size_t out_title_id_size) {
  if (!pkg_path || !out_title_id || out_title_id_size == 0U) {
    return -1;
  }
  out_title_id[0] = '\0';

  void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx",
                         RTLD_NOW | RTLD_GLOBAL);
  if (!appinst) {
    return -1;
  }

  fn_sceAppInstUtilInitialize_t f_init =
      (fn_sceAppInstUtilInitialize_t)dlsym(appinst, "sceAppInstUtilInitialize");
  fn_sceAppInstUtilTerminate_t f_term =
      (fn_sceAppInstUtilTerminate_t)dlsym(appinst, "sceAppInstUtilTerminate");
  fn_sceAppInstUtilGetTitleIdFromPkg_t f_get_tid =
      (fn_sceAppInstUtilGetTitleIdFromPkg_t)dlsym(
          appinst, "sceAppInstUtilGetTitleIdFromPkg");

  if (!f_init || !f_term || !f_get_tid) {
    dlclose(appinst);
    return -1;
  }

  (void)f_init();
  int is_app = 0;
  int rc = f_get_tid(pkg_path, out_title_id, &is_app);
  (void)f_term();
  dlclose(appinst);
  return rc;
}

static int psx_install_pkg_bgft(const char *pkg_path, const char *content_name,
                                char *out_title_id,
                                size_t out_title_id_size,
                                int *out_task_id,
                                int *out_register_rc) {
  if (!pkg_path || !out_task_id || !out_register_rc) {
    return -1;
  }

  void *bgft = NULL;
  fn_sceBgftServiceInit_t f_bgft_init = NULL;
  fn_sceBgftServiceTerm_t f_bgft_term = NULL;
  fn_sceBgftServiceIntDownloadRegisterTaskByStorageEx_t f_register = NULL;
  fn_sceBgftServiceDownloadStartTask_t f_start = NULL;
  fn_sceBgftServiceDownloadGetProgress_t f_progress = NULL;

  if (psx_bgft_resolve(&bgft, &f_bgft_init, &f_bgft_term, &f_register,
                       &f_start, &f_progress) != 0) {
    return -1;
  }

  int init_rc = psx_bgft_ensure_initialized(f_bgft_init);
  if (init_rc != 0) {
    dlclose(bgft);
    return init_rc;
  }

  if (out_title_id && out_title_id_size > 0U) {
    (void)psx_get_title_id_from_pkg(pkg_path, out_title_id, out_title_id_size);
  }

  bgft_download_param_ex params;
  memset(&params, 0, sizeof(params));
  params.param.entitlement_type = 5;
  params.param.id = "";
  params.param.content_url = pkg_path;
  params.param.content_name =
      (content_name && content_name[0] != '\0') ? content_name : "Remote Install";
  params.param.icon_path = "/update/fakepic.png";
  params.param.playgo_scenario_id = "0";
  params.param.option = BGFT_TASK_OPTION_DELETE_AFTER_UPLOAD;
  params.slot = 0;

  int task_id = -1;
  int rc = f_register(&params, &task_id);
  if (rc == 0) {
    rc = f_start(task_id);
  }

  *out_register_rc = rc;
  *out_task_id = task_id;
  dlclose(bgft);
  return 0;
}
#endif

static int psx_bgft_progress(int task_id, SceBgftTaskProgress *out_progress,
                             int *out_rc) {
  if (!out_progress || !out_rc) {
    return -1;
  }

  void *bgft = NULL;
  fn_sceBgftServiceInit_t f_bgft_init = NULL;
  fn_sceBgftServiceTerm_t f_bgft_term = NULL;
  fn_sceBgftServiceIntDownloadRegisterTaskByStorageEx_t f_register = NULL;
  fn_sceBgftServiceDownloadStartTask_t f_start = NULL;
  fn_sceBgftServiceDownloadGetProgress_t f_progress = NULL;

  if (psx_bgft_resolve(&bgft, &f_bgft_init, &f_bgft_term, &f_register,
                       &f_start, &f_progress) != 0) {
    return -1;
  }

  (void)psx_bgft_ensure_initialized(f_bgft_init);
  memset(out_progress, 0, sizeof(*out_progress));
  *out_rc = f_progress(task_id, out_progress);
  dlclose(bgft);
  return 0;
}

static int psx_uninstall_title_id(const char *title_id, int *out_rc) {
  if (!title_id) {
    return -1;
  }

  void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx",
                         RTLD_NOW | RTLD_GLOBAL);
  if (!appinst) {
    return -1;
  }

  fn_sceAppInstUtilInitialize_t f_init =
      (fn_sceAppInstUtilInitialize_t)dlsym(appinst, "sceAppInstUtilInitialize");
  fn_sceAppInstUtilTerminate_t f_term =
      (fn_sceAppInstUtilTerminate_t)dlsym(appinst, "sceAppInstUtilTerminate");
  fn_sceAppInstUtilAppUnInstall_t f_uninstall =
      (fn_sceAppInstUtilAppUnInstall_t)dlsym(appinst,
                                             "sceAppInstUtilAppUnInstall");

  if (!f_init || !f_term || !f_uninstall) {
    dlclose(appinst);
    return -1;
  }

  (void)f_init();
  int rc = f_uninstall(title_id);
  (void)f_term();
  dlclose(appinst);
  if (out_rc) {
    *out_rc = rc;
  }
  return 0;
}

#if ENABLE_PKG_INSTALL
static int psx_install_pkg_path(const char *pkg_path, char *out_title_id,
                                size_t out_title_id_size, int *out_install_rc) {
  if ((pkg_path == NULL) || (out_install_rc == NULL)) {
    return -1;
  }

  void *appinst = dlopen("/system/common/lib/libSceAppInstUtil.sprx",
                         RTLD_NOW | RTLD_GLOBAL);
  if (!appinst) {
    return -1;
  }

  fn_sceAppInstUtilInitialize_t f_init =
      (fn_sceAppInstUtilInitialize_t)dlsym(appinst, "sceAppInstUtilInitialize");
  fn_sceAppInstUtilTerminate_t f_term =
      (fn_sceAppInstUtilTerminate_t)dlsym(appinst, "sceAppInstUtilTerminate");
  fn_sceAppInstUtilAppInstallPkg_t f_install =
      (fn_sceAppInstUtilAppInstallPkg_t)dlsym(appinst,
                                              "sceAppInstUtilAppInstallPkg");
  fn_sceAppInstUtilGetTitleIdFromPkg_t f_get_tid =
      (fn_sceAppInstUtilGetTitleIdFromPkg_t)dlsym(
          appinst, "sceAppInstUtilGetTitleIdFromPkg");

  if (!f_init || !f_term || !f_install) {
    dlclose(appinst);
    return -1;
  }

  (void)f_init();

  if (out_title_id && out_title_id_size > 0U) {
    out_title_id[0] = '\0';
    if (f_get_tid != NULL) {
      int is_app = 0;
      (void)f_get_tid(pkg_path, out_title_id, &is_app);
    }
  }

  int rc = f_install(pkg_path, NULL);
  (void)f_term();
  dlclose(appinst);

  *out_install_rc = rc;
  return 0;
}
#endif

typedef int (*sqlite3_cb_t)(void *, int, char **, char **);

typedef struct {
  int (*open_v2)(const char *, sqlite3 **, int, const char *);
  int (*close)(sqlite3 *);
  int (*exec)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **);
  int (*changes)(sqlite3 *);
  void (*free_fn)(void *);
  const char *(*errmsg)(sqlite3 *);
  void *lib_handle;
} psx_sqlite_api_t;

#define SQLITE_OPEN_READWRITE 0x00000002

static int psx_sqlite_load_api(psx_sqlite_api_t *api) {
  if (api == NULL) {
    return -1;
  }
  memset(api, 0, sizeof(*api));

  api->open_v2 = (int (*)(const char *, sqlite3 **, int, const char *))dlsym(
      RTLD_DEFAULT, "sqlite3_open_v2");
  api->close = (int (*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_close");
  api->exec =
      (int (*)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **))dlsym(
          RTLD_DEFAULT, "sqlite3_exec");
  api->changes = (int (*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_changes");
  api->free_fn = (void (*)(void *))dlsym(RTLD_DEFAULT, "sqlite3_free");
  api->errmsg = (const char *(*)(sqlite3 *))dlsym(RTLD_DEFAULT, "sqlite3_errmsg");

  if (api->open_v2 && api->close && api->exec && api->changes &&
      api->free_fn && api->errmsg) {
    return 0;
  }

  const char *sqlite_libs[] = {
      "/system/common/lib/libSceSqlite.sprx",
      "/system/common/lib/libsqlite3.sprx",
      NULL,
  };

  for (size_t i = 0; sqlite_libs[i] != NULL; i++) {
    void *h = dlopen(sqlite_libs[i], RTLD_NOW | RTLD_GLOBAL);
    if (h == NULL) {
      continue;
    }

    api->open_v2 =
        (int (*)(const char *, sqlite3 **, int, const char *))dlsym(h,
                                                                     "sqlite3_open_v2");
    api->close = (int (*)(sqlite3 *))dlsym(h, "sqlite3_close");
    api->exec =
        (int (*)(sqlite3 *, const char *, sqlite3_cb_t, void *, char **))dlsym(
            h, "sqlite3_exec");
    api->changes = (int (*)(sqlite3 *))dlsym(h, "sqlite3_changes");
    api->free_fn = (void (*)(void *))dlsym(h, "sqlite3_free");
    api->errmsg = (const char *(*)(sqlite3 *))dlsym(h, "sqlite3_errmsg");

    if (api->open_v2 && api->close && api->exec && api->changes &&
        api->free_fn && api->errmsg) {
      api->lib_handle = h;
      return 0;
    }

    dlclose(h);
  }

  memset(api, 0, sizeof(*api));
  return -1;
}

typedef struct {
  char names[64][64];
  size_t count;
} appdb_table_list_t;

typedef struct {
  char names[128][64];
  size_t count;
} appdb_column_list_t;

typedef struct {
  int value;
  int have_value;
} appdb_int_result_t;

static int psx_appdb_collect_tables_cb(void *ctx, int argc, char **argv,
                                       char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
    return 0;
  }

  appdb_table_list_t *list = (appdb_table_list_t *)ctx;
  if (list->count >= (sizeof(list->names) / sizeof(list->names[0]))) {
    return 0;
  }

  size_t n = strlen(argv[0]);
  if (n == 0U || n >= sizeof(list->names[0])) {
    return 0;
  }
  memcpy(list->names[list->count], argv[0], n + 1U);
  list->count++;
  return 0;
}

static int psx_appdb_collect_columns_cb(void *ctx, int argc, char **argv,
                                        char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 2 || argv == NULL || argv[1] == NULL) {
    return 0;
  }

  appdb_column_list_t *list = (appdb_column_list_t *)ctx;
  if (list->count >= (sizeof(list->names) / sizeof(list->names[0]))) {
    return 0;
  }

  size_t n = strlen(argv[1]);
  if (n == 0U || n >= sizeof(list->names[0])) {
    return 0;
  }
  memcpy(list->names[list->count], argv[1], n + 1U);
  list->count++;
  return 0;
}

static int psx_appdb_read_int_cb(void *ctx, int argc, char **argv, char **cols) {
  (void)cols;
  if (ctx == NULL || argc < 1 || argv == NULL || argv[0] == NULL) {
    return 0;
  }
  appdb_int_result_t *r = (appdb_int_result_t *)ctx;
  r->value = atoi(argv[0]);
  r->have_value = 1;
  return 0;
}

static void psx_sql_escape_text(const char *in, char *out, size_t out_size) {
  if (out == NULL || out_size == 0U) {
    return;
  }
  out[0] = '\0';
  if (in == NULL) {
    return;
  }

  size_t o = 0U;
  for (size_t i = 0; in[i] != '\0' && o + 1U < out_size; i++) {
    if (in[i] == '\'') {
      if (o + 2U >= out_size) {
        break;
      }
      out[o++] = '\'';
      out[o++] = '\'';
      continue;
    }
    out[o++] = in[i];
  }
  out[o] = '\0';
}

static int psx_appdb_insert_missing_from_template(psx_sqlite_api_t *sql,
                                                  sqlite3 *db,
                                                  const char *table_name,
                                                  const char *title_id,
                                                  const char *title_name,
                                                  const char *meta_path,
                                                  const char *content_id,
                                                  const char *category) {
  if (!sql || !db || !table_name || !title_id) {
    return -1;
  }

  char q[256];
  int n = snprintf(q, sizeof(q), "PRAGMA table_info(\"%s\")", table_name);
  if (n <= 0 || (size_t)n >= sizeof(q)) {
    return -1;
  }

  appdb_column_list_t cols;
  memset(&cols, 0, sizeof(cols));
  char *err = NULL;
  if (sql->exec(db, q, psx_appdb_collect_columns_cb, &cols, &err) != 0) {
    if (err) {
      sql->free_fn(err);
    }
    return -1;
  }
  if (err) {
    sql->free_fn(err);
    err = NULL;
  }
  if (cols.count == 0U) {
    return -1;
  }

  char esc_tid[96], esc_tname[512], esc_meta[256], esc_cid[160], esc_cat[64];
  psx_sql_escape_text(title_id, esc_tid, sizeof(esc_tid));
  psx_sql_escape_text(title_name ? title_name : title_id, esc_tname,
                      sizeof(esc_tname));
  psx_sql_escape_text(meta_path ? meta_path : "", esc_meta, sizeof(esc_meta));
  psx_sql_escape_text(content_id ? content_id : "", esc_cid, sizeof(esc_cid));
  psx_sql_escape_text(category ? category : "gd", esc_cat, sizeof(esc_cat));

  char col_sql[8192];
  char sel_sql[12288];
  col_sql[0] = '\0';
  sel_sql[0] = '\0';
  size_t col_pos = 0U;
  size_t sel_pos = 0U;

  for (size_t i = 0; i < cols.count; i++) {
    const char *c = cols.names[i];
    if (i > 0) {
      int nn = snprintf(col_sql + col_pos, sizeof(col_sql) - col_pos, ",");
      if (nn <= 0 || (size_t)nn >= (sizeof(col_sql) - col_pos)) {
        return -1;
      }
      col_pos += (size_t)nn;

      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, ",");
      if (nn <= 0 || (size_t)nn >= (sizeof(sel_sql) - sel_pos)) {
        return -1;
      }
      sel_pos += (size_t)nn;
    }

    int nn = snprintf(col_sql + col_pos, sizeof(col_sql) - col_pos, "\"%s\"", c);
    if (nn <= 0 || (size_t)nn >= (sizeof(col_sql) - col_pos)) {
      return -1;
    }
    col_pos += (size_t)nn;

    if (strcmp(c, "titleId") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_tid);
    } else if (strcmp(c, "titleName") == 0 ||
               strcmp(c, "entitlementTitleName") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_tname);
    } else if (strcmp(c, "metaDataPath") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_meta);
    } else if (strcmp(c, "contentId") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_cid);
    } else if (strcmp(c, "visible") == 0 || strcmp(c, "canRemove") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "1");
    } else if (strcmp(c, "externalHddAppStatus") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "0");
    } else if (strcmp(c, "category") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos, "'%s'",
                    esc_cat);
    } else if (strcmp(c, "platform") == 0) {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos,
                    "CASE WHEN '%s'='gde' THEN 'app' ELSE 'game' END",
                    esc_cat);
    } else {
      nn = snprintf(sel_sql + sel_pos, sizeof(sel_sql) - sel_pos,
                    "src.\"%s\"", c);
    }

    if (nn <= 0 || (size_t)nn >= (sizeof(sel_sql) - sel_pos)) {
      return -1;
    }
    sel_pos += (size_t)nn;
  }

  char ins[24576];
  n = snprintf(ins, sizeof(ins),
               "INSERT OR IGNORE INTO \"%s\"(%s) SELECT %s FROM \"%s\" AS src "
               "LIMIT 1",
               table_name, col_sql, sel_sql, table_name);
  if (n <= 0 || (size_t)n >= sizeof(ins)) {
    return -1;
  }

  if (sql->exec(db, ins, NULL, NULL, &err) != 0) {
    if (err) {
      sql->free_fn(err);
    }
    return -1;
  }
  if (err) {
    sql->free_fn(err);
  }
  return 0;
}

static int psx_repair_appdb_visibility_for_title(const char *title_id,
                                                 int *out_tables,
                                                 int *out_rows) {
  if (out_tables) {
    *out_tables = 0;
  }
  if (out_rows) {
    *out_rows = 0;
  }
  if (title_id == NULL || title_id[0] == '\0') {
    return -1;
  }

  psx_sqlite_api_t sql;
  if (psx_sqlite_load_api(&sql) != 0) {
    return -2;
  }

  sqlite3 *db = NULL;
  int rc = sql.open_v2("/system_data/priv/mms/app.db", &db,
                       SQLITE_OPEN_READWRITE, NULL);
  if (rc != 0 || db == NULL) {
    if (sql.lib_handle != NULL) {
      dlclose(sql.lib_handle);
    }
    return -3;
  }

  appdb_table_list_t tables;
  memset(&tables, 0, sizeof(tables));

  char *err = NULL;
  (void)sql.exec(db,
                 "SELECT name FROM sqlite_master WHERE type='table' AND name "
                 "LIKE 'tbl_appbrowse_%'",
                 psx_appdb_collect_tables_cb, &tables, &err);
  if (err != NULL) {
    sql.free_fn(err);
    err = NULL;
  }

  int total_rows = 0;
  int touched_tables = 0;

  char app_dir[FTP_PATH_MAX] = {0};
  char title_name[256] = {0};
  char content_id[128] = {0};
  char category[32] = {0};
  char meta_path[FTP_PATH_MAX] = {0};

  if (resolve_installed_app_dir_by_title(title_id, app_dir, sizeof(app_dir)) ==
      0) {
    char tid_tmp[64] = {0};
    (void)read_installed_game_sfo(app_dir, tid_tmp, sizeof(tid_tmp), title_name,
                                  sizeof(title_name));
    (void)read_installed_game_sfo_field(app_dir, "CONTENT_ID", content_id,
                                        sizeof(content_id));
    (void)read_installed_game_sfo_field(app_dir, "CATEGORY", category,
                                        sizeof(category));
  }
  if (title_name[0] == '\0') {
    (void)snprintf(title_name, sizeof(title_name), "%s", title_id);
  }
  if (content_id[0] == '\0') {
    (void)snprintf(content_id, sizeof(content_id), "FAKE0000-%s_00-0000000000000000",
                   title_id);
  }
  if (category[0] == '\0') {
    (void)snprintf(category, sizeof(category), "gd");
  }
  (void)snprintf(meta_path, sizeof(meta_path), "/user/appmeta/%s", title_id);

  for (size_t i = 0; i < tables.count; i++) {
    char q[1024];
    int n = snprintf(q, sizeof(q),
                     "SELECT COUNT(*) FROM \"%s\" WHERE titleId='%s'",
                     tables.names[i], title_id);
    int row_exists = 0;
    if (n > 0 && (size_t)n < sizeof(q)) {
      appdb_int_result_t cnt;
      memset(&cnt, 0, sizeof(cnt));
      if (sql.exec(db, q, psx_appdb_read_int_cb, &cnt, &err) == 0 &&
          cnt.have_value && cnt.value > 0) {
        row_exists = 1;
      }
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    if (!row_exists) {
      if (psx_appdb_insert_missing_from_template(&sql, db, tables.names[i],
                                                 title_id, title_name,
                                                 meta_path, content_id,
                                                 category) == 0) {
        int ins = sql.changes(db);
        if (ins > 0) {
          touched_tables++;
          total_rows += ins;
        }
      }
    }

    n = snprintf(q, sizeof(q),
                     "UPDATE \"%s\" SET visible=1 WHERE titleId='%s'",
                     tables.names[i], title_id);
    if (n <= 0 || (size_t)n >= sizeof(q)) {
      continue;
    }

    if (sql.exec(db, q, NULL, NULL, &err) == 0) {
      int ch = sql.changes(db);
      if (ch > 0) {
        touched_tables++;
        total_rows += ch;
      }
    }
    if (err != NULL) {
      sql.free_fn(err);
      err = NULL;
    }

    n = snprintf(q, sizeof(q),
                 "UPDATE \"%s\" SET externalHddAppStatus=0 WHERE "
                 "titleId='%s'",
                 tables.names[i], title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }
  }

  (void)sql.exec(db,
                 "UPDATE tbl_appinfo SET val=0 WHERE "
                 "key='_external_hdd_app_status'",
                 NULL, NULL, &err);
  if (err != NULL) {
    sql.free_fn(err);
    err = NULL;
  }

  {
    char q[1024];
    int n = 0;
    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','TITLE_ID','%s')",
                 title_id, title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','TITLE','%s')",
                 title_id, title_name);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','CONTENT_ID','%s')",
                 title_id, content_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','CATEGORY','%s')",
                 title_id, category);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','_metadata_path','%s')",
                 title_id, meta_path);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }

    n = snprintf(q, sizeof(q),
                 "INSERT OR IGNORE INTO tbl_appinfo(titleId,key,val) VALUES('%s','_org_path','/user/app/%s')",
                 title_id, title_id);
    if (n > 0 && (size_t)n < sizeof(q)) {
      (void)sql.exec(db, q, NULL, NULL, &err);
      if (err != NULL) {
        sql.free_fn(err);
        err = NULL;
      }
    }
  }

  (void)sql.close(db);
  if (sql.lib_handle != NULL) {
    dlclose(sql.lib_handle);
  }

  if (out_tables) {
    *out_tables = touched_tables;
  }
  if (out_rows) {
    *out_rows = total_rows;
  }
  return 0;
}

#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

static int append_installed_entries_from_base(const char *base,
                                              char *body,
                                              size_t cap,
                                              size_t *pos,
                                              int *first,
                                              size_t *count_added) {
  if (!base || !body || !pos || !first || !count_added) {
    return -1;
  }

  DIR *dir = opendir(base);
  if (dir == NULL) {
    return 0;
  }

  struct dirent *ent;
  while ((ent = readdir(dir)) != NULL) {
    if ((strcmp(ent->d_name, ".") == 0) || (strcmp(ent->d_name, "..") == 0)) {
      continue;
    }

    char app_dir[FTP_PATH_MAX];
    int n = snprintf(app_dir, sizeof(app_dir), "%s/%s", base, ent->d_name);
    if (n < 0 || (size_t)n >= sizeof(app_dir)) {
      continue;
    }

    struct stat st;
    if (stat(app_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
      continue;
    }

    char title_id[64] = {0};
    char title_name[256] = {0};
    (void)snprintf(title_id, sizeof(title_id), "%.63s", ent->d_name);
    (void)read_installed_game_sfo(app_dir, title_id, sizeof(title_id), title_name,
                                  sizeof(title_name));
    if (title_name[0] == '\0') {
      (void)snprintf(title_name, sizeof(title_name), "%s", title_id);
    }

    char icon_path[FTP_PATH_MAX] = {0};
    int has_icon = (resolve_installed_icon_path(title_id, app_dir, icon_path,
                                                sizeof(icon_path)) == 0);

    if (!*first) {
      if (buf_append_cstr(body, cap, pos, ",") != 0) {
        closedir(dir);
        return -1;
      }
    }
    *first = 0;
    (*count_added)++;

    if (buf_append_cstr(body, cap, pos, "{\"id\":\"") != 0 ||
        json_escape_append(body, cap, pos, title_id) != 0 ||
        buf_append_cstr(body, cap, pos, "\",\"name\":\"") != 0 ||
        json_escape_append(body, cap, pos, title_name) != 0 ||
        buf_append_cstr(body, cap, pos, "\",\"path\":\"") != 0 ||
        json_escape_append(body, cap, pos, app_dir) != 0 ||
        buf_append_cstr(body, cap, pos, "\",\"source\":\"") != 0 ||
        json_escape_append(body, cap, pos, base) != 0 ||
        buf_append_cstr(body, cap, pos, "\",\"has_icon\":") != 0 ||
        buf_append_cstr(body, cap, pos, has_icon ? "true" : "false") != 0 ||
        buf_append_cstr(body, cap, pos, "}") != 0) {
      closedir(dir);
      return -1;
    }
  }

  closedir(dir);
  return 0;
}

static int extract_title_id_from_game_image(const char *safe_path,
                                            char *title_id,
                                            size_t title_id_size) {
  if ((safe_path == NULL) || (title_id == NULL) || (title_id_size < 10U)) {
    return -1;
  }

  title_id[0] = '\0';

  /* 1) Try PKG */
  pkg_context_t pkg_ctx;
  if (pkg_init(&pkg_ctx, safe_path) == PKG_OK) {
    const pkg_entry_t *sfo_entry =
        pkg_find_entry_by_id(&pkg_ctx, PKG_ENTRY_ID_PARAM_SFO);
    if (sfo_entry && sfo_entry->size > 0U && sfo_entry->size <= 65536U) {
      uint8_t *sfo_data = (uint8_t *)malloc((size_t)sfo_entry->size);
      if (sfo_data != NULL) {
        if (pkg_extract_to_buffer(&pkg_ctx, sfo_entry, sfo_data,
                                  (size_t)sfo_entry->size) > 0) {
          (void)sfo_get_string(sfo_data, (size_t)sfo_entry->size, "TITLE_ID",
                               title_id, title_id_size);
          if (title_id[0] == '\0') {
            char cid[64] = "";
            (void)sfo_get_string(sfo_data, (size_t)sfo_entry->size,
                                 "CONTENT_ID", cid, sizeof(cid));
            (void)title_id_from_content_id(cid, title_id, title_id_size);
          }
        }
        free(sfo_data);
      }
    }

    if (title_id[0] == '\0') {
      (void)title_id_from_content_id(pkg_ctx.header.content_id,
                                     title_id, title_id_size);
    }

    pkg_cleanup(&pkg_ctx);
    return (title_id[0] != '\0') ? 0 : -1;
  }

  /* 2) Try exFAT image */
  exfat_context_t ctx;
  if (exfat_init(&ctx, safe_path) != 0) {
    return -1;
  }

  exfat_file_info_t root_entries[256];
  int root_count = exfat_read_directory(&ctx,
                                        ctx.boot_sector.root_dir_first_cluster,
                                        root_entries, 256);

  for (int i = 0; i < root_count; i++) {
    if (!root_entries[i].is_directory) {
      continue;
    }
    if (strcasecmp(root_entries[i].filename, "sce_sys") != 0) {
      continue;
    }

    exfat_file_info_t sce_entries[64];
    int sce_count = exfat_read_directory(&ctx,
                                         root_entries[i].first_cluster,
                       sce_entries, 64);

    for (int j = 0; j < sce_count; j++) {
      if (sce_entries[j].is_directory) {
        continue;
      }

      if (strcasecmp(sce_entries[j].filename, "param.sfo") == 0 &&
          sce_entries[j].data_length > 0U &&
          sce_entries[j].data_length <= 65536U) {
        size_t slen = (size_t)sce_entries[j].data_length;
        uint8_t *sbuf = (uint8_t *)malloc(slen);
        if (sbuf != NULL) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], sbuf,
                                                slen);
          if (got > 0) {
            (void)sfo_get_string(sbuf, (size_t)got, "TITLE_ID", title_id,
                                 title_id_size);
            if (title_id[0] == '\0') {
              char cid[64] = "";
              (void)sfo_get_string(sbuf, (size_t)got, "CONTENT_ID", cid,
                                   sizeof(cid));
              (void)title_id_from_content_id(cid, title_id, title_id_size);
            }
          }
          free(sbuf);
        }
      }

      if (title_id[0] == '\0' &&
          strcasecmp(sce_entries[j].filename, "param.json") == 0 &&
          sce_entries[j].data_length > 0U &&
          sce_entries[j].data_length <= (256U * 1024U)) {
        size_t plen = (size_t)sce_entries[j].data_length;
        uint8_t *pbuf = (uint8_t *)malloc(plen + 1U);
        if (pbuf != NULL) {
          ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], pbuf,
                                                plen);
          if (got > 0) {
            pbuf[got] = '\0';
            (void)json_get_string((char *)pbuf, "titleId", title_id,
                                  title_id_size);
            if (title_id[0] == '\0') {
              (void)json_get_string((char *)pbuf, "title_id", title_id,
                                    title_id_size);
            }
            if (title_id[0] == '\0') {
              char cid[64] = "";
              (void)json_get_string((char *)pbuf, "contentId", cid,
                                    sizeof(cid));
              if (cid[0] == '\0') {
                (void)json_get_string((char *)pbuf, "content_id", cid,
                                      sizeof(cid));
              }
              (void)title_id_from_content_id(cid, title_id, title_id_size);
            }
          }
          free(pbuf);
        }
      }

      if (title_id[0] != '\0') {
        break;
      }
    }
    break;
  }

  exfat_cleanup(&ctx);
  return (title_id[0] != '\0') ? 0 : -1;
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

        /* param.sfo */
        if (strcasecmp(sce_entries[j].filename, "param.sfo") == 0 &&
            sce_entries[j].data_length > 0 &&
            sce_entries[j].data_length <= 65536) {
          size_t slen = (size_t)sce_entries[j].data_length;
          uint8_t *sbuf = (uint8_t *)malloc(slen);
          if (sbuf) {
            ssize_t got = exfat_extract_to_buffer(&ctx, &sce_entries[j], sbuf, slen);
            if (got > 0) {
              sfo_get_string(sbuf, (size_t)got, "TITLE_ID", title_id, sizeof(title_id));
              sfo_get_string(sbuf, (size_t)got, "TITLE", title_name, sizeof(title_name));
              sfo_get_string(sbuf, (size_t)got, "APP_VER", version, sizeof(version));
              sfo_get_string(sbuf, (size_t)got, "CATEGORY", category, sizeof(category));
              {
                char sfo_cid[48] = "";
                sfo_get_string(sbuf, (size_t)got, "CONTENT_ID", sfo_cid, sizeof(sfo_cid));
                if (sfo_cid[0]) {
                  strncpy(content_id, sfo_cid, sizeof(content_id) - 1U);
                  content_id[sizeof(content_id) - 1U] = '\0';
                }
              }
            }
            free(sbuf);
          }
        }

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
              if (!content_id[0]) {
                json_get_string((char *)pbuf, "contentId", content_id, sizeof(content_id));
                if (!content_id[0]) {
                  json_get_string((char *)pbuf, "content_id", content_id, sizeof(content_id));
                }
              }
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

  if (!title_id[0] && content_id[0]) {
    (void)title_id_from_content_id(content_id, title_id, sizeof(title_id));
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

/* builtin_unzip extraction thread — works without libarchive (PS5 safe) */
#ifndef ENABLE_LIBARCHIVE
static void *extract_thread_builtin(void *arg) {
  (void)arg;
  int rc = builtin_unzip(g_extract.archive_path, g_extract.dest_path,
                         &g_extract.cancelled,
                         g_extract.error_msg, sizeof(g_extract.error_msg));
  if (rc != 0) {
    if (g_extract.error_msg[0] == '\0') {
      snprintf(g_extract.error_msg, sizeof(g_extract.error_msg),
               "Extraction failed");
    }
    g_extract.error = 1;
  }
  g_extract.done = 1;
  g_extract.active = 0;
  return NULL;
}
#endif

#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
static int path_has_extension(const char *path, const char *ext) {
  if (path == NULL || ext == NULL) {
    return 0;
  }
  const char *dot = strrchr(path, '.');
  if (dot == NULL) {
    return 0;
  }
  return strcasecmp(dot, ext) == 0;
}
#endif

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
#include <archive.h>
#include <archive_entry.h>

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

#if !defined(ENABLE_LIBARCHIVE) || !ENABLE_LIBARCHIVE
  if (!path_has_extension(safe_path, ".zip")) {
    return error_json(HTTP_STATUS_415_UNSUPPORTED_MEDIA_TYPE,
                      "This build can extract .zip files only; enable libarchive for .7z/.rar/.tar/.gz");
  }
#endif

  /* Set up extraction state */
  memset(&g_extract, 0, sizeof(g_extract));
  strncpy(g_extract.archive_path, safe_path, sizeof(g_extract.archive_path) - 1);
  g_extract.archive_path[sizeof(g_extract.archive_path) - 1] = '\0';
  strncpy(g_extract.dest_path, safe_dst, sizeof(g_extract.dest_path) - 1);
  g_extract.dest_path[sizeof(g_extract.dest_path) - 1] = '\0';
  g_extract.active = 1;

  /* Get archive size for progress tracking */
  struct stat st;
  if (stat(safe_path, &st) == 0) {
    g_extract.total_bytes = (uint64_t)st.st_size;
  }

#if defined(ENABLE_LIBARCHIVE) && ENABLE_LIBARCHIVE
  /* Start extraction thread */
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  (void)pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
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
  /* builtin_unzip fallback - no libarchive dependency (works on PS5) */
  {
    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    (void)pthread_attr_setstacksize(&attr, (size_t)HTTP_THREAD_STACK_SIZE);
    if (pthread_create(&tid, &attr, extract_thread_builtin, NULL) != 0) {
      g_extract.active = 0;
      pthread_attr_destroy(&attr);
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to start extraction thread");
    }
    pthread_attr_destroy(&attr);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  const char *body = "{\"ok\":true,\"message\":\"Extraction started (built-in unzip)\"}";
  http_response_set_body(resp, body, strlen(body));
  return resp;
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

#if (defined(ENABLE_LIBCURL) && ENABLE_LIBCURL) || \
    defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define DL_HAS_BACKEND 1
#else
#define DL_HAS_BACKEND 0
#endif

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

static int dl_has_scheme(const char *url, const char *scheme) {
  size_t n = scheme ? strlen(scheme) : 0U;
  return url != NULL && scheme != NULL && strncasecmp(url, scheme, n) == 0;
}

static int dl_url_is_supported(const char *url, char *reason,
                               size_t reason_size) {
  if (reason && reason_size > 0U) {
    reason[0] = '\0';
  }
  if (url == NULL || url[0] == '\0') {
    if (reason && reason_size > 0U) {
      snprintf(reason, reason_size, "Missing url parameter");
    }
    return 0;
  }
  if (dl_has_scheme(url, "magnet:")) {
    if (reason && reason_size > 0U) {
      snprintf(reason, reason_size,
               "Magnet links need a BitTorrent/DHT engine; this build supports direct HTTP/HTTPS URLs only");
    }
    return 0;
  }
  if (dl_has_scheme(url, "https://")) {
    return 1;
  }
  if (dl_has_scheme(url, "http://")) {
    return 1;
  }
  if (reason && reason_size > 0U) {
    snprintf(reason, reason_size,
             "Unsupported URL scheme; use a direct http:// or https:// URL");
  }
  return 0;
}

static void dl_sanitize_filename(char *name) {
  if (name == NULL || name[0] == '\0') {
    return;
  }
  for (size_t i = 0; name[i] != '\0'; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 32U || name[i] == '/' || name[i] == '\\' || name[i] == ':' ||
        name[i] == '*' || name[i] == '?' || name[i] == '"' ||
        name[i] == '<' || name[i] == '>' || name[i] == '|') {
      name[i] = '_';
    }
  }
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    name[0] = '_';
    name[1] = '\0';
  }
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
  dl_sanitize_filename(out);
}

static int dl_is_directory_path(const char *path) {
  struct stat st;
  return path != NULL && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static int dl_try_destination(const char *candidate, char *safe,
                              size_t safe_size) {
  if (candidate == NULL || candidate[0] == '\0') {
    return 0;
  }
  if (!validate_path(candidate, safe, safe_size)) {
    return 0;
  }
  return dl_is_directory_path(safe);
}

static int dl_normalize_destination(const char *requested, char *safe,
                                    size_t safe_size) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int wants_ps_default = (requested == NULL || requested[0] == '\0' ||
                          strcmp(requested, "/") == 0);
  if (wants_ps_default) {
    static const char *const ps_defaults[] = {
        "/data",
        "/mnt/usb0",
        "/mnt/usb1",
        NULL
    };
    for (size_t i = 0U; ps_defaults[i] != NULL; i++) {
      if (dl_try_destination(ps_defaults[i], safe, safe_size)) {
        return 1;
      }
    }
  }
#endif

  if (dl_try_destination(requested, safe, safe_size)) {
    return 1;
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (requested != NULL && strcmp(requested, "/data") == 0) {
    static const char *const ps_defaults[] = {
        "/mnt/usb0",
        "/mnt/usb1",
        NULL
    };
    for (size_t i = 0U; ps_defaults[i] != NULL; i++) {
      if (dl_try_destination(ps_defaults[i], safe, safe_size)) {
        return 1;
      }
    }
  }
#endif

  if (g_http_root[0] != '\0' && strcmp(g_http_root, "/") != 0 &&
      dl_try_destination(g_http_root, safe, safe_size)) {
    return 1;
  }

  return 0;
}

#if DL_HAS_BACKEND

#if !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5)
#include <curl/curl.h>
#elif defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
#include "pal_curl.h"
#endif

struct dl_write_ctx {
  dl_entry_t *dl;
  int fd;
};

static int dl_write_all(int fd, const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *)data;
  while (len > 0U) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      return -1;
    }
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

typedef int (*psx_sceNetInit_fn)(void);
typedef int (*psx_sceNetPoolCreate_fn)(const char *name, int size, int flags);
typedef int (*psx_sceSslInit_fn)(size_t pool_size);
typedef int (*psx_sceHttpInit_fn)(int libnet_mem_id, int libssl_ctx_id,
                                  size_t pool_size);
typedef int (*psx_sceHttpCreateTemplate_fn)(int libhttp_ctx_id,
                                            const char *user_agent,
                                            int http_ver,
                                            int is_auto_proxy_conf);
typedef int (*psx_sceHttpDeleteTemplate_fn)(int tmpl_id);
typedef int (*psx_sceHttpCreateConnectionWithURL_fn)(int tmpl_id,
                                                     const char *url,
                                                     int keepalive);
typedef int (*psx_sceHttpDeleteConnection_fn)(int conn_id);
typedef int (*psx_sceHttpCreateRequestWithURL2_fn)(int conn_id,
                                                   const char *method,
                                                   const char *url,
                                                   uint64_t content_length);
typedef int (*psx_sceHttpDeleteRequest_fn)(int req_id);
typedef int (*psx_sceHttpSetAutoRedirect_fn)(int id, int enable);
typedef int (*psx_sceHttpSetConnectTimeOut_fn)(int id, uint32_t usec);
typedef int (*psx_sceHttpSetRecvTimeOut_fn)(int id, uint32_t usec);
typedef int (*psx_sceHttpSetSendTimeOut_fn)(int id, uint32_t usec);
typedef int (*psx_sceHttpSetResolveTimeOut_fn)(int id, uint32_t usec);
typedef int (*psx_sceHttpSetResolveRetry_fn)(int id, int retry);
typedef int (*psx_sceHttpSetResponseHeaderMaxSize_fn)(int id, size_t size);
typedef int (*psx_sceHttpSetInflateGZIPEnabled_fn)(int id, int enable);
typedef int (*psx_sceHttpAddRequestHeader_fn)(int id, const char *name,
                                              const char *value,
                                              uint32_t mode);
typedef int (*psx_sceHttpSendRequest_fn)(int req_id, const void *post_data,
                                         size_t size);
typedef int (*psx_sceHttpReadData_fn)(int req_id, void *data, size_t size);
typedef int (*psx_sceHttpGetStatusCode_fn)(int req_id, int *status_code);
typedef int (*psx_sceHttpGetResponseContentLength_fn)(int req_id, int *result,
                                                      uint64_t *content_len);
typedef int (*psx_sceHttpGetLastErrno_fn)(int req_id, int *err_num);
typedef int (*psx_https_callback_t)(int libssl_ctx_id, unsigned int verify_err,
                                    void *const ssl_cert[], int cert_num,
                                    void *user_arg);
typedef int (*psx_sceHttpsSetSslCallback_fn)(int id,
                                             psx_https_callback_t cb,
                                             void *user_arg);

typedef struct {
  int loaded;
  void *net_mod;
  void *ssl_mod;
  void *http_mod;
  psx_sceNetInit_fn sceNetInit;
  psx_sceNetPoolCreate_fn sceNetPoolCreate;
  psx_sceSslInit_fn sceSslInit;
  psx_sceHttpInit_fn sceHttpInit;
  psx_sceHttpCreateTemplate_fn sceHttpCreateTemplate;
  psx_sceHttpDeleteTemplate_fn sceHttpDeleteTemplate;
  psx_sceHttpCreateConnectionWithURL_fn sceHttpCreateConnectionWithURL;
  psx_sceHttpDeleteConnection_fn sceHttpDeleteConnection;
  psx_sceHttpCreateRequestWithURL2_fn sceHttpCreateRequestWithURL2;
  psx_sceHttpDeleteRequest_fn sceHttpDeleteRequest;
  psx_sceHttpSetAutoRedirect_fn sceHttpSetAutoRedirect;
  psx_sceHttpSetConnectTimeOut_fn sceHttpSetConnectTimeOut;
  psx_sceHttpSetRecvTimeOut_fn sceHttpSetRecvTimeOut;
  psx_sceHttpSetSendTimeOut_fn sceHttpSetSendTimeOut;
  psx_sceHttpSetResolveTimeOut_fn sceHttpSetResolveTimeOut;
  psx_sceHttpSetResolveRetry_fn sceHttpSetResolveRetry;
  psx_sceHttpSetResponseHeaderMaxSize_fn sceHttpSetResponseHeaderMaxSize;
  psx_sceHttpSetInflateGZIPEnabled_fn sceHttpSetInflateGZIPEnabled;
  psx_sceHttpAddRequestHeader_fn sceHttpAddRequestHeader;
  psx_sceHttpSendRequest_fn sceHttpSendRequest;
  psx_sceHttpReadData_fn sceHttpReadData;
  psx_sceHttpGetStatusCode_fn sceHttpGetStatusCode;
  psx_sceHttpGetResponseContentLength_fn sceHttpGetResponseContentLength;
  psx_sceHttpGetLastErrno_fn sceHttpGetLastErrno;
  psx_sceHttpsSetSslCallback_fn sceHttpsSetSslCallback;
} psx_http_symbols_t;

static psx_http_symbols_t g_psx_http;
static pthread_mutex_t g_psx_http_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_psx_net_pool = -1;
static int g_psx_ssl_ctx = -1;
static int g_psx_http_ctx = -1;

static void *psx_open_sce_module(const char *name) {
  char path[128];
  int n = snprintf(path, sizeof(path), "/system/common/lib/%s.sprx", name);
  if (n <= 0 || (size_t)n >= sizeof(path)) {
    return NULL;
  }
  return dlopen(path, RTLD_NOW | RTLD_GLOBAL);
}

static int psx_http_load_symbols(char *err, size_t err_size) {
  if (g_psx_http.loaded) {
    return 0;
  }

  int module_rc = 0;
  (void)psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_NET, &module_rc);
  (void)psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_SSL, &module_rc);
  (void)psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_HTTP, &module_rc);

  g_psx_http.net_mod = psx_open_sce_module("libSceNet");
  g_psx_http.ssl_mod = psx_open_sce_module("libSceSsl");
  g_psx_http.http_mod = psx_open_sce_module("libSceHttp");

  if (g_psx_http.net_mod == NULL || g_psx_http.ssl_mod == NULL ||
      g_psx_http.http_mod == NULL) {
    if (err && err_size > 0U) {
      snprintf(err, err_size, "Failed to load SceNet/SceSsl/SceHttp modules");
    }
    return -1;
  }

  g_psx_http.sceNetInit =
      (psx_sceNetInit_fn)dlsym(g_psx_http.net_mod, "sceNetInit");
  g_psx_http.sceNetPoolCreate =
      (psx_sceNetPoolCreate_fn)dlsym(g_psx_http.net_mod, "sceNetPoolCreate");
  g_psx_http.sceSslInit =
      (psx_sceSslInit_fn)dlsym(g_psx_http.ssl_mod, "sceSslInit");
  g_psx_http.sceHttpInit =
      (psx_sceHttpInit_fn)dlsym(g_psx_http.http_mod, "sceHttpInit");
  g_psx_http.sceHttpCreateTemplate =
      (psx_sceHttpCreateTemplate_fn)dlsym(g_psx_http.http_mod,
                                          "sceHttpCreateTemplate");
  g_psx_http.sceHttpDeleteTemplate =
      (psx_sceHttpDeleteTemplate_fn)dlsym(g_psx_http.http_mod,
                                          "sceHttpDeleteTemplate");
  g_psx_http.sceHttpCreateConnectionWithURL =
      (psx_sceHttpCreateConnectionWithURL_fn)dlsym(
          g_psx_http.http_mod, "sceHttpCreateConnectionWithURL");
  g_psx_http.sceHttpDeleteConnection =
      (psx_sceHttpDeleteConnection_fn)dlsym(g_psx_http.http_mod,
                                            "sceHttpDeleteConnection");
  g_psx_http.sceHttpCreateRequestWithURL2 =
      (psx_sceHttpCreateRequestWithURL2_fn)dlsym(
          g_psx_http.http_mod, "sceHttpCreateRequestWithURL2");
  g_psx_http.sceHttpDeleteRequest =
      (psx_sceHttpDeleteRequest_fn)dlsym(g_psx_http.http_mod,
                                         "sceHttpDeleteRequest");
  g_psx_http.sceHttpSetAutoRedirect =
      (psx_sceHttpSetAutoRedirect_fn)dlsym(g_psx_http.http_mod,
                                           "sceHttpSetAutoRedirect");
  g_psx_http.sceHttpSetConnectTimeOut =
      (psx_sceHttpSetConnectTimeOut_fn)dlsym(g_psx_http.http_mod,
                                             "sceHttpSetConnectTimeOut");
  g_psx_http.sceHttpSetRecvTimeOut =
      (psx_sceHttpSetRecvTimeOut_fn)dlsym(g_psx_http.http_mod,
                                          "sceHttpSetRecvTimeOut");
  g_psx_http.sceHttpSetSendTimeOut =
      (psx_sceHttpSetSendTimeOut_fn)dlsym(g_psx_http.http_mod,
                                          "sceHttpSetSendTimeOut");
  g_psx_http.sceHttpSetResolveTimeOut =
      (psx_sceHttpSetResolveTimeOut_fn)dlsym(g_psx_http.http_mod,
                                             "sceHttpSetResolveTimeOut");
  g_psx_http.sceHttpSetResolveRetry =
      (psx_sceHttpSetResolveRetry_fn)dlsym(g_psx_http.http_mod,
                                           "sceHttpSetResolveRetry");
  g_psx_http.sceHttpSetResponseHeaderMaxSize =
      (psx_sceHttpSetResponseHeaderMaxSize_fn)dlsym(
          g_psx_http.http_mod, "sceHttpSetResponseHeaderMaxSize");
  g_psx_http.sceHttpSetInflateGZIPEnabled =
      (psx_sceHttpSetInflateGZIPEnabled_fn)dlsym(
          g_psx_http.http_mod, "sceHttpSetInflateGZIPEnabled");
  g_psx_http.sceHttpAddRequestHeader =
      (psx_sceHttpAddRequestHeader_fn)dlsym(g_psx_http.http_mod,
                                            "sceHttpAddRequestHeader");
  g_psx_http.sceHttpSendRequest =
      (psx_sceHttpSendRequest_fn)dlsym(g_psx_http.http_mod,
                                       "sceHttpSendRequest");
  g_psx_http.sceHttpReadData =
      (psx_sceHttpReadData_fn)dlsym(g_psx_http.http_mod, "sceHttpReadData");
  g_psx_http.sceHttpGetStatusCode =
      (psx_sceHttpGetStatusCode_fn)dlsym(g_psx_http.http_mod,
                                         "sceHttpGetStatusCode");
  g_psx_http.sceHttpGetResponseContentLength =
      (psx_sceHttpGetResponseContentLength_fn)dlsym(
          g_psx_http.http_mod, "sceHttpGetResponseContentLength");
  g_psx_http.sceHttpGetLastErrno =
      (psx_sceHttpGetLastErrno_fn)dlsym(g_psx_http.http_mod,
                                        "sceHttpGetLastErrno");
  g_psx_http.sceHttpsSetSslCallback =
      (psx_sceHttpsSetSslCallback_fn)dlsym(g_psx_http.http_mod,
                                           "sceHttpsSetSslCallback");

  if (g_psx_http.sceNetInit == NULL ||
      g_psx_http.sceNetPoolCreate == NULL ||
      g_psx_http.sceSslInit == NULL ||
      g_psx_http.sceHttpInit == NULL ||
      g_psx_http.sceHttpCreateTemplate == NULL ||
      g_psx_http.sceHttpDeleteTemplate == NULL ||
      g_psx_http.sceHttpCreateConnectionWithURL == NULL ||
      g_psx_http.sceHttpDeleteConnection == NULL ||
      g_psx_http.sceHttpCreateRequestWithURL2 == NULL ||
      g_psx_http.sceHttpDeleteRequest == NULL ||
      g_psx_http.sceHttpSendRequest == NULL ||
      g_psx_http.sceHttpReadData == NULL ||
      g_psx_http.sceHttpGetStatusCode == NULL) {
    if (err && err_size > 0U) {
      snprintf(err, err_size, "SceHttp symbol resolution failed");
    }
    return -1;
  }

  g_psx_http.loaded = 1;
  return 0;
}

static int psx_https_accept_any(int libssl_ctx_id, unsigned int verify_err,
                                void *const ssl_cert[], int cert_num,
                                void *user_arg) {
  (void)libssl_ctx_id;
  (void)verify_err;
  (void)ssl_cert;
  (void)cert_num;
  (void)user_arg;
  return 0;
}

static int psx_http_global_init(char *err, size_t err_size) {
  int result = 0;
  pthread_mutex_lock(&g_psx_http_lock);

  if (g_psx_http_ctx >= 0) {
    pthread_mutex_unlock(&g_psx_http_lock);
    return 0;
  }

  if (psx_http_load_symbols(err, err_size) != 0) {
    pthread_mutex_unlock(&g_psx_http_lock);
    return -1;
  }

  (void)g_psx_http.sceNetInit();

  g_psx_net_pool = g_psx_http.sceNetPoolCreate("zftpd_http", 262144, 0);
  if (g_psx_net_pool < 0) {
    if (err && err_size > 0U) {
      snprintf(err, err_size, "sceNetPoolCreate failed: 0x%08x",
               (unsigned)g_psx_net_pool);
    }
    result = -1;
  }

  if (result == 0) {
    g_psx_ssl_ctx = g_psx_http.sceSslInit(524288U);
    if (g_psx_ssl_ctx < 0) {
      if (err && err_size > 0U) {
        snprintf(err, err_size, "sceSslInit failed: 0x%08x",
                 (unsigned)g_psx_ssl_ctx);
      }
      result = -1;
    }
  }

  if (result == 0) {
    g_psx_http_ctx = g_psx_http.sceHttpInit(g_psx_net_pool, g_psx_ssl_ctx,
                                            524288U);
    if (g_psx_http_ctx < 0) {
      if (err && err_size > 0U) {
        snprintf(err, err_size, "sceHttpInit failed: 0x%08x",
                 (unsigned)g_psx_http_ctx);
      }
      result = -1;
    }
  }

  pthread_mutex_unlock(&g_psx_http_lock);
  return result;
}

static int dl_psx_http_download_to_fd(dl_entry_t *dl, int fd) {
  char init_err[128];
  if (psx_http_global_init(init_err, sizeof(init_err)) != 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg), "%s", init_err);
    return -1;
  }

  int tmpl = -1;
  int conn = -1;
  int req = -1;
  int rc = -1;
  uint8_t *buf = (uint8_t *)malloc(DL_READ_BUF);
  if (buf == NULL) {
    snprintf(dl->error_msg, sizeof(dl->error_msg), "Out of memory");
    return -1;
  }

  tmpl = g_psx_http.sceHttpCreateTemplate(g_psx_http_ctx, "zftpd-ps/2.0",
                                          2, 1);
  if (tmpl < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg),
             "sceHttpCreateTemplate failed: 0x%08x", (unsigned)tmpl);
    goto done;
  }

  if (g_psx_http.sceHttpSetAutoRedirect != NULL) {
    (void)g_psx_http.sceHttpSetAutoRedirect(tmpl, 1);
  }
  if (g_psx_http.sceHttpSetConnectTimeOut != NULL) {
    (void)g_psx_http.sceHttpSetConnectTimeOut(tmpl, 30000000U);
  }
  if (g_psx_http.sceHttpSetRecvTimeOut != NULL) {
    (void)g_psx_http.sceHttpSetRecvTimeOut(tmpl, 60000000U);
  }
  if (g_psx_http.sceHttpSetSendTimeOut != NULL) {
    (void)g_psx_http.sceHttpSetSendTimeOut(tmpl, 30000000U);
  }
  if (g_psx_http.sceHttpSetResolveTimeOut != NULL) {
    (void)g_psx_http.sceHttpSetResolveTimeOut(tmpl, 30000000U);
  }
  if (g_psx_http.sceHttpSetResolveRetry != NULL) {
    (void)g_psx_http.sceHttpSetResolveRetry(tmpl, 2);
  }
  if (g_psx_http.sceHttpSetResponseHeaderMaxSize != NULL) {
    (void)g_psx_http.sceHttpSetResponseHeaderMaxSize(tmpl, 65536U);
  }
  if (g_psx_http.sceHttpsSetSslCallback != NULL &&
      dl_has_scheme(dl->url, "https://")) {
    (void)g_psx_http.sceHttpsSetSslCallback(tmpl, psx_https_accept_any, NULL);
  }

  conn = g_psx_http.sceHttpCreateConnectionWithURL(tmpl, dl->url, 0);
  if (conn < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg),
             "sceHttpCreateConnectionWithURL failed: 0x%08x", (unsigned)conn);
    goto done;
  }
  if (g_psx_http.sceHttpsSetSslCallback != NULL &&
      dl_has_scheme(dl->url, "https://")) {
    (void)g_psx_http.sceHttpsSetSslCallback(conn, psx_https_accept_any, NULL);
  }

  req = g_psx_http.sceHttpCreateRequestWithURL2(conn, "GET", dl->url, 0U);
  if (req < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg),
             "sceHttpCreateRequestWithURL2 failed: 0x%08x", (unsigned)req);
    goto done;
  }

  if (g_psx_http.sceHttpSetAutoRedirect != NULL) {
    (void)g_psx_http.sceHttpSetAutoRedirect(req, 1);
  }
  if (g_psx_http.sceHttpSetInflateGZIPEnabled != NULL) {
    (void)g_psx_http.sceHttpSetInflateGZIPEnabled(req, 0);
  }
  if (g_psx_http.sceHttpSetResponseHeaderMaxSize != NULL) {
    (void)g_psx_http.sceHttpSetResponseHeaderMaxSize(req, 65536U);
  }
  if (g_psx_http.sceHttpsSetSslCallback != NULL &&
      dl_has_scheme(dl->url, "https://")) {
    (void)g_psx_http.sceHttpsSetSslCallback(req, psx_https_accept_any, NULL);
  }
  if (g_psx_http.sceHttpAddRequestHeader != NULL) {
    (void)g_psx_http.sceHttpAddRequestHeader(req, "Accept", "*/*", 0U);
    (void)g_psx_http.sceHttpAddRequestHeader(req, "Connection", "close", 0U);
  }

  rc = g_psx_http.sceHttpSendRequest(req, NULL, 0U);
  if (rc < 0) {
    int last_errno = 0;
    if (g_psx_http.sceHttpGetLastErrno != NULL &&
        g_psx_http.sceHttpGetLastErrno(req, &last_errno) == 0 &&
        last_errno != 0) {
      snprintf(dl->error_msg, sizeof(dl->error_msg),
               "sceHttpSendRequest failed: 0x%08x (last errno: 0x%08x)",
               (unsigned)rc, (unsigned)last_errno);
    } else {
      snprintf(dl->error_msg, sizeof(dl->error_msg),
               "sceHttpSendRequest failed: 0x%08x", (unsigned)rc);
    }
    goto done;
  }

  int status_code = 0;
  rc = g_psx_http.sceHttpGetStatusCode(req, &status_code);
  if (rc < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg),
             "sceHttpGetStatusCode failed: 0x%08x", (unsigned)rc);
    goto done;
  }
  if (status_code >= 400) {
    snprintf(dl->error_msg, sizeof(dl->error_msg),
             "HTTP error status: %d", status_code);
    rc = -1;
    goto done;
  }

  if (g_psx_http.sceHttpGetResponseContentLength != NULL) {
    int length_type = 0;
    uint64_t content_len = 0U;
    if (g_psx_http.sceHttpGetResponseContentLength(req, &length_type,
                                                   &content_len) == 0 &&
        content_len > 0U) {
      dl->total_size = content_len;
    }
  }

  for (;;) {
    while (dl->paused && dl->active && !dl->done) {
      usleep(100000U);
    }
    if (!dl->active || (dl->done && dl->error)) {
      snprintf(dl->error_msg, sizeof(dl->error_msg), "Cancelled by user");
      rc = -1;
      goto done;
    }

    int got = g_psx_http.sceHttpReadData(req, buf, (size_t)DL_READ_BUF);
    if (got < 0) {
      snprintf(dl->error_msg, sizeof(dl->error_msg),
               "sceHttpReadData failed: 0x%08x", (unsigned)got);
      rc = -1;
      goto done;
    }
    if (got == 0) {
      break;
    }
    if (dl_write_all(fd, buf, (size_t)got) != 0) {
      snprintf(dl->error_msg, sizeof(dl->error_msg),
               "Write error: %s", strerror(errno));
      rc = -1;
      goto done;
    }
    dl->downloaded += (uint64_t)got;
  }

  rc = 0;

done:
  if (req >= 0) {
    (void)g_psx_http.sceHttpDeleteRequest(req);
  }
  if (conn >= 0) {
    (void)g_psx_http.sceHttpDeleteConnection(conn);
  }
  if (tmpl >= 0) {
    (void)g_psx_http.sceHttpDeleteTemplate(tmpl);
  }
  free(buf);
  return rc;
}

#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

#if !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5)
static size_t dl_curl_write(void *ptr, size_t size, size_t nmemb, void *userdata) {
  struct dl_write_ctx *ctx = (struct dl_write_ctx *)userdata;
  size_t total = size * nmemb;
  while (ctx->dl->paused && ctx->dl->active && !ctx->dl->done) {
    usleep(100000U);
  }
  if (!ctx->dl->active || (ctx->dl->done && ctx->dl->error)) {
    return 0;
  }
  if (dl_write_all(ctx->fd, ptr, total) != 0) {
    return 0;
  }
  ctx->dl->downloaded += (uint64_t)total;
  return total;
}
#endif

static void *dl_thread(void *arg) {
  dl_entry_t *dl = (dl_entry_t *)arg;
  char filepath[2048];
  if (strcmp(dl->dst_path, "/") == 0) {
    snprintf(filepath, sizeof(filepath), "/%s", dl->filename);
  } else {
    snprintf(filepath, sizeof(filepath), "%s/%s", dl->dst_path, dl->filename);
  }

  int fd = open(filepath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    snprintf(dl->error_msg, sizeof(dl->error_msg), "Cannot create %s: %s",
             filepath, strerror(errno));
    dl->error = 1;
    dl->done = 1;
    dl->active = 0;
    return NULL;
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (dl_psx_http_download_to_fd(dl, fd) != 0) {
    dl->error = 1;
  }
  {
    double elapsed = difftime(time(NULL), dl->start_time);
    if (elapsed > 0.0) {
      dl->speed = (double)dl->downloaded / elapsed;
    }
  }
  close(fd);
  if (dl->error) {
    (void)unlink(filepath);
  }
  dl->done = 1;
  dl->active = 0;
  return NULL;
#else
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
  if (dl->error) {
    (void)unlink(filepath);
  }
  dl->done = 1;
  dl->active = 0;
  return NULL;
#endif
}
#endif /* DL_HAS_BACKEND */

static http_response_t *api_dl_start(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  /* Parse JSON body: {"url":"...","dst":"..."} — simple extraction */
  const char *body_data = request->body;
  size_t body_len = request->body_length;
  char url[DL_URL_MAX] = "";
  char dst[1024] = "";

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

  char unsupported_reason[256];
  if (!dl_url_is_supported(url, unsupported_reason, sizeof(unsupported_reason))) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, unsupported_reason);
  }

  char safe_dst[FTP_PATH_MAX];
  if (!dl_normalize_destination(dst, safe_dst, sizeof(safe_dst))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN,
                      "Invalid or read-only destination path");
  }

  dl_entry_t *dl = dl_find_slot();
  if (!dl) {
    return error_json(HTTP_STATUS_409_CONFLICT, "Max concurrent downloads reached");
  }

  memset(dl, 0, sizeof(dl_entry_t));
  dl->id = g_dl_next_id++;
  strncpy(dl->url, url, sizeof(dl->url) - 1);
  dl->url[sizeof(dl->url) - 1] = '\0';
  strncpy(dl->dst_path, safe_dst, sizeof(dl->dst_path) - 1);
  dl->dst_path[sizeof(dl->dst_path) - 1] = '\0';
  dl_extract_filename(url, dl->filename, sizeof(dl->filename));
  dl->start_time = time(NULL);
  dl->active = 1;

#if DL_HAS_BACKEND
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
  /* Without a download backend, mark as error immediately */
  snprintf(dl->error_msg, sizeof(dl->error_msg),
           "Download not available in this build");
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
    } else if (dl->done && !dl->error) {
      progress = 100;
    }

    char esc_name[512];
    char esc_url[512];
    char esc_error[512];
    size_t esc_pos = 0U;
    esc_name[0] = '\0';
    esc_url[0] = '\0';
    esc_error[0] = '\0';
    (void)json_escape_append(esc_name, sizeof(esc_name), &esc_pos,
                             dl->filename);
    esc_name[(esc_pos < sizeof(esc_name)) ? esc_pos : sizeof(esc_name) - 1U] =
        '\0';
    esc_pos = 0U;
    (void)json_escape_append(esc_url, sizeof(esc_url), &esc_pos, dl->url);
    esc_url[(esc_pos < sizeof(esc_url)) ? esc_pos : sizeof(esc_url) - 1U] =
        '\0';
    esc_pos = 0U;
    (void)json_escape_append(esc_error, sizeof(esc_error), &esc_pos,
                             dl->error ? dl->error_msg : "");
    esc_error[(esc_pos < sizeof(esc_error)) ? esc_pos
                                            : sizeof(esc_error) - 1U] = '\0';

    pos += snprintf(body + pos, sizeof(body) - (size_t)pos,
        "{\"id\":%d,\"name\":\"%s\",\"url\":\"%s\","
        "\"progress\":%d,\"downloaded\":%" PRIu64 ",\"total_size\":%" PRIu64 ","
        "\"speed\":%.0f,\"done\":%s,\"error\":\"%s\",\"paused\":%s}",
        dl->id, esc_name, esc_url,
        progress, (uint64_t)dl->downloaded, (uint64_t)dl->total_size,
        dl->speed, dl->done ? "true" : "false",
        esc_error,
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
 * serve_static — serve files from embedded http_resources.c (compiled-in).
 *
 * All web assets are embedded directly in the binary.  The frontend patch
 * (disabling legacy Stream UI and blocking analytics) and the CSRF token
 * are injected at request time for index.html.
 *
 * Path traversal is prevented by rejecting any URI containing "..".
 *---------------------------------------------------------------------------*/
static http_response_t *serve_static(const http_request_t *request) {
  static const char *k_frontend_patch =
      "<style>.nav-tab[data-view=\"stream\"],#view-stream{display:none !important;}</style>"
      "<script>(function(){function __zftpd_fix(){var b=document.body;if(!b)return;"
      "var h=document.querySelector('header.topbar');if(h){for(var n=b.firstChild;n&&n!==h;){var nx=n.nextSibling;"
      "if(n.nodeType===3){b.removeChild(n);}n=nx;}}"
      "var t=document.querySelector('.nav-tab[data-view=\\\"stream\\\"]');if(t&&t.parentNode)t.parentNode.removeChild(t);"
      "var v=document.getElementById('view-stream');if(v&&v.parentNode)v.parentNode.removeChild(v);}"
      "function __zftpd_is_blocked_url(u){return /google-analytics\\.com\\/mp\\/collect/i.test(String(u||''));}"
      "function __zftpd_patch_net(){"
      "var sb=navigator.sendBeacon;if(sb){navigator.sendBeacon=function(u,d){if(__zftpd_is_blocked_url(u))return true;return sb.apply(this,arguments);};}"
      "var of=window.fetch;if(of){window.fetch=function(input,init){var u=(typeof input==='string')?input:(input&&input.url?input.url:'');"
      "if(__zftpd_is_blocked_url(u)){return new Promise(function(resolve){resolve({ok:true,status:204,text:function(){return Promise.resolve('');},json:function(){return Promise.resolve({});}});});}"
      "return of.apply(this,arguments);};}"
      "var xo=XMLHttpRequest&&XMLHttpRequest.prototype&&XMLHttpRequest.prototype.open;"
      "var xs=XMLHttpRequest&&XMLHttpRequest.prototype&&XMLHttpRequest.prototype.send;"
      "if(xo&&xs){XMLHttpRequest.prototype.open=function(m,u){this.__zftpd_block=__zftpd_is_blocked_url(u);return xo.apply(this,arguments);};"
      "XMLHttpRequest.prototype.send=function(){if(this.__zftpd_block){try{this.readyState=4;this.status=204;if(this.onreadystatechange)this.onreadystatechange();if(this.onload)this.onload();}catch(e){}return;}return xs.apply(this,arguments);};}"
      "}"
      "__zftpd_patch_net();"
      "if(document.readyState==='loading'){document.addEventListener('DOMContentLoaded',__zftpd_fix);}else{__zftpd_fix();}})();</script>";

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

  /* Look up embedded resource */
  const http_resource_t *resource = NULL;
  if (!http_resource_get(path, &resource) || resource == NULL) {
    http_response_t *resp = http_response_create(HTTP_STATUS_404_NOT_FOUND);
    const char *msg = "404 Not Found";
    http_response_set_body(resp, msg, strlen(msg));
    return resp;
  }

  const unsigned char *raw_data = resource->data;
  size_t raw_size = resource->size;
  int is_html = (strstr(path, "index.html") != NULL);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type",
                           is_html ? "text/html; charset=utf-8" : resource->content_type);

  if (!is_html) {
    if (http_response_set_body(resp, (const char *)raw_data, raw_size) != 0) {
      (void)http_response_set_body_ref(resp, (const char *)raw_data, raw_size);
    }
    return resp;
  }

  /* ── index.html: inject frontend patch + optional CSRF token ── */
  {
    size_t patch_len = strlen(k_frontend_patch);
    size_t buf_size = raw_size + patch_len + 256U;
    char *buf = (char *)malloc(buf_size);
    if (buf == NULL) {
      http_response_destroy(resp);
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }

    memcpy(buf, (const char *)raw_data, raw_size);
    size_t out_len = raw_size;

    /* Inject frontend patch before </head> */
    {
      char *insert_at = strstr(buf, "</head>");
      if (insert_at != NULL) {
        size_t prefix_len = (size_t)(insert_at - buf);
        size_t suffix_len = out_len - prefix_len;
        memmove(insert_at + patch_len, insert_at, suffix_len);
        memcpy(insert_at, k_frontend_patch, patch_len);
        out_len += patch_len;
      }
    }

#if ENABLE_WEB_UPLOAD
    {
      const char *token = http_csrf_get_token();
      char meta_tag[128];
      snprintf(meta_tag, sizeof(meta_tag),
               "<meta name=\"csrf-token\" content=\"%s\">", token);

      const char *placeholder = "<!-- CSRF_TOKEN -->";
      char *found = strstr(buf, placeholder);
      if (found != NULL) {
        size_t prefix_len = (size_t)(found - buf);
        size_t placelen = strlen(placeholder);
        size_t taglen = strlen(meta_tag);
        size_t suffix_len = out_len - prefix_len - placelen;

        if (taglen <= placelen) {
          memcpy(found, meta_tag, taglen);
          if (taglen < placelen) {
            memmove(found + taglen, found + placelen, suffix_len);
          }
          out_len = out_len - placelen + taglen;
        } else {
          size_t extra = taglen - placelen;
          if (buf_size < out_len + extra + 1U) {
            size_t new_size = out_len + extra + 256U;
            char *tmp = (char *)realloc(buf, new_size);
            if (tmp == NULL) {
              free(buf);
              http_response_destroy(resp);
              return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
            }
            buf = tmp;
            buf_size = new_size;
            found = buf + prefix_len;
          }
          memmove(found + taglen, found + placelen, suffix_len);
          memcpy(found, meta_tag, taglen);
          out_len = out_len - placelen + taglen;
        }
      }
    }
#endif

    if (http_response_set_body_owned(resp, buf, out_len) == 0) {
      return resp;
    }
    free(buf);
    http_response_destroy(resp);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Body allocation failed");
  }
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

static http_response_t *status_json_200(int ok, const char *message,
                                        int code) {
  if (message == NULL) {
    message = ok ? "ok" : "error";
  }
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  char body[512];
  int n = snprintf(body, sizeof(body),
                   "{\"ok\":%s,\"status\":\"%s\",\"message\":\"%s\",\"code\":%d}",
                   ok ? "true" : "false", ok ? "ok" : "error", message,
                   code);
  http_response_set_body(resp, body, (size_t)n);
  return resp;
}

static http_response_t *png_fallback_response(void) {
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

static http_response_t *api_legacy_disabled_json(const char *json_body) {
  if (json_body == NULL) {
    json_body = "{\"ok\":false}";
  }
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, json_body, strlen(json_body));
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

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (!resp) return NULL;
  http_response_add_header(resp, "Content-Type", "application/json");
  
  char body[128];
  int blen = snprintf(body, sizeof(body), "{\"status\":\"ok\",\"threshold\":%d}", threshold);
  http_response_set_body(resp, body, (size_t)blen);
  return resp;
}

/*===========================================================================*
 * GET /api/admin/launch?id=... or /api/admin/launch?path=...
 *===========================================================================*/
static http_response_t *api_admin_launch(const http_request_t *request) {
  char title_id[64] = {0};

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  const char *id_param = strstr(query, "id=");
  if (id_param != NULL) {
    id_param += 3; /* skip "id=" */
    size_t id_len = 0U;
    while ((id_param[id_len] != '\0') && (id_param[id_len] != '&')) {
      if (id_len >= (sizeof(title_id) - 1U)) {
        return error_json(HTTP_STATUS_400_BAD_REQUEST,
                          "'id' parameter too long");
      }
      title_id[id_len] = id_param[id_len];
      id_len++;
    }
    title_id[id_len] = '\0';
  } else {
    char path[1024] = "";
    if (parse_path_param(query, path, sizeof(path)) != 0) {
      return error_json(HTTP_STATUS_400_BAD_REQUEST,
                        "Missing 'id' or valid 'path' parameter");
    }

    char safe[FTP_PATH_MAX];
    if (!validate_path(path, safe, sizeof(safe))) {
      return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
    }

    if (extract_title_id_from_game_image(safe, title_id, sizeof(title_id)) != 0) {
      if (extract_title_id_from_app_dir(safe, title_id, sizeof(title_id)) != 0) {
        return error_json(HTTP_STATUS_400_BAD_REQUEST,
                          "Unable to resolve TITLE_ID from image/app path");
      }
    }
  }

  if (title_id[0] == '\0') {
    launch_diag_log("input", title_id, -1, "missing launch target");
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or invalid launch target");
  }

  /* sanitize title id for runtime launch API */
  for (size_t i = 0; title_id[i] != '\0'; i++) {
    unsigned char c = (unsigned char)title_id[i];
    if (!(isalnum(c) || c == '_' || c == '-')) {
      launch_diag_log("input", title_id, -2, "invalid title id format");
      return error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid title id format");
    }
    title_id[i] = (char)toupper(c);
  }
  launch_diag_log("input", title_id, 0, "launch request received");

  char installed_app_dir[FTP_PATH_MAX];
  if (resolve_installed_app_dir_by_title(title_id, installed_app_dir,
                                         sizeof(installed_app_dir)) != 0) {
    launch_diag_log("preflight_fs", title_id, -30,
                    "title id not found in installed app directories");
    return status_json_200(0,
                           "Launch blocked: title not installed on this console",
                           -30);
  }
  launch_diag_log("preflight_fs", title_id, 0, installed_app_dir);

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    /*
     * We dynamically load libSceLncUtil.sprx, libSceUserService.sprx,
     * and libSceSystemService.sprx. Since this is a payload, these are
     * not statically linked and must be resolved at runtime using POSIX dlopen.
     */
    void *userService = NULL;
    void *lncUtil = NULL;
    void *sysService = NULL;

    int mrc = 0;
    if (psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_SYS_CORE, &mrc) ==
        0) {
      launch_diag_log("sysmodule", title_id, mrc, "SYS_CORE");
    } else {
      launch_diag_log("sysmodule", title_id, mrc,
                      "SYS_CORE loader unavailable");
    }
    if (psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_SYSTEM_SERVICE,
                                    &mrc) == 0) {
      launch_diag_log("sysmodule", title_id, mrc, "SYSTEM_SERVICE");
    }
    if (psx_sysmodule_load_internal(SCE_SYSMODULE_INTERNAL_USER_SERVICE,
                                    &mrc) == 0) {
      launch_diag_log("sysmodule", title_id, mrc, "USER_SERVICE");
    }

    int (*f_sceUserServiceGetForegroundUser)(int32_t *) =
        (int (*)(int32_t *))dlsym(RTLD_DEFAULT,
                                   "sceUserServiceGetForegroundUser");

    int (*f_sceUserServiceInitialize)(void *) =
      (int (*)(void *))dlsym(RTLD_DEFAULT, "sceUserServiceInitialize");

    int (*f_sceUserServiceGetLoginUserIdList)(void *) =
      (int (*)(void *))dlsym(RTLD_DEFAULT,
                             "sceUserServiceGetLoginUserIdList");

    int (*f_sceUserServiceGetInitialUser)(int32_t *) =
      (int (*)(int32_t *))dlsym(RTLD_DEFAULT, "sceUserServiceGetInitialUser");

    uint32_t (*f_sceLncUtilLaunchApp)(const char *, const char **,
                                      LncAppParam *) =
        (uint32_t(*)(const char *, const char **,
                     LncAppParam *))dlsym(RTLD_DEFAULT,
                                          "sceLncUtilLaunchApp");

    int (*f_sceSystemServiceLaunchApp)(const char *, const char **,
                       void *) =
      (int (*)(const char *, const char **,
           void *))dlsym(RTLD_DEFAULT,
                  "sceSystemServiceLaunchApp");

    int (*f_sceSystemServiceLoadExec)(const char *, const char **) =
      (int (*)(const char *, const char **))dlsym(RTLD_DEFAULT,
                  "sceSystemServiceLoadExec");

    int (*f_sceLncUtilInitialize)(void) =
        (int (*)(void))dlsym(RTLD_DEFAULT, "sceLncUtilInitialize");

    int (*f_sceLncUtilGetAppId)(const char *) =
      (int (*)(const char *))dlsym(RTLD_DEFAULT, "sceLncUtilGetAppId");

#if defined(PLATFORM_PS4)
    if (f_sceUserServiceGetForegroundUser == NULL &&
        sceUserServiceGetForegroundUser != NULL) {
      f_sceUserServiceGetForegroundUser = sceUserServiceGetForegroundUser;
      launch_diag_log("ps4_import", title_id, 0,
                      "sceUserServiceGetForegroundUser");
    }
    if (f_sceUserServiceInitialize == NULL &&
        sceUserServiceInitialize != NULL) {
      f_sceUserServiceInitialize = sceUserServiceInitialize;
      launch_diag_log("ps4_import", title_id, 0, "sceUserServiceInitialize");
    }
    if (f_sceUserServiceGetLoginUserIdList == NULL &&
        sceUserServiceGetLoginUserIdList != NULL) {
      f_sceUserServiceGetLoginUserIdList = sceUserServiceGetLoginUserIdList;
      launch_diag_log("ps4_import", title_id, 0,
                      "sceUserServiceGetLoginUserIdList");
    }
    if (f_sceUserServiceGetInitialUser == NULL &&
        sceUserServiceGetInitialUser != NULL) {
      f_sceUserServiceGetInitialUser = sceUserServiceGetInitialUser;
      launch_diag_log("ps4_import", title_id, 0, "sceUserServiceGetInitialUser");
    }
    if (f_sceLncUtilLaunchApp == NULL && sceLncUtilLaunchApp != NULL) {
      f_sceLncUtilLaunchApp = sceLncUtilLaunchApp;
      launch_diag_log("ps4_import", title_id, 0, "sceLncUtilLaunchApp");
    }
    if (f_sceSystemServiceLaunchApp == NULL &&
        sceSystemServiceLaunchApp != NULL) {
      f_sceSystemServiceLaunchApp = sceSystemServiceLaunchApp;
      launch_diag_log("ps4_import", title_id, 0, "sceSystemServiceLaunchApp");
    }
    if (f_sceSystemServiceLoadExec == NULL &&
        sceSystemServiceLoadExec != NULL) {
      f_sceSystemServiceLoadExec = sceSystemServiceLoadExec;
      launch_diag_log("ps4_import", title_id, 0, "sceSystemServiceLoadExec");
    }
    if (f_sceLncUtilInitialize == NULL && sceLncUtilInitialize != NULL) {
      f_sceLncUtilInitialize = sceLncUtilInitialize;
      launch_diag_log("ps4_import", title_id, 0, "sceLncUtilInitialize");
    }
    if (f_sceLncUtilGetAppId == NULL && sceLncUtilGetAppId != NULL) {
      f_sceLncUtilGetAppId = sceLncUtilGetAppId;
      launch_diag_log("ps4_import", title_id, 0, "sceLncUtilGetAppId");
    }
#endif

    if (f_sceLncUtilLaunchApp == NULL || f_sceLncUtilInitialize == NULL) {
      lncUtil = dlopen("/system/common/lib/libSceLncUtil.sprx",
                       RTLD_NOW | RTLD_GLOBAL);
      if (lncUtil != NULL) {
        launch_diag_log("dlopen", title_id, 0, "libSceLncUtil.sprx");
      } else {
        const char *err = dlerror();
        launch_diag_log("dlopen", title_id, -10,
                        (err != NULL) ? err : "libSceLncUtil.sprx failed");
      }
      if (lncUtil != NULL) {
        if (f_sceLncUtilLaunchApp == NULL) {
          f_sceLncUtilLaunchApp =
              (uint32_t(*)(const char *, const char **,
                           LncAppParam *))dlsym(lncUtil,
                                                "sceLncUtilLaunchApp");
        }
        if (f_sceLncUtilInitialize == NULL) {
          f_sceLncUtilInitialize =
              (int (*)(void))dlsym(lncUtil, "sceLncUtilInitialize");
        }
        if (f_sceLncUtilGetAppId == NULL) {
          f_sceLncUtilGetAppId =
              (int (*)(const char *))dlsym(lncUtil, "sceLncUtilGetAppId");
        }
      }
    }

    if ((f_sceSystemServiceLaunchApp == NULL) ||
        (f_sceSystemServiceLoadExec == NULL) ||
        (f_sceLncUtilLaunchApp == NULL) ||
        (f_sceLncUtilInitialize == NULL) ||
        (f_sceLncUtilGetAppId == NULL)) {
      sysService = dlopen("/system/common/lib/libSceSystemService.sprx",
                          RTLD_NOW | RTLD_GLOBAL);
      if (sysService != NULL) {
        launch_diag_log("dlopen", title_id, 0, "libSceSystemService.sprx");
        if (f_sceSystemServiceLaunchApp == NULL) {
          f_sceSystemServiceLaunchApp =
              (int (*)(const char *, const char **,
                       void *))dlsym(sysService,
                                    "sceSystemServiceLaunchApp");
        }
        if (f_sceSystemServiceLoadExec == NULL) {
          f_sceSystemServiceLoadExec =
            (int (*)(const char *, const char **))dlsym(
              sysService, "sceSystemServiceLoadExec");
        }
        /*
         * On PS4 the LncUtil entrypoints are commonly exported by
         * libSceSystemService rather than a standalone libSceLncUtil.sprx.
         * Itemzflow links these stubs directly; dynamic payloads need to
         * resolve them from the loaded module.
         */
        if (f_sceLncUtilLaunchApp == NULL) {
          f_sceLncUtilLaunchApp =
              (uint32_t(*)(const char *, const char **,
                           LncAppParam *))dlsym(sysService,
                                                "sceLncUtilLaunchApp");
        }
        if (f_sceLncUtilInitialize == NULL) {
          f_sceLncUtilInitialize =
              (int (*)(void))dlsym(sysService, "sceLncUtilInitialize");
        }
        if (f_sceLncUtilGetAppId == NULL) {
          f_sceLncUtilGetAppId =
              (int (*)(const char *))dlsym(sysService, "sceLncUtilGetAppId");
        }
      } else {
        const char *err = dlerror();
        launch_diag_log("dlopen", title_id, -11,
                        (err != NULL) ? err : "libSceSystemService.sprx failed");
      }
    }

#if defined(PLATFORM_PS4)
    if ((f_sceSystemServiceLaunchApp == NULL) ||
        (f_sceSystemServiceLoadExec == NULL) ||
        (f_sceLncUtilLaunchApp == NULL) ||
        (f_sceLncUtilInitialize == NULL) ||
        (f_sceLncUtilGetAppId == NULL)) {
      const char *sys_path = "/system/common/lib/libSceSystemService.sprx";
      void *sym = NULL;
      int ps4_rc = 0;

      if (f_sceSystemServiceLaunchApp == NULL) {
        sym = NULL;
        ps4_rc =
            ps4_load_prx_symbol(sys_path, "sceSystemServiceLaunchApp", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceSystemServiceLaunchApp");
        if (sym != NULL) {
          f_sceSystemServiceLaunchApp =
              (int (*)(const char *, const char **, void *))sym;
        }
      }
      if (f_sceSystemServiceLoadExec == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(sys_path, "sceSystemServiceLoadExec",
                                     &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceSystemServiceLoadExec");
        if (sym != NULL) {
          f_sceSystemServiceLoadExec =
              (int (*)(const char *, const char **))sym;
        }
      }
      if (f_sceLncUtilLaunchApp == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(sys_path, "sceLncUtilLaunchApp", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceLncUtilLaunchApp");
        if (sym != NULL) {
          f_sceLncUtilLaunchApp =
              (uint32_t(*)(const char *, const char **, LncAppParam *))sym;
        }
      }
      if (f_sceLncUtilInitialize == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(sys_path, "sceLncUtilInitialize", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceLncUtilInitialize");
        if (sym != NULL) {
          f_sceLncUtilInitialize = (int (*)(void))sym;
        }
      }
      if (f_sceLncUtilGetAppId == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(sys_path, "sceLncUtilGetAppId", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceLncUtilGetAppId");
        if (sym != NULL) {
          f_sceLncUtilGetAppId = (int (*)(const char *))sym;
        }
      }
    }
#endif

    if ((f_sceUserServiceGetForegroundUser == NULL) ||
        (f_sceUserServiceInitialize == NULL) ||
        (f_sceUserServiceGetLoginUserIdList == NULL) ||
        (f_sceUserServiceGetInitialUser == NULL)) {
      userService = dlopen("/system/common/lib/libSceUserService.sprx",
                           RTLD_NOW | RTLD_GLOBAL);
      launch_diag_log("dlopen", title_id, (userService != NULL) ? 0 : -12,
                      "libSceUserService.sprx");
      if (userService != NULL) {
        if (f_sceUserServiceGetForegroundUser == NULL) {
          f_sceUserServiceGetForegroundUser =
              (int (*)(int32_t *))dlsym(userService,
                                        "sceUserServiceGetForegroundUser");
        }
        if (f_sceUserServiceInitialize == NULL) {
          f_sceUserServiceInitialize =
              (int (*)(void *))dlsym(userService,
                                     "sceUserServiceInitialize");
        }
        if (f_sceUserServiceGetLoginUserIdList == NULL) {
          f_sceUserServiceGetLoginUserIdList =
              (int (*)(void *))dlsym(userService,
                                     "sceUserServiceGetLoginUserIdList");
        }
        if (f_sceUserServiceGetInitialUser == NULL) {
          f_sceUserServiceGetInitialUser =
              (int (*)(int32_t *))dlsym(userService,
                                        "sceUserServiceGetInitialUser");
        }
      }
    }

#if defined(PLATFORM_PS4)
    if ((f_sceUserServiceGetForegroundUser == NULL) ||
        (f_sceUserServiceInitialize == NULL) ||
        (f_sceUserServiceGetLoginUserIdList == NULL) ||
        (f_sceUserServiceGetInitialUser == NULL)) {
      const char *user_path = "/system/common/lib/libSceUserService.sprx";
      void *sym = NULL;
      int ps4_rc = 0;

      if (f_sceUserServiceGetForegroundUser == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(user_path,
                                     "sceUserServiceGetForegroundUser", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceUserServiceGetForegroundUser");
        if (sym != NULL) {
          f_sceUserServiceGetForegroundUser = (int (*)(int32_t *))sym;
        }
      }
      if (f_sceUserServiceInitialize == NULL) {
        sym = NULL;
        ps4_rc =
            ps4_load_prx_symbol(user_path, "sceUserServiceInitialize", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceUserServiceInitialize");
        if (sym != NULL) {
          f_sceUserServiceInitialize = (int (*)(void *))sym;
        }
      }
      if (f_sceUserServiceGetLoginUserIdList == NULL) {
        sym = NULL;
        ps4_rc = ps4_load_prx_symbol(user_path,
                                     "sceUserServiceGetLoginUserIdList", &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceUserServiceGetLoginUserIdList");
        if (sym != NULL) {
          f_sceUserServiceGetLoginUserIdList = (int (*)(void *))sym;
        }
      }
      if (f_sceUserServiceGetInitialUser == NULL) {
        sym = NULL;
        ps4_rc =
            ps4_load_prx_symbol(user_path, "sceUserServiceGetInitialUser",
                                &sym);
        launch_diag_log("ps4_prx_dlsym", title_id, ps4_rc,
                        "sceUserServiceGetInitialUser");
        if (sym != NULL) {
          f_sceUserServiceGetInitialUser = (int (*)(int32_t *))sym;
        }
      }
    }
#endif

    if ((f_sceLncUtilLaunchApp == NULL) &&
        (f_sceSystemServiceLaunchApp == NULL)) {
      launch_diag_log("symbols", title_id, -3,
                      "missing sceLncUtilLaunchApp and sceSystemServiceLaunchApp");
      if (userService != NULL)
        dlclose(userService);
      if (lncUtil != NULL)
        dlclose(lncUtil);
      if (sysService != NULL)
        dlclose(sysService);
      return status_json_200(
          0,
          "Launch API unavailable (sceLncUtilLaunchApp/sceSystemServiceLaunchApp)",
          -3);
    }

#if defined(PLATFORM_PS4)
    if (f_sceLncUtilLaunchApp == NULL) {
      launch_diag_log("symbols", title_id, -4,
                      "PS4 requires sceLncUtilLaunchApp; SystemService fallback disabled");
      if (userService != NULL)
        dlclose(userService);
      if (lncUtil != NULL)
        dlclose(lncUtil);
      if (sysService != NULL)
        dlclose(sysService);
      return status_json_200(
          0,
          "Launch blocked: PS4 LncUtil unavailable (SystemService fallback can crash ShellUI)",
          -4);
    }
#endif

    if (f_sceLncUtilInitialize != NULL) {
      int init_rc = f_sceLncUtilInitialize();
      launch_diag_log("lnc_init", title_id, init_rc,
                      "sceLncUtilInitialize");
      if ((init_rc < 0) &&
          ((uint32_t)init_rc != SCE_LNC_UTIL_ERROR_ALREADY_INITIALIZED)) {
        if (userService != NULL)
          dlclose(userService);
        if (lncUtil != NULL)
          dlclose(lncUtil);
        return status_json_200(0, "Failed to initialize Launch API", init_rc);
      }
    }

    if (f_sceLncUtilGetAppId != NULL) {
      int appid_rc = f_sceLncUtilGetAppId(title_id);
      launch_diag_log("preflight", title_id, appid_rc,
                      "sceLncUtilGetAppId");
    }

    if (f_sceUserServiceInitialize != NULL) {
      int init_params[8];
      memset(init_params, 0, sizeof(init_params));
      init_params[0] = 256; /* priority (Itemzflow-like) */
      int uinit_rc = f_sceUserServiceInitialize((void *)init_params);
      launch_diag_log("user_init", title_id, uinit_rc,
                      "sceUserServiceInitialize");
    }

    int32_t userId = 0;
    int have_fg_user = 0;
    if (f_sceUserServiceGetForegroundUser != NULL) {
      int urc = f_sceUserServiceGetForegroundUser(&userId);
      if (urc < 0) {
        launch_diag_log("user", title_id, -20,
                        "sceUserServiceGetForegroundUser failed");
      } else if (!psx_user_id_is_valid(userId)) {
        launch_diag_log("user", title_id, userId,
                        "sceUserServiceGetForegroundUser invalid user");
      } else {
        have_fg_user = 1;
        launch_diag_log("user", title_id, 0,
                        "sceUserServiceGetForegroundUser ok");
      }
    }

    if (!have_fg_user && (f_sceUserServiceGetInitialUser != NULL)) {
      int irc = f_sceUserServiceGetInitialUser(&userId);
      if ((irc >= 0) && psx_user_id_is_valid(userId)) {
        have_fg_user = 1;
        launch_diag_log("user", title_id, 0,
                        "fallback sceUserServiceGetInitialUser ok");
      } else {
        launch_diag_log("user", title_id, (irc < 0) ? irc : userId,
                        "fallback sceUserServiceGetInitialUser failed");
      }
    }

    if (!have_fg_user && (f_sceUserServiceGetLoginUserIdList != NULL)) {
      struct {
        int32_t userId[4];
      } login_list;
      for (size_t i = 0; i < 4; i++) {
        login_list.userId[i] = -1;
      }

      int lrc = f_sceUserServiceGetLoginUserIdList((void *)&login_list);
      if (lrc >= 0) {
        for (size_t i = 0; i < 4; i++) {
          if (psx_user_id_is_valid(login_list.userId[i])) {
            userId = login_list.userId[i];
            have_fg_user = 1;
            launch_diag_log("user", title_id, 0,
                            "fallback sceUserServiceGetLoginUserIdList ok");
            break;
          }
        }
      }
    }

    if (!have_fg_user) {
      launch_diag_log("done", title_id, -21,
                      "No logged-in user context; launch aborted");
      if (userService != NULL)
        dlclose(userService);
      if (lncUtil != NULL)
        dlclose(lncUtil);
      if (sysService != NULL)
        dlclose(sysService);
      return status_json_200(
          0,
          "Launch blocked: no logged-in user context (prevents ShellUI crash)",
          -21);
    }

    LncAppParam param;
    memset(&param, 0, sizeof(param));
    param.sz = sizeof(LncAppParam);
    param.user_id = (uint32_t)userId;
    param.app_opt = 0;
    param.crash_report = 0;
#if defined(PLATFORM_PS4)
    param.check_flag = LNC_SKIP_SYSTEM_UPDATE_CHECK;
#else
    param.check_flag = LNC_FLAG_NONE;
#endif

    uint32_t res = 0xFFFFFFFFU;
    if (f_sceLncUtilLaunchApp != NULL) {
      uint32_t candidates[1];
      size_t candidate_count = 0U;
      candidates[candidate_count++] = (uint32_t)userId;

      const LncAppParamFlag flag_candidates[] = {
#if defined(PLATFORM_PS4)
          LNC_SKIP_SYSTEM_UPDATE_CHECK,
          LNC_FLAG_NONE,
#else
          LNC_FLAG_NONE,
          LNC_SKIP_SYSTEM_UPDATE_CHECK,
#endif
          LNC_SKIP_LAUNCH_CHECK,
      };

      for (size_t i = 0; i < candidate_count; i++) {
        uint32_t uid = candidates[i];
        int dup = 0;
        for (size_t j = 0; j < i; j++) {
          if (candidates[j] == uid) {
            dup = 1;
            break;
          }
        }
        if (dup) {
          continue;
        }

        for (size_t f = 0; f < (sizeof(flag_candidates) / sizeof(flag_candidates[0]));
             f++) {
          param.user_id = uid;
          param.check_flag = flag_candidates[f];

          char detail[96];
          (void)snprintf(detail, sizeof(detail),
                         "sceLncUtilLaunchApp uid=%u flag=0x%X",
                         (unsigned)uid, (unsigned)param.check_flag);
          launch_diag_log("launch_call", title_id, 0, detail);
          res = f_sceLncUtilLaunchApp(title_id, NULL, &param);
          launch_diag_log("launch_try_result", title_id, (int)res, detail);

          if (res == 0 || res == SCE_LNC_UTIL_ERROR_ALREADY_RUNNING) {
            break;
          }
          if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
            /* invalid param: try another flag/uid */
            continue;
          }
          /* non-parameter launch error: keep last status and stop retries */
          break;
        }

        if ((res == 0) ||
            (res == SCE_LNC_UTIL_ERROR_ALREADY_RUNNING) ||
            (res != SCE_LNC_UTIL_ERROR_INVALID_PARAM)) {
          break;
        }
      }

#if !defined(PLATFORM_PS4)
      if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
        launch_diag_log("launch_call", title_id, 0,
                        "sceLncUtilLaunchApp param=NULL");
        res = f_sceLncUtilLaunchApp(title_id, NULL, NULL);
        launch_diag_log("launch_try_result", title_id, (int)res,
                        "sceLncUtilLaunchApp param=NULL");
      }

      if ((res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) &&
          (f_sceSystemServiceLaunchApp != NULL)) {
        launch_diag_log("launch_call", title_id, 0,
                        "sceSystemServiceLaunchApp argv=NULL,param=NULL");
        res = (uint32_t)f_sceSystemServiceLaunchApp(title_id, NULL, NULL);
        launch_diag_log("launch_try_result", title_id, (int)res,
                        "sceSystemServiceLaunchApp argv=NULL,param=NULL");

        if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
          param.check_flag = LNC_FLAG_NONE;
          launch_diag_log("launch_call", title_id, 0,
                          "sceSystemServiceLaunchApp argv=NULL,param=&LncAppParam(flag=0x0)");
          res =
              (uint32_t)f_sceSystemServiceLaunchApp(title_id, NULL, &param);
          launch_diag_log("launch_try_result", title_id, (int)res,
                          "sceSystemServiceLaunchApp argv=NULL,param=&LncAppParam(flag=0x0)");
        }
      }
#else
      if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
        launch_diag_log("launch_result", title_id, (int)res,
                        "PS4 LncUtil rejected all parameter variants; unsafe fallbacks skipped");
      }
#endif
    } else {
      /* Fallback path on systems where LncUtil symbol is unavailable */
      launch_diag_log("launch_call", title_id, 0,
                      "using sceSystemServiceLaunchApp fallback");
      res = (uint32_t)f_sceSystemServiceLaunchApp(title_id, NULL, NULL);
      launch_diag_log("launch_try_result", title_id, (int)res,
                      "sceSystemServiceLaunchApp argv=NULL,param=NULL");

      if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
        uint32_t candidates[1];
        size_t candidate_count = 0U;
        candidates[candidate_count++] = (uint32_t)userId;

        const LncAppParamFlag flag_candidates[] = {
#if defined(PLATFORM_PS4)
            LNC_SKIP_SYSTEM_UPDATE_CHECK,
#else
            LNC_FLAG_NONE,
#endif
        };

        for (size_t i = 0; i < candidate_count; i++) {
          uint32_t uid = candidates[i];
          int dup = 0;
          for (size_t j = 0; j < i; j++) {
            if (candidates[j] == uid) {
              dup = 1;
              break;
            }
          }
          if (dup) {
            continue;
          }

          for (size_t f = 0;
               f < (sizeof(flag_candidates) / sizeof(flag_candidates[0])); f++) {
            param.user_id = uid;
            param.check_flag = flag_candidates[f];

            char detail[128];
            (void)snprintf(detail, sizeof(detail),
                           "sceSystemServiceLaunchApp uid=%u flag=0x%X",
                           (unsigned)uid, (unsigned)param.check_flag);
            launch_diag_log("launch_call", title_id, 0, detail);
            res =
                (uint32_t)f_sceSystemServiceLaunchApp(title_id, NULL, &param);
            launch_diag_log("launch_try_result", title_id, (int)res, detail);

            if (launch_result_is_success(res)) {
              break;
            }
            if (res == SCE_LNC_UTIL_ERROR_INVALID_PARAM) {
              continue;
            }
            break;
          }

          if (launch_result_is_success(res) ||
              (res != SCE_LNC_UTIL_ERROR_INVALID_PARAM)) {
            break;
          }
        }
      }
    }

    if (res == SCE_LNC_ERROR_APP_NOT_FOUND) {
      int fix_tables = 0;
      int fix_rows = 0;
      int fix_rc = psx_repair_appdb_visibility_for_title(title_id, &fix_tables,
                                                          &fix_rows);
      char fix_detail[128];
      (void)snprintf(fix_detail, sizeof(fix_detail),
                     "appdb self-heal rc=%d tables=%d rows=%d", fix_rc,
                     fix_tables, fix_rows);
      launch_diag_log("selfheal", title_id, fix_rc, fix_detail);

      if (fix_rc == 0) {
        if (f_sceLncUtilLaunchApp != NULL) {
          param.user_id = have_fg_user ? (uint32_t)userId : 0U;
#if defined(PLATFORM_PS4)
          param.check_flag = LNC_SKIP_SYSTEM_UPDATE_CHECK;
#else
          param.check_flag = LNC_FLAG_NONE;
#endif
          launch_diag_log("launch_call", title_id, 0,
                          "post-repair sceLncUtilLaunchApp uid=fg/0");
          res = f_sceLncUtilLaunchApp(title_id, NULL, &param);
          launch_diag_log("launch_try_result", title_id, (int)res,
                          "post-repair sceLncUtilLaunchApp uid=fg/0");
        } else if (f_sceSystemServiceLaunchApp != NULL) {
          param.user_id = have_fg_user ? (uint32_t)userId : 0U;
#if defined(PLATFORM_PS4)
          param.check_flag = LNC_SKIP_SYSTEM_UPDATE_CHECK;
#else
          param.check_flag = LNC_FLAG_NONE;
#endif
          launch_diag_log("launch_call", title_id, 0,
                          "post-repair sceSystemServiceLaunchApp uid=fg/0");
          res =
              (uint32_t)f_sceSystemServiceLaunchApp(title_id, NULL, &param);
          launch_diag_log("launch_try_result", title_id, (int)res,
                          "post-repair sceSystemServiceLaunchApp uid=fg/0");
        }
      }
    }

#if !defined(PLATFORM_PS4)
    if ((res == SCE_LNC_ERROR_APP_NOT_FOUND) &&
        (f_sceSystemServiceLoadExec != NULL)) {
      char eboot_path[FTP_PATH_MAX];
      int en = snprintf(eboot_path, sizeof(eboot_path), "/user/app/%s/eboot.bin",
                        title_id);
      if (en > 0 && (size_t)en < sizeof(eboot_path) && access(eboot_path, R_OK) == 0) {
        launch_diag_log("launch_call", title_id, 0,
                        "fallback sceSystemServiceLoadExec /user/app/<TITLE_ID>/eboot.bin");
        res = (uint32_t)f_sceSystemServiceLoadExec(eboot_path, NULL);
        launch_diag_log("launch_try_result", title_id, (int)res,
                        "fallback sceSystemServiceLoadExec /user/app/<TITLE_ID>/eboot.bin");
      }
    }
#else
    if ((res == SCE_LNC_ERROR_APP_NOT_FOUND) &&
        (f_sceSystemServiceLoadExec != NULL)) {
      launch_diag_log("launch_result", title_id, (int)res,
                      "PS4 LoadExec fallback skipped to preserve ShellUI user context");
    }
#endif
    launch_diag_log("launch_result", title_id, (int)res,
                    "launch API returned");

    if (userService != NULL)
      dlclose(userService);
    if (lncUtil != NULL)
      dlclose(lncUtil);
    if (sysService != NULL)
      dlclose(sysService);

    char msg[128];
    if (launch_result_is_success(res)) {
      launch_diag_log("done", title_id, 0, "launch accepted");
      (void)snprintf(msg, sizeof(msg),
                     "{\"status\": \"ok\", \"message\": \"Game %s launched successfully!\"}",
                     title_id);
      http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
      http_response_add_header(resp, "Content-Type", "application/json");
      http_response_set_body(resp, (const uint8_t *)msg, strlen(msg));
      /* PS4 ShellUI can crash if a system notification races app focus change. */
#if !defined(PLATFORM_PS4)
      pal_notification_send("Game Launch executed.");
#endif
      return resp;
    }

    (void)snprintf(msg, sizeof(msg), "Launch failed: 0x%08X", res);
  launch_diag_log("done", title_id, (int)res, msg);
    return status_json_200(0, msg, (int)res);
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

static http_response_t *api_games_installed(const http_request_t *request) {
  (void)request;

  enum { GAMES_BODY_CAP = 512U * 1024U };
  char *body = (char *)malloc(GAMES_BODY_CAP);
  if (body == NULL) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  size_t pos = 0U;
  if (buf_append_cstr(body, GAMES_BODY_CAP, &pos,
                      "{\"ok\":true,\"entries\":[") != 0) {
    free(body);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  int first = 1;
  size_t count_added = 0U;
  const char *bases[] = {"/user/app", "/system_ex/app", "/mnt/ext0/user/app",
                         NULL};
  for (size_t bi = 0; bases[bi] != NULL; bi++) {
    if (append_installed_entries_from_base(bases[bi], body, GAMES_BODY_CAP,
                                           &pos, &first,
                                           &count_added) != 0) {
      free(body);
      return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
    }
  }

  if (buf_append_cstr(body, GAMES_BODY_CAP, &pos, "]}") != 0) {
    free(body);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Response too large");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    free(body);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, body, pos) != 0) {
    free(body);
    http_response_destroy(resp);
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Body allocation failed");
  }
  return resp;
}

static http_response_t *api_games_icon(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return png_fallback_response();
  }

  char title_id[64] = {0};
  if (parse_query_param(query, "id", title_id, sizeof(title_id)) != 0) {
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
  (void)parse_query_param(query, "path", path_hint, sizeof(path_hint));

  char icon_path[FTP_PATH_MAX] = {0};
  if (resolve_installed_icon_path((title_id[0] != '\0') ? title_id : NULL,
                                  (path_hint[0] != '\0') ? path_hint : NULL,
                                  icon_path, sizeof(icon_path)) != 0) {
    return png_fallback_response();
  }

  FILE *fp = fopen(icon_path, "rb");
  if (fp == NULL) {
    return png_fallback_response();
  }
  if (fseek(fp, 0, SEEK_END) != 0) {
    fclose(fp);
    return png_fallback_response();
  }
  long flen = ftell(fp);
  if (flen <= 0 || flen > (8 * 1024 * 1024)) {
    fclose(fp);
    return png_fallback_response();
  }
  if (fseek(fp, 0, SEEK_SET) != 0) {
    fclose(fp);
    return png_fallback_response();
  }

  uint8_t *buf = (uint8_t *)malloc((size_t)flen);
  if (buf == NULL) {
    fclose(fp);
    return png_fallback_response();
  }
  size_t got = fread(buf, 1, (size_t)flen, fp);
  fclose(fp);
  if (got != (size_t)flen) {
    free(buf);
    return png_fallback_response();
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "no-store");
  if (http_response_set_body_owned(resp, buf, got) != 0) {
    free(buf);
    http_response_destroy(resp);
    return png_fallback_response();
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
    (void)parse_query_param(query, "id", requested_id, sizeof(requested_id));
    for (size_t i = 0; requested_id[i] != '\0'; i++) {
      requested_id[i] = (char)toupper((unsigned char)requested_id[i]);
    }
  }

  if (buf_append_cstr(body, sizeof(body), &pos,
                      "{\"ok\":true,\"message\":\"Visibility reindex completed\",\"scanned\":[") !=
      0) {
    return error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
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
          psx_repair_appdb_visibility_for_title(requested_id, &touched_tables,
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
            psx_repair_appdb_visibility_for_title(title_id, &touched_tables,
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
      (void)buf_append_cstr(body, sizeof(body), &pos, ",");
    }
    first = 0;
    (void)buf_append_cstr(body, sizeof(body), &pos, "\"");
    (void)json_escape_append(body, sizeof(body), &pos, bases[i]);
    (void)buf_append_cstr(body, sizeof(body), &pos, "\"");

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

  (void)buf_append_cstr(body, sizeof(body), &pos, "],\"items_seen\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%zu", count_added);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)buf_append_cstr(body, sizeof(body), &pos, ",\"hints\":[");
  (void)buf_append_cstr(body, sizeof(body), &pos,
                        "\"Use Refresh Installed in Games tab\",");
  (void)buf_append_cstr(body, sizeof(body), &pos,
                        "\"If titles still missing, restart shell/console\"");
  (void)buf_append_cstr(body, sizeof(body), &pos, "],\"sqlite_repair\":{");
  (void)buf_append_cstr(body, sizeof(body), &pos, "\"available\":");
  (void)buf_append_cstr(body, sizeof(body), &pos,
                        sqlite_available ? "true" : "false");
  (void)buf_append_cstr(body, sizeof(body), &pos, ",\"titles\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_titles);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)buf_append_cstr(body, sizeof(body), &pos, ",\"tables\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_tables);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)buf_append_cstr(body, sizeof(body), &pos, ",\"rows\":");
  {
    char num[32];
    int n = snprintf(num, sizeof(num), "%d", repaired_rows);
    if (n > 0 && (size_t)n < sizeof(num)) {
      (void)buf_append_bytes(body, sizeof(body), &pos, num, (size_t)n);
    }
  }
  (void)buf_append_cstr(body, sizeof(body), &pos, "}}");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, strlen(body));
  return resp;
}

static http_response_t *api_games_install_status(const http_request_t *request) {
  (void)request;

  int active = g_game_install_state.active;
  int percent = g_game_install_state.last_percent;
  int error_rc = g_game_install_state.last_error;
  unsigned long len = g_game_install_state.last_length;
  unsigned long tx = g_game_install_state.last_transferred;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (active && g_game_install_state.task_id >= 0) {
    SceBgftTaskProgress p;
    int rc = 0;
    if (psx_bgft_progress(g_game_install_state.task_id, &p, &rc) == 0) {
      if (rc == 0) {
        len = p.length;
        tx = p.transferred;
        error_rc = p.error_result;
        if (len > 0UL) {
          percent = (int)((tx * 100UL) / len);
          if (percent > 100) {
            percent = 100;
          }
        }

        g_game_install_state.last_percent = percent;
        g_game_install_state.last_error = error_rc;
        g_game_install_state.last_length = len;
        g_game_install_state.last_transferred = tx;

        if ((len > 0UL && tx >= len) || percent >= 100) {
          g_game_install_state.active = 0;
          active = 0;
        }
      } else {
        error_rc = rc;
        g_game_install_state.last_error = rc;
      }
    }
  }
#endif

  char body[768];
  int n = snprintf(
      body, sizeof(body),
      "{\"ok\":true,\"active\":%s,\"task_id\":%d,\"progress\":%d,\"error\":%d,\"length\":%lu,\"transferred\":%lu,\"title_id\":\"%s\",\"path\":\"%s\"}",
      active ? "true" : "false", g_game_install_state.task_id, percent,
      error_rc, len, tx, g_game_install_state.title_id,
      g_game_install_state.path);

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, (size_t)n);
  return resp;
}

static http_response_t *api_games_uninstall(const http_request_t *request) {
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char title_id[64] = {0};
  if (parse_query_param(query, "id", title_id, sizeof(title_id)) != 0 ||
      !is_valid_title_id_for_uninstall(title_id)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid title id");
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int rc = -1;
  if (psx_uninstall_title_id(title_id, &rc) != 0) {
    return status_json_200(0, "Uninstall API unavailable", -1);
  }
  if (rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Uninstall failed: 0x%08X", (unsigned)rc);
    return status_json_200(0, msg, rc);
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
  return status_json_200(0, "Uninstall only available on PS4/PS5", -1);
#endif
}

static http_response_t *api_games_install(const http_request_t *request) {
#if !ENABLE_PKG_INSTALL
  (void)request;
  return error_json(HTTP_STATUS_409_CONFLICT,
                    "PKG installation is disabled for this build");
#else
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[FTP_PATH_MAX] = {0};
  if (parse_path_param(query, path, sizeof(path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  if (!has_pkg_extension(safe)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Install supports only PKG/FPKG files");
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int install_rc = -1;
  int task_id = -1;
  char title_id[64] = {0};
  if (psx_install_pkg_bgft(safe, "Remote PKG Install", title_id,
                           sizeof(title_id), &task_id,
                           &install_rc) == 0) {
    if (install_rc == 0 && task_id >= 0) {
      g_game_install_state.active = 1;
      g_game_install_state.task_id = task_id;
      g_game_install_state.last_percent = 0;
      g_game_install_state.last_error = 0;
      g_game_install_state.last_length = 0UL;
      g_game_install_state.last_transferred = 0UL;
      (void)snprintf(g_game_install_state.title_id,
                     sizeof(g_game_install_state.title_id), "%s",
                     title_id[0] ? title_id : "");
      (void)snprintf(g_game_install_state.path, sizeof(g_game_install_state.path),
                     "%s", safe);
    }
  } else if (psx_install_pkg_path(safe, title_id, sizeof(title_id),
                                  &install_rc) != 0) {
    return status_json_200(0, "Install API unavailable", -1);
  }
  if (install_rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Install failed: 0x%08X",
                   (unsigned)install_rc);
    return status_json_200(0, msg, install_rc);
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
  return status_json_200(0, "Install only available on PS4/PS5", -1);
#endif
#endif
}

static http_response_t *api_games_reinstall(const http_request_t *request) {
#if !ENABLE_PKG_INSTALL
  (void)request;
  return error_json(HTTP_STATUS_409_CONFLICT,
                    "PKG installation is disabled for this build");
#else
  if ((request->method != HTTP_METHOD_POST) &&
      (request->method != HTTP_METHOD_GET)) {
    return error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char path[FTP_PATH_MAX] = {0};
  if (parse_path_param(query, path, sizeof(path)) != 0) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing path parameter");
  }

  char safe[FTP_PATH_MAX];
  if (!validate_path(path, safe, sizeof(safe))) {
    return error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
  }

  if (!has_pkg_extension(safe)) {
    return error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Reinstall supports only PKG/FPKG files");
  }

  char title_id[64] = {0};
  (void)extract_title_id_from_game_image(safe, title_id, sizeof(title_id));

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int uninstall_rc = -1;
  if (title_id[0] != '\0') {
    if (psx_uninstall_title_id(title_id, &uninstall_rc) != 0) {
      uninstall_rc = -1;
    }
  }

  int install_rc = -1;
  int task_id = -1;
  char install_title[64] = {0};
  if (psx_install_pkg_bgft(safe, "Remote PKG Reinstall", install_title,
                           sizeof(install_title), &task_id,
                           &install_rc) == 0) {
    if (install_rc == 0 && task_id >= 0) {
      g_game_install_state.active = 1;
      g_game_install_state.task_id = task_id;
      g_game_install_state.last_percent = 0;
      g_game_install_state.last_error = 0;
      g_game_install_state.last_length = 0UL;
      g_game_install_state.last_transferred = 0UL;
      (void)snprintf(g_game_install_state.title_id,
                     sizeof(g_game_install_state.title_id), "%s",
                     install_title[0] ? install_title : title_id);
      (void)snprintf(g_game_install_state.path, sizeof(g_game_install_state.path),
                     "%s", safe);
    }
  } else if (psx_install_pkg_path(safe, install_title, sizeof(install_title),
                                  &install_rc) != 0) {
    return status_json_200(0, "Install API unavailable", -1);
  }
  if (install_rc < 0) {
    char msg[96];
    (void)snprintf(msg, sizeof(msg), "Reinstall failed: 0x%08X",
                   (unsigned)install_rc);
    return status_json_200(0, msg, install_rc);
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
  return status_json_200(0, "Reinstall only available on PS4/PS5", -1);
#endif
#endif
}
