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

/** @file transfer.c @brief FTP RETR/STOR/APPE/REST transfer commands. */
#include "ftp_commands.h"
#include "ftp_buffer_pool.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include "pal_filesystem.h"
#include "pal_network.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
#include <sys/mount.h>
#endif
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <sys/mount.h>
extern int _fstatfs(int, struct statfs *);
#endif

/* Fallback: pal_fileio.h may be suppressed by a transitive include guard */
#ifndef PAL_FILE_WRITE_CHUNK_MAX
#  if defined(PLATFORM_PS5) || defined(PS5)
#    define PAL_FILE_WRITE_CHUNK_MAX 131072U  /* 128 KB */
#  elif defined(PLATFORM_PS4) || defined(PS4)
#    define PAL_FILE_WRITE_CHUNK_MAX  65536U  /*  64 KB */
#  else
#    define PAL_FILE_WRITE_CHUNK_MAX 262144U  /* 256 KB */
#  endif
#endif

/* PFS serializes inode creation on PS4/PS5. Serialize only O_CREAT opens
 * in userspace to avoid multi-session journal contention and client timeouts. */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static pthread_mutex_t g_pfs_create_mtx = PTHREAD_MUTEX_INITIALIZER;

/* pthread_mutex_timedlock() is absent from the OrbisOS (PS4/PS5) SDK.
 * This helper polls with trylock + nanosleep to emulate a timed lock.
 * Returns 0 on success, ETIMEDOUT if the deadline passes. */
static int pfs_mutex_lock_timeout(pthread_mutex_t *mtx, int timeout_s) {
  struct timespec sleep_ts;
  sleep_ts.tv_sec = 0;
  sleep_ts.tv_nsec = 10000000; /* 10 ms */
  int elapsed_ms = 0;
  const int limit_ms = timeout_s * 1000;
  while (elapsed_ms < limit_ms) {
    if (pthread_mutex_trylock(mtx) == 0) {
      return 0;
    }
    nanosleep(&sleep_ts, NULL);
    elapsed_ms += 10;
  }
  return ETIMEDOUT;
}

/* exFAT/msdosfs/ufs bypass the PFS creation lock; unknown filesystems
 * use the conservative serialized path. */
static int pfs_needs_serialisation(const char *file_path) {
  /* Extract parent directory */
  char dir[FTP_PATH_MAX];
  const char *slash = strrchr(file_path, '/');
  if (slash != NULL) {
    size_t dlen = (size_t)(slash - file_path);
    if (dlen == 0U) {
      /* path like "/file" — parent is "/" */
      dir[0] = '/';
      dir[1] = '\0';
    } else if (dlen >= sizeof(dir)) {
      return 1;
    } else {
      memcpy(dir, file_path, dlen);
      dir[dlen] = '\0';
    }
  } else {
    return 1;
  }

  /* _fstatfs() is the OrbisOS fd-based statfs; more reliable than
   * path-based statfs() on PS4/PS5 for certain mount types. */
  int dfd = open(dir, O_RDONLY);
  if (dfd < 0) {
    return 1;
  }

  struct statfs sfs;
  memset(&sfs, 0, sizeof(sfs));
  int sfs_ok = (_fstatfs(dfd, &sfs) == 0);
  close(dfd);

  if (sfs_ok == 0) {
    return 1;
  }

  if (sfs.f_fstypename[0] != '\0') {
    if (strcmp(sfs.f_fstypename, "exfat") == 0 ||
        strcmp(sfs.f_fstypename, "msdosfs") == 0 ||
        strcmp(sfs.f_fstypename, "ufs") == 0) {
      return 0;
    }
  }

  return 1;
}
#endif

