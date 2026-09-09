/* HTTP system, telemetry, disk and network-admin API. */
#include "http_api.h"
#include "http_api_internal.h"
#include "ftp_config.h"
#include "ftp_instance.h"
#include "ftp_server.h"
#include "pal_network.h"
#include "pal_notification.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#ifndef _WIN32
#include <sys/ioctl.h>
#endif
#if defined(PLATFORM_LINUX) && __has_include(<sys/sysinfo.h>)
#define HAS_SYSINFO 1
#include <sys/sysinfo.h>
#endif
#if defined(PLATFORM_MACOS) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || defined(PS4) || defined(PS5) || defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
#include <mach/mach.h>
#include <mach/vm_statistics.h>
#endif

#define NOTIFY_TEXT_MAX 1024
#define NOTIFY_ICON_MAX 64
#define DISK_TREE_MAX_CHILDREN 512

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


static http_response_t *api_stats(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";

  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  uint64_t disk_total = 0U;
  uint64_t disk_used = 0U;
  uint64_t disk_free = 0U;
  const char *disk_path = NULL;
  int disk_ok = get_best_disk_stats(safe, &disk_path, &disk_total, &disk_used,
                                    &disk_free);

  uint32_t items = 0U;
  int items_ok = count_dir_items(safe, &items);

  uint64_t boot_epoch = 0U;
  int boot_ok = get_boot_epoch_seconds(&boot_epoch);

  int32_t temp_c = 0;
  int temp_ok = get_cpu_temp_c(&temp_c);

  char body[1024];
  size_t pos = 0U;
  size_t cap = sizeof(body);

  if (http_api_buf_append_cstr(body, cap, &pos, "{\"path\":\"") != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  if (http_api_json_escape_append(body, cap, &pos, path) != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }
  if (http_api_buf_append_cstr(body, cap, &pos, "\"") != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  if (disk_ok == 0) {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"disk_used\":") != 0 ||
        http_api_buf_append_u64(body, cap, &pos, disk_used) != 0 ||
        http_api_buf_append_cstr(body, cap, &pos, ",\"disk_total\":") != 0 ||
        http_api_buf_append_u64(body, cap, &pos, disk_total) != 0 ||
        http_api_buf_append_cstr(body, cap, &pos, ",\"disk_free\":") != 0 ||
        http_api_buf_append_u64(body, cap, &pos, disk_free) != 0 ||
        http_api_buf_append_cstr(body, cap, &pos, ",\"disk_path\":\"") != 0 ||
        http_api_json_escape_append(body, cap, &pos,
                           (disk_path != NULL) ? disk_path : "") != 0 ||
        http_api_buf_append_cstr(body, cap, &pos, "\"") != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (http_api_buf_append_cstr(body, cap, &pos,
                        ",\"disk_used\":null,\"disk_total\":null,"
                        "\"disk_free\":null,\"disk_path\":null") != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (temp_ok == 0) {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"cpu_temp\":") != 0 ||
        http_api_buf_append_i32(body, cap, &pos, temp_c) != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"cpu_temp\":null") != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (boot_ok == 0) {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"uptime\":") != 0 ||
        http_api_buf_append_u64(body, cap, &pos, boot_epoch) != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"uptime\":null") != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (items_ok == 0) {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"items_in_dir\":") != 0 ||
        http_api_buf_append_u32(body, cap, &pos, items) != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  } else {
    if (http_api_buf_append_cstr(body, cap, &pos, ",\"items_in_dir\":null") != 0) {
      return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
    }
  }

  if (http_api_buf_append_cstr(body, cap, &pos, "}") != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_set_body(resp, body, pos);
  return resp;
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


static int notify_icon_is_safe(const char *icon) {
  if ((icon == NULL) || (icon[0] == '\0')) {
    return 0;
  }
  for (const char *p = icon; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if (!(isalnum(c) || c == '_')) {
      return 0;
    }
  }
  return 1;
}


static http_response_t *api_notify(const http_request_t *request) {
  if ((request->method != HTTP_METHOD_GET) &&
      (request->method != HTTP_METHOD_POST)) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use GET or POST");
  }

  const char *query = strchr(request->uri, '?');
  if (query == NULL) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing query string (text=...)");
  }

  char text[NOTIFY_TEXT_MAX];
  if (http_api_parse_query_param(query, "text", text, sizeof(text)) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                      "Missing or empty 'text' parameter");
  }

  /* Reject control characters so toast text stays printable */
  for (const char *p = text; *p != '\0'; p++) {
    unsigned char c = (unsigned char)*p;
    if ((c < 0x20U) || (c == 0x7FU)) {
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                        "Notification text contains control characters");
    }
  }

  char icon[NOTIFY_ICON_MAX];
  icon[0] = '\0';
  if (http_api_parse_query_param(query, "icon", icon, sizeof(icon)) == 0) {
    if (!notify_icon_is_safe(icon)) {
      return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST,
                        "Invalid 'icon' parameter (use [A-Za-z0-9_])");
    }
  } else {
    (void)snprintf(icon, sizeof(icon), "%s", "icon_system");
  }

  pal_notification_send_ex(text, icon);

  char body[NOTIFY_TEXT_MAX + NOTIFY_ICON_MAX + 64];
  size_t pos = 0;
  size_t cap = sizeof(body);

  if (http_api_buf_append_cstr(body, cap, &pos, "{\"ok\":true,\"text\":\"") != 0 ||
      http_api_json_escape_append(body, cap, &pos, text) != 0 ||
      http_api_buf_append_cstr(body, cap, &pos, "\",\"icon\":\"") != 0 ||
      http_api_json_escape_append(body, cap, &pos, icon) != 0 ||
      http_api_buf_append_cstr(body, cap, &pos, "\"}") != 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
  return resp;
}


