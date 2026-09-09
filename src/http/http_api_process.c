/* HTTP process inspection and control API. */
#include "http_api_internal.h"
#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(PLATFORM_MACOS) || defined(__APPLE__)
#include <sys/proc.h>
#include <sys/sysctl.h>
#endif
#if defined(PLATFORM_LINUX) && __has_include(<sys/sysinfo.h>)
#define HAS_SYSINFO 1
#endif

static http_response_t *api_processes(const http_request_t *request) {
  (void)request;

  size_t cap = 256 * 1024; /* 256 KB */
  char *body = (char *)malloc(cap);
  if (body == NULL) {
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, "Out of memory");
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
          (void)http_api_json_escape_append(body, cap, &pos, name);
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
      (void)http_api_json_escape_append(body, cap, &pos, comm);
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


static int parse_pid_from_body(const char *body, size_t len, int *out_pid) {
  static const char key[] = "\"pid\"";
  if (body == NULL || out_pid == NULL || len < sizeof(key) - 1U) return -1;

  size_t pos = SIZE_MAX;
  for (size_t i = 0U; i + sizeof(key) - 1U <= len; i++) {
    if (memcmp(body + i, key, sizeof(key) - 1U) == 0) {
      pos = i + sizeof(key) - 1U;
      break;
    }
  }
  if (pos == SIZE_MAX) return -1;
  while (pos < len && (body[pos] == ' ' || body[pos] == '\t')) pos++;
  if (pos >= len || body[pos++] != ':') return -1;
  while (pos < len && (body[pos] == ' ' || body[pos] == '\t')) pos++;
  if (pos >= len || body[pos] < '0' || body[pos] > '9') return -1;

  unsigned value = 0U;
  while (pos < len && body[pos] >= '0' && body[pos] <= '9') {
    unsigned digit = (unsigned)(body[pos] - '0');
    if (value > ((unsigned)INT_MAX - digit) / 10U) return -1;
    value = value * 10U + digit;
    pos++;
  }
  if (value == 0U) return -1;
  *out_pid = (int)value;
  return 0;
}


static http_response_t *api_process_kill(const http_request_t *request) {
  if (request->method != HTTP_METHOD_POST) {
    return http_api_error_json(HTTP_STATUS_405_METHOD_NOT_ALLOWED, "Use POST");
  }

  int pid = 0;
  if (parse_pid_from_body(request->body, request->body_length, &pid) != 0) {
    return http_api_error_json(HTTP_STATUS_400_BAD_REQUEST, "Missing or invalid pid");
  }

  /* Safety: never kill PID 1 or negative PIDs */
  if (pid <= 1) {
    return http_api_error_json(HTTP_STATUS_403_FORBIDDEN, "Cannot kill system process");
  }

  if (kill((pid_t)pid, SIGTERM) != 0) {
    char msg[64];
    snprintf(msg, sizeof(msg), "kill failed: %s", strerror(errno));
    return http_api_error_json(HTTP_STATUS_500_INTERNAL_ERROR, msg);
  }

  http_response_t *resp = http_response_create(HTTP_STATUS_200_OK);
  http_response_add_header(resp, "Content-Type", "application/json");
  char body[64];
  int len = snprintf(body, sizeof(body), "{\"success\":true,\"pid\":%d}", pid);
  http_response_set_body(resp, body, (size_t)len);
  return resp;
}


http_response_t *http_api_process_handle(const http_request_t *request) {
  if (request == NULL) return NULL;
  if (http_api_route_is(request->uri, "/api/process/kill")) return api_process_kill(request);
  if (http_api_route_is(request->uri, "/api/processes")) return api_processes(request);
  return NULL;
}
