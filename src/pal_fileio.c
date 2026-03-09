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

/**
 * @file pal_fileio.c
 * @brief Platform Abstraction Layer - File I/O Implementation
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 */

#include "pal_fileio.h"
#include "ftp_log.h"
#include "pal_alloc.h"
#include "pal_network.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <unistd.h>

/* Fallback buffer size for non-sendfile platforms */
#define FALLBACK_BUFFER_SIZE FTP_BUFFER_SIZE

/*
 * PAL_FILE_WRITE_CHUNK_MAX is defined in pal_fileio.h.
 * This fallback guard protects against edge cases where a transitive include
 * of pal_fileio.h fires before our own #include above (e.g. via ftp_types.h),
 * causing the header guard to suppress the macro on the second pass.
 */
#ifndef PAL_FILE_WRITE_CHUNK_MAX
#  if defined(PLATFORM_PS5) || defined(PS5)
#    define PAL_FILE_WRITE_CHUNK_MAX 131072U  /* 128 KB */
#  elif defined(PLATFORM_PS4) || defined(PS4)
#    define PAL_FILE_WRITE_CHUNK_MAX  65536U  /*  64 KB */
#  else
#    define PAL_FILE_WRITE_CHUNK_MAX 262144U  /* 256 KB */
#  endif
#endif

/* Max recursion depth for cross-device directory move */
#define PAL_MOVE_MAX_DEPTH 64U

/*
 * PAL_FILE_COPY_BUFFER_SIZE — per-buffer size for the copy pipeline.
 *
 * PS5 USB→NVMe throughput analysis (exFAT → PFS, 12 GB file):
 *
 *   Observed serial throughput:   135 MB/s
 *   Target pipelined throughput:  215 MB/s
 *
 *   USB exFAT sequential read:   ~363 MB/s
 *   NVMe PFS sequential write:   ~215 MB/s
 *
 *   Serial model: 1/(1/363 + 1/215) = 135 MB/s  ✓ matches observation
 *   Pipelined:    min(363, 215)      = 215 MB/s  ✓ matches target
 *
 * PS4 USB→HDD/SSD throughput analysis:
 *
 *   USB exFAT sequential read:   ~320 MB/s
 *   HDD 5400 RPM PFS write:       ~85 MB/s  →  serial ~67 MB/s,  pipelined  85
 * MB/s (+27%) SSD aftermarket PFS write:   ~175 MB/s  →  serial ~113 MB/s,
 * pipelined 175 MB/s (+55%)
 *
 *   1 MB buffers are sufficient on PS4: T_read(1MB) = 3.3 ms is well under
 *   T_write_HDD(1MB) = 12.3 ms, so the reader always finishes before the
 *   writer and the pipeline never stalls.  4 MB buffers add memory pressure
 *   (PS4 daemon budget ~78 MB) with no throughput gain.
 *
 * The serial read+write loop leaves one device idle during every cycle.
 * A double-buffer pipeline overlaps USB reads with NVMe/HDD writes fully,
 * recovering the throughput gap on both platforms.
 *
 * IMPORTANT: the copy path writes the FULL buffer in a single write()
 * call, bypassing pal_file_write_all()'s PAL_WRITE_CHUNK_MAX subdivision.
 * That limit (128 KB on PS5, 64 KB on PS4) exists to prevent TCP stalls
 * in cmd_STOR; it has no relevance for a file-to-file copy.
 */
#ifndef PAL_FILE_COPY_BUFFER_SIZE
#if defined(PLATFORM_PS5)
#define PAL_FILE_COPY_BUFFER_SIZE                                              \
  (4U * 1024U *                                                                \
   1024U) /* 4 MB — NVMe ~215 MB/s, T_write=19ms covers T_read=12ms */
#elif defined(PLATFORM_PS4)
#define PAL_FILE_COPY_BUFFER_SIZE                                              \
  (1024U * 1024U) /* 1 MB — HDD ~85 MB/s,  T_write=12ms covers T_read=3ms  */
#else
#define PAL_FILE_COPY_BUFFER_SIZE FTP_BUFFER_SIZE
#endif
#endif

/*---------------------------------------------------------------------------*
 * DOUBLE-BUFFER COPY PIPELINE (PS4 and PS5)
 *
 * Two buffers alternate between a reader thread (USB exFAT) and the
 * calling thread (NVMe/HDD PFS writer), fully overlapping I/O:
 *
 *   Cycle N:    [read buf A from USB]  [write buf B to storage]
 *   Cycle N+1:  [read buf B from USB]  [write buf A to storage]
 *
 * PS5: writer (NVMe, ~215 MB/s) is the bottleneck; reader (USB, ~363 MB/s)
 *      always finishes first.  Net throughput: 215 MB/s.
 * PS4: writer (HDD, ~85 MB/s) is the bottleneck; reader (USB, ~320 MB/s)
 *      always finishes first.  Net throughput: ~85 MB/s (vs ~67 MB/s serial).
 *
 * Thread roles:
 *   Main thread   — writer: drains the filled buffer to dst_fd, calls
 *                           the progress callback, signals cv_free.
 *   Reader thread — reads from src_fd into the free buffer, signals
 *                           cv_ready when a full chunk is available.
 *
 * State machine (protected by pipe_mtx):
 *   fill_idx    — buffer currently being read into (0 or 1)
 *   drain_idx   — buffer currently being written out (1-fill_idx)
 *   len[i]      — bytes available in buffer i (0 = free, >0 = ready)
 *   done        — reader has hit EOF or error
 *   reader_err  — non-zero errno set by reader on error
 *---------------------------------------------------------------------------*/
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
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

    /* Wait until the fill buffer is free (len[fi] == 0) */
    while ((p->len[fi] != 0U) && (p->done == 0)) {
      pthread_cond_wait(&p->cv_free, &p->mtx);
    }
    if (p->done != 0) {
      break; /* main thread requested stop (write error or cancel) */
    }

    pthread_mutex_unlock(&p->mtx);

    /* Read without holding the lock — this is the slow USB read */
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
      /* EOF */
      p->done = 1;
      pthread_cond_signal(&p->cv_ready);
      break;
    }

    p->len[fi] = (size_t)n;
    p->fill_idx = 1 - fi; /* swap to the other buffer */
    pthread_cond_signal(&p->cv_ready);
  }
  pthread_mutex_unlock(&p->mtx);
  return NULL;
}
#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

/*===========================================================================*
 * ZERO-COPY FILE TRANSFER
 *===========================================================================*/

/**
 * @brief Send file data via socket (zero-copy)
 *
 * DESIGN RATIONALE:
 * - sendfile() eliminates userspace copy (kernel-direct transfer)
 * - Fallback to buffered I/O maintains portability
 * - Performance difference: 2-3x throughput improvement
 */
