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
 *   GET|POST /api/notify?text=...   Custom PS4/PS5 system notification
 *   GET /                           Serve embedded index.html
 *   GET /style.css                  Serve embedded stylesheet
 *   GET /app.js                     Serve embedded JavaScript
 */

#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "ftp_instance.h"
#include "ftp_path.h"
#include "ftp_server.h" /* ftp_server_context_t — for network reset endpoint */
#include "ftp_log.h"
#include "http_config.h"
#include "pal_fileio.h"
#include "pal_network.h"      /* pal_network_reset_ftp_stack() */
#include "pal_notification.h" /* pal_notification_send_ex() — /api/notify */
#include "exfat_unpacker.h"  /* exFAT image parsing for game metadata */
#include "pkg_unpacker.h"    /* PKG archive parsing for game metadata */
#include "builtin_unzip.h"   /* built-in ZIP extractor (PS5 fallback) */
#include "transfer/transfer_manager.h"
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
 * FORWARD DECLARATIONS
 *===========================================================================*/

static http_response_t *api_game_meta(const http_request_t *request);
static http_response_t *api_game_icon(const http_request_t *request);
#if ENABLE_WEB_UPLOAD
#endif
static http_response_t *api_admin_launch(const http_request_t *request);
static http_response_t *api_games_installed(const http_request_t *request);
static http_response_t *api_games_install_status(const http_request_t *request);
static http_response_t *api_games_icon(const http_request_t *request);
static http_response_t *api_games_repair_visibility(const http_request_t *request);
static http_response_t *api_games_uninstall(const http_request_t *request);
static http_response_t *api_games_install(const http_request_t *request);
static http_response_t *api_games_reinstall(const http_request_t *request);

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
uint64_t http_api_dir_size_with_partial(const char *path, int *out_partial) {
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


#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) ||                        \
    defined(PLATFORM_PS5) || defined(PS4) || defined(PS5)
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
      return http_api_error_json(HTTP_STATUS_403_FORBIDDEN,
                        "Invalid or missing CSRF token");
    }
  }
#endif

  /*  Download manager — /api/download/start, status, pause, cancel
   *
   *  IMPORTANT: These longer-prefix routes MUST come BEFORE the
   *  generic /api/download handler below, because strncmp matches
   *  left-to-right and "/api/download" (13 chars) is a prefix of
   *  "/api/download/start" (19 chars).
   *
   *  Route order:          Match example:
   *    /api/download/start   → http_api_transfer_start()     ✓
   *    /api/download/status  → http_api_transfer_status()    ✓
   *    /api/download/pause   → http_api_transfer_pause()     ✓
   *    /api/download/cancel  → http_api_transfer_cancel()    ✓
   *    /api/download?path=   → api_download()     ✓  (file download)
   */
  if (strncmp(request->uri, "/api/download/start", 19) == 0) {
    return http_api_transfer_start(request);
  }
  if (strncmp(request->uri, "/api/download/status", 20) == 0) {
    return http_api_transfer_status(request);
  }
  if (strncmp(request->uri, "/api/download/pause", 19) == 0) {
    return http_api_transfer_pause(request);
  }
  if (strncmp(request->uri, "/api/download/cancel", 20) == 0) {
    return http_api_transfer_cancel(request);
  }

  {
    http_response_t *file_resp = http_api_files_handle(request);
    if (file_resp != NULL) return file_resp;
  }

  {
    http_response_t *system_resp = http_api_system_handle(request);
    if (system_resp != NULL) return system_resp;
  }

  {
    http_response_t *process_resp = http_api_process_handle(request);
    if (process_resp != NULL) return process_resp;
  }

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
    return http_api_archive_progress(request);
  }
  if (strncmp(request->uri, "/api/extract_cancel", 19) == 0) {
    return http_api_archive_cancel(request);
  }
  if (strncmp(request->uri, "/api/extract", 12) == 0) {
    return http_api_archive_start(request);
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
    return http_api_legacy_disabled_json(
        "{\"ok\":true,\"enabled\":false,\"status\":\"offline\"}");
  }
  if (strncmp(request->uri, "/api/stream/start", 17) == 0 ||
      strncmp(request->uri, "/api/stream/stop", 16) == 0 ||
      strncmp(request->uri, "/api/stream", 11) == 0) {
    return http_api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Stream disabled\"}");
  }
  if (strncmp(request->uri, "/api/admin/installed", 20) == 0) {
    return http_api_legacy_disabled_json(
        "{\"ok\":true,\"installed\":false}");
  }
  if (strncmp(request->uri, "/api/admin/install", 18) == 0) {
    return http_api_legacy_disabled_json(
        "{\"ok\":false,\"message\":\"Install API not available\"}");
  }

  /*  Static resources (index.html, style.css, app.js)  */
  return http_static_serve(request);
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