static ftp_error_t upload_open_file(ftp_session_t *session,
                                    const char *path, int flags,
                                    const char *failure_message,
                                    int *out_fd) {
  if (session == NULL || path == NULL || failure_message == NULL ||
      out_fd == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int held_pfs_mtx = 0;
  if ((flags & O_CREAT) != 0 && pfs_needs_serialisation(path)) {
    if (pfs_mutex_lock_timeout(&g_pfs_create_mtx, 10) != 0) {
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Server busy, please retry.");
    }
    held_pfs_mtx = 1;
  }
#endif

  int fd = pal_file_open(path, flags, FILE_PERM);
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (held_pfs_mtx != 0) {
    pthread_mutex_unlock(&g_pfs_create_mtx);
  }
#endif
  if (fd < 0) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  failure_message);
  }
  *out_fd = fd;
  return FTP_OK;
}

static int upload_receive_single(ftp_session_t *session, int fd, void *buffer,
                                 size_t buffer_size, uint64_t *total_received,
                                 int *fail_stage, int *saved_errno) {
  if (buffer == NULL) {
    *fail_stage = 1;
    return -1;
  }
  for (;;) {
    ssize_t n = ftp_session_recv_data(session, buffer, buffer_size);
    if (n < 0) {
      if (errno == EINTR) continue;
      *saved_errno = errno;
      *fail_stage = 2;
      return -1;
    }
    if (n == 0) return 0;
    if (pal_file_write_all(fd, buffer, (size_t)n) != n) {
      *saved_errno = errno;
      *fail_stage = 3;
      return -1;
    }
    *total_received += (uint64_t)n;
    session->last_activity = time(NULL);
  }
}

static void upload_sync_file(int fd, int success) {
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  (void)fd;
  (void)success;
#elif defined(__linux__)
  if (success != 0) (void)fdatasync(fd);
#else
  if (success != 0) (void)fsync(fd);
#endif
}

static ftp_error_t upload_failure_reply(ftp_session_t *session,
                                        const char *event,
                                        uint64_t bytes_received,
                                        int fail_stage, int saved_errno) {
  ftp_log_session_event(session, event, FTP_ERR_UNKNOWN, bytes_received);
  char detail[128];
  if (fail_stage == 2) {
    (void)snprintf(detail, sizeof(detail),
                   "Transfer failed: network receive error (errno=%d).",
                   saved_errno);
  } else if (fail_stage == 3) {
    (void)snprintf(detail, sizeof(detail),
                   "Transfer failed: disk write error (errno=%d).",
                   saved_errno);
  } else {
    (void)snprintf(detail, sizeof(detail), "Transfer failed.");
  }
  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                detail);
}