ssize_t pal_sendfile(int sock_fd, int file_fd, off_t *offset, size_t count) {
  /* Validate parameters */
  if ((sock_fd < 0) || (file_fd < 0)) {
    errno = EINVAL;
    return -1;
  }

  if ((offset == NULL) || (*offset < 0)) {
    errno = EINVAL;
    return -1;
  }

  if (count == 0U) {
    return 0;
  }

  /*
   * NOTE: PAL_FILE_WRITE_CHUNK_MAX is intentionally NOT applied here.
   *
   * That cap (128 KB on PS5, 64 KB on PS4) exists to keep per-chunk write
   * latency under ~5 ms so the FTP double-buffer producer never starves the
   * kernel TCP recv-buffer long enough to trigger a client inactivity timeout.
   * It is an FTP protocol concern, not a platform abstraction concern.
   *
   * Enforcing it here silently degraded HTTP /api/download throughput from
   * ~300 MB/s to ~240 MB/s: the HTTP server passes 1 MB chunks but each call
   * was internally cut to 128 KB, generating 8x more syscalls per MB.
   *
   * The FTP caller (ftp_commands.c cmd_RETR) now passes pre-capped chunks
   * using PAL_FILE_WRITE_CHUNK_MAX directly, so the limit is preserved where
   * it matters without penalising other callers.
   */

#if defined(__linux__)
  /*
   * Linux sendfile(2)
   * Signature: sendfile(out_fd, in_fd, offset, count)
   */
  ssize_t result = sendfile(sock_fd, file_fd, offset, count);
  return result;

#elif defined(__FreeBSD__) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * FreeBSD sendfile(2)
   * Signature: sendfile(in_fd, out_fd, offset, count, hdtr, sbytes, flags)
   *
   * IMPORTANT: Different parameter order than Linux!
   */
  off_t sbytes = 0;
  off_t start_offset = *offset;

  int ret = sendfile(file_fd, sock_fd, start_offset, count, NULL, &sbytes, 0);

  /*
   * Update offset by bytes actually sent.
   * FreeBSD updates sbytes even on EAGAIN (partial send).
   */
  if (sbytes > 0) {
    *offset += sbytes;
  }

  if (ret == 0) {
    /* Success: all bytes sent */
    return sbytes;
  } else if ((ret == -1) && (errno == EAGAIN)) {
    /* Non-blocking socket: partial send — caller retries */
    return sbytes;
  } else if ((ret == -1) && (errno == EINTR)) {
    /*
     * Interrupted by signal mid-transfer.
     *
     * IMPORTANT: *offset has already been advanced by sbytes above.
     * Returning -1 here would cause the caller to retry from the
     * old offset, re-sending bytes already transmitted — file
     * corruption guaranteed.
     *
     * Return sbytes (>= 0) so the caller advances its own position
     * and retries from the correct point.
     */
    return sbytes; /* may be 0 if interrupted before any byte sent */
  } else if ((ret == -1) && ((errno == EIO) || (errno == ESTALE) ||
                             (errno == EBADF) || (errno == EFAULT))) {
    /*
     * Fatal storage-level error during transfer.
     *
     * EIO    : underlying device I/O error (USB read failure, NVMe fault)
     * ESTALE : stale vnode — filesystem unmounted while transfer was in flight
     * EBADF  : fd invalidated — should never reach here, but guard anyway
     * EFAULT : kernel memory fault — extremely rare; never retry
     *
     * CRITICAL: ALWAYS return -1 here, regardless of sbytes.
     *
     * Previous behaviour returned sbytes when sbytes > 0, signalling
     * a "partial success" to the caller.  This caused the caller's loop
     * to decrement its remaining-byte counter and call pal_sendfile()
     * again on the same (now-corrupted/unmounted) vnode.  On PS5/PS4
     * that second call can trigger an unrecoverable kernel panic.
     *
     * Safety: *offset has already been advanced by sbytes above, so no
     * data is double-sent.  The caller sees -1, breaks out of its loop,
     * and closes the connection cleanly — which is the correct behaviour.
     *
     * FTP callers (ftp_commands.c): they detect sent < 0, check errno,
     * and skip the EAGAIN retry loop (see the EIO guard there), falling
     * through to the read()-based cooldown path without any sendfile retry.
     */
    return -1;

  } else {
    /*
     * EINVAL / ENOSYS / ENXIO / other unexpected sendfile() error.
     *
     * On PS5 and PS4, sendfile(2) is not supported for every file+socket
     * combination.  Known cases:
     *   - exFAT / FAT32 USB drives: sendfile can return EINVAL because
     *     the kernel vnode driver for exFAT does not implement the
     *     sendfile vnode operation.
     *   - Special pseudo-files (pipes, device nodes, etc.)
     *
     * If sbytes == 0 (sendfile failed before sending a single byte) we
     * transparently fall back to pread(2) + pal_send_all().  The caller
     * never notices the switch — it just receives the expected data.
     *
     * If sbytes > 0 (sendfile sent some data then failed with EINVAL,
     * which is unusual but possible) we continue the fallback from the
     * already-advanced *offset so no bytes are skipped or duplicated.
     *
     * IMPORTANT: this fallback MUST NOT be used for EIO/ESTALE — those
     * errors indicate the underlying storage is gone and pread() will
     * either also fail or return stale data from an inconsistent vnode.
     * They are handled in the branch above.
     */
    static _Thread_local char fb_buf[FALLBACK_BUFFER_SIZE];

    size_t fb_remaining = count - (size_t)sbytes;

    while (fb_remaining > 0U) {
      size_t chunk = (fb_remaining < (size_t)FALLBACK_BUFFER_SIZE)
                         ? fb_remaining
                         : (size_t)FALLBACK_BUFFER_SIZE;

      ssize_t nread = pread(file_fd, fb_buf, chunk, *offset);
      if (nread <= 0) {
        /* EOF or read error — return what we managed to send */
        return (sbytes > 0) ? sbytes : -1;
      }

      ssize_t nsent = pal_send_all(sock_fd, fb_buf, (size_t)nread, 0);
      if (nsent < 0) {
        return (sbytes > 0) ? sbytes : -1;
      }

      *offset      += (off_t)nsent;
      sbytes       += (off_t)nsent;
      fb_remaining -= (size_t)nsent;
    }

    return sbytes;
  }

#else
  /*
   * Fallback: Buffered read/write
   * Used on platforms without sendfile() support
   */
  static _Thread_local char buffer[FALLBACK_BUFFER_SIZE];

  /* Read from file at specified offset */
  ssize_t nread = pread(
      file_fd, buffer,
      (count < FALLBACK_BUFFER_SIZE) ? count : FALLBACK_BUFFER_SIZE, *offset);
  if (nread <= 0) {
    return nread; /* EOF or error */
  }

  /* Send via socket */
  ssize_t nsent = pal_send_all(sock_fd, buffer, (size_t)nread, 0);
  if (nsent < 0) {
    return -1;
  }
  *offset += nsent;
  return nsent;
#endif
}

/*===========================================================================*
 * FILE OPERATIONS
 *===========================================================================*/

