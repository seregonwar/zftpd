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

/** @file copy.c @brief Atomic single-file copy pipeline. */

#include "pal_fileio.h"
#include "fileio_internal.h"
#include "ftp_log.h"
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* File-to-file copies use large writes; network write chunk limits do not apply. */
#ifndef PAL_FILE_COPY_BUFFER_SIZE
#if defined(PLATFORM_PS5)
#define PAL_FILE_COPY_BUFFER_SIZE                                              \
  (4U * 1024U *                                                                \
   1024U) /* 4 MB — NVMe ~215 MB/s, T_write=19ms covers T_read=12ms */
#elif defined(PLATFORM_PS4)
#define PAL_FILE_COPY_BUFFER_SIZE                                              \
  (1024U * 1024U) /* 1 MB — HDD ~85 MB/s,  T_write=12ms covers T_read=3ms  */
#else
#define PAL_FILE_COPY_BUFFER_SIZE (4U * 1024U * 1024U) /* 4 MB */
#endif
#endif

/* Large copies overlap source reads and destination writes. */
#include <pthread.h>

typedef struct {
  uint8_t *buf[2]; /* two PAL_FILE_COPY_BUFFER_SIZE buffers          */
  size_t len[2];   /* bytes filled in each buffer (0 = free)         */
  int fill_idx;    /* index reader is filling right now              */
  int src_fd;      /* source file descriptor                         */
  size_t buf_sz;   /* PAL_FILE_COPY_BUFFER_SIZE                      */
  int done;        /* reader set to 1 on EOF or error                */
  int reader_err;  /* errno from reader (0 = ok)                     */
  pthread_mutex_t mtx;
  pthread_cond_t cv_ready; /* writer waits: "buffer filled and ready"        */
  pthread_cond_t cv_free;  /* reader waits: "buffer drained and free"        */
} copy_pipe_t;

static void *copy_reader_thread(void *arg) {
  copy_pipe_t *p = (copy_pipe_t *)arg;

  pthread_mutex_lock(&p->mtx);
  for (;;) {
    int fi = p->fill_idx;

    while ((p->len[fi] != 0U) && (p->done == 0)) {
      pthread_cond_wait(&p->cv_free, &p->mtx);
    }
    if (p->done != 0) {
      break;
    }

    pthread_mutex_unlock(&p->mtx);

    ssize_t n;
    do {
      n = read(p->src_fd, p->buf[fi], p->buf_sz);
    } while ((n < 0) && (errno == EINTR));

    pthread_mutex_lock(&p->mtx);

    if (n < 0) {
      p->reader_err = errno;
      p->done = 1;
      pthread_cond_signal(&p->cv_ready);
      break;
    }
    if (n == 0) {
      p->done = 1;
      pthread_cond_signal(&p->cv_ready);
      break;
    }

    p->len[fi] = (size_t)n;
    p->fill_idx = 1 - fi;
    pthread_cond_signal(&p->cv_ready);
  }
  pthread_mutex_unlock(&p->mtx);
  return NULL;
}

#define PAL_COPY_PIPELINE_MIN_BYTES ((uint64_t)PAL_FILE_COPY_BUFFER_SIZE * 2U)