ftp_error_t cmd_RETR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  vfs_node_t node;
  err = vfs_open(&node, resolved);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot open file.");
  }
  uint64_t file_size = vfs_get_size(&node);

  vfs_stat_t st;
  int have_stat = 0;
  if (vfs_stat(resolved, &st) == FTP_OK) {
    have_stat = 1;
  }
  if (have_stat != 0) {
    uint32_t fmt = (uint32_t)(st.mode & (uint32_t)S_IFMT);
    if (fmt != (uint32_t)S_IFREG) {
      vfs_close(&node);
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Not a regular file.");
    }
  }

  off_t offset = session->restart_offset;
  if ((offset < 0) || ((uint64_t)offset > file_size)) {
    vfs_close(&node);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid offset.");
  }
  vfs_set_offset(&node, (uint64_t)offset);

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    vfs_close(&node);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  size_t remaining = (size_t)(file_size - (uint64_t)offset);
  uint64_t bytes_sent = 0U;

  /* Encryption and throttling require userspace, so this RETR path cannot use them. */
  int use_sendfile = ((vfs_get_caps(&node) & VFS_CAP_SENDFILE) != 0U) &&
                     (FTP_TRANSFER_RATE_LIMIT_BPS == 0U);

  {
    char diag[320];
    /* Limit filename length to fit the fixed-size log buffer.
     * FTP_PATH_MAX (4096) exceeds sizeof(diag) so GCC warns about
     * potential truncation; use %.*s to bound the output explicitly. */
    const char *short_name = strrchr(resolved, '/');
    if (short_name != NULL) { short_name++; } else { short_name = resolved; }
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    struct statfs sfs;
    const char *fstype = "unknown";
    if (_fstatfs(node.fd, &sfs) == 0) { fstype = sfs.f_fstypename; }
    int max_fn = (int)(sizeof(diag) - 140);
    if (max_fn < 0) { max_fn = 0; }
    snprintf(diag, sizeof(diag),
      "[RETR] file=%.*s size=%llu fs=%s sendfile=%d "
      "chunk=%u eagain_sleep=%u sndbuf=%u",
      max_fn, short_name, (unsigned long long)file_size, fstype, use_sendfile,
      (unsigned)FTP_RETR_SENDFILE_CHUNK,
      (unsigned)FTP_SENDFILE_EAGAIN_SLEEP_US,
      (unsigned)FTP_TCP_DATA_SNDBUF);
#else
    int max_fn = (int)(sizeof(diag) - 80);
    if (max_fn < 0) { max_fn = 0; }
    snprintf(diag, sizeof(diag),
      "[RETR] file=%.*s size=%llu sendfile=%d chunk=%u",
      max_fn, short_name, (unsigned long long)file_size, use_sendfile,
      (unsigned)FTP_RETR_SENDFILE_CHUNK);
#endif
    ftp_log_line(FTP_LOG_INFO, diag);
  }
#if FTP_ENABLE_CRYPTO
  if (session->crypto.active != 0U) {
    use_sendfile = 0;
  }
#endif

  if (use_sendfile == 0) {
    vfs_close(&node);
    ftp_session_close_data_connection(session);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                  "sendfile unavailable (crypto/rate-limit active).");
  }

  /* RETR is intentionally sendfile-only: zero-copy succeeds or the transfer aborts. */

  pal_socket_cork(session->data_fd);

  while (remaining > 0U) {
    ssize_t sent =
        pal_sendfile(session->data_fd, node.fd, &offset,
                     (remaining > (size_t)FTP_RETR_SENDFILE_CHUNK)
                         ? (size_t)FTP_RETR_SENDFILE_CHUNK
                         : remaining);

    if (sent > 0) {
      remaining -= (size_t)sent;
      bytes_sent += (uint64_t)sent;
      session->last_activity = time(NULL);
      atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)sent);

      /* Evict pages already sent from the kernel page cache. */
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
      if (node.fd >= 0) {
        off_t evict_start = offset - (off_t)sent;
        if (evict_start >= 0) {
          (void)posix_fadvise(node.fd, evict_start,
                              (off_t)sent, POSIX_FADV_DONTNEED);
        }
      }
#endif
      continue;
    }

    if ((sent < 0) && (errno == EINTR)) {
      continue;
    }

    /* Retrying a bad vnode can panic PS4/PS5, so storage faults are terminal. */
    if ((sent < 0) && ((errno == EIO) || (errno == ESTALE) ||
                       (errno == EBADF) || (errno == EFAULT))) {
      remaining = 1U;
      break;
    }

    /* Retry bounded TCP back-pressure/driver stalls. */
    {
      int recovered = 0;
      for (int r = 0; r < FTP_SENDFILE_EAGAIN_RETRIES; r++) {
        usleep(FTP_SENDFILE_EAGAIN_SLEEP_US);
        ssize_t r_sent =
            pal_sendfile(session->data_fd, node.fd, &offset,
                         (remaining > (size_t)FTP_RETR_SENDFILE_CHUNK)
                             ? (size_t)FTP_RETR_SENDFILE_CHUNK
                             : remaining);
        if (r_sent > 0) {
          remaining -= (size_t)r_sent;
          bytes_sent += (uint64_t)r_sent;
          session->last_activity = time(NULL);
          atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)r_sent);
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
          if (node.fd >= 0) {
            off_t evict_start = offset - (off_t)r_sent;
            if (evict_start >= 0) {
              (void)posix_fadvise(node.fd, evict_start,
                                  (off_t)r_sent, POSIX_FADV_DONTNEED);
            }
          }
#endif
          recovered = 1;
          break;
        }
        if ((r_sent < 0) && (errno == EINTR)) {
          r--; /* don't count EINTR as a retry */
        }
        if ((r_sent < 0) && ((errno == EIO) || (errno == ESTALE) ||
                             (errno == EBADF) || (errno == EFAULT))) {
          break;
        }
      }

      if (recovered) {
        continue;
      }
    }

    remaining = 1U;
    break;
  }

  pal_socket_uncork(session->data_fd);

  vfs_close(&node);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (remaining == 0U) {
    atomic_fetch_add(&session->stats.files_sent, 1U);
    ftp_log_session_event(session, "RETR_OK", FTP_OK, bytes_sent);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }

  ftp_log_session_event(session, "RETR_FAIL", FTP_ERR_UNKNOWN, bytes_sent);
  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                "Transfer failed.");
}

