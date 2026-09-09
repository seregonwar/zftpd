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

/** @file directory.c @brief Directory and path query operations. */
#include "fileio_internal.h"
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*===========================================================================*
 * DIRECTORY OPERATIONS
 *===========================================================================*/


ftp_error_t fileio_parent_path(const char *path, char *out, size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0U)
    return FTP_ERR_INVALID_PARAM;

  const char *slash = strrchr(path, '/');
  if (slash == NULL) {
    if (out_size < 2U) return FTP_ERR_PATH_TOO_LONG;
    memcpy(out, ".", 2U);
    return FTP_OK;
  }

  size_t len = (size_t)(slash - path);
  if (len == 0U) len = 1U;
  if (len >= out_size) return FTP_ERR_PATH_TOO_LONG;
  memcpy(out, path, len);
  out[len] = '\0';
  return FTP_OK;
}

ftp_error_t fileio_join_path(const char *parent, const char *name, char *out,
                             size_t out_size) {
  if (parent == NULL || name == NULL || out == NULL || out_size == 0U)
    return FTP_ERR_INVALID_PARAM;

  size_t parent_len = strlen(parent);
  size_t name_len = strlen(name);
  size_t sep = parent_len > 0U && parent[parent_len - 1U] != '/' ? 1U : 0U;
  if (parent_len > out_size - 1U || sep > out_size - 1U - parent_len ||
      name_len > out_size - 1U - parent_len - sep)
    return FTP_ERR_PATH_TOO_LONG;

  memcpy(out, parent, parent_len);
  size_t pos = parent_len;
  if (sep != 0U) out[pos++] = '/';
  memcpy(out + pos, name, name_len);
  out[pos + name_len] = '\0';
  return FTP_OK;
}

static int path_mode(const char *path, mode_t *mode) {
  if (path == NULL || mode == NULL) return FTP_ERR_INVALID_PARAM;
  struct stat st;
  if (stat(path, &st) == 0) {
    *mode = st.st_mode;
    return 1;
  }
  if (errno == ENOENT || errno == ENOTDIR) return 0;
  return fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);
}

ftp_error_t pal_dir_create(const char *path, mode_t mode) {
  if (path == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (mkdir(path, mode) < 0) {
    if (errno == EEXIST) return FTP_ERR_DIR_EXISTS;
    return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
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
    if (errno == ENOTEMPTY || errno == ENOTDIR) return FTP_ERR_INVALID_PARAM;
    return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
  }

  return FTP_OK;
}

/**
 * @brief Check if path exists
 */
int pal_path_exists(const char *path) {
  mode_t mode = 0;
  return path_mode(path, &mode);
}

int pal_path_is_directory(const char *path) {
  mode_t mode = 0;
  int result = path_mode(path, &mode);
  return result == 1 ? (S_ISDIR(mode) ? 1 : 0) : result;
}

int pal_path_is_file(const char *path) {
  mode_t mode = 0;
  int result = path_mode(path, &mode);
  return result == 1 ? (S_ISREG(mode) ? 1 : 0) : result;
}