static ftp_error_t
pal_file_copy_atomic_ex(const char *src_path, const char *dst_path,
                        pal_copy_progress_cb_t cb, void *user_data,
                        uint64_t *cumulative, int *out_errno) {
  if ((src_path == NULL) || (dst_path == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  struct stat st;
  if (stat(src_path, &st) < 0) {
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    default:
      return FTP_ERR_FILE_STAT;
    }
  }

  if ((st.st_mode & S_IFMT) != S_IFREG) {
    return FTP_ERR_INVALID_PARAM;
  }

  int src_fd = -1;
  int dst_fd = -1;
  uint8_t *copy_buf = NULL; /* heap-allocated; freed in cleanup */
  ftp_error_t out_err = FTP_ERR_FILE_WRITE;

  /*
   * TEMP FILENAME STRATEGY — safe for exFAT 255-char name limit
   *
   *  Old: "<dst_path>.zftpd-tmp-<pid>-<counter>"   (appends ~25 chars)
   *  New: "<dst_dir>/.zftpd.<pid>.<counter>.tmp"    (fixed short name)
   *
   *  The old scheme overflows when the destination filename already
   *  approaches 255 characters (common with PS5 game directories).
   *  The new scheme puts a short, fixed-length temp file in the same
   *  directory so the final same-FS rename() is always valid.
   */
  static atomic_uint_fast32_t g_tmp_counter = ATOMIC_VAR_INIT(0U);
  uint_fast32_t counter = atomic_fetch_add(&g_tmp_counter, 1U);

  /* Find parent directory of dst_path */
  char tmp_path[FTP_PATH_MAX];
  const char *last_slash = strrchr(dst_path, '/');
  if (last_slash != NULL) {
    size_t dir_len = (size_t)(last_slash - dst_path);
    int n = snprintf(tmp_path, sizeof(tmp_path), "%.*s/.zftpd.%lu.%lu.tmp",
                     (int)dir_len, dst_path, (unsigned long)getpid(),
                     (unsigned long)counter);
    if ((n < 0) || ((size_t)n >= sizeof(tmp_path))) {
      return FTP_ERR_PATH_TOO_LONG;
    }
  } else {
    int n = snprintf(tmp_path, sizeof(tmp_path), ".zftpd.%lu.%lu.tmp",
                     (unsigned long)getpid(), (unsigned long)counter);
    if ((n < 0) || ((size_t)n >= sizeof(tmp_path))) {
      return FTP_ERR_PATH_TOO_LONG;
    }
  }

  src_fd = open(src_path, O_RDONLY);

  /*
   * SOURCE FD CACHE POLICY — F_NOCACHE + POSIX_FADV_SEQUENTIAL
   *
   * ┌────────────────────────────────────────────────────────────────────┐
   * │  WHY F_NOCACHE ON THE SOURCE fd?                                   │
   * │                                                                    │
   * │  For a sequential file-to-file copy the source pages are read      │
   * │  exactly once and never needed again.  Without F_NOCACHE, every    │
   * │  read() places the just-read pages in the kernel page cache.  For  │
   * │  a 12 GB game file this fills all available daemon RSS (~3-4 GB    │
   * │  on PS5) within the first few seconds of the copy.  Once RAM is    │
   * │  full the kernel must evict older cache pages on every new read,   │
   * │  adding eviction overhead that degrades throughput from 400 MB/s   │
   * │  (clean cache) to ~250 MB/s (cache thrashing).                     │
   * │                                                                    │
   * │  F_NOCACHE (FreeBSD equivalent of O_DIRECT) bypasses the page      │
   * │  cache for this fd: source blocks are read directly into the       │
   * │  pipeline buffer without caching, keeping RAM pressure flat for    │
   * │  the entire duration of the copy regardless of file size.          │
   * │                                                                    │
   * │  SAFETY:                                                           │
   * │  • This fd is NEVER passed to sendfile() — pal_file_copy_atomic_ex │
   * │    uses read()+write() loops only.  The F_NOCACHE + sendfile()     │
   * │    KP concern described in pal_file_open() does NOT apply here.    │
   * │  • F_NOCACHE on the SOURCE (read) fd is safe on PS5 PFS even for  │
   * │    encrypted /data files: decryption happens inside the kernel     │
   * │    at the block layer before the data reaches userspace.           │
   * │  • F_NOCACHE is deliberately NOT set on the DESTINATION (write) fd │
   * │    (dst_fd, opened below).  The write-side has PFS alignment       │
   * │    constraints: F_NOCACHE forces 512-byte-aligned write() calls    │
   * │    and OrbisOS PFS returns EINVAL for unaligned writes, breaking   │
   * │    the copy.  Write caching is beneficial anyway (write-back       │
   * │    coalescing helps the NVMe controller build large sequential      │
   * │    extents rather than many small random writes).                  │
   * │                                                                    │
   * │  RESULT: sustained 400 MB/s for /data → /mnt/ext1 (USB-C M.2)    │
   * │  instead of 400 MB/s for small files / 250 MB/s for 12+ GB files. │
   * └────────────────────────────────────────────────────────────────────┘
   *
   * POSIX_FADV_SEQUENTIAL is also set: hints the kernel read-ahead engine
   * to prefetch large contiguous extents.  On PS5 this doubles read-ahead
   * window from the default (128 KB) to 2× the file system block size,
   * keeping the pipeline reader thread from stalling on rotational-latency-
   * equivalent NVMe seek delays between extents.
   */
#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)
  if (src_fd >= 0) {
#ifdef F_NOCACHE
    /*
     * Bypass page cache for source reads.
     * Failure is non-fatal: fall through to cached reads (slower but correct).
     */
    (void)fcntl(src_fd, F_NOCACHE, 1);
#endif
#ifdef POSIX_FADV_SEQUENTIAL
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
  }
#endif /* PLATFORM_PS5 || PLATFORM_PS4 */
  if (src_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] open(src) failed: errno=%d path=%s", e,
               src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (out_errno != NULL) {
      *out_errno = e;
    }
    switch (e) {
    case ENOENT:
      out_err = FTP_ERR_NOT_FOUND;
      break;
    case EACCES:
    case EPERM:
      out_err = FTP_ERR_PERMISSION;
      break;
    default:
      out_err = FTP_ERR_FILE_OPEN;
      break;
    }
    goto cleanup;
  }

  mode_t mode = (mode_t)(st.st_mode & 0777);

  /*
   * PRE-FLIGHT SPACE CHECK (PS4/PS5 PFS silent-full workaround)
   *
   * PFS on /data does not always propagate ENOSPC through write() — it
   * can silently return 0, leaving errno==0 and making the failure
   * completely opaque.  Check available space up front so we can abort
   * immediately with a clear diagnostic rather than failing mid-copy
   * with errno=0 deep inside the write pipeline.
   *
   * statvfs() on the DESTINATION directory (not src) gives us the free
   * blocks on the target filesystem.  We accept up to a ~1 % statvfs
   * race (file is being written by someone else), so the check is a
   * warning rather than a hard block — we still attempt the copy and
   * rely on the write-loop fix to surface ENOSPC if it happens anyway.
   */
  {
    /* Extract parent directory of dst_path for statvfs */
    char dst_dir[FTP_PATH_MAX];
    const char *dst_slash = strrchr(dst_path, '/');
    if (dst_slash != NULL && dst_slash != dst_path) {
      size_t dlen = (size_t)(dst_slash - dst_path);
      if (dlen < sizeof(dst_dir)) {
        memcpy(dst_dir, dst_path, dlen);
        dst_dir[dlen] = '\0';
      } else {
        dst_dir[0] = '\0'; /* fallback: skip check */
      }
    } else {
      dst_dir[0] = '/';
      dst_dir[1] = '\0';
    }

    if (dst_dir[0] != '\0') {
      struct statvfs vfs;
      if (statvfs(dst_dir, &vfs) == 0) {
        uint64_t free_bytes =
            (uint64_t)vfs.f_bavail * (uint64_t)vfs.f_frsize;
        uint64_t need_bytes = (uint64_t)st.st_size;
        if (need_bytes > free_bytes) {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] pre-flight ENOSPC: need=%llu free=%llu dst=%s",
                   (unsigned long long)need_bytes,
                   (unsigned long long)free_bytes, dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = ENOSPC;
          }
          return FTP_ERR_FILE_WRITE;
        }
      }
    }
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC, mode);
#else
  dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