/* Non-console STOR overlaps network receive with disk writes using two buffers. */

typedef struct {
  void *buf[2];
  size_t len[2];
  int active;
  int fd;
  int error;
  int done;
  uint64_t written;
  pthread_mutex_t mtx;
  pthread_cond_t cv_ready;
  pthread_cond_t cv_free;
} stor_pipe_t;

static void *stor_writer_thread(void *arg) {
  stor_pipe_t *p = (stor_pipe_t *)arg;

  pthread_mutex_lock(&p->mtx);
  for (;;) {
    while ((p->len[1 - p->active] == 0U) && (p->done == 0)) {
      pthread_cond_wait(&p->cv_ready, &p->mtx);
    }

    int drain = 1 - p->active;
    size_t nbytes = p->len[drain];

    if ((nbytes == 0U) && (p->done != 0)) {
      break;
    }

    pthread_mutex_unlock(&p->mtx);

    ssize_t w = pal_file_write_all(p->fd, p->buf[drain], nbytes);
    int write_ok = (w == (ssize_t)nbytes) ? 1 : 0;

    pthread_mutex_lock(&p->mtx);
    if (write_ok != 0) {
      p->written += (uint64_t)nbytes;
    } else {
      p->error = errno;
    }
    p->len[drain] = 0U;

    pthread_cond_signal(&p->cv_free);

    if (write_ok == 0) {
      break;
    }
  }
  pthread_mutex_unlock(&p->mtx);
  return NULL;
}

ftp_error_t cmd_STOR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Fresh non-console uploads use a temporary sibling and atomic rename. */
  char tmp_path[FTP_PATH_MAX];
  int was_fresh_upload = (session->restart_offset == 0) ? 1 : 0;
#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)
  /* Temp-file writes add significant PFS latency on consoles; write directly there. */
  int use_atomic = 0;
#else
  int use_atomic = (session->restart_offset == 0) ? 1 : 0;
