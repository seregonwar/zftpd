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
#include <unistd.h>

#define FALLBACK_BUFFER_SIZE FTP_BUFFER_SIZE

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

#if defined(__linux__)
  return sendfile(sock_fd, file_fd, offset, count);

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
  /* Portable fallback for platforms without sendfile. */
  static _Thread_local char buffer[FALLBACK_BUFFER_SIZE];

  ssize_t nread = pread(
      file_fd, buffer,
      (count < FALLBACK_BUFFER_SIZE) ? count : FALLBACK_BUFFER_SIZE, *offset);
  if (nread <= 0) {
    return nread;
  }

  ssize_t nsent = pal_send_all(sock_fd, buffer, (size_t)nread, 0);
  if (nsent < 0) {
    return -1;
  }
  *offset += nsent;
  return nsent;
#endif
}
