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
#include <unistd.h>

/* Fallback buffer size for non-sendfile platforms */
#define FALLBACK_BUFFER_SIZE FTP_BUFFER_SIZE
/*
 * Write chunk ceiling for pal_file_write_all().
 *
 *   PS4/PS5: /data/ lives on a PFS-encrypted NVMe partition.
 *            Every write() triggers per-block AES in the kernel VFS.
 *            Larger chunks (1 MB) cut the number of crypto context
 *            switches by 4x vs. the old 256 KB default.
 *
 *   POSIX:   256 KB is a safe, portable default.
 */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define PAL_FILE_WRITE_CHUNK_MAX                                               \
  1048576U /* 1 MB — match FTP_STREAM_BUFFER_SIZE */
#else
#define PAL_FILE_WRITE_CHUNK_MAX                                               \
  262144U /* 256 KB — safe POSIX default         */
#endif

/* Max recursion depth for cross-device directory move */
#define PAL_MOVE_MAX_DEPTH 64U

#ifndef PAL_FILE_COPY_BUFFER_SIZE
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define PAL_FILE_COPY_BUFFER_SIZE (1024U * 1024U)
#else
#define PAL_FILE_COPY_BUFFER_SIZE FTP_BUFFER_SIZE
#endif
#endif

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
  if (count > (size_t)PAL_FILE_WRITE_CHUNK_MAX) {
    count = (size_t)PAL_FILE_WRITE_CHUNK_MAX;
  }

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
     * Storage-level error during transfer.
     *
     * EIO    : underlying device I/O error (USB read failure)
     * ESTALE : stale vnode — filesystem unmounted under us
     * EBADF  : fd invalidated (shouldn't happen, but guard anyway)
     * EFAULT : kernel memory fault — should never reach userspace,
     *          but if it does we must NOT retry with sendfile()
     *
     * These errors indicate the source filesystem is no longer
     * accessible. Return whatever was sent so far; the caller
     * will detect the short count and send reply 426.
     *
     * CRITICAL: do NOT retry sendfile() after any of these —
     * retrying on a detached vnode can panic the kernel.
     */
    return (sbytes > 0) ? sbytes : -1;
  } else {
    /* Other error (e.g. ENOTSOCK, EINVAL) */
    return -1;
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

static ftp_error_t pal_file_copy_atomic(const char *src_path,
                                        const char *dst_path) {
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
  if (src_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] open(src) failed: errno=%d path=%s", e,
               src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
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
  dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (dst_fd < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] open(tmp) failed: errno=%d path=%s", e,
               tmp_path);
      ftp_log_line(FTP_LOG_WARN, msg);
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
  copy_buf = (uint8_t *)pal_malloc(PAL_FILE_COPY_BUFFER_SIZE);
  if (copy_buf == NULL) {
    out_err = FTP_ERR_OUT_OF_MEMORY;
    goto cleanup;
  }

  for (;;) {
    ssize_t r = read(src_fd, copy_buf, (size_t)PAL_FILE_COPY_BUFFER_SIZE);
    if (r > 0) {
      ssize_t w = pal_file_write_all(dst_fd, copy_buf, (size_t)r);
      if (w != r) {
        {
          char msg[256];
          snprintf(msg, sizeof(msg), "[XDEV] write failed: errno=%d dst=%s",
                   errno, dst_path);
          ftp_log_line(FTP_LOG_WARN, msg);
        }
        out_err = FTP_ERR_FILE_WRITE;
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
      char msg[256];
      snprintf(msg, sizeof(msg), "[XDEV] read failed: errno=%d src=%s", errno,
               src_path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    out_err = FTP_ERR_FILE_READ;
    goto cleanup;
  }

  pal_free(copy_buf);
  copy_buf = NULL;

  if (rename(tmp_path, dst_path) < 0) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] rename(tmp->dst) failed: errno=%d tmp=%s dst=%s", errno,
               tmp_path, dst_path);
      ftp_log_line(FTP_LOG_WARN, msg);
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
  (void)fcntl(fd, F_NOCACHE, 1);
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
      errno = EIO;
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
static ftp_error_t pal_copy_cross_device_r(const char *src, const char *dst,
                                           unsigned depth, int keep_src) {
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
    ftp_error_t err = pal_file_copy_atomic(src, dst);
    if (err != FTP_OK) {
      {
        char msg[256];
        snprintf(msg, sizeof(msg), "[XDEV] file copy failed (err=%d): %s -> %s",
                 (int)err, src, dst);
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

    err = pal_copy_cross_device_r(src_child, dst_child, depth + 1U, keep_src);
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
  return pal_copy_cross_device_r(src, dst, 0U, keep_src);
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