/*===========================================================================*
 * GET /api/dirsize?path=<dir> — Recursive directory size
 *
 *  Returns the total size in bytes of all regular files under path.
 *  Called lazily by the frontend after the listing is already rendered,
 *  so it does not block the initial directory load.
 *
 *  RESPONSE: {"path":"/some/dir","size":123456789}
 *===========================================================================*/


/*===========================================================================*
 * GET /api/download — File Download
 *
 *  Reads the file and sends it with Content-Disposition: attachment.
 *  For large files, uses the sendfile_fd field so the server can
 *  stream with sendfile() / read+write loop.
 *===========================================================================*/



#if ENABLE_WEB_UPLOAD

/*===========================================================================*
 * POST /api/mkdir — Create directory
 *
 *   POST /api/mkdir?path=/parent&name=new_folder
 *   Returns: {"ok":true,"path":"/parent/new_folder","name":"new_folder"}
 *===========================================================================*/


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


/*===========================================================================*
 * POST /api/rename — Rename file or directory in-place
 *
 *   ┌─────────────────────────────────────────────────┐
 *   │  POST /api/rename?path=/dir/old.txt&name=new    │
 *   │                                                 │
 *   │  old  = http_api_validate_path(path)                     │
 *   │  new  = parent(old) + '/' + name                │
 *   │  pal_file_rename(old, new)                      │
 *   │  result -> {"ok":true,"path":"/dir/new"}        │
 *   └─────────────────────────────────────────────────┘
 *===========================================================================*/


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

#endif

/*===========================================================================*
 * GET /api/stats/ram  — RAM usage
 *
 *  RESPONSE: { "used": N, "cached": N, "buffers": N, "free": N, "total": N }
 *===========================================================================*/



/*===========================================================================*
 * GET /api/stats/system  — CPU temp, uptime, boot time
 *
 *  RESPONSE: { "cpu_temp": N|null, "uptime_seconds": N|null,
 *               "boot_epoch": N|null }
 *===========================================================================*/


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


/*===========================================================================*
 * GET|POST /api/notify?text=<msg>[&icon=<name>]
 *
 *  Show a custom PS4/PS5 system notification (toast). On non-console builds
 *  the message is forwarded to syslog via pal_notification_send_ex().
 *
 *  Intended for local-network automation (e.g. Home Assistant):
 *    GET /api/notify?text=Washing%20machine%20is%20done
 *
 *  RESPONSE: { "ok": true, "text": "...", "icon": "..." }
 *===========================================================================*/




/*===========================================================================*
 * GET /api/disk/info  — Disk usage for largest mounted volume
 *
 *  RESPONSE: { "used": N, "free": N, "total": N, "path": "..." }
 *===========================================================================*/


/*===========================================================================*
 * GET /api/disk/tree?path=X  — Directory tree (1 level deep, with sizes)
 *
 *  RESPONSE: { "name": "dirname", "type": "directory",
 *               "size": N, "children": [ { "name":..., "type":..., "size":...
 *}, ...] }
 *===========================================================================*/



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


