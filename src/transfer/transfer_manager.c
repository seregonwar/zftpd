#include "transfer/transfer_manager.h"
#include "transfer_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

static transfer_job_t g_jobs[TRANSFER_MAX_ACTIVE];
static pthread_mutex_t g_jobs_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_next_id = 1;

static int has_scheme(const char *url, const char *scheme) {
  size_t n = scheme != NULL ? strlen(scheme) : 0U;
  return url != NULL && scheme != NULL && strncasecmp(url, scheme, n) == 0;
}

void transfer_job_set_error(transfer_job_t *job, const char *fmt, ...) {
  if (job == NULL || fmt == NULL) return;
  va_list ap;
  va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#endif
  (void)vsnprintf(job->error_msg, sizeof(job->error_msg), fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
  va_end(ap);
  atomic_store(&job->error, 1);
}

int transfer_job_wait_if_paused(transfer_job_t *job) {
  if (job == NULL) return -1;
  while (atomic_load(&job->paused) != 0 &&
         atomic_load(&job->cancel_requested) == 0) {
    usleep(100000U);
  }
  return atomic_load(&job->cancel_requested) != 0 ? -1 : 0;
}

int transfer_write_all(int fd, const void *data, size_t len) {
  const unsigned char *p = (const unsigned char *)data;
  while (len > 0U) {
    ssize_t n = write(fd, p, len);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    if (n == 0) return -1;
    p += (size_t)n;
    len -= (size_t)n;
  }
  return 0;
}

static void sanitize_filename(char *name) {
  if (name == NULL) return;
  for (size_t i = 0U; name[i] != '\0'; i++) {
    unsigned char c = (unsigned char)name[i];
    if (c < 32U || strchr("/\\:*?\"<>|", (int)c) != NULL) name[i] = '_';
  }
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  c = (char)tolower((unsigned char)c);
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

static void extract_filename(const char *url, char *out, size_t out_size) {
  const char *end = url + strcspn(url, "?#");
  const char *start = end;
  while (start > url && start[-1] != '/') start--;
  size_t pos = 0U;
  while (start < end && pos + 1U < out_size) {
    if (*start == '%' && start + 2 < end) {
      int hi = hex_value(start[1]);
      int lo = hex_value(start[2]);
      if (hi >= 0 && lo >= 0) {
        out[pos++] = (char)((hi << 4) | lo);
        start += 3;
        continue;
      }
    }
    out[pos++] = *start++;
  }
  out[pos] = '\0';
  sanitize_filename(out);
}

int transfer_url_supported(const char *url, char *reason, size_t reason_size) {
  if (reason != NULL && reason_size > 0U) reason[0] = '\0';
  if (url == NULL || url[0] == '\0') {
    if (reason != NULL) (void)snprintf(reason, reason_size, "Missing URL");
    return 0;
  }
#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
  if (has_scheme(url, "http://") || has_scheme(url, "https://") ||
      has_scheme(url, "ftp://") || has_scheme(url, "ftps://")) return 1;
#endif
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
  if (has_scheme(url, "nfs://")) return 1;
#endif
  if (reason != NULL && reason_size > 0U) {
    (void)snprintf(reason, reason_size,
                   "Unsupported URL scheme or backend unavailable");
  }
  return 0;
}

static transfer_job_t *find_job(int id) {
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    if (g_jobs[i].id == id) return &g_jobs[i];
  }
  return NULL;
}

static transfer_job_t *alloc_job(void) {
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE; i++) {
    if (g_jobs[i].id == 0 || atomic_load(&g_jobs[i].done) != 0) return &g_jobs[i];
  }
  return NULL;
}

static int run_backend(transfer_job_t *job, int fd) {
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
  if (has_scheme(job->url, "nfs://")) return transfer_backend_nfs_run(job, fd);
#endif
#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
  if (has_scheme(job->url, "http://") || has_scheme(job->url, "https://") ||
      has_scheme(job->url, "ftp://") || has_scheme(job->url, "ftps://")) {
    return transfer_backend_curl_run(job, fd);
  }
#endif
  transfer_job_set_error(job, "No transfer backend for URL");
  return -1;
}