#endif

  if (use_atomic != 0) {
    const char *slash = strrchr(resolved, '/');
    if (slash != NULL) {
      size_t dir_len = (size_t)(slash - resolved);
      size_t tail_len = strlen(slash + 1);
      /* Bound basename arithmetic before constructing the temporary path. */
      const size_t overhead = sizeof("/.zftpd.tmp.");
      if (dir_len < sizeof(tmp_path) - overhead) {
        size_t max_tail = sizeof(tmp_path) - dir_len - overhead;
        if (tail_len > max_tail) { tail_len = max_tail; }
      } else {
        tail_len = 0;
      }
      int n = snprintf(tmp_path, sizeof(tmp_path), "%.*s/.zftpd.tmp.%.*s",
                       (int)dir_len, resolved, (int)tail_len, slash + 1);
      (void)n;
    } else {
      size_t name_len = strlen(resolved);
      size_t max_name = sizeof(tmp_path) - sizeof(".zftpd.tmp.");
      if (name_len > max_name) { name_len = max_name; }
      int n = snprintf(tmp_path, sizeof(tmp_path), ".zftpd.tmp.%.*s",
                       (int)name_len, resolved);
      (void)n;
    }
  }

  const char *write_path = (use_atomic != 0) ? tmp_path : resolved;

  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) {
    open_flags |= O_TRUNC;
  }

  /* Open before 150/accept: PFS O_CREAT can block long enough to fill the
   * client-facing TCP receive window if data transfer has already started. */
  int fd = -1;
  err = upload_open_file(session, write_path, open_flags,
                         "Cannot create file.", &fd);
  if (err != FTP_OK) {
    if (use_atomic != 0) (void)unlink(tmp_path);
    return err;
  }

  if (session->restart_offset > 0) {
    if (lseek(fd, session->restart_offset, SEEK_SET) < 0) {
      pal_file_close(fd);
      if (use_atomic != 0) {
        (void)unlink(tmp_path);
      } else if (was_fresh_upload != 0) {
        (void)unlink(write_path);
      }
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Seek failed.");
    }
  }

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    pal_file_close(fd);
    if (use_atomic != 0) {
      (void)unlink(tmp_path);
    } else if (was_fresh_upload != 0) {
      (void)unlink(write_path);
    }
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  /* OrbisOS receive buffers cannot reliably absorb double-buffer writer stalls;
   * consoles therefore keep recv/write single-buffered. */

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  void *buf0 = ftp_buffer_acquire();
  void *buf1 = NULL;
#else
  void *buf0 = ftp_buffer_acquire();
  void *buf1 = ftp_buffer_acquire();