#endif
  if (dst_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] open(tmp) failed: errno=%d path=%s", e,
               tmp_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (out_errno != NULL) {
      *out_errno = e;
    }
    switch (e) {
    case EACCES:
    case EPERM:
      out_err = FTP_ERR_PERMISSION;
      break;
    default:
      out_err = FTP_ERR_FILE_OPEN;
      break;
    }
    goto cleanup;
  }

  /*
   * DESIGN RATIONALE — heap vs. static _Thread_local:
   *
   * static _Thread_local would allocate PAL_FILE_COPY_BUFFER_SIZE (1 MB on
   * PS4/PS5) permanently for every thread that ever calls this function,
   * for the thread's entire lifetime — even when idle between transfers.
   * With N concurrent FTP sessions that means N MB of non-reclaimable RSS.
   *
   * A single malloc/free per copy call returns the memory immediately after
   * the operation, keeping the daemon's footprint minimal when idle.
   */
  /*=========================================================================*
   * COPY LOOP
   *
   * PS5:   double-buffer pipeline — reader thread (USB) runs in parallel
   *        with the writer (NVMe), achieving min(USB_bw, NVMe_bw) = 215 MB/s.
   *
   * Other: simple serial read→write loop.
   *=========================================================================*/

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*-----------------------------------------------------------------------*
   * PS4/PS5 double-buffer copy pipeline
   *
   * Allocate both buffers up-front.  If either malloc fails, fall through
   * to the serial path (pal_malloc returns NULL gracefully).
   *-----------------------------------------------------------------------*/
  {
    uint8_t *dbuf0 = (uint8_t *)pal_malloc(PAL_FILE_COPY_BUFFER_SIZE);
    uint8_t *dbuf1 = (uint8_t *)pal_malloc(PAL_FILE_COPY_BUFFER_SIZE);

    /* Log arena state immediately after the two allocs so we can correlate
     * with SceShellCore heap pressure messages in the system log. */
    {
      pal_alloc_stats_t ast;
      pal_alloc_get_stats(&ast);
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] pipeline alloc: buf0=%s buf1=%s "
               "arena_inuse=%llu peak=%llu failures=%llu file=%s",
               (dbuf0 != NULL) ? "ok" : "NULL",
               (dbuf1 != NULL) ? "ok" : "NULL",
               (unsigned long long)ast.bytes_in_use,
               (unsigned long long)ast.bytes_peak,
               (unsigned long long)ast.failures,
               src_path);
      ftp_log_line((dbuf0 && dbuf1) ? FTP_LOG_INFO : FTP_LOG_WARN, msg);
    }

    if ((dbuf0 != NULL) && (dbuf1 != NULL)) {
      /* Initialise pipeline state */
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
                 "falling back to serial copy for %s",
                 pt_ret, src_path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }

      if (thread_ok != 0) {
        /* Writer loop: drain the buffer that the reader just filled */
        ssize_t written = 0; /* last write result — checked after join */
        int write_errno = 0; /* saved errno from the last failed write();
                              * hoisted outside the for loop so it remains
                              * accessible after break for the post-join log */
        pthread_mutex_lock(&pipe.mtx);
        for (;;) {
          /* Wait for a filled buffer or EOF/error */
          while ((pipe.len[1 - pipe.fill_idx] == 0U) && (pipe.done == 0)) {
            pthread_cond_wait(&pipe.cv_ready, &pipe.mtx);
          }

          int drain_idx = 1 - pipe.fill_idx;
          size_t nbytes = pipe.len[drain_idx];

          if ((nbytes == 0U) && (pipe.done != 0)) {
            break; /* EOF — pipeline drained */
          }

          pthread_mutex_unlock(&pipe.mtx);

          /*
           * Write the full buffer in a single write() call.
           *
           * We intentionally bypass pal_file_write_all()'s
           * PAL_WRITE_CHUNK_MAX=128 KB subdivision here.  That limit
           * exists to prevent TCP recv-buffer stalls in cmd_STOR; it
           * has no relevance for a file-to-file copy.  A single 4 MB
           * write() is processed by the PFS driver as a sequential
           * extent, avoiding per-chunk AES-XTS context setup overhead.
           */
          written = 0;
          write_errno = 0; /* reset each iteration; saved before mutex lock */
          {
            const uint8_t *p_out = pipe.buf[drain_idx];
            size_t remaining = nbytes;
            while (remaining > 0U) {
              ssize_t w = write(dst_fd, p_out, remaining);
              if (w > 0) {
                p_out += (size_t)w;
                remaining -= (size_t)w;
                written += w;
                continue;
              }
              if ((w < 0) && (errno == EINTR)) {
                continue;
              }
              /*
               * Save errno NOW — pthread_mutex_lock() below can
               * overwrite it on success (POSIX does not guarantee
               * errno is untouched on a successful call).
               *
               * IMPORTANT — w == 0 case (PS4/PS5 PFS quirk):
               * POSIX does not define write() returning 0 for a
               * positive count on a regular file.  On Orbis/Prospero
               * PFS, a full filesystem silently returns 0 instead of
               * -1 + ENOSPC.  Detect this and synthesise ENOSPC so
               * the log always shows a meaningful errno value.
               */
              write_errno = (w == 0) ? ENOSPC : errno;
              written = -1;
              break;
            }
          }

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

          /* Report progress */
          if ((cb != NULL) && (cumulative != NULL)) {
            *cumulative += (uint64_t)written;
            if (cb(*cumulative, user_data) < 0) {
              pipe.done = 1;
              pthread_cond_signal(&pipe.cv_free);
              out_err = FTP_ERR_UNKNOWN; /* cancelled */
              break;
            }
          }

          /* Mark buffer as free and wake reader */
          pipe.len[drain_idx] = 0U;
          pthread_cond_signal(&pipe.cv_free);
        }
        pthread_mutex_unlock(&pipe.mtx);

        /* Join reader; collect any read-side error */
        (void)pthread_join(reader_tid, NULL);

        /*
         * Post-join result resolution.
         *
         * Priority: write error > read error > cancellation > success.
         *
         * IMPORTANT: do NOT use `out_err` to detect write failures here.
         * `out_err` is initialised to FTP_ERR_FILE_WRITE (line ~376) and
         * is only changed during the loop for cancellation (FTP_ERR_UNKNOWN)
         * or left at its initial value on both success AND write failure.
         * Using it as a success/failure discriminator therefore confuses the
         * two cases and causes a successful pipeline copy to return
         * FTP_ERR_FILE_WRITE.  Use `written < 0` instead — it is set to -1
         * exclusively by the write-error break path.
         */
        if (written < 0) {
          /* write() failed — write_errno was captured before mutex lock */
          char msg[256];
          snprintf(msg, sizeof(msg), "[COPY] write failed: errno=%d dst=%s",
                   write_errno, dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = write_errno;
          }
          out_err = FTP_ERR_FILE_WRITE;
        } else if (pipe.reader_err != 0) {
          char msg[256];
          snprintf(msg, sizeof(msg), "[COPY] read failed: errno=%d src=%s",
                   pipe.reader_err, src_path);
          ftp_log_line(FTP_LOG_WARN, msg);
          if (out_errno != NULL) {
            *out_errno = pipe.reader_err;
          }
          out_err = FTP_ERR_FILE_READ;
        } else if (out_err == FTP_ERR_UNKNOWN) {
          /* cancelled by progress callback — out_err already set */
        } else {
          /* Pipeline completed successfully */
          out_err = FTP_OK;
        }
      } else {
        /* pthread_create failed — fall through to serial path below */
        out_err = FTP_ERR_FILE_WRITE; /* will be overwritten by serial path */
      }

      pthread_mutex_destroy(&pipe.mtx);
      pthread_cond_destroy(&pipe.cv_ready);
      pthread_cond_destroy(&pipe.cv_free);

      pal_free(dbuf0);
      pal_free(dbuf1);

      if (thread_ok != 0) {
        /* Pipeline ran (success or failure) — skip serial fallback */
        if (out_err != FTP_OK) {
          goto cleanup;
        }
        goto copy_done;
      }
      /* thread_ok == 0: fall through to serial path (already logged above) */
    } else {
      /* One or both malloc failed — free whichever succeeded and fall through */
      {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] pipeline malloc failed (buf0=%s buf1=%s) — "
                 "falling back to serial copy for %s",
                 (dbuf0 != NULL) ? "ok" : "NULL",
                 (dbuf1 != NULL) ? "ok" : "NULL",
                 src_path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      pal_free(dbuf0);
      pal_free(dbuf1);
    }
  }
  /* --- Serial fallback (malloc failure or pthread_create failure) --- */
