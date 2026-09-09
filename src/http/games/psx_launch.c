#include "games_internal.h"
#include "../http_api_internal.h"
#include "ftp_config.h"
#include "ftp_log.h"
#include "pal_notification.h"
#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <dlfcn.h>
#endif

#define SCE_LNC_UTIL_ERROR_ALREADY_RUNNING 0x8094000CU
#define SCE_LNC_UTIL_ERROR_ALREADY_INITIALIZED 0x80940018U
#define SCE_LNC_UTIL_ERROR_INVALID_PARAM 0x80940005U
#define SCE_LNC_ERROR_APP_NOT_FOUND 0x80940031U
#define SCE_LNC_APP_ID_BIG_BASE 0x60000000U
#define SCE_LNC_APP_ID_TYPE_MASK 0xFF000000U


#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

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

#endif

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






http_response_t *http_games_launch(const http_request_t *request) {
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

    if (games_extract_title_id_from_image(safe, title_id, sizeof(title_id)) != 0) {
      if (games_extract_title_id_from_app_dir(safe, title_id, sizeof(title_id)) != 0) {
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
  if (games_resolve_installed_app_dir(title_id, installed_app_dir,
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
      int fix_rc = games_psx_repair_appdb_visibility(title_id, &fix_tables,
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