/*===========================================================================*
 * POST /api/process/kill  — Send SIGTERM to a process
 *
 *  REQUEST BODY: { "pid": N }
 *  RESPONSE:     { "success": true } | { "error": "..." }
 *===========================================================================*/



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
      if (http_api_buf_append_cstr(body, cap, pos, ",") != 0) {
        closedir(dir);
        return -1;
      }
    }
    *first = 0;
    (*count_added)++;

    if (http_api_buf_append_cstr(body, cap, pos, "{\"id\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, title_id) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"name\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, title_name) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"path\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, app_dir) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"source\":\"") != 0 ||
        http_api_json_escape_append(body, cap, pos, base) != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "\",\"has_icon\":") != 0 ||
        http_api_buf_append_cstr(body, cap, pos, has_icon ? "true" : "false") != 0 ||
        http_api_buf_append_cstr(body, cap, pos, "}") != 0) {
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
  if (query) (void)http_api_parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
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
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
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
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
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
  if (query) (void)http_api_parse_path_param(query, path, sizeof(path));

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
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
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Not a valid PKG or exFAT image");
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
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Icon not found in image");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "image/png");
  http_response_add_header(resp, "Cache-Control", "max-age=86400");
  
  if (http_response_set_body_owned(resp, icon_data, icon_size) != 0) {
    free(icon_data);
    http_response_destroy(resp);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to send icon");
  }
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

/*===========================================================================*
 * GET /api/admin/fan?threshold=...
 * Sets the fan threshold on PS4/PS5 using /dev/icc_fan ioctl 0xC01C8F07.
 *===========================================================================*/

/*===========================================================================*
 * GET /api/admin/launch?id=... or /api/admin/launch?path=...
 *===========================================================================*/
static http_response_t *api_admin_launch(const http_request_t *request) {
  char title_id[64] = {0};

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  const char *id_param = strstr(query, "id=");
  if (id_param != NULL) {
    id_param += 3; /* skip "id=" */
    size_t id_len = 0U;
    while ((id_param[id_len] != '\0') && (id_param[id_len] != '&')) {
      if (id_len >= (sizeof(title_id) - 1U)) {
        return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                          "'id' parameter too long");
      }
      title_id[id_len] = id_param[id_len];
      id_len++;
    }
    title_id[id_len] = '\0';
  } else {
    char path[1024] = "";
    if (http_api_parse_path_param(query, path, sizeof(path)) != 0) {
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                        "Missing 'id' or valid 'path' parameter");
    }

    char safe[FTP_PATH_MAX];
    if (!http_api_validate_path(path, safe, sizeof(safe))) {
      return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Path traversal blocked");
    }

    if (extract_title_id_from_game_image(safe, title_id, sizeof(title_id)) != 0) {
      if (extract_title_id_from_app_dir(safe, title_id, sizeof(title_id)) != 0) {
        return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                          "Unable to resolve TITLE_ID from image/app path");
      }
    }
  }

  if (title_id[0] == '\0') {
    launch_diag_log("input", title_id, -1, "missing launch target");
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or invalid launch target");
  }

  /* sanitize title id for runtime launch API */
  for (size_t i = 0; title_id[i] != '\0'; i++) {
    unsigned char c = (unsigned char)title_id[i];
    if (!(isalnum(c) || c == '_' || c == '-')) {
      launch_diag_log("input", title_id, -2, "invalid title id format");
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid title id format");
    }
    title_id[i] = (char)toupper(c);
  }
  launch_diag_log("input", title_id, 0, "launch request received");

  char installed_app_dir[FTP_PATH_MAX];
  if (resolve_installed_app_dir_by_title(title_id, installed_app_dir,
                                         sizeof(installed_app_dir)) != 0) {
    launch_diag_log("preflight_fs", title_id, -30,
                    "title id not found in installed app directories");
    return http_api_status_json_200(0,
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
      return http_api_status_json_200(
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
      return http_api_status_json_200(
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
        return http_api_status_json_200(0, "Failed to initialize Launch API", init_rc);
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
      return http_api_status_json_200(
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
    return http_api_status_json_200(0, msg, (int)res);
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
    if (append_installed_entries_from_base(bases[bi], body, GAMES_BODY_CAP,
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
  if (resolve_installed_icon_path((title_id[0] != '\0') ? title_id : NULL,
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
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST or GET");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing query string");
  }

  char title_id[64] = {0};
  if (http_api_parse_query_param(query, "id", title_id, sizeof(title_id)) != 0 ||
      !is_valid_title_id_for_uninstall(title_id)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid title id");
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int rc = -1;
  if (psx_uninstall_title_id(title_id, &rc) != 0) {
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

  if (!has_pkg_extension(safe)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
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

  if (!has_pkg_extension(safe)) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
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