#endif /* PLATFORM_PS4 || PLATFORM_PS5 */

  {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[XDEV] serial copy starting: file_size=%llu buf=%u src=%s",
             (unsigned long long)st.st_size,
             (unsigned)PAL_FILE_COPY_BUFFER_SIZE,
             src_path);
    ftp_log_line(FTP_LOG_INFO, msg);
  }

  copy_buf = (uint8_t *)pal_malloc(PAL_FILE_COPY_BUFFER_SIZE);
  if (copy_buf == NULL) {
    {
      pal_alloc_stats_t ast;
      pal_alloc_get_stats(&ast);
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] serial malloc failed: arena_inuse=%llu peak=%llu "
               "failures=%llu buf_needed=%u src=%s",
               (unsigned long long)ast.bytes_in_use,
               (unsigned long long)ast.bytes_peak,
               (unsigned long long)ast.failures,
               (unsigned)PAL_FILE_COPY_BUFFER_SIZE,
               src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    out_err = FTP_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }

  {
    uint64_t serial_written = 0U;
    uint64_t serial_last_log = 0U;
    /* Log every 64 MB so we can see how far the copy got before failing */
    const uint64_t LOG_INTERVAL = 64U * 1024U * 1024U;

    for (;;) {
      ssize_t r = read(src_fd, copy_buf, (size_t)PAL_FILE_COPY_BUFFER_SIZE);
      if (r > 0) {
        /*
         * Write the full buffer in a single write() loop — do NOT call
         * pal_file_write_all(), which subdivides writes into
         * PAL_FILE_WRITE_CHUNK_MAX (128 KB on PS5, 64 KB on PS4).
         *
         * That limit exists exclusively to prevent TCP recv-buffer stalls
         * in cmd_STOR.  It has no relevance for a file-to-file copy: large
         * single write() calls let the USB exFAT driver allocate contiguous
         * extents and avoid per-chunk FAT chain updates, which is the same
         * rationale used by the double-buffer pipeline above.
         *
         * On PS5 (NVMe PFS, ~215 MB/s write bandwidth), using 128 KB chunks
         * instead of 4 MB chunks causes ~32x more write() syscalls per buffer,
         * each with its own AES-XTS context setup in the kernel VFS layer.
         * This is the primary cause of the observed 130 MB/s cap vs the
         * 230 MB/s target.
         *
         * PS4/PS5 PFS quirk: a full filesystem silently returns write() == 0
         * instead of -1 + ENOSPC.  Detect and synthesise ENOSPC so the log
         * always shows a meaningful errno value.
         */
        {
          const uint8_t *p_wr    = copy_buf;
          size_t         rem_wr  = (size_t)r;
          int            write_ok = 1;
          int            write_errno = 0;

          while (rem_wr > 0U) {
            ssize_t w = write(dst_fd, p_wr, rem_wr);
            if (w > 0) {
              p_wr   += (size_t)w;
              rem_wr -= (size_t)w;
              continue;
            }
            if ((w < 0) && (errno == EINTR)) {
              continue;
            }
            /*
             * w == 0: PS4/PS5 PFS silent ENOSPC — synthesise the real errno
             * so the caller and log see a meaningful error code.
             */
            write_errno = (w == 0) ? ENOSPC : errno;
            write_ok = 0;
            break;
          }

          if (write_ok == 0) {
            {
              char msg[256];
              snprintf(msg, sizeof(msg),
                       "[XDEV] write failed: errno=%d written_so_far=%llu "
                       "file_size=%llu dst=%s",
                       write_errno,
                       (unsigned long long)serial_written,
                       (unsigned long long)st.st_size,
                       dst_path);
              ftp_log_line(FTP_LOG_WARN, msg);
              if (out_errno != NULL) {
                *out_errno = write_errno;
              }
            }
            out_err = FTP_ERR_FILE_WRITE;
            goto cleanup;
          }
        }
        serial_written += (uint64_t)r;

        /* Periodic progress log */
        if (serial_written - serial_last_log >= LOG_INTERVAL) {
          serial_last_log = serial_written;
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] serial progress: %llu / %llu bytes (%.1f%%) dst=%s",
                   (unsigned long long)serial_written,
                   (unsigned long long)st.st_size,
                   (st.st_size > 0)
                       ? (100.0 * (double)serial_written / (double)st.st_size)
                       : 0.0,
                   dst_path);
          ftp_log_line(FTP_LOG_INFO, msg);
        }

        /* Report progress to caller; check for cancellation */
        if ((cb != NULL) && (cumulative != NULL)) {
          *cumulative += (uint64_t)r;
          if (cb(*cumulative, user_data) < 0) {
            out_err = FTP_ERR_UNKNOWN; /* cancelled */
            goto cleanup;
          }
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
                 "[XDEV] read failed: errno=%d written_so_far=%llu src=%s",
                 e, (unsigned long long)serial_written, src_path);
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

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
copy_done:;
#endif

  /*
   * POST-COPY CACHE EVICTION — release source file pages from page cache.
   *
   * After the copy pipeline drains the source file, all its pages are still
   * pinned in the page cache (unless F_NOCACHE was effective above).
   * POSIX_FADV_DONTNEED marks them as immediately reclaimable without
   * waiting for memory pressure.
   *
   * This matters most when:
   *   (a) F_NOCACHE is unavailable (non-PS4/PS5 platforms)
   *   (b) F_NOCACHE failed silently (fcntl returned -1)
   *   (c) The src filesystem does not honour F_NOCACHE (some exFAT builds)
   *
   * On PS5 this keeps the daemon RSS low between consecutive copy operations,
   * so that the page cache arena is fresh for the NEXT file's read pipeline
   * rather than full of the previous file's stale blocks.
   *
   * Safe on both Linux and FreeBSD/PS5: fadvise DONTNEED on a read-only fd
   * simply marks pages as low-priority; it never writes or invalidates data.
   */
#if defined(POSIX_FADV_DONTNEED)
  if (src_fd >= 0) {
    (void)posix_fadvise(src_fd, 0, 0, POSIX_FADV_DONTNEED);
  }
#endif

  /* Log completion before the atomic rename so a crash here is diagnosable */
  {
    char msg[256];
    snprintf(msg, sizeof(msg),
             "[XDEV] copy complete, renaming tmp -> dst: %s", dst_path);
    ftp_log_line(FTP_LOG_INFO, msg);
  }

  if (rename(tmp_path, dst_path) < 0) {
    {
      int e = errno;
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] rename(tmp->dst) failed: errno=%d tmp=%s dst=%s", e,
               tmp_path, dst_path);
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
  pal_free(copy_buf); /* safe: pal_free(NULL) is a no-op */
  if (dst_fd >= 0) {
    (void)close(dst_fd);
  }
  if (src_fd >= 0) {
    (void)close(src_fd);
  }
  if (out_err != FTP_OK) {
    (void)unlink(tmp_path);
  }
  return out_err;
}

