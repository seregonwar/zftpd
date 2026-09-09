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

/** @file tree.c @brief Recursive copy, move and tree removal operations. */
#include "fileio_internal.h"
#include "ftp_log.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Recursive fallback used when a file operation crosses filesystems. */

static int is_dot_entry(const char *name) {
  return name != NULL && name[0] == '.' &&
         (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'));
}

static int canonicalize_target(const char *path, char *out, size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0U) return -1;
  if (realpath(path, out) != NULL) return 0;
  if (errno != ENOENT) return -1;

  const char *slash = strrchr(path, '/');
  const char *name = slash != NULL ? slash + 1 : path;
  if (name[0] == '\0') return -1;

  char parent[FTP_PATH_MAX];
  if (fileio_parent_path(path, parent, sizeof(parent)) != FTP_OK) return -1;
  char parent_real[FTP_PATH_MAX];
  if (realpath(parent, parent_real) == NULL) return -1;
  return fileio_join_path(parent_real, name, out, out_size) == FTP_OK ? 0 : -1;
}

static int canonicalize_entry_no_follow(const char *path, char *out,
                                        size_t out_size) {
  if (path == NULL || out == NULL || out_size == 0U) return -1;
  if (strcmp(path, "/") == 0) {
    if (out_size < 2U) return -1;
    memcpy(out, "/", 2U);
    return 0;
  }

  const char *slash = strrchr(path, '/');
  const char *name = slash != NULL ? slash + 1 : path;
  if (name[0] == '\0') return -1;

  char parent[FTP_PATH_MAX];
  if (fileio_parent_path(path, parent, sizeof(parent)) != FTP_OK) return -1;
  char parent_real[FTP_PATH_MAX];
  if (realpath(parent, parent_real) == NULL) return -1;
  return fileio_join_path(parent_real, name, out, out_size) == FTP_OK ? 0 : -1;
}

static int path_is_same_or_child(const char *path, const char *root) {
  size_t root_len = strlen(root);
  if (strcmp(path, root) == 0) return 1;
  if (root_len == 1U && root[0] == '/') return path[0] == '/';
  return root_len > 0U && strncmp(path, root, root_len) == 0 &&
         path[root_len] == '/';
}

