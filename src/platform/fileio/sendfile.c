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

/** @file sendfile.c @brief Platform sendfile abstraction. */
#include "pal_fileio.h"
#include "pal_network.h"
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <sys/mount.h>
extern int _fstatfs(int, struct statfs *);
#endif

#define FALLBACK_BUFFER_SIZE FTP_BUFFER_SIZE

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5) || !HAS_SENDFILE
static ssize_t send_file_buffered(int sock_fd, int file_fd, off_t *offset,
                                  size_t count) {
  static _Thread_local char buffer[FALLBACK_BUFFER_SIZE];
  size_t chunk = count < sizeof(buffer) ? count : sizeof(buffer);
  ssize_t nread = pread(file_fd, buffer, chunk, *offset);
  if (nread <= 0) {
    if (nread == 0) errno = 0;
    return nread;
  }

  /* One send exposes backpressure to the caller. Advance only by bytes the
   * socket accepted; the next pread rereads any unsent tail at that offset. */
  ssize_t nsent = send(sock_fd, buffer, (size_t)nread, 0);
  if (nsent > 0) *offset += (off_t)nsent;
  return nsent;
}
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
static int console_sendfile_safe(int file_fd) {
  struct statfs fs;
  /* Detect the actual fd's mount, including image mounts under /mnt. A path
   * prefix or a cached page cannot establish vnode pager compatibility. */
  if (_fstatfs(file_fd, &fs) != 0) return 0;
  if (fs.f_fstypename[0] == '\0') return 0;
  static const char *const unsafe[] = {
      "exfatfs", "exfat", "msdosfs", "nullfs", "pfsmnt", "pfs"};
  for (size_t i = 0U; i < sizeof(unsafe) / sizeof(unsafe[0]); i++) {
    if (strncmp(fs.f_fstypename, unsafe[i], sizeof(fs.f_fstypename)) == 0)
      return 0;
  }
  return 1;
}
#endif

#if defined(__linux__)
/* sendfile() has no MSG_NOSIGNAL variant and raises SIGPIPE on a reset peer.
 * Block it around the call and drain it before restoring the mask: embedders
 * (FFI library, test harnesses) do not ignore SIGPIPE process-wide. */
static ssize_t sendfile_nosigpipe(int sock_fd, int file_fd, off_t *offset,
                                  size_t count) {
  sigset_t pipe_set;
  sigset_t old_set;
  int blocked = 0;

  (void)sigemptyset(&pipe_set);
  (void)sigaddset(&pipe_set, SIGPIPE);
  if (pthread_sigmask(SIG_BLOCK, &pipe_set, &old_set) == 0) {
    blocked = 1;
  }

  ssize_t sent = sendfile(sock_fd, file_fd, offset, count);

  if (blocked != 0) {
    if ((sent < 0) && (errno == EPIPE)) {
      /* Pending: unblocking would deliver it and kill the process. */
      struct timespec zero;
      zero.tv_sec = 0;
      zero.tv_nsec = 0;
      while ((sigtimedwait(&pipe_set, NULL, &zero) == -1) && (errno == EINTR)) {
      }
    }
    (void)pthread_sigmask(SIG_SETMASK, &old_set, NULL);
  }

  return sent;
}
#endif

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

  /* Chunk policy belongs to callers; the PAL forwards the requested count. */

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /* These console vnode pagers can panic inside sendfile before returning an
   * error. Both FTP RETR and HTTP downloads must bypass that syscall upfront. */
  if (!console_sendfile_safe(file_fd))
    return send_file_buffered(sock_fd, file_fd, offset, count);
#endif

#if defined(__linux__)
  return sendfile_nosigpipe(sock_fd, file_fd, offset, count);

#elif defined(__APPLE__) && defined(__MACH__)
  off_t sent = (off_t)count;
  int ret = sendfile(file_fd, sock_fd, *offset, &sent, NULL, 0);
  if (sent > 0) *offset += sent;
  if (ret == 0 || sent > 0) return sent;
  return -1;

#elif defined(__FreeBSD__) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /* FreeBSD uses the inverse fd order and reports partial bytes via sbytes. */
  off_t sbytes = 0;
  off_t start_offset = *offset;

  int ret = sendfile(file_fd, sock_fd, start_offset, count, NULL, &sbytes, 0);

  if (sbytes > 0) {
    *offset += sbytes;
  }

  if (ret == 0) {
    return sbytes;
  } else if ((ret == -1) && (errno == EAGAIN)) {
    return sbytes;
  } else if ((ret == -1) && (errno == EINTR)) {
    /* sbytes already advanced the offset, so expose the partial transfer. */
    return sbytes;
  } else if ((ret == -1) && pal_file_error_is_fatal(errno)) {
    /* Retrying a fatal storage error on an invalid vnode can panic OrbisOS. */
    return -1;

  } else {
    /* Do not hide unsupported sendfile paths behind an implicit copy fallback. */
    return (sbytes > 0) ? sbytes : -1;
  }

#else
  return send_file_buffered(sock_fd, file_fd, offset, count);
#endif
}
ssize_t pal_sendfile_retry(int sock_fd, int file_fd, off_t *offset, size_t count,
                           unsigned retries, unsigned sleep_us) {
  unsigned attempt = 0U;
  for (;;) {
    ssize_t sent = pal_sendfile(sock_fd, file_fd, offset, count);
    if (sent > 0) return sent;
    int error = errno;
    if (error == EINTR) continue;
    if (sent < 0 && pal_file_error_is_fatal(error)) return -1;
    if (error != EAGAIN && error != EWOULDBLOCK) return sent;
    if (attempt++ >= retries) {
      errno = error;
      return -1;
    }
    if (sleep_us > 0U) usleep(sleep_us);
  }
}