/**
 * @brief Safe file open
 */
int pal_file_open(const char *path, int flags, mode_t mode) {
  if (path == NULL) {
    errno = EINVAL;
    return FTP_ERR_INVALID_PARAM;
  }

  /* Check path length */
  size_t len = strlen(path);
  if (len >= FTP_PATH_MAX) {
    errno = ENAMETOOLONG;
    return FTP_ERR_PATH_TOO_LONG;
  }

  /* Open file */
  int fd = open(path, flags, mode);
  if (fd < 0) {
    /* Map errno to FTP error code */
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    case EMFILE:
    case ENFILE:
      return FTP_ERR_OUT_OF_MEMORY;
    default:
      return FTP_ERR_FILE_OPEN;
    }
  }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * F_NOCACHE / POSIX_FADV_SEQUENTIAL — PLATFORM NOTES
   *
   * F_NOCACHE (FreeBSD O_DIRECT equivalent):
   *   Bypasses the kernel page cache for this fd. This is intentionally
   *   NOT set on PS5 because sendfile(2) relies on the page cache to pin
   *   source pages before DMA-ing them to the socket buffer. Setting
   *   F_NOCACHE on an fd that is later passed to sendfile() produces
   *   undefined behavior on FreeBSD; on PS5's modified kernel this
   *   manifests as a kernel panic when the file resides on exFAT/USB.
   *
   *   PS4: F_NOCACHE is retained — PS4 does not use sendfile() for the
   *   data transfer path (VFS_CAP_SENDFILE is not set by vfs_open on PS4
   *   for USB-backed files via the fstatfs check added above).
   *
   * POSIX_FADV_SEQUENTIAL:
   *   Safe on PS5 — does not affect sendfile() compatibility.
   *   Retained for read-ahead hinting on sequential transfers.
   */
#if defined(PLATFORM_PS4)
#ifdef F_NOCACHE
  /* F_NOCACHE is only safe for read-only fds on PFS-encrypted partitions.
   * For write fds, OrbisOS PFS enforces alignment constraints that cause
   * write() to fail with EINVAL after ~512 KB on /data/pkg/ and similar
   * encrypted mounts.  GoldHEN/ftpsrv never set F_NOCACHE; skip it. */
  if ((flags & O_WRONLY) == 0 && (flags & O_RDWR) == 0) {
    (void)fcntl(fd, F_NOCACHE, 1);
  }
#endif
#endif /* PLATFORM_PS4 only — explicitly excluded from PS5 */

#if defined(PLATFORM_PS5) && defined(POSIX_FADV_SEQUENTIAL)
  (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#endif

  return fd;
}

/**
 * @brief Safe file close
 *
 * PLATFORM NOTE — EINTR semantics differ between Linux and FreeBSD:
 *
 *   Linux:   close() interrupted by a signal still closes the fd.
 *            Retrying would attempt to close an fd already freed and
 *            potentially reused by another thread — silent data corruption.
 *
 *   FreeBSD: close() interrupted by a signal does NOT close the fd.
 *            The fd remains open and MUST be retried, or it leaks.
 *            This is the behaviour on PS4 and PS5.
 *
 * Failure to handle this distinction on PS5 causes one leaked fd per
 * interrupted close(), accumulating into BUDGET_FD_FILE exhaustion
 * (visible in klog as "called fdescfree(), but remain BUDGET_FD_FILE").
 *
 * @pre  fd >= 0
 * @note Thread-safety: safe (fd is caller-owned)
 */
ftp_error_t pal_file_close(int fd) {
  if (fd < 0) {
    return FTP_ERR_INVALID_PARAM;
  }

#if defined(__FreeBSD__) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * FreeBSD/PS4/PS5: EINTR means fd is STILL OPEN — must retry.
   * Cap iterations to avoid looping forever on a persistent signal storm.
   */
  {
    unsigned retries = 0U;
    const unsigned MAX_CLOSE_RETRIES = 8U;
    while (close(fd) < 0) {
      if (errno != EINTR) {
        return FTP_ERR_FILE_WRITE;
      }
      if (++retries >= MAX_CLOSE_RETRIES) {
        return FTP_ERR_FILE_WRITE; /* give up; fd leaks, not much we can do */
      }
    }
  }
#else
  /*
   * Linux / generic POSIX: close() with EINTR has already closed the fd.
   * Do NOT retry — the fd number may be reused by this point.
   */
  if (close(fd) < 0) {
    if (errno != EINTR) {
      return FTP_ERR_FILE_WRITE;
    }
    /* EINTR on Linux: fd is gone, treat as success */
  }
#endif

  return FTP_OK;
}

/**
 * @brief Get file status
 */
ftp_error_t pal_file_stat(const char *path, struct stat *st) {
  if ((path == NULL) || (st == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (stat(path, st) < 0) {
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
      return FTP_ERR_PERMISSION;
    default:
      return FTP_ERR_FILE_STAT;
    }
  }

  return FTP_OK;
}

/**
 * @brief Get file status from descriptor
 */
ftp_error_t pal_file_fstat(int fd, struct stat *st) {
  if ((fd < 0) || (st == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (fstat(fd, st) < 0) {
    return FTP_ERR_FILE_STAT;
  }

  return FTP_OK;
}

/**
 * @brief Read file data
 */
ssize_t pal_file_read(int fd, void *buffer, size_t count) {
  if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
    errno = EINVAL;
    return -1;
  }

  return read(fd, buffer, count);
}

/**
 * @brief Write file data
 */
ssize_t pal_file_write(int fd, const void *buffer, size_t count) {
  if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
    errno = EINVAL;
    return -1;
  }

  return write(fd, buffer, count);
}

ssize_t pal_file_write_all(int fd, const void *buffer, size_t count) {
  if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
    errno = EINVAL;
    return -1;
  }

  const uint8_t *p = (const uint8_t *)buffer;
  size_t total = 0U;

  while (total < count) {
    size_t remaining = count - total;
    size_t chunk = remaining;
    if (chunk > (size_t)PAL_FILE_WRITE_CHUNK_MAX) {
      chunk = (size_t)PAL_FILE_WRITE_CHUNK_MAX;
    }
    ssize_t n = write(fd, p + total, chunk);
    if (n > 0) {
      total += (size_t)n;
      continue;
    }
    if (n == 0) {
      /* POSIX does not define write() returning 0 for count > 0.
       * On PS4/PS5 PFS a full filesystem silently returns 0 instead
       * of -1 + ENOSPC.  Map this to ENOSPC so callers see the real
       * cause rather than the misleading EIO. */
      errno = ENOSPC;
      return -1;
    }
    if (errno == EINTR) {
      continue;
    }
    return -1;
  }

  return (ssize_t)total;
}