static void *transfer_worker(void *opaque) {
  transfer_job_t *job = (transfer_job_t *)opaque;
  char final_path[TRANSFER_PATH_MAX + TRANSFER_NAME_MAX + 2U];
  char part_path[sizeof(final_path) + 16U];
  int n = snprintf(final_path, sizeof(final_path), "%s%s%s", job->dst_path,
                   strcmp(job->dst_path, "/") == 0 ? "" : "/", job->filename);
  if (n < 0 || (size_t)n >= sizeof(final_path)) {
    transfer_job_set_error(job, "Destination path too long");
    goto finished;
  }
  n = snprintf(part_path, sizeof(part_path), "%s.zftpd.part", final_path);
  if (n < 0 || (size_t)n >= sizeof(part_path)) {
    transfer_job_set_error(job, "Temporary path too long");
    goto finished;
  }

  struct stat st;
  job->resume_from = (stat(part_path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
                         ? (uint64_t)st.st_size
                         : 0U;
  atomic_store(&job->downloaded, job->resume_from);

  int fd = open(part_path, O_WRONLY | O_CREAT, 0644);
  if (fd < 0) {
    transfer_job_set_error(job, "Cannot create partial file: %s", strerror(errno));
    goto finished;
  }
  if (job->resume_from > 0U) {
    if (lseek(fd, (off_t)job->resume_from, SEEK_SET) < 0) {
      transfer_job_set_error(job, "Cannot resume partial file: %s", strerror(errno));
      (void)close(fd);
      goto finished;
    }
  } else if (ftruncate(fd, 0) != 0) {
    transfer_job_set_error(job, "Cannot truncate partial file: %s", strerror(errno));
    (void)close(fd);
    goto finished;
  }

  int rc = run_backend(job, fd);
  if (rc == 0 && fsync(fd) != 0) {
    transfer_job_set_error(job, "fsync failed: %s", strerror(errno));
    rc = -1;
  }
  (void)close(fd);

  if (rc == 0 && atomic_load(&job->cancel_requested) == 0) {
    if (rename(part_path, final_path) != 0) {
      transfer_job_set_error(job, "Final rename failed: %s", strerror(errno));
    }
  } else if (atomic_load(&job->cancel_requested) != 0) {
    (void)unlink(part_path);
  }

finished:
  atomic_store(&job->active, 0);
  atomic_store(&job->done, 1);
  return NULL;
}

int transfer_start(const char *url, const char *dst_dir, int *out_id,
                   char *out_name, size_t out_name_size,
                   char *error, size_t error_size) {
  char reason[TRANSFER_ERROR_MAX];
  if (!transfer_url_supported(url, reason, sizeof(reason)) || dst_dir == NULL) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "%s", reason[0] ? reason : "Invalid destination");
    return -1;
  }
  if (strlen(url) >= TRANSFER_URL_MAX || strlen(dst_dir) >= TRANSFER_PATH_MAX) {
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "Transfer URL or destination is too long");
    return -1;
  }

  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = alloc_job();
  if (job == NULL) {
    pthread_mutex_unlock(&g_jobs_lock);
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "Maximum concurrent transfers reached");
    return -1;
  }

  memset(job, 0, sizeof(*job));
  job->id = g_next_id++;
  (void)snprintf(job->url, sizeof(job->url), "%s", url);
  (void)snprintf(job->dst_path, sizeof(job->dst_path), "%s", dst_dir);
  extract_filename(url, job->filename, sizeof(job->filename));
  if (job->filename[0] == '\0') {
    (void)snprintf(job->filename, sizeof(job->filename), "download_%d", job->id);
  }
  job->start_time = time(NULL);
  atomic_store(&job->active, 1);

  pthread_t tid;
  pthread_attr_t attr;
  (void)pthread_attr_init(&attr);
  (void)pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  int thread_rc = pthread_create(&tid, &attr, transfer_worker, job);
  (void)pthread_attr_destroy(&attr);
  if (thread_rc != 0) {
    memset(job, 0, sizeof(*job));
    pthread_mutex_unlock(&g_jobs_lock);
    if (error != NULL && error_size > 0U)
      (void)snprintf(error, error_size, "Failed to start transfer thread");
    return -1;
  }

  if (out_id != NULL) *out_id = job->id;
  if (out_name != NULL && out_name_size > 0U)
    (void)snprintf(out_name, out_name_size, "%s", job->filename);
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

size_t transfer_snapshot_all(transfer_snapshot_t *out, size_t capacity) {
  if (out == NULL || capacity == 0U) return 0U;
  size_t count = 0U;
  pthread_mutex_lock(&g_jobs_lock);
  for (size_t i = 0U; i < TRANSFER_MAX_ACTIVE && count < capacity; i++) {
    transfer_job_t *job = &g_jobs[i];
    if (job->id == 0) continue;
    transfer_snapshot_t *snap = &out[count++];
    memset(snap, 0, sizeof(*snap));
    snap->id = job->id;
    snap->active = atomic_load(&job->active);
    snap->done = atomic_load(&job->done);
    snap->paused = atomic_load(&job->paused);
    snap->error = atomic_load(&job->error);
    snap->total_size = atomic_load(&job->total_size);
    snap->downloaded = atomic_load(&job->downloaded);
    (void)snprintf(snap->url, sizeof(snap->url), "%s", job->url);
    (void)snprintf(snap->filename, sizeof(snap->filename), "%s", job->filename);
    if (snap->error != 0) {
      (void)snprintf(snap->error_msg, sizeof(snap->error_msg), "%s",
                     job->error_msg);
    }
    double elapsed = difftime(time(NULL), job->start_time);
    uint64_t session_bytes = snap->downloaded >= job->resume_from
                                 ? snap->downloaded - job->resume_from
                                 : 0U;
    snap->speed = elapsed > 0.0 ? (double)session_bytes / elapsed : 0.0;
  }
  pthread_mutex_unlock(&g_jobs_lock);
  return count;
}

int transfer_toggle_pause(int id, int *out_paused) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL || atomic_load(&job->done) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  int paused = atomic_load(&job->paused) == 0 ? 1 : 0;
  atomic_store(&job->paused, paused);
  if (out_paused != NULL) *out_paused = paused;
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}

int transfer_cancel(int id) {
  pthread_mutex_lock(&g_jobs_lock);
  transfer_job_t *job = find_job(id);
  if (job == NULL || atomic_load(&job->done) != 0) {
    pthread_mutex_unlock(&g_jobs_lock);
    return -1;
  }
  atomic_store(&job->cancel_requested, 1);
  atomic_store(&job->paused, 0);
  pthread_mutex_unlock(&g_jobs_lock);
  return 0;
}