static int copy_write_full(int fd, const uint8_t *data, size_t size,
                           int *saved_errno) {
  size_t offset = 0U;
  while (offset < size) {
    ssize_t n = write(fd, data + offset, size - offset);
    if (n > 0) {
      offset += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (saved_errno != NULL) *saved_errno = (n == 0) ? ENOSPC : errno;
    return -1;
  }
  return 0;
}

static ftp_error_t copy_report_progress(uint64_t bytes, uint64_t *cumulative,
                                        pal_copy_progress_cb_t cb,
                                        void *user_data) {
  uint64_t total = bytes;
  if (cumulative != NULL) {
    *cumulative += bytes;
    total = *cumulative;
  }
  if (cb != NULL && cb(total, user_data) < 0) return FTP_ERR_CANCELLED;
  return FTP_OK;
}

static atomic_uint_fast32_t g_tmp_counter = ATOMIC_VAR_INIT(0U);

static ftp_error_t build_temp_path(const char *dst_path, char *out,
                                   size_t out_size) {
  char parent[FTP_PATH_MAX];
  ftp_error_t err = fileio_parent_path(dst_path, parent, sizeof(parent));
  if (err != FTP_OK) return err;

  char name[64];
  uint_fast32_t counter = atomic_fetch_add(&g_tmp_counter, 1U);
  int n = snprintf(name, sizeof(name), ".zftpd.%lu.%lu.tmp",
                   (unsigned long)getpid(), (unsigned long)counter);
  if (n < 0 || (size_t)n >= sizeof(name)) return FTP_ERR_PATH_TOO_LONG;
  return fileio_join_path(parent, name, out, out_size);
}

static ftp_error_t create_temp_file(const char *dst_path, mode_t mode,
                                    char *tmp_path, size_t tmp_size,
                                    int *fd_out, int *out_errno) {
  if (fd_out == NULL) return FTP_ERR_INVALID_PARAM;
  for (unsigned attempt = 0U; attempt < 16U; attempt++) {
    ftp_error_t err = build_temp_path(dst_path, tmp_path, tmp_size);
    if (err != FTP_OK) return err;
    int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifndef PLATFORM_PS4
#ifndef PLATFORM_PS5
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#endif
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int fd = open(tmp_path, flags, mode);
    if (fd >= 0) { *fd_out = fd; return FTP_OK; }
    int error = errno;
    if (error == EEXIST) continue;
    if (out_errno != NULL) *out_errno = error;
    return fileio_error_from_errno(error, FTP_ERR_FILE_OPEN);
  }
  if (out_errno != NULL) *out_errno = EEXIST;
  return FTP_ERR_FILE_OPEN;
}

/*===========================================================================*
 * FILE OPERATIONS
 *===========================================================================*/

ftp_error_t
pal_file_copy_atomic_ex(const char *src_path, const char *dst_path,
                        pal_copy_progress_cb_t cb, void *user_data,
                        uint64_t *cumulative, int *out_errno) {
  if ((src_path == NULL) || (dst_path == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }
  if (out_errno != NULL) *out_errno = 0;

  struct stat st;
  if (stat(src_path, &st) < 0)
    return fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);

  if ((st.st_mode & S_IFMT) != S_IFREG) {
    return FTP_ERR_INVALID_PARAM;
  }

  int src_fd = -1;
  int dst_fd = -1;
  int tmp_created = 0;
  uint8_t *copy_buf = NULL;
  size_t copy_buf_size = 0U;
  ftp_error_t out_err = FTP_ERR_FILE_WRITE;

  char tmp_path[FTP_PATH_MAX] = {0};

  src_fd = open(src_path, O_RDONLY);

  /* Source pages are one-shot copy data; avoid polluting the page cache where supported. */
  if (src_fd >= 0) {
#ifdef F_NOCACHE
    (void)fcntl(src_fd, F_NOCACHE, 1);
#endif
#ifdef F_RDAHEAD
    (void)fcntl(src_fd, F_RDAHEAD, 1);
#endif
#if defined(POSIX_FADV_SEQUENTIAL) && !defined(PLATFORM_PS4) && !defined(PS4)
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
  if (src_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] open(src) failed: errno=%d path=%.*s", e,
               PAL_LOG_PATH_CHARS, src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (out_errno != NULL) {
      *out_errno = e;
    }
    out_err = fileio_error_from_errno(e, FTP_ERR_FILE_OPEN);
    goto cleanup;
  }

  mode_t mode = (mode_t)(st.st_mode & 0777);

  /* PFS may report a full filesystem as write()==0, so reject known ENOSPC early. */
  {
    char dst_dir[FTP_PATH_MAX];
    if (fileio_parent_path(dst_path, dst_dir, sizeof(dst_dir)) == FTP_OK) {
      struct statvfs vfs;
      if (statvfs(dst_dir, &vfs) == 0) {
        uint64_t free_bytes =
            (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        uint64_t need_bytes = (uint64_t)st.st_size;
        if (need_bytes > free_bytes) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] pre-flight ENOSPC: need=%llu free=%llu dst=%.*s",
                   (unsigned long long)need_bytes,
                   (unsigned long long)free_bytes, PAL_LOG_PATH_CHARS,
                   dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = ENOSPC;
          }
          return FTP_ERR_FILE_WRITE;
        }
      }
    }
  }

  out_err = create_temp_file(dst_path, mode, tmp_path, sizeof(tmp_path),
                             &dst_fd, out_errno);
  if (out_err != FTP_OK) {
    if (out_errno != NULL && *out_errno != 0) {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] create temp failed: errno=%d dst=%.*s", *out_errno,
               PAL_LOG_PATH_CHARS, dst_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    goto cleanup;
  }
  tmp_created = 1;

  /* VM-backed buffers avoid fragmenting the daemon buddy allocator. */
  if (st.st_size == 0) goto copy_done;

  if ((uint64_t)st.st_size >= PAL_COPY_PIPELINE_MIN_BYTES) {
    uint8_t *dbuf0 = (uint8_t *)mmap(NULL, PAL_FILE_COPY_BUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dbuf0 == MAP_FAILED) { dbuf0 = NULL; }
    uint8_t *dbuf1 = (uint8_t *)mmap(NULL, PAL_FILE_COPY_BUFFER_SIZE,
                                      PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (dbuf1 == MAP_FAILED) { dbuf1 = NULL; }

    if ((dbuf0 != NULL) && (dbuf1 != NULL)) {
      copy_pipe_t pipe;
      pipe.buf[0] = dbuf0;
      pipe.buf[1] = dbuf1;
      pipe.len[0] = 0U;
      pipe.len[1] = 0U;
      pipe.fill_idx = 0;
      pipe.src_fd = src_fd;
      pipe.buf_sz = (size_t)PAL_FILE_COPY_BUFFER_SIZE;
      pipe.done = 0;
      pipe.reader_err = 0;
      pthread_mutex_init(&pipe.mtx, NULL);
      pthread_cond_init(&pipe.cv_ready, NULL);
      pthread_cond_init(&pipe.cv_free, NULL);

      pthread_t reader_tid;
      int pt_ret = pthread_create(&reader_tid, NULL, copy_reader_thread, &pipe);
      int thread_ok = (pt_ret == 0) ? 1 : 0;

      /* Log pthread_create result — on PS4 this can fail with EAGAIN (thread
       * limit) or ENOMEM (stack allocation failed under memory pressure). */
      if (thread_ok == 0) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] pthread_create failed: errno=%d — "
                 "falling back to serial copy for %.*s",
                 pt_ret, PAL_LOG_PATH_CHARS, src_path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }

      if (thread_ok != 0) {
        ssize_t written = 0; /* last write result — checked after join */
        int write_errno = 0; /* saved errno from the last failed write();
                              * hoisted outside the for loop so it remains
                              * accessible after break for the post-join log */
        pthread_mutex_lock(&pipe.mtx);
        for (;;) {
          while ((pipe.len[1 - pipe.fill_idx] == 0U) && (pipe.done == 0)) {
            pthread_cond_wait(&pipe.cv_ready, &pipe.mtx);
          }

          int drain_idx = 1 - pipe.fill_idx;
          size_t nbytes = pipe.len[drain_idx];

          if ((nbytes == 0U) && (pipe.done != 0)) {
            break;
          }

          pthread_mutex_unlock(&pipe.mtx);

          /* Keep file-to-file writes large to avoid PFS per-write overhead. */
          write_errno = 0;
          written = copy_write_full(dst_fd, pipe.buf[drain_idx], nbytes,
                                    &write_errno) == 0
                        ? (ssize_t)nbytes
                        : -1;

          pthread_mutex_lock(&pipe.mtx);

          if (written < 0) {
            /*
             * Write error — signal reader to stop, then break.
             * We log below after joining the reader thread.
             */
            if (out_errno != NULL) {
              *out_errno = write_errno;
            }
            pipe.done = 1;
            pthread_cond_signal(&pipe.cv_free);
            out_err = FTP_ERR_FILE_WRITE;
            break;
          }

          if (copy_report_progress((uint64_t)nbytes, cumulative, cb,
                                   user_data) == FTP_ERR_CANCELLED) {
            pipe.done = 1;
            pthread_cond_signal(&pipe.cv_free);
            out_err = FTP_ERR_CANCELLED;
            break;
          }

          pipe.len[drain_idx] = 0U;
          pthread_cond_signal(&pipe.cv_free);
        }
        pthread_mutex_unlock(&pipe.mtx);

        (void)pthread_join(reader_tid, NULL);

        /* Preserve write errors ahead of reader errors and cancellation. */
        if (written < 0) {
          char msg[256];
          snprintf(msg, sizeof(msg), "[COPY] write failed: errno=%d dst=%.*s",
                   write_errno, PAL_LOG_PATH_CHARS, dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = write_errno;
          }
          out_err = FTP_ERR_FILE_WRITE;
        } else if (pipe.reader_err != 0) {
          char msg[256];
          snprintf(msg, sizeof(msg), "[COPY] read failed: errno=%d src=%.*s",
                   pipe.reader_err, PAL_LOG_PATH_CHARS, src_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = pipe.reader_err;
          }
          out_err = FTP_ERR_FILE_READ;
        } else if (out_err == FTP_ERR_CANCELLED) {
        } else {
          out_err = FTP_OK;
        }
      } else {
        out_err = FTP_ERR_FILE_WRITE;
      }

      pthread_mutex_destroy(&pipe.mtx);
      pthread_cond_destroy(&pipe.cv_ready);
      pthread_cond_destroy(&pipe.cv_free);

      (void)munmap(dbuf0, PAL_FILE_COPY_BUFFER_SIZE);
      (void)munmap(dbuf1, PAL_FILE_COPY_BUFFER_SIZE);

      if (thread_ok != 0) {
        if (out_err != FTP_OK) {
          goto cleanup;
        }
        goto copy_done;
      }
    } else {
      {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] pipeline mmap failed (buf0=%s buf1=%s) — "
                 "falling back to serial copy for %.*s",
                 (dbuf0 != NULL) ? "ok" : "NULL",
                 (dbuf1 != NULL) ? "ok" : "NULL",
                 PAL_LOG_PATH_CHARS, src_path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      if (dbuf0 != NULL) { (void)munmap(dbuf0, PAL_FILE_COPY_BUFFER_SIZE); }
      if (dbuf1 != NULL) { (void)munmap(dbuf1, PAL_FILE_COPY_BUFFER_SIZE); }
    }
  }

  copy_buf_size = ((uint64_t)st.st_size < (uint64_t)PAL_FILE_COPY_BUFFER_SIZE)
                      ? (size_t)st.st_size
                      : (size_t)PAL_FILE_COPY_BUFFER_SIZE;
  copy_buf = (uint8_t *)mmap(NULL, copy_buf_size, PROT_READ | PROT_WRITE,
                              MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (copy_buf == MAP_FAILED) {
    copy_buf = NULL;
  }
  if (copy_buf == NULL) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] serial mmap failed: bufsz=%u errno=%d src=%.*s",
               (unsigned)copy_buf_size,
               errno,
               PAL_LOG_PATH_CHARS, src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    out_err = FTP_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }

  {
    uint64_t serial_written = 0U;
    for (;;) {
      ssize_t r = read(src_fd, copy_buf, copy_buf_size);
      if (r > 0) {
        /* The serial fallback uses the same large-write primitive as the pipeline. */
        int write_errno = 0;
        if (copy_write_full(dst_fd, copy_buf, (size_t)r, &write_errno) != 0) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] write failed: errno=%d written_so_far=%llu "
                   "file_size=%llu dst=%.*s",
                   write_errno, (unsigned long long)serial_written,
                   (unsigned long long)st.st_size, PAL_LOG_PATH_CHARS,
                   dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) *out_errno = write_errno;
          out_err = FTP_ERR_FILE_WRITE;
          goto cleanup;
        }
        serial_written += (uint64_t)r;


        if (copy_report_progress((uint64_t)r, cumulative, cb, user_data) ==
            FTP_ERR_CANCELLED) {
          out_err = FTP_ERR_CANCELLED;
          goto cleanup;
        }
        continue;
      }
      if (r == 0) {
        break;
      }
      if (errno == EINTR) {
        continue;
      }
      {
        int e = errno;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] read failed: errno=%d written_so_far=%llu src=%.*s",
                 e, (unsigned long long)serial_written, PAL_LOG_PATH_CHARS,
                 src_path);
        ftp_log_line(FTP_LOG_WARN, msg);
        if (out_errno != NULL) {
          *out_errno = e;
        }
      }
      out_err = FTP_ERR_FILE_READ;
    goto cleanup;
  } /* end serial for(;;) loop */
  } /* end serial_written scope */

  out_err = FTP_OK;

