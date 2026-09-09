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

/** @file listing.c @brief FTP LIST/NLST/MLSD/MLST commands. */
#include "ftp_commands.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_filesystem.h"
#include "pal_network.h"
#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__) || (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
#include <sys/mount.h>
#endif

static ftp_error_t send_control_raw(ftp_session_t *session,
                                    const char *payload) {
  if ((session == NULL) || (payload == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (session->ctrl_fd < 0) {
    return FTP_ERR_SOCKET_SEND;
  }

  size_t len = strlen(payload);
  if (len == 0U) {
    return FTP_OK;
  }

  ssize_t sent = pal_send_all(session->ctrl_fd, payload, len, 0);
  if (sent != (ssize_t)len) {
    return FTP_ERR_SOCKET_SEND;
  }

  session->last_activity = time(NULL);
  return FTP_OK;
}

static const char *mlsx_type_from_mode(uint32_t mode) {
  return (((mode & (uint32_t)S_IFMT) == (uint32_t)S_IFDIR)) ? "dir" : "file";
}

static void mlsx_format_modify_time(int64_t mtime, char *buffer,
                                    size_t size) {
  if ((buffer == NULL) || (size == 0U)) {
    return;
  }

  time_t unix_time = (time_t)mtime;
  struct tm tm_time;
  memset(&tm_time, 0, sizeof(tm_time));
  gmtime_r(&unix_time, &tm_time);
  (void)strftime(buffer, size, "%Y%m%d%H%M%S", &tm_time);
}

static ftp_error_t ftp_path_to_client_path(const ftp_session_t *session,
                                           const char *resolved,
                                           char *output, size_t size) {
  if ((session == NULL) || (resolved == NULL) || (output == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (size < 2U) {
    return FTP_ERR_PATH_TOO_LONG;
  }

  if (strcmp(session->root_path, "/") == 0) {
    size_t len = strlen(resolved);
    if ((len + 1U) > size) {
      return FTP_ERR_PATH_TOO_LONG;
    }
    memcpy(output, resolved, len + 1U);
    return FTP_OK;
  }

  size_t root_len = strlen(session->root_path);
  if (strncmp(resolved, session->root_path, root_len) != 0) {
    return FTP_ERR_PATH_INVALID;
  }

  const char *suffix = resolved + root_len;
  if (*suffix == '\0') {
    output[0] = '/';
    output[1] = '\0';
    return FTP_OK;
  }

  if (*suffix == '/') {
    size_t len = strlen(suffix);
    if ((len + 1U) > size) {
      return FTP_ERR_PATH_TOO_LONG;
    }
    memcpy(output, suffix, len + 1U);
    return FTP_OK;
  }

  int n = snprintf(output, size, "/%s", suffix);
  if ((n < 0) || ((size_t)n >= size)) {
    return FTP_ERR_PATH_TOO_LONG;
  }

  return FTP_OK;
}

static ftp_error_t mlsx_format_line(const vfs_stat_t *st,
                                    const char *display_name,
                                    int leading_space, char *buffer,
                                    size_t size) {
  if ((st == NULL) || (display_name == NULL) || (buffer == NULL) ||
      (size == 0U)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char timebuf[20];
  memset(timebuf, 0, sizeof(timebuf));
  mlsx_format_modify_time(st->mtime, timebuf, sizeof(timebuf));

  const char *type_str = mlsx_type_from_mode(st->mode);
  unsigned long long size_value =
      (((st->mode & (uint32_t)S_IFMT) == (uint32_t)S_IFDIR))
          ? 0ULL
          : (unsigned long long)st->size;
  unsigned mode_value = (unsigned)(st->mode & 07777U);
  const char *prefix = (leading_space != 0) ? " " : "";

  int n = snprintf(buffer, size,
                   "%stype=%s;size=%llu;modify=%s;unix.mode=%04o; %.*s\r\n",
                   prefix, type_str, size_value, timebuf, mode_value,
                   (int)(size > 50 ? size - 50 : 0), display_name);
  if ((n < 0) || ((size_t)n >= size)) {
    return FTP_ERR_PATH_TOO_LONG;
  }

  return FTP_OK;
}


static ftp_error_t send_directory_listing(ftp_session_t *session,
                                          const char *path, int detailed) {
  DIR *dir = opendir(path);
  if (dir == NULL) {
    return FTP_ERR_DIR_OPEN;
  }

  char line_buffer[FTP_LIST_LINE_SIZE];
  struct dirent *entry;

  int skip_stat = 0;
  if (FTP_LIST_SAFE_MODE != 0) {
    if ((strncmp(path, "/dev", 4) == 0) &&
        ((path[4] == '\0') || (path[4] == '/'))) {
      skip_stat = 1;
    } else if ((strncmp(path, "/proc", 5) == 0) &&
               ((path[5] == '\0') || (path[5] == '/'))) {
      skip_stat = 1;
    } else if ((strncmp(path, "/sys", 4) == 0) &&
               ((path[4] == '\0') || (path[4] == '/'))) {
      skip_stat = 1;
    }
#if defined(__APPLE__) ||                                                      \
    (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
    if (skip_stat == 0) {
      struct statfs sfs;
      if (statfs(path, &sfs) == 0) {
        const char *t = sfs.f_fstypename;
        if ((t != NULL) &&
            ((strcmp(t, "devfs") == 0) || (strcmp(t, "procfs") == 0) ||
             (strcmp(t, "fdescfs") == 0) || (strcmp(t, "sysfs") == 0) ||
             (strcmp(t, "linsysfs") == 0))) {
          skip_stat = 1;
        }
      }
    }
#endif
  }

  while ((entry = readdir(dir)) != NULL) {
    /* Skip . and .. */
    if ((strcmp(entry->d_name, ".") == 0) ||
        (strcmp(entry->d_name, "..") == 0)) {
      continue;
    }

    if (detailed != 0) {
      /* Detailed listing (ls -l format) */
      vfs_stat_t st;
      int have_stat = 0;
      if (skip_stat == 0) {
        char fullpath[FTP_PATH_MAX];
        int max_path_len = (int)(sizeof(fullpath) - 2 - strlen(entry->d_name));
        if (max_path_len < 0) { continue; }
        int n =
            snprintf(fullpath, sizeof(fullpath), "%.*s/%s", max_path_len, path, entry->d_name);
        if ((n >= 0) && ((size_t)n < sizeof(fullpath))) {
          if (vfs_stat(fullpath, &st) == FTP_OK) {
            have_stat = 1;
          }
        }
      }
      if (have_stat == 0) {
        memset(&st, 0, sizeof(st));
        if (entry->d_type == DT_DIR) {
          st.mode = (uint32_t)S_IFDIR;
        } else {
          st.mode = (uint32_t)S_IFREG;
        }
      }

      /* Format: -rw-r--r-- 1 user group size date filename */
      char perms[11];
      perms[0] = (((st.mode & S_IFMT) == S_IFDIR)) ? 'd' : '-';
      perms[1] = ((st.mode & S_IRUSR) != 0U) ? 'r' : '-';
      perms[2] = ((st.mode & S_IWUSR) != 0U) ? 'w' : '-';
      perms[3] = ((st.mode & S_IXUSR) != 0U) ? 'x' : '-';
      perms[4] = ((st.mode & S_IRGRP) != 0U) ? 'r' : '-';
      perms[5] = ((st.mode & S_IWGRP) != 0U) ? 'w' : '-';
      perms[6] = ((st.mode & S_IXGRP) != 0U) ? 'x' : '-';
      perms[7] = ((st.mode & S_IROTH) != 0U) ? 'r' : '-';
      perms[8] = ((st.mode & S_IWOTH) != 0U) ? 'w' : '-';
      perms[9] = ((st.mode & S_IXOTH) != 0U) ? 'x' : '-';
      perms[10] = '\0';

      /* Format time */
      struct tm tm_time;
      time_t mtime = (time_t)st.mtime;
      gmtime_r(&mtime, &tm_time);

      char time_str[32];
      strftime(time_str, sizeof(time_str), "%b %d %H:%M", &tm_time);

      int n = snprintf(line_buffer, sizeof(line_buffer),
                       "%s 1 ftp ftp %10lld %s %s\r\n", perms,
                       (long long)st.size, time_str, entry->d_name);
      if ((n < 0) || ((size_t)n >= sizeof(line_buffer))) {
        continue;
      }
    } else {
      /* Simple listing (names only) */
      int n =
          snprintf(line_buffer, sizeof(line_buffer), "%s\r\n", entry->d_name);

      if ((n < 0) || ((size_t)n >= sizeof(line_buffer))) {
        continue;
      }
    }

    /* Send line */
    (void)ftp_session_send_data(session, line_buffer, strlen(line_buffer));
  }

  closedir(dir);

  return FTP_OK;
}

static ftp_error_t cmd_list_common(ftp_session_t *session, const char *args,
                                   int detailed) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  char scratch[FTP_PATH_MAX];
  const char *path_arg;
  if (ftp_path_has_list_flag(args, session->cwd, scratch, sizeof(scratch))) {
    path_arg = ftp_path_skip_list_flag(args, session->cwd);
  } else {
    path_arg = (args != NULL && *args != '\0') ? args : session->cwd;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  if (detailed != 0) {
    ftp_log_line(FTP_LOG_INFO, "[DBG] LIST: opening data connection...");
  }
  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    if (detailed != 0) {
      char dbg[64];
      (void)snprintf(dbg, sizeof(dbg),
                     "[DBG] LIST: data conn FAILED err=%d", (int)err);
      ftp_log_line(FTP_LOG_INFO, dbg);
    }
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }
  if (detailed != 0) {
    ftp_log_line(FTP_LOG_INFO, "[DBG] LIST: data conn OK, sending 150");
  }

  (void)ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);
  usleep(50000);
  err = send_directory_listing(session, resolved, detailed);
  ftp_session_close_data_connection(session);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Error reading directory.");
  }
  return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
}

ftp_error_t cmd_LIST(ftp_session_t *session, const char *args) {
  return cmd_list_common(session, args, 1);
}

ftp_error_t cmd_NLST(ftp_session_t *session, const char *args) {
  return cmd_list_common(session, args, 0);
}

/* RFC 3659 machine-readable directory listing. */
ftp_error_t cmd_MLSD(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  const char *path_arg = (args != NULL) ? args : session->cwd;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  /* Keep 150 and 226 in separate control-channel reads for fragile clients. */
  usleep(50000);

  DIR *dir = opendir(resolved);
  if (dir != NULL) {
    struct dirent *entry;
    char line_buffer[FTP_LIST_LINE_SIZE];

    while ((entry = readdir(dir)) != NULL) {
      if ((strcmp(entry->d_name, ".") == 0) ||
          (strcmp(entry->d_name, "..") == 0)) {
        continue;
      }

      vfs_stat_t st;
      int have_stat = 0;
      char fullpath[FTP_PATH_MAX];
      int max_ml = (int)(sizeof(fullpath) - 2 - strlen(entry->d_name));
      if (max_ml < 0) { continue; }
      int n = snprintf(fullpath, sizeof(fullpath), "%.*s/%s", max_ml, resolved,
                       entry->d_name);
      if ((n >= 0) && ((size_t)n < sizeof(fullpath))) {
        if (vfs_stat(fullpath, &st) == FTP_OK) {
          have_stat = 1;
        }
      }

      if (have_stat == 0) {
        memset(&st, 0, sizeof(st));
        if (entry->d_type == DT_DIR) {
          st.mode = (uint32_t)S_IFDIR;
        } else {
          st.mode = (uint32_t)S_IFREG;
        }
      }

      if (mlsx_format_line(&st, entry->d_name, 0, line_buffer,
                           sizeof(line_buffer)) == FTP_OK) {
        size_t line_len = strlen(line_buffer);
        (void)ftp_session_send_data(session, line_buffer, line_len);
      }
    }

    closedir(dir);
  }

  ftp_session_close_data_connection(session);

  return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
}

ftp_error_t cmd_MLST(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  const char *path_arg = ((args != NULL) && (args[0] != '\0')) ? args
                                                                : session->cwd;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  vfs_stat_t st;
  err = vfs_stat(resolved, &st);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  char client_path[FTP_PATH_MAX];
  err = ftp_path_to_client_path(session, resolved, client_path,
                                sizeof(client_path));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  char listing_line[FTP_LIST_LINE_SIZE];
  err = mlsx_format_line(&st, client_path, 1, listing_line,
                         sizeof(listing_line));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "MLST formatting failed.");
  }

  char reply[FTP_REPLY_BUFFER_SIZE];
  int n = snprintf(reply, sizeof(reply), "250-Listing %.*s\r\n",
                   (int)(sizeof(reply) - 17), client_path);
  /* 250-Listing + \r\n + NUL = 17 bytes reserved */
  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "MLST formatting failed.");
  }

  err = send_control_raw(session, reply);
  if (err != FTP_OK) {
    return err;
  }

  err = send_control_raw(session, listing_line);
  if (err != FTP_OK) {
    return err;
  }

  return send_control_raw(session, "250 End\r\n");
}