/**
 * @brief Seek to file position
 */
off_t pal_file_seek(int fd, off_t offset, int whence) {
  if (fd < 0) {
    errno = EINVAL;
    return -1;
  }

  return lseek(fd, offset, whence);
}

/**
 * @brief Truncate file
 */
ftp_error_t pal_file_truncate(int fd, off_t len) {
  if ((fd < 0) || (len < 0)) {
    return FTP_ERR_INVALID_PARAM;
  }

#ifdef PLATFORM_PS4
  /* PS4: Use syscall directly */
  if (syscall(480, fd, len) < 0) {
    return FTP_ERR_FILE_WRITE;
  }
#else
  /* POSIX: Standard ftruncate */
  if (ftruncate(fd, len) < 0) {
    return FTP_ERR_FILE_WRITE;
  }
#endif

  return FTP_OK;
}

/**
 * @brief Delete file
 */
ftp_error_t pal_file_delete(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (unlink(path) < 0) {
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    case EISDIR:
      return FTP_ERR_INVALID_PARAM; /* Use rmdir for directories */
    default:
      return FTP_ERR_FILE_WRITE;
    }
  }

  return FTP_OK;
}

/*===========================================================================*
 * CROSS-DEVICE MOVE (EXDEV fallback)
 *
 *   rename() fails with EXDEV when src and dst live on different
 *   filesystems (e.g. /data/homebrew → /mnt/ext1).  For regular
 *   files we already had pal_file_copy_atomic().  The functions
 *   below extend the fallback to entire directory trees.
 *
 *       pal_file_rename
 *            │
 *            ├── rename()  ─── OK? done
 *            │
 *            └── EXDEV?
 *                 │
 *                 ├── regular file → pal_file_copy_atomic + unlink
 *                 │
 *                 └── directory    → pal_move_cross_device_r
 *                                      ├── mkdir dst
 *                                      ├── for each entry:
 *                                      │    ├── file → copy_atomic + unlink
 *                                      │    └── dir  → recurse
 *                                      └── rmdir src
 *===========================================================================*/

/**
 * @brief Remove a directory tree recursively (depth-first).
 *
 * Used to clean up the source tree after a successful cross-device
 * copy, or to roll back a partial destination on failure.
 */
static ftp_error_t pal_dir_remove_recursive(const char *path, unsigned depth) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }
  if (depth > PAL_MOVE_MAX_DEPTH) {
    return FTP_ERR_PATH_TOO_LONG;
  }

  DIR *dir = opendir(path);
  if (dir == NULL) {
    if (errno == ENOENT) {
      return FTP_OK;
    }
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] opendir(cleanup) failed: errno=%d path=%s", errno, path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return FTP_ERR_DIR_OPEN;
  }

  struct dirent *ent;
  ftp_error_t err = FTP_OK;

  while ((ent = readdir(dir)) != NULL) {
    /* Skip "." and ".." */
    if ((ent->d_name[0] == '.') &&
        ((ent->d_name[1] == '\0') ||
         ((ent->d_name[1] == '.') && (ent->d_name[2] == '\0')))) {
      continue;
    }

    char child[FTP_PATH_MAX];
    int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
    if ((n < 0) || ((size_t)n >= sizeof(child))) {
      err = FTP_ERR_PATH_TOO_LONG;
      break;
    }

    struct stat st;
    if (stat(child, &st) < 0) {
      err = FTP_ERR_FILE_STAT;
      break;
    }

    if (S_ISDIR(st.st_mode)) {
      err = pal_dir_remove_recursive(child, depth + 1U);
    } else {
      if (unlink(child) != 0) {
        {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] unlink(cleanup) failed: errno=%d path=%s", errno,
                   child);
          ftp_log_line(FTP_LOG_WARN, msg);
        }
        err = FTP_ERR_FILE_WRITE;
      }
    }

    if (err != FTP_OK) {
      break;
    }
  }

  (void)closedir(dir);

  if (err == FTP_OK) {
    if (rmdir(path) < 0) {
      {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] rmdir(cleanup) failed: errno=%d path=%s", errno, path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      err = FTP_ERR_FILE_WRITE;
    }
  }

  return err;
}

/**
 * @brief Recursively move a directory tree across filesystems.
 *
 * Creates the destination directory, copies every file with
 * pal_file_copy_atomic(), recurses into subdirectories, then
 * removes each source entry after its copy succeeds.
 *
 * On failure the partial destination is cleaned up and the
 * source is left intact so the user can retry.
 */