static http_response_t *api_disk_info(const http_request_t *request) {
  (void)request;

  uint64_t total = 0, used = 0, free_b = 0;
  const char *disk_path = NULL;
  get_best_disk_stats(http_api_get_root(), &disk_path, &total, &used, &free_b);

  char body[256];
  size_t pos = 0;
  size_t cap = sizeof(body);
  pos += (size_t)snprintf(body + pos, cap - pos,
                          "{\"used\":%" PRIu64 ",\"free\":%" PRIu64
                          ",\"total\":%" PRIu64 ",\"path\":\"",
                          used, free_b, total);
  (void)http_api_json_escape_append(body, cap, &pos, disk_path ? disk_path : "/");
  pos += (size_t)snprintf(body + pos, cap - pos, "\"}");

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");
  http_response_set_body(resp, body, pos);
  return resp;
}


static http_response_t *api_disk_tree(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  char path[1024] = "/";
  if (query != NULL) {
    (void)http_api_parse_path_param(query, path, sizeof(path));
  }

  char safe[FTP_PATH_MAX];
  if (!http_api_validate_path(path, safe, sizeof(safe))) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Forbidden path");
  }

  DIR *dir = opendir(safe);
  if (dir == NULL) {
    return http_api_error_json(HTTP_STATUS_404_NOT_FOUND, "Directory not found");
  }

  /* Allocate a generous output buffer — tree JSON can be large */
  size_t cap = 512 * 1024; /* 512 KB */
  char *body = (char *)malloc(cap);
  if (body == NULL) {
    closedir(dir);
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
  }

  size_t pos = 0;

  /* Header: root node name */
  const char *dirname = strrchr(safe, '/');
  dirname = (dirname && dirname[1] != '\0') ? dirname + 1 : safe;

  pos += (size_t)snprintf(body + pos, cap - pos, "{\"name\":\"");
  (void)http_api_json_escape_append(body, cap, &pos, dirname);
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
    (void)http_api_json_escape_append(body, cap, &pos, ent->d_name);
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


static http_response_t *api_network_reset(const http_request_t *request) {
  if (request == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Null request");
  }

  /* Only POST is accepted */
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED,
                      "Use POST /api/network/reset");
  }

  char body[128];
  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  if (resp == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "OOM");
  }
  http_response_add_header(resp, "Content-Type", "application/json");
  http_response_add_header(resp, "Cache-Control", "no-store");

  if (http_api_server_ctx() == NULL) {
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
      pal_network_reset_ftp_stack(http_api_server_ctx()->sessions, FTP_MAX_SESSIONS);

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


static http_response_t *api_admin_fan(const http_request_t *request) {
  const char *query = strchr(request->uri, '?');
  if (!query) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing threshold parameter");
  }

  int threshold = 0;
  if (sscanf(query, "?threshold=%d", &threshold) != 1) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Invalid threshold parameter format");
  }

  /* Clamp to safe operating values */
  if (threshold < 40) { threshold = 40; }
  if (threshold > 90) { threshold = 90; }

#ifndef _WIN32
  int fd = open("/dev/icc_fan", O_RDONLY, 0);
  if (fd < 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Failed to open /dev/icc_fan (Unsupported OS)");
  }

  char data[10] = {0x00, 0x00, 0x00, 0x00, 0x00, (char)threshold, 0x00, 0x00, 0x00, 0x00};
  int ret = ioctl(fd, 0xC01C8F07, data);
  close(fd);

  if (ret < 0) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Fan control ioctl failed");
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

http_response_t *http_api_system_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/stats/ram")) return api_stats_ram(request);
  if (http_api_route_is(request->uri, "/api/stats/system")) return api_stats_system(request);
  if (http_api_route_is(request->uri, "/api/status")) return api_status(request);
  if (http_api_route_is(request->uri, "/api/notify")) return api_notify(request);
  if (http_api_route_is(request->uri, "/api/stats")) return api_stats(request);
  if (http_api_route_is(request->uri, "/api/disk/info")) return api_disk_info(request);
  if (http_api_route_is(request->uri, "/api/disk/tree")) return api_disk_tree(request);
  if (http_api_route_is(request->uri, "/api/network/reset")) return api_network_reset(request);
  if (http_api_route_is(request->uri, "/api/admin/fan")) return api_admin_fan(request);
  return NULL;
}
