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

/** @file basic.c @brief Basic file descriptor and metadata operations. */
#include "fileio_internal.h"
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#if defined(PLATFORM_PS5) || defined(PS5)
#define PAL_FILE_WRITE_CHUNK_MAX (1024U * 1024U)
#elif defined(PLATFORM_PS4) || defined(PS4)
#define PAL_FILE_WRITE_CHUNK_MAX (64U * 1024U)
#else
#define PAL_FILE_WRITE_CHUNK_MAX (256U * 1024U)
#endif

#if defined(PLATFORM_PS4) || defined(PS4)
#define PS4_SYS_FTRUNCATE 480
#endif

int pal_file_error_is_fatal(int error) {
  return error == EIO || error == ESTALE || error == EBADF || error == EFAULT;
}

ftp_error_t fileio_error_from_errno(int error, ftp_error_t fallback) {
  switch (error) {
  case ENOENT:
    return FTP_ERR_NOT_FOUND;
  case EACCES:
  case EPERM:
    return FTP_ERR_PERMISSION;
  case ENAMETOOLONG:
    return FTP_ERR_PATH_TOO_LONG;
  case ENOMEM:
  case EMFILE:
  case ENFILE:
    return FTP_ERR_OUT_OF_MEMORY;
  default:
    return fallback;
  }
}

int pal_file_open(const char *path, int flags, mode_t mode) {
  if (path == NULL) {
    errno = EINVAL;
    return FTP_ERR_INVALID_PARAM;
  }

  size_t len = strlen(path);
  if (len >= FTP_PATH_MAX) {
    errno = ENAMETOOLONG;
    return FTP_ERR_PATH_TOO_LONG;
  }

  int fd = open(path, flags, mode);
  if (fd < 0) return fileio_error_from_errno(errno, FTP_ERR_FILE_OPEN);

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /* PS5 keeps read pages pager-backed for sendfile; PS4 uses F_NOCACHE only on reads. */
#if defined(PLATFORM_PS4)
#ifdef F_NOCACHE
  if ((flags & O_WRONLY) == 0 && (flags & O_RDWR) == 0) {
    (void)fcntl(fd, F_NOCACHE, 1);
  }
#endif
#endif /* PLATFORM_PS4 only — explicitly excluded from PS5 */

#if defined(PLATFORM_PS5) && defined(POSIX_FADV_SEQUENTIAL)
  if ((flags & O_WRONLY) == 0 && (flags & O_RDWR) == 0)
    (void)posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
#endif
#endif

  return fd;
}

/* FreeBSD/OrbisOS leaves an fd open after close(EINTR); Linux does not. */
ftp_error_t pal_file_close(int fd) {
  if (fd < 0) {
    return FTP_ERR_INVALID_PARAM;
  }

#if defined(__FreeBSD__) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  {
    unsigned retries = 0U;
    const unsigned MAX_CLOSE_RETRIES = 8U;
    while (close(fd) < 0) {
      if (errno != EINTR) {
        return FTP_ERR_FILE_WRITE;
      }
      if (++retries >= MAX_CLOSE_RETRIES) {
        return FTP_ERR_FILE_WRITE;
      }
    }
  }
#else
  if (close(fd) < 0) {
    if (errno != EINTR) {
      return FTP_ERR_FILE_WRITE;
    }
  }
#endif

  return FTP_OK;
}

ftp_error_t pal_file_stat(const char *path, struct stat *st) {
  if ((path == NULL) || (st == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (stat(path, st) < 0) return fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);

  return FTP_OK;
}

ftp_error_t pal_file_fstat(int fd, struct stat *st) {
  if ((fd < 0) || (st == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (fstat(fd, st) < 0) {
    return FTP_ERR_FILE_STAT;
  }

  return FTP_OK;
}

ssize_t pal_file_read(int fd, void *buffer, size_t count) {
  if (fd < 0 || (buffer == NULL && count != 0U)) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0U) return 0;
  return read(fd, buffer, count);
}

ssize_t pal_file_write(int fd, const void *buffer, size_t count) {
  if (fd < 0 || (buffer == NULL && count != 0U)) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0U) return 0;
  return write(fd, buffer, count);
}

ssize_t pal_file_write_all(int fd, const void *buffer, size_t count) {
  if (fd < 0 || (buffer == NULL && count != 0U)) {
    errno = EINVAL;
    return -1;
  }
  if (count == 0U) return 0;

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
      /* Some PFS builds report a full filesystem as write()==0. */
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

off_t pal_file_seek(int fd, off_t offset, int whence) {
  if (fd < 0) {
    errno = EINVAL;
    return -1;
  }

  return lseek(fd, offset, whence);
}

ftp_error_t pal_file_truncate(int fd, off_t len) {
  if ((fd < 0) || (len < 0)) {
    return FTP_ERR_INVALID_PARAM;
  }

#ifdef PLATFORM_PS4
  if (syscall(PS4_SYS_FTRUNCATE, fd, len) < 0) {
    return FTP_ERR_FILE_WRITE;
  }
#else
  if (ftruncate(fd, len) < 0) {
    return FTP_ERR_FILE_WRITE;
  }
#endif

  return FTP_OK;
}

ftp_error_t pal_file_delete(const char *path) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (unlink(path) < 0) {
    if (errno == EISDIR) return FTP_ERR_INVALID_PARAM;
    return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
  }

  return FTP_OK;
}

ftp_error_t pal_file_chmod(const char *path, mode_t mode) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (chmod(path, mode) < 0) {
    int error = errno;
    if (error == EINVAL
#if defined(EOPNOTSUPP)
        || error == EOPNOTSUPP
#elif defined(ENOTSUP)
        || error == ENOTSUP
#endif
    ) return FTP_ERR_PERMISSION;
    return fileio_error_from_errno(error, FTP_ERR_FILE_WRITE);
  }

  return FTP_OK;
}