static ftp_error_t pal_copy_cross_device_r_ex(const char *src, const char *dst,
                                              unsigned depth, int keep_src,
                                              pal_copy_progress_cb_t cb,
                                              void *user_data,
                                              uint64_t *cumulative,
                                              int *out_errno) {
  if ((src == NULL) || (dst == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }
  if (depth > PAL_MOVE_MAX_DEPTH) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] max depth %u exceeded: %s",
               (unsigned)PAL_MOVE_MAX_DEPTH, src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return FTP_ERR_PATH_TOO_LONG;
  }

  if (depth == 0U) {
    char msg[256];
    snprintf(msg, sizeof(msg), "[XDEV] cross-device move: %s -> %s", src, dst);
    ftp_log_line(FTP_LOG_INFO, msg);
  }

  /* Stat source to get permissions */
  struct stat src_st;
  if (stat(src, &src_st) < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] stat(src) failed: errno=%d path=%s", e,
               src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return (e == ENOENT) ? FTP_ERR_NOT_FOUND : FTP_ERR_FILE_STAT;
  }

  /*-------------------------------------------------------*
   * Leaf: regular file -> atomic copy + delete source     *
   *-------------------------------------------------------*/
  if (S_ISREG(src_st.st_mode)) {
    /* Use a local errno so the error log below always shows the real OS code
     * even when the caller passes NULL for out_errno (e.g. the async copy
     * thread).  If the caller DID provide out_errno, we forward into it. */
    int local_errno = 0;
    int *errno_ptr = (out_errno != NULL) ? out_errno : &local_errno;
    ftp_error_t err =
        pal_file_copy_atomic_ex(src, dst, cb, user_data, cumulative, errno_ptr);
    if (err != FTP_OK) {
      {
        int os_err = *errno_ptr; /* always valid: either *out_errno or local_errno */
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] file copy failed (err=%d, errno=%d): %s -> %s",
                 (int)err, os_err, src, dst);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      return err;
    }
    if (keep_src == 0) {
      if (unlink(src) < 0) {
        {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] unlink(src) failed: errno=%d path=%s", errno, src);
          ftp_log_line(FTP_LOG_WARN, msg);
        }
        /* Copy succeeded but source delete failed — not fatal,
           caller gets an error but data is safe at dst.        */
        return FTP_ERR_FILE_WRITE;
      }
    }
    return FTP_OK;
  }

  /*-------------------------------------------------------*
   * Branch: directory → mkdir dst, recurse, rmdir src     *
   *-------------------------------------------------------*/
  if (!S_ISDIR(src_st.st_mode)) {
    /* Symlinks, devices, etc. — skip silently */
    return FTP_OK;
  }

  mode_t mode = (mode_t)(src_st.st_mode & 0777);

  /*
   * DATA LOSS BUG — fixed here.
   *
   * Original code: mkdir() silently accepts EEXIST, then on failure
   * calls pal_dir_remove_recursive(dst) unconditionally — deleting a
   * directory that pre-existed and was not created by this move.
   *
   * Fix: track whether WE created dst.  Roll back only our own work;
   * never touch a directory that existed before this call.
   */
  int dst_created_by_us = 0;
  if (mkdir(dst, mode) < 0) {
    if (errno != EEXIST) {
      int e = errno;
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "[XDEV] mkdir(dst) failed: errno=%d path=%s",
                 e, dst);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      return FTP_ERR_FILE_WRITE;
    }
    /* dst already existed — do not delete it on rollback */
  } else {
    dst_created_by_us = 1;
  }

  DIR *dir = opendir(src);
  if (dir == NULL) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] opendir(src) failed: errno=%d path=%s",
               errno, src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (dst_created_by_us != 0) {
      (void)rmdir(dst); /* undo our mkdir before returning */
    }
    return FTP_ERR_DIR_OPEN;
  }

  struct dirent *ent;
  ftp_error_t err = FTP_OK;

  while ((ent = readdir(dir)) != NULL) {
    /* Skip "." and ".." */
    if ((ent->d_name[0] == '.') &&
        ((ent->d_name[1] == '\0') ||
         ((ent->d_name[1] == '.') && (ent->d_name[2] == '\0')))) {
      continue;
    }

    char src_child[FTP_PATH_MAX];
    char dst_child[FTP_PATH_MAX];

    int ns = snprintf(src_child, sizeof(src_child), "%s/%s", src, ent->d_name);
    int nd = snprintf(dst_child, sizeof(dst_child), "%s/%s", dst, ent->d_name);

    if ((ns < 0) || ((size_t)ns >= sizeof(src_child)) || (nd < 0) ||
        ((size_t)nd >= sizeof(dst_child))) {
      err = FTP_ERR_PATH_TOO_LONG;
      break;
    }

    err = pal_copy_cross_device_r_ex(src_child, dst_child, depth + 1U, keep_src,
                                     cb, user_data, cumulative, out_errno);
    if (err != FTP_OK) {
      break;
    }
  }

  (void)closedir(dir);

  if (err != FTP_OK) {
    /*
     * Roll back only if we created dst.  If it pre-existed, leave it
     * alone — its original contents are not our responsibility.
     * Source is untouched so the user can retry.
     */
    if (dst_created_by_us != 0) {
      (void)pal_dir_remove_recursive(dst, 0U);
    }
    return err;
  }

  if (keep_src == 0) {
    /* Source directory should now be empty — remove it */
    if (rmdir(src) < 0) {
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "[XDEV] rmdir(src) failed: errno=%d path=%s",
                 errno, src);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      return FTP_ERR_FILE_WRITE;
    }
  }

  if (depth == 0U) {
    ftp_log_line(FTP_LOG_INFO, "[XDEV] cross-device operation completed OK");
  }

  return FTP_OK;
}

/**
 * @brief Rename/move file or directory
 *
 * Falls back to recursive copy + delete when rename() returns
 * EXDEV (source and destination on different filesystems).
 */
ftp_error_t pal_file_rename(const char *old_path, const char *new_path) {
  if ((old_path == NULL) || (new_path == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (rename(old_path, new_path) < 0) {
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    case EXDEV:
      return FTP_ERR_CROSS_DEVICE;
    default:
      return FTP_ERR_FILE_WRITE;
    }
  }

  return FTP_OK;
}

/**
 * @brief Recursively copy file or directory
 */
ftp_error_t pal_file_copy_recursive(const char *src, const char *dst,
                                    int keep_src) {
  uint64_t cum = 0U;
  return pal_copy_cross_device_r_ex(src, dst, 0U, keep_src, NULL, NULL, &cum,
                                    NULL);
}

ftp_error_t pal_file_copy_recursive_ex(const char *src, const char *dst,
                                       int keep_src, pal_copy_progress_cb_t cb,
                                       void *user_data, int *out_errno) {
  uint64_t cum = 0U;
  return pal_copy_cross_device_r_ex(src, dst, 0U, keep_src, cb, user_data, &cum,
                                    out_errno);
}

/*===========================================================================*
 * DIRECTORY OPERATIONS
 *===========================================================================*/

/**
 * @brief Create directory
 */
ftp_error_t pal_dir_create(const char *path, mode_t mode) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (mkdir(path, mode) < 0) {
    switch (errno) {
    case EEXIST:
      return FTP_ERR_DIR_EXISTS; /* Already exists */
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    case ENOENT:
      return FTP_ERR_NOT_FOUND; /* Parent doesn't exist */
    default:
      return FTP_ERR_FILE_WRITE;
    }
  }

  return FTP_OK;
}

/**
 * @brief Remove directory
 */
ftp_error_t pal_dir_remove(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (rmdir(path) < 0) {
    switch (errno) {
    case ENOENT:
      return FTP_ERR_NOT_FOUND;
    case EACCES:
    case EPERM:
      return FTP_ERR_PERMISSION;
    case ENOTEMPTY:
      return FTP_ERR_INVALID_PARAM; /* Directory not empty */
    case ENOTDIR:
      return FTP_ERR_INVALID_PARAM; /* Not a directory */
    default:
      return FTP_ERR_FILE_WRITE;
    }
  }

  return FTP_OK;
}

/**
 * @brief Check if path exists
 */
int pal_path_exists(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  struct stat st;
  if (stat(path, &st) == 0) {
    return 1; /* Exists */
  }

  if (errno == ENOENT) {
    return 0; /* Does not exist */
  }

  return FTP_ERR_FILE_STAT; /* Other error */
}

/**
 * @brief Check if path is a directory
 */
int pal_path_is_directory(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  struct stat st;
  if (stat(path, &st) < 0) {
    return FTP_ERR_FILE_STAT;
  }

  return S_ISDIR(st.st_mode) ? 1 : 0;
}

/**
 * @brief Check if path is a regular file
 */
int pal_path_is_file(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  struct stat st;
  if (stat(path, &st) < 0) {
    return FTP_ERR_FILE_STAT;
  }

  return S_ISREG(st.st_mode) ? 1 : 0;
}

/**
 * @brief Remove a directory tree recursively (public wrapper)
 *
 * Wraps the static pal_dir_remove_recursive() for use by callers outside
 * this translation unit (e.g. http_api.c api_delete with recursive=1).
 *
 * @param[in] path  Root of the tree to remove (must not be NULL or "/")
 * @return FTP_OK on success, FTP_ERR_* on failure
 *
 * @note Thread-safety: NOT thread-safe
 * @note All files and subdirectories are removed depth-first.
 *       This is irreversible — call only after user confirmation.
 * @warning Do NOT call on paths that are still in use by other processes.
 */
ftp_error_t pal_dir_remove_recursive_pub(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }
  return pal_dir_remove_recursive(path, 0U);
}
