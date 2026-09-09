#ifndef ZFTPD_TRANSFER_INTERNAL_H
#define ZFTPD_TRANSFER_INTERNAL_H

#include "transfer/transfer_manager.h"
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>

typedef struct {
  atomic_int active;
  atomic_int done;
  atomic_int paused;
  atomic_int cancel_requested;
  atomic_int error;
  int id;
  char url[TRANSFER_URL_MAX];
  char dst_path[TRANSFER_PATH_MAX];
  char filename[TRANSFER_NAME_MAX];
  char error_msg[TRANSFER_ERROR_MAX];
  atomic_uint_fast64_t total_size;
  atomic_uint_fast64_t downloaded;
  uint64_t resume_from;
  time_t start_time;
} transfer_job_t;

void transfer_job_set_error(transfer_job_t *job, const char *fmt, ...);
int transfer_job_wait_if_paused(transfer_job_t *job);
int transfer_write_all(int fd, const void *data, size_t len);

#if defined(ENABLE_LIBCURL) && ENABLE_LIBCURL
int transfer_backend_curl_run(transfer_job_t *job, int fd);
#endif
#if defined(ENABLE_LIBNFS) && ENABLE_LIBNFS
int transfer_backend_nfs_run(transfer_job_t *job, int fd);
#endif

#endif