static ftp_error_t validate_copy_roots(const char *src, const char *dst) {
  char src_real[FTP_PATH_MAX];
  char dst_real[FTP_PATH_MAX];
  struct stat st;
  if (lstat(src, &st) < 0) return fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);

  int src_ok = S_ISLNK(st.st_mode)
                   ? canonicalize_entry_no_follow(src, src_real, sizeof(src_real))
                   : (realpath(src, src_real) != NULL ? 0 : -1);
  if (src_ok != 0 || canonicalize_target(dst, dst_real, sizeof(dst_real)) != 0)
    return FTP_ERR_PATH_INVALID;
  if (path_is_same_or_child(dst_real, src_real)) return FTP_ERR_INVALID_PARAM;
  return FTP_OK;
}

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
               "[XDEV] opendir(cleanup) failed: errno=%d path=%.*s", errno,
               PAL_LOG_PATH_CHARS, path);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return FTP_ERR_DIR_OPEN;
  }

  struct dirent *ent;
  ftp_error_t err = FTP_OK;

  while ((ent = readdir(dir)) != NULL) {
    if (is_dot_entry(ent->d_name)) continue;

    char child[FTP_PATH_MAX];
    err = fileio_join_path(path, ent->d_name, child, sizeof(child));
    if (err != FTP_OK) break;

    struct stat st;
    if (lstat(child, &st) < 0) {
      err = fileio_error_from_errno(errno, FTP_ERR_FILE_STAT);
      break;
    }

    if (S_ISDIR(st.st_mode)) {
      err = pal_dir_remove_recursive(child, depth + 1U);
    } else {
      if (unlink(child) != 0) {
        {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] unlink(cleanup) failed: errno=%d path=%.*s", errno,
                   PAL_LOG_PATH_CHARS, child);
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
                 "[XDEV] rmdir(cleanup) failed: errno=%d path=%.*s", errno,
                 PAL_LOG_PATH_CHARS, path);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      err = FTP_ERR_FILE_WRITE;
    }
  }

  return err;
}

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
      snprintf(msg, sizeof(msg), "[XDEV] max depth %u exceeded: %.*s",
               (unsigned)PAL_MOVE_MAX_DEPTH, PAL_LOG_PATH_CHARS, src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return FTP_ERR_PATH_TOO_LONG;
  }

  if (depth == 0U) {
    ftp_error_t root_err = validate_copy_roots(src, dst);
    if (root_err != FTP_OK) return root_err;

    char msg[256];
    snprintf(msg, sizeof(msg), "[XDEV] cross-device move: %.*s -> %.*s",
             PAL_LOG_PATH_PAIR_CHARS, src, PAL_LOG_PATH_PAIR_CHARS, dst);
    ftp_log_line(FTP_LOG_INFO, msg);
  }

  struct stat src_st;
  if (lstat(src, &src_st) < 0) {
    int e = errno;
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] stat(src) failed: errno=%d path=%.*s", e,
               PAL_LOG_PATH_CHARS, src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    return (e == ENOENT) ? FTP_ERR_NOT_FOUND : FTP_ERR_FILE_STAT;
  }

  if (S_ISLNK(src_st.st_mode)) {
    char target[FTP_PATH_MAX];
    ssize_t n = readlink(src, target, sizeof(target) - 1U);
    if (n < 0) return fileio_error_from_errno(errno, FTP_ERR_FILE_READ);
    target[n] = '\0';
    if (symlink(target, dst) < 0)
      return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
    if (keep_src == 0 && unlink(src) < 0)
      return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
    return FTP_OK;
  }

  if (S_ISREG(src_st.st_mode)) {
    int local_errno = 0;
    int *errno_ptr = (out_errno != NULL) ? out_errno : &local_errno;
    ftp_error_t err =
        pal_file_copy_atomic_ex(src, dst, cb, user_data, cumulative, errno_ptr);
    if (err != FTP_OK) {
      {
        int os_err = *errno_ptr;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] file copy failed (err=%d, errno=%d): %.*s -> %.*s",
                 (int)err, os_err, PAL_LOG_PATH_PAIR_CHARS, src,
                 PAL_LOG_PATH_PAIR_CHARS, dst);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      return err;
    }
    if (keep_src == 0) {
      if (unlink(src) < 0) {
        {
          char msg[256];
          snprintf(msg, sizeof(msg),
                   "[XDEV] unlink(src) failed: errno=%d path=%.*s", errno,
                   PAL_LOG_PATH_CHARS, src);
          ftp_log_line(FTP_LOG_WARN, msg);
        }
        return FTP_ERR_FILE_WRITE;
      }
    }
    return FTP_OK;
  }

  if (!S_ISDIR(src_st.st_mode)) {
    return FTP_ERR_INVALID_PARAM;
  }

  mode_t mode = (mode_t)(src_st.st_mode & 0777);

  /* Never roll back a destination directory that pre-dated this operation. */
  int dst_created_by_us = 0;
  if (mkdir(dst, mode) < 0) {
    if (errno != EEXIST) {
      int e = errno;
      {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] mkdir(dst) failed: errno=%d path=%.*s", e,
                 PAL_LOG_PATH_CHARS, dst);
        ftp_log_line(FTP_LOG_WARN, msg);
      }
      return FTP_ERR_FILE_WRITE;
    }
  } else {
    dst_created_by_us = 1;
  }

  DIR *dir = opendir(src);
  if (dir == NULL) {
    {
      char msg[256];
      snprintf(msg, sizeof(msg),
               "[XDEV] opendir(src) failed: errno=%d path=%.*s", errno,
               PAL_LOG_PATH_CHARS, src);
      ftp_log_line(FTP_LOG_WARN, msg);
    }
    if (dst_created_by_us != 0) {
      (void)rmdir(dst);
    }
    return FTP_ERR_DIR_OPEN;
  }

  struct dirent *ent;
  ftp_error_t err = FTP_OK;

  while ((ent = readdir(dir)) != NULL) {
    if (is_dot_entry(ent->d_name)) continue;

    char src_child[FTP_PATH_MAX];
    char dst_child[FTP_PATH_MAX];
    err = fileio_join_path(src, ent->d_name, src_child, sizeof(src_child));
    if (err == FTP_OK)
      err = fileio_join_path(dst, ent->d_name, dst_child, sizeof(dst_child));
    if (err != FTP_OK) break;

    err = pal_copy_cross_device_r_ex(src_child, dst_child, depth + 1U, keep_src,
                                     cb, user_data, cumulative, out_errno);
    if (err != FTP_OK) {
      break;
    }
  }

  (void)closedir(dir);

  if (err != FTP_OK) {
    if (dst_created_by_us != 0) {
      (void)pal_dir_remove_recursive(dst, 0U);
    }
    return err;
  }

  if (keep_src == 0) {
    if (rmdir(src) < 0) {
      {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "[XDEV] rmdir(src) failed: errno=%d path=%.*s", errno,
                 PAL_LOG_PATH_CHARS, src);
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

ftp_error_t pal_file_rename(const char *old_path, const char *new_path) {
  if ((old_path == NULL) || (new_path == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (rename(old_path, new_path) < 0) {
    if (errno == EXDEV) return FTP_ERR_CROSS_DEVICE;
    return fileio_error_from_errno(errno, FTP_ERR_FILE_WRITE);
  }

  return FTP_OK;
}

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


ftp_error_t pal_dir_remove_recursive_pub(const char *path) {
  if (path == NULL || path[0] == '\0') return FTP_ERR_INVALID_PARAM;

  char resolved[FTP_PATH_MAX];
  if (realpath(path, resolved) != NULL && strcmp(resolved, "/") == 0)
    return FTP_ERR_PERMISSION;
  if (strcmp(path, "/") == 0) return FTP_ERR_PERMISSION;

  return pal_dir_remove_recursive(path, 0U);
}
