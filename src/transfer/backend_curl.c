#include "transfer_internal.h"

#include <curl/curl.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(ZFTPD_EMBEDDED_CA_BUNDLE) && ZFTPD_EMBEDDED_CA_BUNDLE
#include "zftpd_ca_bundle.h"
#endif

#define CURL_BUFFER_BYTES (512L * 1024L)

static pthread_once_t g_curl_once = PTHREAD_ONCE_INIT;
static CURLcode g_curl_init_result = CURLE_FAILED_INIT;

static void curl_global_init_once(void) {
  g_curl_init_result = curl_global_init(CURL_GLOBAL_DEFAULT);
}

typedef struct {
  transfer_job_t *job;
  int fd;
} curl_write_ctx_t;

static size_t curl_write_cb(void *ptr, size_t size, size_t nmemb, void *opaque) {
  curl_write_ctx_t *ctx = (curl_write_ctx_t *)opaque;
  if (ctx == NULL || ctx->job == NULL || ptr == NULL) return 0U;
  if (size != 0U && nmemb > SIZE_MAX / size) return 0U;
  if (transfer_job_wait_if_paused(ctx->job) != 0) return 0U;
  size_t bytes = size * nmemb;
  return transfer_write_all(ctx->fd, ptr, bytes) == 0 ? bytes : 0U;
}

static int curl_progress_cb(void *opaque, curl_off_t dltotal, curl_off_t dlnow,
                            curl_off_t ultotal, curl_off_t ulnow) {
  transfer_job_t *job = (transfer_job_t *)opaque;
  (void)ultotal;
  (void)ulnow;
  if (job == NULL || atomic_load(&job->cancel_requested) != 0) return 1;
  if (dltotal > 0) {
    atomic_store(&job->total_size, job->resume_from + (uint64_t)dltotal);
  }
  if (dlnow >= 0) {
    atomic_store(&job->downloaded, job->resume_from + (uint64_t)dlnow);
  }
  return 0;
}

static void curl_set_ca_path(CURL *curl) {
#if defined(ZFTPD_EMBEDDED_CA_BUNDLE) && ZFTPD_EMBEDDED_CA_BUNDLE
  struct curl_blob ca_blob;
  ca_blob.data = zftpd_ca_bundle;
  ca_blob.len = zftpd_ca_bundle_len;
  ca_blob.flags = CURL_BLOB_NOCOPY;
  (void)curl_easy_setopt(curl, CURLOPT_CAINFO_BLOB, &ca_blob);
#elif defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  static const char *const candidates[] = {
      "/user/homebrew/etc/ca-bundle.crt",
      "/data/zftpd/cacert.pem",
      "/data/cacert.pem",
      NULL};
  for (size_t i = 0U; candidates[i] != NULL; i++) {
    if (access(candidates[i], R_OK) == 0) {
      (void)curl_easy_setopt(curl, CURLOPT_CAINFO, candidates[i]);
      break;
    }
  }
#else
  (void)curl;
#endif
}

static void curl_configure(CURL *curl, transfer_job_t *job,
                           curl_write_ctx_t *wctx, char *error_buffer) {
  (void)curl_easy_setopt(curl, CURLOPT_URL, job->url);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, curl_write_cb);
  (void)curl_easy_setopt(curl, CURLOPT_WRITEDATA, wctx);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, curl_progress_cb);
  (void)curl_easy_setopt(curl, CURLOPT_XFERINFODATA, job);
  (void)curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  (void)curl_easy_setopt(curl, CURLOPT_ERRORBUFFER, error_buffer);
  (void)curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 10L);
  (void)curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
  (void)curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);
  (void)curl_easy_setopt(curl, CURLOPT_USERAGENT, "zftpd/1.5");
  (void)curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  (void)curl_easy_setopt(curl, CURLOPT_BUFFERSIZE, CURL_BUFFER_BYTES);
#if LIBCURL_VERSION_NUM >= 0x075500
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https,ftp,ftps");
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "http,https,ftp,ftps");
#else
  (void)curl_easy_setopt(curl, CURLOPT_PROTOCOLS,
                         (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS |
                                CURLPROTO_FTP | CURLPROTO_FTPS));
  (void)curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS,
                         (long)(CURLPROTO_HTTP | CURLPROTO_HTTPS |
                                CURLPROTO_FTP | CURLPROTO_FTPS));
#endif
  if (job->resume_from > 0U) {
    (void)curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                           (curl_off_t)job->resume_from);
  }
  curl_set_ca_path(curl);
}

static int curl_perform_once(transfer_job_t *job, int fd, CURLcode *out_code,
                             long *out_status) {
  CURL *curl = curl_easy_init();
  if (curl == NULL) {
    transfer_job_set_error(job, "curl_easy_init failed");
    return -1;
  }

  char error_buffer[CURL_ERROR_SIZE];
  error_buffer[0] = '\0';
  curl_write_ctx_t wctx = {job, fd};
  curl_configure(curl, job, &wctx, error_buffer);

  CURLcode code = curl_easy_perform(curl);
  long status = 0L;
  (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

#if LIBCURL_VERSION_NUM >= 0x073700
  curl_off_t total = 0;
  if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &total) ==
          CURLE_OK &&
      total > 0) {
    atomic_store(&job->total_size, job->resume_from + (uint64_t)total);
  }
#endif

  curl_easy_cleanup(curl);
  if (out_code != NULL) *out_code = code;
  if (out_status != NULL) *out_status = status;

  if (code == CURLE_OK) return 0;
  if (atomic_load(&job->cancel_requested) != 0) {
    transfer_job_set_error(job, "Cancelled by user");
  } else if (error_buffer[0] != '\0') {
    transfer_job_set_error(job, "curl: %s", error_buffer);
  } else {
    transfer_job_set_error(job, "curl: %s", curl_easy_strerror(code));
  }
  return -1;
}

int transfer_backend_curl_run(transfer_job_t *job, int fd) {
  if (job == NULL || fd < 0) return -1;
  (void)pthread_once(&g_curl_once, curl_global_init_once);
  if (g_curl_init_result != CURLE_OK) {
    transfer_job_set_error(job, "curl_global_init failed: %s",
                           curl_easy_strerror(g_curl_init_result));
    return -1;
  }

  CURLcode code = CURLE_OK;
  long status = 0L;
  if (curl_perform_once(job, fd, &code, &status) == 0) return 0;

  if (job->resume_from > 0U && atomic_load(&job->cancel_requested) == 0 &&
      (code == CURLE_RANGE_ERROR || status == 416L)) {
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
      transfer_job_set_error(job, "Cannot restart non-resumable download: %s",
                             strerror(errno));
      return -1;
    }
    job->resume_from = 0U;
    job->error_msg[0] = '\0';
    atomic_store(&job->error, 0);
    atomic_store(&job->downloaded, 0U);
    atomic_store(&job->total_size, 0U);
    return curl_perform_once(job, fd, &code, &status);
  }
  return -1;
}
