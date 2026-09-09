#include "transfer_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <nfsc/libnfs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define NFS_READ_CAP (1024U * 1024U)

static void nfs_error(transfer_job_t *job, struct nfs_context *nfs,
                      const char *operation, int rc) {
  const char *detail = nfs != NULL ? nfs_get_error(nfs) : NULL;
  if (detail != NULL && detail[0] != '\0') {
    transfer_job_set_error(job, "NFS %s failed (%d): %s", operation, rc,
                           detail);
  } else {
    transfer_job_set_error(job, "NFS %s failed (%d)", operation, rc);
  }
}

int transfer_backend_nfs_run(transfer_job_t *job, int fd) {
  if (job == NULL || fd < 0) return -1;

  struct nfs_context *nfs = nfs_init_context();
  if (nfs == NULL) {
    transfer_job_set_error(job, "NFS context allocation failed");
    return -1;
  }
  nfs_set_autoreconnect(nfs, 3);

  struct nfs_url *url = nfs_parse_url_full(nfs, job->url);
  if (url == NULL) {
    nfs_error(job, nfs, "URL parse", -1);
    nfs_destroy_context(nfs);
    return -1;
  }

  int rc = nfs_mount(nfs, url->server, url->path);
  if (rc != 0) {
    nfs_error(job, nfs, "mount", rc);
    nfs_destroy_url(url);
    nfs_destroy_context(nfs);
    return -1;
  }

  struct nfsfh *fh = NULL;
  rc = nfs_open(nfs, url->file, O_RDONLY, &fh);
  if (rc != 0) {
    nfs_error(job, nfs, "open", rc);
    (void)nfs_umount(nfs);
    nfs_destroy_url(url);
    nfs_destroy_context(nfs);
    return -1;
  }

  struct nfs_stat_64 st;
  memset(&st, 0, sizeof(st));
  rc = nfs_fstat64(nfs, fh, &st);
  if (rc != 0) {
    nfs_error(job, nfs, "fstat", rc);
    goto cleanup;
  }

  atomic_store(&job->total_size, st.nfs_size);
  if (job->resume_from > st.nfs_size) {
    if (ftruncate(fd, 0) != 0 || lseek(fd, 0, SEEK_SET) < 0) {
      transfer_job_set_error(job, "Cannot reset oversized partial: %s",
                             strerror(errno));
      rc = -1;
      goto cleanup;
    }
    job->resume_from = 0U;
    atomic_store(&job->downloaded, 0U);
  } else if (job->resume_from == st.nfs_size) {
    atomic_store(&job->downloaded, st.nfs_size);
    rc = 0;
    goto cleanup;
  }

  size_t chunk = nfs_get_readmax(nfs);
  if (chunk == 0U || chunk > NFS_READ_CAP) chunk = NFS_READ_CAP;
  unsigned char *buffer = (unsigned char *)malloc(chunk);
  if (buffer == NULL) {
    transfer_job_set_error(job, "NFS read buffer allocation failed");
    rc = -1;
    goto cleanup;
  }

  uint64_t offset = job->resume_from;
  rc = 0;
  while (offset < st.nfs_size) {
    if (transfer_job_wait_if_paused(job) != 0) {
      transfer_job_set_error(job, "Cancelled by user");
      rc = -1;
      break;
    }
    size_t want = chunk;
    uint64_t remain = st.nfs_size - offset;
    if (remain < (uint64_t)want) want = (size_t)remain;
    int got = nfs_pread(nfs, fh, offset, (uint64_t)want, buffer);
    if (got < 0) {
      nfs_error(job, nfs, "read", got);
      rc = -1;
      break;
    }
    if (got == 0) {
      transfer_job_set_error(job, "NFS server returned EOF before expected size");
      rc = -1;
      break;
    }
    if (transfer_write_all(fd, buffer, (size_t)got) != 0) {
      transfer_job_set_error(job, "Local write failed: %s", strerror(errno));
      rc = -1;
      break;
    }
    offset += (uint64_t)got;
    atomic_store(&job->downloaded, offset);
  }
  free(buffer);

cleanup:
  if (fh != NULL) (void)nfs_close(nfs, fh);
  (void)nfs_umount(nfs);
  nfs_destroy_url(url);
  nfs_destroy_context(nfs);
  return rc == 0 ? 0 : -1;
}