copy_done:;

  /* Evict only clean source pages; DONTNEED on writable PFS fds can force a costly sync. */
#if defined(POSIX_FADV_DONTNEED) && !defined(PLATFORM_PS4) && !defined(PS4)
  if (src_fd >= 0) {
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_DONTNEED);
  }
#endif

  if (rename(tmp_path, dst_path) < 0) {
    {
      int e = errno;
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] rename(tmp->dst) failed: errno=%d tmp=%.*s dst=%.*s",
               e, PAL_LOG_PATH_PAIR_CHARS, tmp_path, PAL_LOG_PATH_PAIR_CHARS,
               dst_path);
      ftp_log_line(FTP_LOG_WARN, msg);
      if (out_errno != NULL) {
        *out_errno = e;
      }
    }
    out_err = FTP_ERR_FILE_WRITE;
    goto cleanup;
  }

  out_err = FTP_OK;

cleanup:
  if (copy_buf != NULL) {
    (void)munmap(copy_buf, copy_buf_size);
  }
  if (dst_fd >= 0) {
    (void)close(dst_fd);
  }
  if (src_fd >= 0) {
    (void)close(src_fd);
  }
  if (out_err != FTP_OK && tmp_created != 0) {
    (void)unlink(tmp_path);
  }
  return out_err;
}