#endif
  size_t buf_sz = ftp_buffer_size();
  uint64_t total_received = 0U;
  int ok = 1;
  int fail_stage = 0; /* 1 = no buffer, 2 = recv error, 3 = write error */
  int saved_errno = 0;

  if ((buf0 == NULL) || (buf1 == NULL)) {
    void *buffer = (buf0 != NULL) ? buf0 : buf1;
    ftp_buffer_release((buf0 != NULL) ? buf1 : buf0);

    if (upload_receive_single(session, fd, buffer, buf_sz,
                              &total_received, &fail_stage,
                              &saved_errno) != 0) {
      ok = 0;
    }

    ftp_buffer_release(buffer);
  } else {
    stor_pipe_t pipe;
    pipe.buf[0] = buf0;
    pipe.buf[1] = buf1;
    pipe.len[0] = 0U;
    pipe.len[1] = 0U;
    pipe.active = 0;
    pipe.fd = fd;
    pipe.error = 0;
    pipe.done = 0;
    pipe.written = 0U;
    pthread_mutex_init(&pipe.mtx, NULL);
    pthread_cond_init(&pipe.cv_ready, NULL);
    pthread_cond_init(&pipe.cv_free, NULL);

    pthread_t writer;
    int thread_ok =
        (pthread_create(&writer, NULL, stor_writer_thread, &pipe) == 0) ? 1 : 0;
    if (thread_ok == 0) {
      if (upload_receive_single(session, fd, buf0, buf_sz,
                                &total_received, &fail_stage,
                                &saved_errno) != 0) {
        ok = 0;
      }
    } else {
      while (1) {
        pthread_mutex_lock(&pipe.mtx);

        while (pipe.len[pipe.active] != 0U) {
          if (pipe.error != 0) {
            pthread_mutex_unlock(&pipe.mtx);
            goto recv_done;
          }
          pthread_cond_wait(&pipe.cv_free, &pipe.mtx);
        }

        if (pipe.error != 0) {
          pthread_mutex_unlock(&pipe.mtx);
          break;
        }

        int fill_idx = pipe.active;
        pthread_mutex_unlock(&pipe.mtx);

        ssize_t n = ftp_session_recv_data(session, pipe.buf[fill_idx], buf_sz);
        if (n < 0) {
          if (errno == EINTR) {
            continue;
          }
          saved_errno = errno;
          fail_stage = 2;
          ok = 0;
          break;
        }
        if (n == 0) {
          break;
        }

        total_received += (uint64_t)n;
        session->last_activity = time(NULL);

        pthread_mutex_lock(&pipe.mtx);
        pipe.len[fill_idx] = (size_t)n;
        pipe.active = 1 - fill_idx;
        pthread_cond_signal(&pipe.cv_ready);
        pthread_mutex_unlock(&pipe.mtx);
      }

    recv_done:
      pthread_mutex_lock(&pipe.mtx);
      pipe.done = 1;
      pthread_cond_signal(&pipe.cv_ready);
      pthread_mutex_unlock(&pipe.mtx);

      (void)pthread_join(writer, NULL);

      if (pipe.error != 0) {
        saved_errno = pipe.error;
        fail_stage = 3;
        ok = 0;
      }
    }

    pthread_mutex_destroy(&pipe.mtx);
    pthread_cond_destroy(&pipe.cv_ready);
    pthread_cond_destroy(&pipe.cv_free);
    ftp_buffer_release(buf0);
    ftp_buffer_release(buf1);
  }

  upload_sync_file(fd, ok);

  /* Avoid POSIX_FADV_DONTNEED on writable PFS files: FreeBSD may turn it
   * into a synchronous crypto-backed flush with severe tail latency. */
  pal_file_close(fd);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    /* rename() exposes the completed file atomically. */
    if (use_atomic != 0) {
      if (rename(tmp_path, resolved) != 0) {
        (void)unlink(tmp_path);
        return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                      "Rename to final path failed.");
      }
    }

    atomic_fetch_add(&session->stats.files_received, 1U);
    ftp_log_session_event(session, "STOR_OK", FTP_OK, total_received);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }

  if (use_atomic != 0) {
    (void)unlink(tmp_path);
  } else if (was_fresh_upload != 0) {
    /* Remove failed fresh uploads; keep resumed partials for REST continuation. */
    (void)unlink(write_path);
  }

  return upload_failure_reply(session, "STOR_FAIL", total_received,
                              fail_stage, saved_errno);
}

/* APPE seeks to REST offset when set; otherwise it uses O_APPEND. */
ftp_error_t cmd_APPE(ftp_session_t *session, const char *args) {
  if (session == NULL || args == NULL) return FTP_ERR_INVALID_PARAM;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) open_flags |= O_APPEND;

  int fd = -1;
  err = upload_open_file(session, resolved, open_flags, "Cannot open file.", &fd);
  if (err != FTP_OK) return err;

  if (session->restart_offset > 0 &&
      lseek(fd, session->restart_offset, SEEK_SET) < 0) {
    pal_file_close(fd);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Seek failed.");
  }

  (void)ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    pal_file_close(fd);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  void *buffer = ftp_buffer_acquire();
  uint64_t total_received = 0U;
  int fail_stage = 0;
  int saved_errno = 0;
  int ok = upload_receive_single(session, fd, buffer, ftp_buffer_size(),
                                 &total_received, &fail_stage,
                                 &saved_errno) == 0;

  upload_sync_file(fd, ok);
  pal_file_close(fd);
  ftp_buffer_release(buffer);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    ftp_log_session_event(session, "APPE_OK", FTP_OK, total_received);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }
  return upload_failure_reply(session, "APPE_FAIL", total_received,
                              fail_stage, saved_errno);
}

ftp_error_t cmd_REST(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char *endptr;
  long long offset = strtoll(args, &endptr, 10);

  if ((*endptr != '\0') || (offset < 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid offset.");
  }

  session->restart_offset = (off_t)offset;

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "Restart position accepted.");
}
