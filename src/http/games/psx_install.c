#include "games_internal.h"
#include "ftp_config.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef ENABLE_PKG_INSTALL
#define ENABLE_PKG_INSTALL 0
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <dlfcn.h>
typedef enum {
  BGFT_TASK_OPTION_NONE = 0x0,
  BGFT_TASK_OPTION_DELETE_AFTER_UPLOAD = 0x1,
} bgft_task_option_t;
typedef struct {
  int user_id; int entitlement_type; const char *id; const char *content_url;
  const char *content_ex_url; const char *content_name; const char *icon_path;
  const char *sku_id; bgft_task_option_t option; const char *playgo_scenario_id;
  const char *release_date; const char *package_type; const char *package_sub_type;
  unsigned long package_size;
} bgft_download_param;
typedef struct { bgft_download_param param; unsigned int slot; } bgft_download_param_ex;
typedef struct { void *heap; size_t heapSize; } bgft_init_params;
typedef struct {
  unsigned int bits; int error_result; unsigned long length; unsigned long transferred;
  unsigned long lengthTotal; unsigned long transferredTotal; unsigned int numIndex;
  unsigned int numTotal; unsigned int restSec; unsigned int restSecTotal;
  int preparingPercent; int localCopyPercent;
} SceBgftTaskProgress;
#endif

static games_install_snapshot_t g_game_install_state = {0};

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

int games_psx_install_bgft(const char *pkg_path, const char *content_name,
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

static int games_psx_bgft_progress(int task_id, SceBgftTaskProgress *out_progress,
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

int games_psx_uninstall(const char *title_id, int *out_rc) {
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
int games_psx_install_path(const char *pkg_path, char *out_title_id,
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


#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

void games_install_state_begin(int task_id, const char *title_id,
                               const char *path) {
  memset(&g_game_install_state, 0, sizeof(g_game_install_state));
  g_game_install_state.active = 1;
  g_game_install_state.task_id = task_id;
  if (title_id != NULL) {
    (void)snprintf(g_game_install_state.title_id,
                   sizeof(g_game_install_state.title_id), "%s", title_id);
  }
  if (path != NULL) {
    (void)snprintf(g_game_install_state.path,
                   sizeof(g_game_install_state.path), "%s", path);
  }
}

void games_install_state_snapshot(games_install_snapshot_t *out) {
  if (out != NULL) *out = g_game_install_state;
}

int games_install_state_refresh(games_install_snapshot_t *out) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (g_game_install_state.active && g_game_install_state.task_id >= 0) {
    SceBgftTaskProgress p;
    int rc = 0;
    if (games_psx_bgft_progress(g_game_install_state.task_id, &p, &rc) == 0) {
      g_game_install_state.last_error = (rc == 0) ? p.error_result : rc;
      g_game_install_state.last_length = p.lengthTotal;
      g_game_install_state.last_transferred = p.transferredTotal;
      if (p.lengthTotal > 0UL) {
        unsigned long pct = (p.transferredTotal * 100UL) / p.lengthTotal;
        if (pct > 100UL) pct = 100UL;
        g_game_install_state.last_percent = (int)pct;
      } else if (p.preparingPercent > 0) {
        g_game_install_state.last_percent = p.preparingPercent;
      }
      if (g_game_install_state.last_error != 0 ||
          g_game_install_state.last_percent >= 100) {
        g_game_install_state.active = 0;
      }
    }
  }
#endif
  games_install_state_snapshot(out);
  return 0;
}
