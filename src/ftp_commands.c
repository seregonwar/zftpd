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
 * @file ftp_commands.c
 * @brief FTP command handlers implementation
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 *
 * PROTOCOL: RFC 959 (File Transfer Protocol)
 * EXTENSIONS: RFC 3659 (MLST, MLSD, SIZE, MDTM)
 *
 */

#include "ftp_commands.h"
#include "ftp_buffer_pool.h"
#include "ftp_crypto.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include "pal_filesystem.h"
#include "pal_network.h"
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(__APPLE__) ||                                                      \
    (defined(__FreeBSD__) && !defined(PLATFORM_PS4) && !defined(PLATFORM_PS5))
#include <sys/mount.h>
#endif
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/*===========================================================================*
 * PFS FILE-CREATION SERIALISER
 *
 * On PS4/PS5, pal_file_open(O_CREAT) on a PFS-encrypted partition acquires
 * an inode-allocation lock inside the kernel.  When two FTP sessions call it
 * simultaneously the second one is forced to spin-wait on that same lock,
 * turning a ~5 s open into a >20 s open — past FileZilla's command-response
 * timeout — with 0 bytes transferred.
 *
 * Serialising O_CREAT opens at the application level lets each session
 * proceed without journal contention.  The total elapsed time for two
 * parallel uploads is ~10 s instead of >20 s, comfortably within the
 * FileZilla command-response timeout.
 *
 * The lock is only held during the open() call itself (typically 3–8 s on
 * PFS); the actual data transfer runs fully in parallel.
 *===========================================================================*/
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
#endif

/*===========================================================================*
 * FORWARD DECLARATIONS
 *===========================================================================*/

static ftp_error_t start_async_copy(ftp_session_t *session,
                                    const char *src_ftp_path,
                                    const char *dst_ftp_path, int is_move);

/*===========================================================================*
 * AUTHENTICATION AND CONTROL
 *===========================================================================*/

/**
 * @brief USER command - Specify user name
 */
ftp_error_t cmd_USER(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /*
   * Open server — accept any username and log in immediately.
   *
   *    Android apps (File Manager+, SuperFTP) expect 230 right
   *    after USER and never send PASS.  GoldHEN/ftpsrv does the
   *    same: USER always returns 230.
   *
   *    Clients that DO send PASS (FileZilla, WinSCP) will just
   *    receive a harmless 230 from cmd_PASS too.
   */
  (void)args;
  session->user_ok = 1U;
  session->authenticated = 1U;
  session->auth_attempts = 0U;
  return ftp_session_send_reply(session, FTP_REPLY_230_LOGGED_IN, NULL);
}

/**
 * @brief PASS command - Specify password
 */
ftp_error_t cmd_PASS(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Already logged in from USER — accept PASS as harmless no-op */
  session->authenticated = 1U;
  session->auth_attempts = 0U;
  return ftp_session_send_reply(session, FTP_REPLY_230_LOGGED_IN, NULL);
}

/**
 * @brief QUIT command - Terminate session
 */
ftp_error_t cmd_QUIT(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_221_GOODBYE, NULL);
}

/**
 * @brief NOOP command - No operation
 */
ftp_error_t cmd_NOOP(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, NULL);
}

/*===========================================================================*
 * NAVIGATION
 *===========================================================================*/

/**
 * @brief CWD command - Change working directory
 */
ftp_error_t cmd_CWD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Check if directory exists */
  int is_dir = pal_path_is_directory(resolved);

  if (is_dir != 1) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Not a directory.");
  }

  /* Update CWD */
  size_t len = strlen(resolved);
  if (len >= sizeof(session->cwd)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Path too long.");
  }

  memcpy(session->cwd, resolved, len + 1U);

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "Directory changed.");
}

/**
 * @brief CDUP command - Change to parent directory
 */
ftp_error_t cmd_CDUP(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Navigate to parent: "../" */
  return cmd_CWD(session, "..");
}

/**
 * @brief PWD command - Print working directory
 */
ftp_error_t cmd_PWD(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Format: 257 "pathname" */
  char reply[FTP_REPLY_BUFFER_SIZE];
  int n = snprintf(reply, sizeof(reply), "\"%s\" is current directory.",
                   session->cwd);

  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

/*===========================================================================*
 * DIRECTORY LISTING
 *===========================================================================*/

/**
 * @brief Helper: Send directory listing via data connection
 */
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
        int n =
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, entry->d_name);
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

/**
 * @brief LIST command - Detailed directory listing
 */
ftp_error_t cmd_LIST(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path (use CWD if no args) */
  const char *path_arg = (args != NULL) ? args : session->cwd;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Open data connection FIRST, then send 150.
   * GoldHEN/ftpsrv does accept() before 150. This ensures the
   * client receives 150 after data channel is ready, preventing
   * 150+226 from merging in the same TCP segment.
   */
  ftp_log_line(FTP_LOG_INFO, "[DBG] LIST: opening data connection...");
  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    char dbg[64];
    snprintf(dbg, sizeof(dbg), "[DBG] LIST: data conn FAILED err=%d", (int)err);
    ftp_log_line(FTP_LOG_INFO, dbg);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }
  ftp_log_line(FTP_LOG_INFO, "[DBG] LIST: data conn OK, sending 150");
  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  /* Protocol pacing: ensure the 150 reply is delivered as a
   * separate TCP segment before the data transfer + 226.
   * On LAN, LIST completes in microseconds, so 150 and 226
   * merge in the client's recv() buffer.  File Manager+
   * cannot handle multi-reply reads, causing Loading Error.
   */
  usleep(50000); /* 50 ms */

  /* Send listing */
  err = send_directory_listing(session, resolved, 1);

  /* Close data connection */
  ftp_session_close_data_connection(session);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Error reading directory.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
}

/**
 * @brief NLST command - Name list
 */
ftp_error_t cmd_NLST(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Similar to LIST but simpler format */
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

  /* Protocol pacing: prevent 150 and 226 from merging */
  usleep(50000); /* 50 ms */

  err = send_directory_listing(session, resolved, 0);

  ftp_session_close_data_connection(session);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Error reading directory.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
}

/**
 * @brief MLSD command - Machine listing (RFC 3659)
 *
 *   Unlike LIST which outputs human-readable "ls -l" lines,
 *   MLSD outputs machine-readable facts per entry:
 *
 *     type=dir;size=0;modify=20250222120000; dirname
 *     type=file;size=1234;modify=20250222120000; filename
 *
 *   Android apps (File Manager+, SuperFTP) use MLSD exclusively
 *   and cannot parse LIST format.
 */
ftp_error_t cmd_MLSD(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path (use CWD if no args) */
  const char *path_arg = (args != NULL) ? args : session->cwd;

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Open data connection FIRST, then send 150 (same as LIST) */
  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  /* Protocol pacing: prevent 150 and 226 from merging */
  usleep(50000); /* 50 ms */

  /* Read directory and send machine-readable entries */
  DIR *dir = opendir(resolved);
  if (dir != NULL) {
    struct dirent *entry;
    char line_buffer[FTP_LIST_LINE_SIZE];

    while ((entry = readdir(dir)) != NULL) {
      /* Skip . and .. */
      if ((strcmp(entry->d_name, ".") == 0) ||
          (strcmp(entry->d_name, "..") == 0)) {
        continue;
      }

      /* Stat the entry */
      vfs_stat_t st;
      int have_stat = 0;
      char fullpath[FTP_PATH_MAX];
      int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", resolved,
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

      /* Format modify time: YYYYMMDDHHMMSS */
      struct tm tm_time;
      time_t mtime = (time_t)st.mtime;
      gmtime_r(&mtime, &tm_time);
      char timebuf[20];
      strftime(timebuf, sizeof(timebuf), "%Y%m%d%H%M%S", &tm_time);

      /*
       * RFC 3659 fact line format:
       *
       *   type=dir;size=0;modify=20250222120000; dirname\r\n
       *   type=file;size=1234;modify=20250222120000; filename\r\n
       *                                            ^
       *                                   space before name is required
       */
      const char *type_str = (((st.mode & S_IFMT) == S_IFDIR)) ? "dir" : "file";

      n = snprintf(line_buffer, sizeof(line_buffer),
                   "type=%s;size=%lld;modify=%s; %s\r\n", type_str,
                   (long long)st.size, timebuf, entry->d_name);

      if ((n > 0) && ((size_t)n < sizeof(line_buffer))) {
        (void)ftp_session_send_data(session, line_buffer, (size_t)n);
      }
    }

    closedir(dir);
  }

  /* Close data connection */
  ftp_session_close_data_connection(session);

  return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE, NULL);
}

/**
 * @brief MLST command - Machine list single file (RFC 3659)
 */
ftp_error_t cmd_MLST(ftp_session_t *session, const char *args) {
  /* Redirect to STAT which provides per-file info on the control channel */
  return cmd_STAT(session, args);
}

/*===========================================================================*
 * FILE TRANSFER
 *===========================================================================*/

/**
 * @brief RETR command - Retrieve (download) file
 */
ftp_error_t cmd_RETR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
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

  /* Handle REST (resume) offset */
  off_t offset = session->restart_offset;
  if ((offset < 0) || ((uint64_t)offset > file_size)) {
    vfs_close(&node);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid offset.");
  }
  vfs_set_offset(&node, (uint64_t)offset);

  /* Open data connection */
  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    vfs_close(&node);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  size_t remaining = (size_t)(file_size - (uint64_t)offset);
  uint64_t bytes_sent = 0U;

  /*
   * sendfile eligibility: kernel-to-kernel transfer
   *
   *   Disabled when encryption is active (XOR must happen in userspace)
   *   or when rate limiting is on (sendfile can't be throttled).
   */
  int use_sendfile = ((vfs_get_caps(&node) & VFS_CAP_SENDFILE) != 0U) &&
                     (FTP_TRANSFER_RATE_LIMIT_BPS == 0U);
#if FTP_ENABLE_CRYPTO
  if (session->crypto.active != 0U) {
    use_sendfile = 0;
  }
#endif

  /*=========================================================================*
   *  Transfer loop: sendfile with read() cooldown retry
   *
   *  PS4/PS5 exFAT driver stalls sendfile() after ~28 MB of continuous
   *  kernel-to-kernel transfer (page cache pressure). Instead of falling
   *  back to slow read() permanently, we alternate:
   *
   *     sendfile burst (kernel speed, ~28 MB)
   *       -> stall detected
   *       -> read() cooldown (1 MB, releases page pressure)
   *       -> retry sendfile (if it works, another 28 MB burst)
   *       -> repeat until file complete
   *
   *  If sendfile fails immediately on retry (0 bytes), the driver truly
   *  cannot recover and we finish the rest via read().
   *
   *    ┌──────────┐    stall    ┌──────────────┐   1 MB done  ┌──────────┐
   *    │ sendfile  │──────────►│ read cooldown │────────────►│ sendfile  │
   *    │  burst    │           │   (1 MB)      │             │  retry    │
   *    └─────┬────┘           └──────────────┘             └─────┬────┘
   *     done │                                          0 bytes │ ok
   *          ▼                                                  ▼
   *        [226]                                    ┌───────────────┐
   *                                                 │ read() finish │
   *                                                 └───────┬──────┘
   *                                                    done │
   *                                                         ▼
   *                                                       [226]
   *=========================================================================*/

/* Cooldown: bytes of read() between sendfile retries */
#define SENDFILE_COOLDOWN_BYTES (1U << 20) /* 1 MB */

  void *buf = NULL;
  size_t buf_sz = 0U;

  while (remaining > 0U) {

    /*-- sendfile burst --*/
    if (use_sendfile != 0) {
      pal_socket_cork(session->data_fd);
      int sf_sent_any = 0;

      while (remaining > 0U) {
        ssize_t sent =
            pal_sendfile(session->data_fd, node.fd, &offset, remaining);

        if (sent <= 0) {
          if ((sent < 0) && (errno == EINTR)) {
            continue;
          }
          /* Stalled: sync offset and switch to read() cooldown */
          vfs_set_offset(&node, (uint64_t)offset);

          if (sf_sent_any == 0) {
            /* sendfile failed immediately on retry -> give up */
            use_sendfile = 0;
          }
          break;
        }

        sf_sent_any = 1;
        remaining -= (size_t)sent;
        bytes_sent += (uint64_t)sent;
        session->last_activity = time(NULL);
        atomic_fetch_add(&session->stats.bytes_sent, (uint64_t)sent);
      }
      pal_socket_uncork(session->data_fd);

      if (remaining == 0U) {
        break; /* transfer complete */
      }
      if (use_sendfile != 0) {
        /* Stalled but sent some data: run cooldown then retry */
        continue;
      }
      /* use_sendfile == 0: permanent fallback, drop through */
    }

    /*-- read() path: cooldown (limited) or finish (unlimited) --*/
    if (buf == NULL) {
      buf = ftp_buffer_acquire();
      buf_sz = ftp_buffer_size();
    }
    if (buf == NULL) {
      remaining = 1U; /* can't allocate buffer, force 426 */
      break;
    }

    /*
     * If sendfile is still eligible, run cooldown for SENDFILE_COOLDOWN_BYTES
     * then break back to the outer loop to retry sendfile.
     * If sendfile is permanently disabled, run until transfer complete.
     */
    int can_retry_sf = ((vfs_get_caps(&node) & VFS_CAP_SENDFILE) != 0U) &&
                       (use_sendfile == 0) &&
                       (remaining > SENDFILE_COOLDOWN_BYTES);
    /* Only retry if sendfile actually worked at least once this transfer */
    if ((can_retry_sf != 0) && (bytes_sent > 0U)) {
      can_retry_sf = 1;
    } else {
      can_retry_sf = 0;
    }

    size_t cooldown_left = can_retry_sf ? SENDFILE_COOLDOWN_BYTES : remaining;
    int read_error = 0;

    pal_socket_cork(session->data_fd);
    while ((remaining > 0U) && (cooldown_left > 0U)) {
      size_t want = (remaining < buf_sz) ? remaining : buf_sz;
      if (want > cooldown_left) {
        want = cooldown_left;
      }
      ssize_t n = vfs_read(&node, buf, want);
      if (n <= 0) {
        if ((n < 0) && (errno == EINTR)) {
          continue;
        }
        read_error = 1;
        break;
      }

      ssize_t sent = ftp_session_send_data(session, buf, (size_t)n);
      if (sent != n) {
        remaining = 1U;
        read_error = 1;
        break;
      }

      bytes_sent += (uint64_t)sent;
      remaining -= (size_t)n;
      cooldown_left -= (size_t)n;
      session->last_activity = time(NULL);
    }
    pal_socket_uncork(session->data_fd);

    if (read_error != 0) {
      break;
    }

    /* After cooldown, re-enable sendfile for retry */
    if ((can_retry_sf != 0) && (remaining > 0U)) {
      use_sendfile = 1;
      /* Sync offset for sendfile: vfs_read already advanced the
         internal file position, read it back for sendfile's &offset */
      offset = (off_t)(file_size - (uint64_t)remaining);
    }
  }

  ftp_buffer_release(buf);
#undef SENDFILE_COOLDOWN_BYTES

  /* Cleanup */
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

/*===========================================================================*
 *  DOUBLE-BUFFERED WRITER — overlaps recv() and write()
 *
 *   ┌────────────┐  mutex+cond  ┌────────────┐
 *   │ FTP thread │ ──► swap ──► │ Writer thr │
 *   │  recv()    │              │  write()   │
 *   │  buf[0]    │              │  buf[1]    │
 *   └────────────┘              └────────────┘
 *
 *  The FTP thread fills the active buffer via recv(), then swaps
 *  buffers with the writer thread which drains the filled buffer
 *  to disk.  This overlaps network I/O with PFS crypto writes.
 *===========================================================================*/

typedef struct {
  void *buf[2];     /* two buffers (from pool or malloc)     */
  size_t len[2];    /* bytes stored in each buffer           */
  int active;       /* index currently being filled by recv  */
  int fd;           /* destination file descriptor           */
  int error;        /* writer error errno (0 = ok)           */
  int done;         /* set by recv thread on EOF/error       */
  uint64_t written; /* total bytes flushed to disk           */
  pthread_mutex_t mtx;
  pthread_cond_t cv_ready; /* writer waits: "data ready to write" */
  pthread_cond_t cv_free;  /* recv waits:  "buffer free to fill"  */
} stor_pipe_t;

static void *stor_writer_thread(void *arg) {
  stor_pipe_t *p = (stor_pipe_t *)arg;

  pthread_mutex_lock(&p->mtx);
  for (;;) {
    /* Wait for data or done signal */
    while ((p->len[1 - p->active] == 0U) && (p->done == 0)) {
      pthread_cond_wait(&p->cv_ready, &p->mtx);
    }

    int drain = 1 - p->active;
    size_t nbytes = p->len[drain];

    if ((nbytes == 0U) && (p->done != 0)) {
      break; /* recv finished, nothing left to write */
    }

    /* Unlock while writing (slow PFS crypto path) */
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

    /* Signal recv thread that buffer is free */
    pthread_cond_signal(&p->cv_free);

    if (write_ok == 0) {
      break; /* write error */
    }
  }
  pthread_mutex_unlock(&p->mtx);
  return NULL;
}

/**
 * @brief STOR command - Store (upload) file
 *
 *  REST + STOR resume workflow
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  Client:  REST 52428800        <- set offset = 50 MB
 *  Server:  350 Restart accepted
 *  Client:  STOR bigfile.pkg     <- resumes here
 *  Server:  opens file WITHOUT O_TRUNC, lseek(offset)
 *           receives remaining bytes and writes from offset
 *
 *  If restart_offset == 0 the file is truncated as usual.
 */
ftp_error_t cmd_STOR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /*
   * Atomic write strategy
   * ~~~~~~~~~~~~~~~~~~~~~
   *   Fresh upload (offset == 0):
   *     write to  /path/.zftpd.tmp.FILENAME
   *     rename()  to final path on success
   *     → external daemons (ShadowMount) never see a partial file
   *
   *   Resume upload (offset > 0):
   *     write directly to the original file (need to lseek)
   */
  char tmp_path[FTP_PATH_MAX];
  int was_fresh_upload = (session->restart_offset == 0) ? 1 : 0;
#if defined(PLATFORM_PS5) || defined(PLATFORM_PS4)
  /* PS4/PS5 /data/: no ShadowMount watching, skip atomic temp→rename overhead.
   * On PS4, PFS-encrypted writes through a temp file add ~40ms per 256 KB
   * chunk; the double-buffer producer stalls waiting for the writer, the TCP
   * recv buffer fills, and the FileZilla client times out after 20 s. */
  int use_atomic = 0;
#else
  int use_atomic = (session->restart_offset == 0) ? 1 : 0;
#endif

  if (use_atomic != 0) {
    /* Build temp name: /dir/.zftpd.tmp.basename */
    const char *slash = strrchr(resolved, '/');
    if (slash != NULL) {
      size_t dir_len = (size_t)(slash - resolved);
      snprintf(tmp_path, sizeof(tmp_path), "%.*s/.zftpd.tmp.%s", (int)dir_len,
               resolved, slash + 1);
    } else {
      snprintf(tmp_path, sizeof(tmp_path), ".zftpd.tmp.%s", resolved);
    }
  }

  const char *write_path = (use_atomic != 0) ? tmp_path : resolved;

  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) {
    open_flags |= O_TRUNC;
  }

  /*
   * Open the destination file BEFORE sending 150 or accepting the data
   * connection.  This matches ftpsrv's proven ordering.
   *
   * On PS4/PS5, pal_file_open(O_CREAT|O_TRUNC) on a PFS-encrypted partition
   * (/data/pkg/) can block for several seconds while the filesystem allocates
   * and encrypts the inode.  The critical insight is WHERE that latency is
   * hidden:
   *
   *   Wrong order (150 → accept → open):
   *     The client receives 150 and immediately connects / starts sending.
   *     The server is still blocked in open().  The TCP receive buffer fills
   *     in milliseconds (LAN speed >> PFS throughput), the window drops to 0,
   *     the sender stalls, and FileZilla's 20-second DATA INACTIVITY timer
   *     fires — even though 0 bytes were transferred.
   *
   *   Correct order (open → 150 → accept):
   *     The latency is absorbed while the client is waiting for the STOR
   *     command response (FileZilla's COMMAND-RESPONSE timeout, 20 s).
   *     When 150 finally arrives the server is already ready to call recv();
   *     data starts flowing immediately and the inactivity timer never fires.
   */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int held_pfs_mtx = 0;
  if ((open_flags & O_CREAT) != 0) {
    if (pfs_mutex_lock_timeout(&g_pfs_create_mtx, 10) == 0) {
      held_pfs_mtx = 1;
    } else {
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Server busy, please retry.");
    }
  }
#endif
  int fd = pal_file_open(write_path, open_flags, FILE_PERM);
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (held_pfs_mtx != 0) {
    pthread_mutex_unlock(&g_pfs_create_mtx);
  }
#endif
  if (fd < 0) {
    if (use_atomic != 0) {
      (void)unlink(tmp_path);
    }
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot create file.");
  }

  /* Seek to restart offset for resume uploads */
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

  /*=========================================================================*
   *  Receive/write strategy: single-buffer on PS4/PS5, double-buffer elsewhere
   *
   *  WHY DOUBLE-BUFFER FAILS ON PS4/PS5 (root-cause analysis)
   *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   *  The double-buffer design requires that the TCP kernel receive buffer
   *  (SO_RCVBUF) is large enough to absorb all incoming data during the
   *  interval when both application buffers are full and recv() is not
   *  being called (= the writer thread's PFS write latency per chunk).
   *
   *  On OrbisOS (PS4/PS5), setsockopt(SO_RCVBUF) on an already-accepted
   *  socket is silently capped to the system default (~256–512 KB on the
   *  firmware versions tested), regardless of the requested value.
   *  Setting it on the LISTENING socket before bind()/listen() should
   *  propagate 4 MB via the 3-way handshake, but this is unreliable across
   *  firmware versions — empirically the accepted socket often retains the
   *  kernel default of ~256 KB.
   *
   *  Result: with SO_RCVBUF ≈ 256 KB and two 256 KB app buffers:
   *
   *    stall point = app_bufs + kernel_rcvbuf ≈ 512 KB + 256 KB ≈ 768 KB
   *
   *  This matches the observed ~800 KB abort point precisely across all
   *  tested firmware versions (1.0 MB → 800 KB → 818 KB — always sub-1 MB).
   *
   *  After ~768 KB are received (in ~44 ms at 18 MB/s LAN), the TCP window
   *  drops to zero.  FileZilla sees no ACKs while the producer waits on
   *  pthread_cond_wait.  After 20 s of zero-window, FileZilla fires the
   *  data-inactivity timeout and aborts the connection.
   *
   *  WHY SINGLE-BUFFER WORKS (same path as cmd_APPE)
   *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
   *  In single-buffer mode, recv() is called immediately after every
   *  write() returns.  The TCP window reopens within one write cycle (~14 ms
   *  at 18 MB/s). FileZilla's inactivity timer never accumulates.
   *
   *  cmd_APPE uses single-buffer and consistently achieves 17+ MB/s on the
   *  same PFS-encrypted /data/ partition, confirming that write latency is
   *  not a bottleneck once the file is open — the constraint is entirely
   *  the TCP zero-window imposed by the insufficient kernel recv buffer.
   *
   *  PLATFORM DECISION
   *  ~~~~~~~~~~~~~~~~~
   *  PS4/PS5 : acquire only one buffer → fall through to single-buffer path.
   *            SO_RCVBUF is unreliable; double-buffer causes zero-window
   *            stalls that trigger FileZilla's 20 s data-inactivity timeout.
   *  Other   : acquire two buffers → double-buffer path.
   *            SO_RCVBUF is fully controllable and the pipeline genuinely
   *            improves throughput.
   *=========================================================================*/

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /*
   * Force single-buffer on PS4/PS5: acquire only buf0 and leave buf1 = NULL.
   * The (buf0 == NULL || buf1 == NULL) branch below is the single-buffer path;
   * it handles NULL buf1 correctly (ftp_buffer_release(NULL) is a no-op).
   */
  void *buf0 = ftp_buffer_acquire();
  void *buf1 = NULL; /* intentionally NULL — forces single-buffer path */
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
    /*
     * Fallback: if we can't get two buffers, use single-buffer mode.
     * This happens when the pool is exhausted under heavy load.
     */
    void *buffer = (buf0 != NULL) ? buf0 : buf1;
    ftp_buffer_release((buf0 != NULL) ? buf1 : buf0);

    while (1) {
      if (buffer == NULL) {
        fail_stage = 1;
        ok = 0;
        break;
      }
      ssize_t n = ftp_session_recv_data(session, buffer, buf_sz);
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
      ssize_t written = pal_file_write_all(fd, buffer, (size_t)n);
      if (written != n) {
        saved_errno = errno;
        fail_stage = 3;
        ok = 0;
        break;
      }
      total_received += (uint64_t)n;
      session->last_activity = time(NULL);
    }

    ftp_buffer_release(buffer);
  } else {
    /*
     * Double-buffered path: spawn a writer thread.
     */
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
      /* Thread creation failed — fall back to single-buffer */
      while (1) {
        ssize_t n = ftp_session_recv_data(session, buf0, buf_sz);
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
        ssize_t written = pal_file_write_all(fd, buf0, (size_t)n);
        if (written != n) {
          saved_errno = errno;
          fail_stage = 3;
          ok = 0;
          break;
        }
        total_received += (uint64_t)n;
        session->last_activity = time(NULL);
      }
    } else {
      /*
       * Producer loop: fill buf[active], then hand off to writer.
       */
      while (1) {
        pthread_mutex_lock(&pipe.mtx);

        /* Wait for our active buffer to be free */
        while (pipe.len[pipe.active] != 0U) {
          if (pipe.error != 0) {
            pthread_mutex_unlock(&pipe.mtx);
            goto recv_done;
          }
          pthread_cond_wait(&pipe.cv_free, &pipe.mtx);
        }

        /* Check if writer hit an error */
        if (pipe.error != 0) {
          pthread_mutex_unlock(&pipe.mtx);
          break;
        }

        int fill_idx = pipe.active;
        pthread_mutex_unlock(&pipe.mtx);

        /* Receive into the free buffer (slow, don't hold lock) */
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
          break; /* EOF */
        }

        total_received += (uint64_t)n;
        session->last_activity = time(NULL);

        /* Hand the filled buffer to the writer */
        pthread_mutex_lock(&pipe.mtx);
        pipe.len[fill_idx] = (size_t)n;
        pipe.active = 1 - fill_idx; /* swap to other buffer */
        pthread_cond_signal(&pipe.cv_ready);
        pthread_mutex_unlock(&pipe.mtx);
      }

    recv_done:
      /* Signal writer that recv is done */
      pthread_mutex_lock(&pipe.mtx);
      pipe.done = 1;
      pthread_cond_signal(&pipe.cv_ready);
      pthread_mutex_unlock(&pipe.mtx);

      (void)pthread_join(writer, NULL);

      /* Collect writer result */
      if (pipe.error != 0) {
        saved_errno = pipe.error;
        fail_stage = 3;
        ok = 0;
      }
      total_received = pipe.written + (total_received - pipe.written);
    }

    pthread_mutex_destroy(&pipe.mtx);
    pthread_cond_destroy(&pipe.cv_ready);
    pthread_cond_destroy(&pipe.cv_free);
    ftp_buffer_release(buf0);
    ftp_buffer_release(buf1);
  }

  /*
   * Flush strategy — platform-specific
   *
   *   OrbisOS:  close() already flushes dirty pages to NVMe.
   *             An explicit fsync() forces a full controller barrier
   *             through PFS crypto — adds 5-15ms of pure overhead.
   *
   *   Linux:    fdatasync() flushes data only (skips metadata).
   *
   *   Other:    fsync() as safety default.
   */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  /* OrbisOS: close() flushes; skip explicit sync */
  (void)0;
#elif defined(__linux__)
  if (ok != 0) {
    (void)fdatasync(fd);
  }
#else
  if (ok != 0) {
    (void)fsync(fd);
  }
#endif
  pal_file_close(fd);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    /*
     * Atomic commit: rename temp → final
     *
     * rename() is atomic on POSIX: ShadowMount's stat() will
     * see either the old file or the new complete file, never
     * a half-written intermediate state.
     */
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

  /* On failure, clean up partial file */
  if (use_atomic != 0) {
    (void)unlink(tmp_path);
  } else if (was_fresh_upload != 0) {
    /*
     * Non-atomic path (PS4/PS5): the file was opened with O_CREAT|O_TRUNC
     * directly on the destination.  If the transfer failed, an empty or
     * partial file now sits on disk.  Delete it so that a subsequent LIST
     * does not show a ghost file and cause the client to prompt for
     * overwrite (or silently skip the upload).
     *
     * Resume uploads (was_fresh_upload == 0) are intentionally left alone
     * so the client can attempt REST+STOR/APPE again.
     */
    (void)unlink(write_path);
  }

  ftp_log_session_event(session, "STOR_FAIL", FTP_ERR_UNKNOWN, total_received);

  char detail[128];
  if (fail_stage == 2) {
    snprintf(detail, sizeof(detail),
             "Transfer failed: network receive error (errno=%d).", saved_errno);
  } else if (fail_stage == 3) {
    snprintf(detail, sizeof(detail),
             "Transfer failed: disk write error (errno=%d).", saved_errno);
  } else {
    snprintf(detail, sizeof(detail), "Transfer failed.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                detail);
}

/**
 * @brief APPE command - Append to file
 *
 *  REST + APPE resume workflow
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *  If restart_offset > 0, the file is opened for writing (not append)
 *  and seeked to the offset. Otherwise it opens with O_APPEND.
 */
ftp_error_t cmd_APPE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Resolve path */
  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /*
   * Open flags depend on REST offset:
   *
   *   offset == 0  ->  O_WRONLY | O_CREAT | O_APPEND  (true append)
   *   offset >  0  ->  O_WRONLY | O_CREAT             (seek to offset)
   */
  int open_flags = O_WRONLY | O_CREAT;
  if (session->restart_offset == 0) {
    open_flags |= O_APPEND;
  }

  /* Open file BEFORE sending 150 — same rationale as cmd_STOR */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  int held_pfs_mtx_appe = 0;
  if ((open_flags & O_CREAT) != 0) {
    if (pfs_mutex_lock_timeout(&g_pfs_create_mtx, 10) == 0) {
      held_pfs_mtx_appe = 1;
    } else {
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Server busy, please retry.");
    }
  }
#endif
  int fd = pal_file_open(resolved, open_flags, FILE_PERM);
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  if (held_pfs_mtx_appe != 0) {
    pthread_mutex_unlock(&g_pfs_create_mtx);
  }
#endif
  if (fd < 0) {
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot open file.");
  }

  /* Seek to restart offset when provided */
  if (session->restart_offset > 0) {
    if (lseek(fd, session->restart_offset, SEEK_SET) < 0) {
      pal_file_close(fd);
      session->restart_offset = 0;
      return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                    "Seek failed.");
    }
  }

  ftp_session_send_reply(session, FTP_REPLY_150_FILE_OK, NULL);

  err = ftp_session_open_data_connection(session);
  if (err != FTP_OK) {
    pal_file_close(fd);
    session->restart_offset = 0;
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA, NULL);
  }

  void *buffer = ftp_buffer_acquire();
  size_t buf_sz = ftp_buffer_size();
  uint64_t total_received = 0U;
  int ok = 1;
  int fail_stage = 0; /* 1 = no buffer, 2 = recv error, 3 = write error */
  int saved_errno = 0;

  while (1) {
    if (buffer == NULL) {
      fail_stage = 1;
      ok = 0;
      break;
    }
    ssize_t n = ftp_session_recv_data(session, buffer, buf_sz);

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

    ssize_t written = pal_file_write_all(fd, buffer, (size_t)n);
    if (written != n) {
      saved_errno = errno;
      fail_stage = 3;
      ok = 0;
      break;
    }

    total_received += (uint64_t)n;
    session->last_activity = time(NULL);
  }

  /*
   * Flush strategy — see cmd_STOR comment for rationale.
   */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
  (void)0;
#elif defined(__linux__)
  if (ok != 0) {
    (void)fdatasync(fd);
  }
#else
  if (ok != 0) {
    (void)fsync(fd);
  }
#endif
  pal_file_close(fd);
  ftp_buffer_release(buffer);
  ftp_session_close_data_connection(session);
  session->restart_offset = 0;

  if (ok != 0) {
    ftp_log_session_event(session, "APPE_OK", FTP_OK, total_received);
    return ftp_session_send_reply(session, FTP_REPLY_226_TRANSFER_COMPLETE,
                                  NULL);
  }

  ftp_log_session_event(session, "APPE_FAIL", FTP_ERR_UNKNOWN, total_received);

  char detail[128];
  if (fail_stage == 2) {
    snprintf(detail, sizeof(detail),
             "Transfer failed: network receive error (errno=%d).", saved_errno);
  } else if (fail_stage == 3) {
    snprintf(detail, sizeof(detail),
             "Transfer failed: disk write error (errno=%d).", saved_errno);
  } else {
    snprintf(detail, sizeof(detail), "Transfer failed.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_426_TRANSFER_ABORTED,
                                detail);
}

/**
 * @brief REST command - Set restart offset
 */
ftp_error_t cmd_REST(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Parse offset */
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

/*===========================================================================*
 * FILE MANAGEMENT
 *===========================================================================*/

/**
 * @brief DELE command - Delete file
 */
ftp_error_t cmd_DELE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_file_delete(resolved);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot delete file.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "File deleted.");
}

/**
 * @brief RMD command - Remove directory
 */
ftp_error_t cmd_RMD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_dir_remove(resolved);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot remove directory.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "Directory removed.");
}

/**
 * @brief MKD command - Make directory
 */
ftp_error_t cmd_MKD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  err = pal_dir_create(resolved, DIR_PERM);

  if (err != FTP_OK) {
    if ((err == FTP_ERR_DIR_EXISTS) && (pal_path_is_directory(resolved) == 1)) {
      /*
       * +---------------------------------------------------------+
       * | CONCURRENCY HANDLING                                    |
       * | Directory was just created by another active thread.    |
       * | We treat this EEXIST as a success to prevent FileZilla  |
       * | from aborting the entire directory tree upload.         |
       * +---------------------------------------------------------+
       */
    } else {
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Cannot create directory.");
    }
  }

  char reply[FTP_REPLY_BUFFER_SIZE];
  int n = snprintf(reply, sizeof(reply), "\"%s\" created.", resolved);

  /* VULN-05 fix: check for truncation (same as cmd_PWD) */
  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED,
                                  "Directory created.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

/**
 * @brief RNFR command - Rename from
 */
ftp_error_t cmd_RNFR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Check if file exists */
  if (pal_path_exists(resolved) != 1) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  /* Store source path */
  size_t len = strlen(resolved);
  if (len >= sizeof(session->rename_from)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Path too long.");
  }

  memcpy(session->rename_from, resolved, len + 1U);

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "Ready for RNTO.");
}

/**
 * @brief RNTO command - Rename to
 */
ftp_error_t cmd_RNTO(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Check if RNFR was called */
  if (session->rename_from[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "RNFR required first.");
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    session->rename_from[0] = '\0';
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  /* Perform rename */
  err = pal_file_rename(session->rename_from, resolved);

  if (err == FTP_ERR_CROSS_DEVICE) {
    ftp_error_t async_err =
        start_async_copy(session, session->rename_from, args, 1);
    /* Clear rename_from state */
    session->rename_from[0] = '\0';
    return async_err;
  }

  /* Clear rename_from */
  session->rename_from[0] = '\0';

  if (err != FTP_OK) {
    /*
     * Map ftp_error_t to a human-readable detail so the FTP client
     * (FileZilla, WinSCP, etc.) shows something actionable instead
     * of the opaque "Rename failed.".
     *
     *   550 Permission denied.
     *   550 Source not found.
     *   550 Path too long.
     *   ...
     */
    const char *detail;
    switch (err) {
    case FTP_ERR_NOT_FOUND:
      detail = "Source not found.";
      break;
    case FTP_ERR_PERMISSION:
      detail = "Permission denied.";
      break;
    case FTP_ERR_PATH_TOO_LONG:
      detail = "Path too long.";
      break;
    case FTP_ERR_OUT_OF_MEMORY:
      detail = "Out of memory.";
      break;
    case FTP_ERR_DIR_OPEN:
      detail = "Cannot open directory.";
      break;
    case FTP_ERR_FILE_OPEN:
      detail = "Cannot open file.";
      break;
    case FTP_ERR_FILE_READ:
      detail = "Read error during copy.";
      break;
    default: {
      static _Thread_local char buf[64];
      snprintf(buf, sizeof(buf), "Rename failed (err=%d).", (int)err);
      detail = buf;
      break;
    }
    }
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR, detail);
  }

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "File renamed.");
}

/*===========================================================================*
 * DATA CONNECTION
 *===========================================================================*/

/**
 * @brief PORT command - Active mode data connection
 */
ftp_error_t cmd_PORT(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Parse h1,h2,h3,h4,p1,p2 */
  unsigned int h1, h2, h3, h4, p1, p2;

  if (sscanf(args, "%u,%u,%u,%u,%u,%u", &h1, &h2, &h3, &h4, &p1, &p2) != 6) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT format.");
  }

  /* Validate ranges */
  if ((h1 > 255U) || (h2 > 255U) || (h3 > 255U) || (h4 > 255U) || (p1 > 255U) ||
      (p2 > 255U)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid PORT values.");
  }

  /* Build IP address */
  char ip[INET_ADDRSTRLEN];
  snprintf(ip, sizeof(ip), "%u.%u.%u.%u", h1, h2, h3, h4);

  /* Build port */
  uint16_t port = (uint16_t)((p1 << 8) | p2);

  /* Debug: log PORT target */
  {
    char dbg[128];
    snprintf(dbg, sizeof(dbg), "[DBG] PORT target: %s:%u", ip, (unsigned)port);
    ftp_log_line(FTP_LOG_INFO, dbg);
  }
  /* Create sockaddr */
  ftp_error_t err = pal_make_sockaddr(ip, port, &session->data_addr);
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid address.");
  }

  /*
   * VULN-04 fix: validate PORT IP against control connection
   *
   *   RFC 2577 (FTP Security Considerations) recommends that
   *   servers verify the PORT IP matches the client's control
   *   connection IP to prevent SSRF (bounce attacks).
   *
   *   Compile with -DFTP_PORT_ALLOW_FOREIGN_IP=1 to disable
   *   this check for NAT environments (Android emulators).
   *
   *   control IP:  session->client_ip  (e.g. "192.168.1.50")
   *   PORT IP:     ip                  (e.g. "192.168.1.1")
   *   mismatch  -> 501 rejected
   */
#ifndef FTP_PORT_ALLOW_FOREIGN_IP
#define FTP_PORT_ALLOW_FOREIGN_IP 0
#endif

#if !FTP_PORT_ALLOW_FOREIGN_IP
  if (strcmp(ip, session->client_ip) != 0) {
    char dbg[128];
    snprintf(dbg, sizeof(dbg),
             "[SEC] PORT IP mismatch: client=%s port=%s (rejected)",
             session->client_ip, ip);
    ftp_log_line(FTP_LOG_INFO, dbg);
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "PORT address mismatch.");
  }
#endif

  /* Set mode to active */
  session->data_mode = FTP_DATA_MODE_ACTIVE;

  return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                "PORT command successful.");
}

/**
 * @brief PASV command - Passive mode data connection
 */
ftp_error_t cmd_PASV(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (session->pasv_fd >= 0) {
    PAL_CLOSE(session->pasv_fd);
    session->pasv_fd = -1;
  }

  /* Create passive listener socket */
  int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot create socket.");
  }

  /* Enable address reuse */
  (void)pal_socket_set_reuseaddr(fd);

  /*
   * Set SO_RCVBUF on the LISTENING socket BEFORE bind/listen.
   *
   * On FreeBSD/PS4/PS5 the kernel copies the listening socket's receive
   * buffer size into each accepted connection during the 3-way handshake.
   * Setting SO_RCVBUF on the accepted socket after accept() is too late:
   * the kernel caps post-connect increases to kern.ipc.maxsockbuf (~1 MB
   * on OrbisOS), which is why STOR transfers stall after exactly 1 MB.
   * Setting it here propagates the full 4 MB to every accepted data socket.
   */
  {
    int rcvbuf = (int)FTP_TCP_RCVBUF;
    (void)PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  /* Resolve the local IP from the control connection so the PASV
   * listener is bound to the specific interface — not INADDR_ANY.
   * On OrbisOS/FreeBSD (PS4/PS5), binding to INADDR_ANY causes
   * inbound SYNs to be silently dropped when the kernel routes the
   * incoming connection via a specific interface that doesn't match
   * the wildcard binding, even though the 227 reply advertises the
   * correct IP.  Binding to the exact local address fixes this. */
  uint32_t ip = 0U;
  {
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));
    if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr *)&local,
                        &local_len) == 0) {
      ip = PAL_NTOHL(local.sin_addr.s_addr);
    }
  }
  if (ip == 0U) {
    char ip_str[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK) {
      struct in_addr ia;
      if (PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
        ip = PAL_NTOHL(ia.s_addr);
      }
    }
  }

  /* Bind to the resolved local IP, ephemeral port (port 0 = auto-assign) */
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = (ip != 0U) ? PAL_HTONL(ip) : PAL_HTONL(INADDR_ANY);
  addr.sin_port = 0; /* Auto-assign port */

  if (PAL_BIND(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Bind failed.");
  }

  /* Listen */
  if (PAL_LISTEN(fd, 1) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Listen failed.");
  }

  /* Get assigned port */
  struct sockaddr_in pasv_addr;
  socklen_t addr_len = sizeof(pasv_addr);
  if (PAL_GETSOCKNAME(fd, (struct sockaddr *)&pasv_addr, &addr_len) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot get socket name.");
  }

  session->pasv_fd = fd;
  session->data_mode = FTP_DATA_MODE_PASSIVE;

  /* Format reply: 227 Entering Passive Mode (h1,h2,h3,h4,p1,p2) */
  if (ip == 0U) {
    ip = PAL_NTOHL(pasv_addr.sin_addr.s_addr);
  }
  uint16_t port = PAL_NTOHS(pasv_addr.sin_port);

  unsigned int h1 = (ip >> 24) & 0xFFU;
  unsigned int h2 = (ip >> 16) & 0xFFU;
  unsigned int h3 = (ip >> 8) & 0xFFU;
  unsigned int h4 = ip & 0xFFU;
  unsigned int p1 = (port >> 8) & 0xFFU;
  unsigned int p2 = port & 0xFFU;

  char reply[FTP_REPLY_BUFFER_SIZE];
  snprintf(reply, sizeof(reply), "Entering Passive Mode (%u,%u,%u,%u,%u,%u).",
           h1, h2, h3, h4, p1, p2);

  return ftp_session_send_reply(session, FTP_REPLY_227_PASV_MODE, reply);
}

/*---------------------------------------------------------------------------*
 * EPSV  (RFC 2428 — Extended Passive Mode)
 *
 *   Client:  EPSV
 *   Server:  229 Entering Extended Passive Mode (|||port|)
 *
 *   WinSCP and many IPv6-aware clients try EPSV first.
 *   Without it they fall back to PORT which often fails behind NAT.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_EPSV(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Reuse PASV socket setup */
  if (session->pasv_fd >= 0) {
    PAL_CLOSE(session->pasv_fd);
    session->pasv_fd = -1;
  }

  int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) {
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot create socket.");
  }

  (void)pal_socket_set_reuseaddr(fd);

  /* Set SO_RCVBUF on the listener — same rationale as cmd_PASV */
  {
    int rcvbuf = (int)FTP_TCP_RCVBUF;
    (void)PAL_SETSOCKOPT(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  }

  /* Bind to the local IP of the control connection — not INADDR_ANY.
   * Same rationale as cmd_PASV: on OrbisOS/FreeBSD, INADDR_ANY causes
   * inbound SYNs to be silently dropped. */
  uint32_t epsv_ip = 0U;
  {
    struct sockaddr_in local;
    socklen_t local_len = (socklen_t)sizeof(local);
    memset(&local, 0, sizeof(local));
    if (PAL_GETSOCKNAME(session->ctrl_fd, (struct sockaddr *)&local,
                        &local_len) == 0) {
      epsv_ip = PAL_NTOHL(local.sin_addr.s_addr);
    }
  }
  if (epsv_ip == 0U) {
    char ip_str[INET_ADDRSTRLEN];
    if (pal_network_get_primary_ip(ip_str, sizeof(ip_str)) == FTP_OK) {
      struct in_addr ia;
      if (PAL_INET_PTON(AF_INET, ip_str, &ia) == 1) {
        epsv_ip = PAL_NTOHL(ia.s_addr);
      }
    }
  }

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = (epsv_ip != 0U) ? PAL_HTONL(epsv_ip) : PAL_HTONL(INADDR_ANY);
  addr.sin_port = 0;

  if (PAL_BIND(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Bind failed.");
  }

  if (PAL_LISTEN(fd, 1) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Listen failed.");
  }

  struct sockaddr_in pasv_addr;
  socklen_t addr_len = sizeof(pasv_addr);
  if (PAL_GETSOCKNAME(fd, (struct sockaddr *)&pasv_addr, &addr_len) < 0) {
    PAL_CLOSE(fd);
    return ftp_session_send_reply(session, FTP_REPLY_425_CANT_OPEN_DATA,
                                  "Cannot get socket name.");
  }

  session->pasv_fd = fd;
  session->data_mode = FTP_DATA_MODE_PASSIVE;

  /*
   * RFC 2428: 229 Entering Extended Passive Mode (|||port|)
   *
   * The triple-pipe delimiter is protocol-agnostic (works for IPv4 + IPv6).
   * The client already knows the server IP from the control connection.
   */
  uint16_t port = PAL_NTOHS(pasv_addr.sin_port);
  char reply[FTP_REPLY_BUFFER_SIZE];
  snprintf(reply, sizeof(reply), "Entering Extended Passive Mode (|||%u|).",
           (unsigned)port);

  return ftp_session_send_reply(session, FTP_REPLY_229_EPSV_MODE, reply);
}

/*---------------------------------------------------------------------------*
 * OPTS  (RFC 2389 — Feature Negotiation)
 *
 *   Client:  OPTS UTF8 ON
 *   Server:  200 UTF8 mode enabled.
 *
 *   Almost every modern client sends "OPTS UTF8 ON" right after FEAT.
 *   Without this command, they get 500 Unknown Command → may disconnect.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_OPTS(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "OPTS requires an argument.");
  }

  /* Case-insensitive check for "UTF8 ON" / "UTF8" / "utf8 on" */
  char upper[64];
  size_t len = strlen(args);
  if (len >= sizeof(upper)) {
    len = sizeof(upper) - 1U;
  }
  for (size_t i = 0U; i < len; i++) {
    upper[i] = (char)toupper((unsigned char)args[i]);
  }
  upper[len] = '\0';

  if ((strncmp(upper, "UTF8", 4) == 0) &&
      (len == 4U || strcmp(upper + 4, " ON") == 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "UTF8 mode enabled.");
  }

  /*
   *  OPTS MLST type*;size*;modify*;
   *  Some clients send this to negotiate MLST facts.
   *  Accept it silently.
   */
  if (strncmp(upper, "MLST", 4) == 0) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK, "MLST OPTS set.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                "Option not recognized.");
}

/*---------------------------------------------------------------------------*
 * SITE  (RFC 959 — Site-Specific Commands)
 *
 *   Client:  SITE CHMOD 755 somefile.txt
 *   Server:  200 CHMOD ok.          (no-op on consoles)
 *
 *   WinSCP sends SITE CHMOD after every upload. Without this
 *   command the client logs errors and some abort the transfer.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_SITE(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL || args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE requires a command.");
  }

  /* Accept CHMOD as a no-op (console filesystems don't use UNIX perms) */
  char upper[16];
  size_t len = strlen(args);
  if (len > 5U) {
    len = 5U;
  }
  for (size_t i = 0U; i < len; i++) {
    upper[i] = (char)toupper((unsigned char)args[i]);
  }
  upper[len] = '\0';

  if (strncmp(upper, "CHMOD", 5) == 0) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "CHMOD command successful.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_502_NOT_IMPLEMENTED,
                                "SITE command not supported.");
}

/*---------------------------------------------------------------------------*
 * CLNT  (Client Identification)
 *
 *   Client:  CLNT SuperFTP/1.0
 *   Server:  200 Noted.
 *
 *   Android apps (File Manager+, SuperFTP) send CLNT to identify
 *   themselves before USER/PASS. Without it they get 500 Unknown
 *   Command and disconnect immediately.
 *---------------------------------------------------------------------------*/

ftp_error_t cmd_CLNT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, "Noted.");
}

/*===========================================================================*
 * INFORMATION
 *===========================================================================*/

/**
 * @brief SIZE command - Return file size
 */
ftp_error_t cmd_SIZE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

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

  char reply[64];
  snprintf(reply, sizeof(reply), "%llu", (unsigned long long)st.size);

  return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief MDTM command - Return modification time
 */
ftp_error_t cmd_MDTM(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  struct stat st;
  err = pal_file_stat(resolved, &st);

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  /* Format: YYYYMMDDhhmmss */
  struct tm tm_time;
  gmtime_r(&st.st_mtime, &tm_time);

  char reply[32];
  snprintf(reply, sizeof(reply), "%04d%02d%02d%02d%02d%02d",
           tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
           tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec);

  return ftp_session_send_reply(session, FTP_REPLY_213_FILE_STATUS, reply);
}

/**
 * @brief STAT command - Status
 */
ftp_error_t cmd_STAT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Simple status reply */
  return ftp_session_send_reply(session, FTP_REPLY_211_SYSTEM_STATUS,
                                "Server status OK.");
}

/**
 * @brief SYST command - System type
 */
ftp_error_t cmd_SYST(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_215_SYSTEM_TYPE, NULL);
}

/**
 * @brief FEAT command - Feature list
 */
ftp_error_t cmd_FEAT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /*
   * FEAT reply (RFC 2389)
   *
   *  211-Extensions supported:
   *   SIZE
   *   MDTM
   *   REST STREAM
   *   APPE
   *   UTF8
   *  211 End
   */
  const char *features[] = {"Extensions supported:",
#if FTP_ENABLE_SIZE
                            " SIZE",
#endif
#if FTP_ENABLE_MDTM
                            " MDTM",
#endif
#if FTP_ENABLE_REST
                            " REST STREAM",
#endif
                            " APPE",
                            " EPSV",
#if FTP_ENABLE_UTF8
                            " UTF8",
#endif
#if FTP_ENABLE_MLST
                            " MLSD",
                            " MLST",
#endif
#if FTP_ENABLE_CRYPTO
                            " XCRYPT",
#endif
                            " CPFR",
                            " CPTO",
                            " COPY",
                            "End"};

  return ftp_session_send_multiline_reply(
      session, FTP_REPLY_211_SYSTEM_STATUS, features,
      sizeof(features) / sizeof(features[0]));
}

/**
 * @brief HELP command - Help information
 */
ftp_error_t cmd_HELP(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  const char *lines[] = {"Supported commands:",
                         " USER PASS QUIT NOOP CWD CDUP PWD",
                         " LIST NLST MLSD MLST",
                         " RETR STOR APPE REST",
                         " DELE RMD MKD RNFR RNTO",
                         " PORT PASV SIZE MDTM STAT",
                         " SYST FEAT HELP TYPE MODE STRU",
                         "End"};

  return ftp_session_send_multiline_reply(session, FTP_REPLY_214_HELP, lines,
                                          sizeof(lines) / sizeof(lines[0]));
}

/*===========================================================================*
 * ASYNC BACKGROUND COPY
 *===========================================================================*/

typedef struct {
  ftp_session_t *session;
  char src_path[FTP_PATH_MAX];
  char dst_path[FTP_PATH_MAX];
  int is_move;
} ftp_copy_task_t;

static void *ftp_copy_thread_func(void *arg) {
  ftp_copy_task_t *task = (ftp_copy_task_t *)arg;
  ftp_session_t *session = task->session;

  ftp_log_line(FTP_LOG_INFO, "[COPY] Background task started");

  ftp_error_t err =
      pal_file_copy_recursive(task->src_path, task->dst_path, !task->is_move);

  pthread_mutex_lock(&session->copy_mutex);
  session->copy_in_progress = 0;
  pthread_mutex_unlock(&session->copy_mutex);

  if (err == FTP_OK) {
    ftp_log_line(FTP_LOG_INFO, "[COPY] Background task completed successfully");
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "[COPY] Background task failed: err=%d",
             (int)err);
    ftp_log_line(FTP_LOG_WARN, msg);
  }

  free(task);
  return NULL;
}

static ftp_error_t start_async_copy(ftp_session_t *session,
                                    const char *src_ftp_path,
                                    const char *dst_ftp_path, int is_move) {
  pthread_mutex_lock(&session->copy_mutex);
  if (session->copy_in_progress) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_450_FILE_UNAVAILABLE,
                                  "Operation already in progress.");
  }

  /* Validate paths */
  char src_resolved[FTP_PATH_MAX];
  char dst_resolved[FTP_PATH_MAX];
  if (ftp_path_resolve(session, src_ftp_path, src_resolved,
                       sizeof(src_resolved)) != FTP_OK ||
      ftp_path_resolve(session, dst_ftp_path, dst_resolved,
                       sizeof(dst_resolved)) != FTP_OK) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  if (strcmp(src_resolved, dst_resolved) == 0) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_553_FILENAME_INVALID,
                                  "Source and destination are the same.");
  }

  ftp_copy_task_t *task = malloc(sizeof(ftp_copy_task_t));
  if (task == NULL) {
    pthread_mutex_unlock(&session->copy_mutex);
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Memory allocation failed.");
  }

  task->session = session;
  strncpy(task->src_path, src_resolved, sizeof(task->src_path) - 1);
  task->src_path[sizeof(task->src_path) - 1] = '\0';
  strncpy(task->dst_path, dst_resolved, sizeof(task->dst_path) - 1);
  task->dst_path[sizeof(task->dst_path) - 1] = '\0';
  task->is_move = is_move;

  session->copy_in_progress = 1;

  if (session->copy_thread_valid) {
    pthread_join(session->copy_thread, NULL);
  }

  if (pthread_create(&session->copy_thread, NULL, ftp_copy_thread_func, task) !=
      0) {
    session->copy_in_progress = 0;
    session->copy_thread_valid = 0;
    pthread_mutex_unlock(&session->copy_mutex);
    free(task);
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Failed to create background thread.");
  }

  session->copy_thread_valid = 1;
  pthread_mutex_unlock(&session->copy_mutex);

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                is_move ? "Move started in background."
                                        : "Copy started in background.");
}

ftp_error_t cmd_CPFR(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: CPFR <path>");
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid source path.");
  }

  if (!pal_path_exists(resolved)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Source does not exist.");
  }

  strncpy(session->copy_from, args, sizeof(session->copy_from) - 1);
  session->copy_from[sizeof(session->copy_from) - 1] = '\0';

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "File exists, ready for destination name.");
}

ftp_error_t cmd_CPTO(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: CPTO <path>");
  }

  if (session->copy_from[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "Bad sequence of commands (use CPFR first).");
  }

  ftp_error_t result = start_async_copy(session, session->copy_from, args, 0);

  /* Clear the copy_from state regardless of success to prevent reuse */
  session->copy_from[0] = '\0';

  return result;
}

ftp_error_t cmd_COPY(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char src_arg[FTP_PATH_MAX];
  char dst_arg[FTP_PATH_MAX];

  const char *space = strchr(args, ' ');
  if (space == NULL) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: COPY <src> <dst>");
  }

  size_t src_len = (size_t)(space - args);
  if (src_len >= sizeof(src_arg)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Paths too long.");
  }

  strncpy(src_arg, args, src_len);
  src_arg[src_len] = '\0';

  const char *dst_start = space + 1;
  while (*dst_start == ' ')
    dst_start++; /* skip extra spaces */

  if (*dst_start == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Syntax: COPY <src> <dst>");
  }

  strncpy(dst_arg, dst_start, sizeof(dst_arg) - 1);
  dst_arg[sizeof(dst_arg) - 1] = '\0';

  return start_async_copy(session, src_arg, dst_arg, 0);
}

/*===========================================================================*
 * TRANSFER PARAMETERS
 *===========================================================================*/

/**
 * @brief TYPE command - Set transfer type
 */
ftp_error_t cmd_TYPE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'A') || (args[0] == 'a')) {
    session->transfer_type = FTP_TYPE_ASCII;
  } else if ((args[0] == 'I') || (args[0] == 'i')) {
    session->transfer_type = FTP_TYPE_BINARY;
  } else {
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                  "Type not supported.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, "Type set.");
}

/**
 * @brief MODE command - Set transfer mode
 */
ftp_error_t cmd_MODE(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'S') || (args[0] == 's')) {
    session->transfer_mode = FTP_MODE_STREAM;
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "Mode set to Stream.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                "Only Stream mode supported.");
}

/**
 * @brief STRU command - Set file structure
 */
ftp_error_t cmd_STRU(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((args[0] == 'F') || (args[0] == 'f')) {
    session->file_structure = FTP_STRU_FILE;
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "Structure set to File.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                "Only File structure supported.");
}

/*===========================================================================*
 * ENCRYPTION (ChaCha20)
 *
 *  AUTH XCRYPT handshake:
 *
 *    Client                          Server
 *    ──────                          ──────
 *    AUTH XCRYPT ──────────────────►
 *                ◄────────────────── 234 XCRYPT <24-hex-nonce>
 *
 *    Both sides derive:
 *      session_key = ChaCha20_KDF(PSK, nonce)
 *
 *    All subsequent traffic is XORed with ChaCha20 keystream.
 *
 *===========================================================================*/

#if FTP_ENABLE_CRYPTO

/**
 * @brief Convert nibble (0-15) to hex character
 */
static char nibble_to_hex(uint8_t n) {
  return (n < 10U) ? (char)('0' + n) : (char)('a' + (n - 10U));
}

/**
 * @brief Generate cryptographic random nonce from /dev/urandom or fallback
 */
static int generate_nonce(uint8_t *buf, size_t len) {
  /*
   * /dev/urandom is available on Linux, macOS.
   * Falls back to time-based PRNG if unavailable.
   */
  int fd = pal_file_open("/dev/urandom", O_RDONLY, 0);
  if (fd >= 0) {
    ssize_t n = pal_file_read(fd, buf, len);
    pal_file_close(fd);
    if (n == (ssize_t)len) {
      return 0;
    }
  }

  /* Fallback: time-based seed (weaker but functional) */
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  uint64_t seed = ((uint64_t)ts.tv_sec * 1000000000ULL) + (uint64_t)ts.tv_nsec;
  for (size_t i = 0U; i < len; i++) {
    seed = (seed * 6364136223846793005ULL) + 1442695040888963407ULL;
    buf[i] = (uint8_t)(seed >> 33U);
  }
  return 0;
}

ftp_error_t cmd_AUTH(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Only XCRYPT mechanism is supported */
  if ((strcmp(args, "XCRYPT") != 0) && (strcmp(args, "xcrypt") != 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                  "Unsupported AUTH mechanism.");
  }

  /* Already encrypted? */
  if (session->crypto.active != 0U) {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "Already encrypted.");
  }

  /* Generate 12-byte random nonce */
  uint8_t nonce[12];
  (void)generate_nonce(nonce, sizeof(nonce));

  /* Derive session key from PSK + nonce */
  static const uint8_t psk[32] = FTP_CRYPTO_PSK;
  uint8_t session_key[32];
  ftp_crypto_derive_key(psk, nonce, session_key);

  /* Format nonce as hex string for reply */
  char hex_nonce[25]; /* 24 hex chars + NUL */
  for (size_t i = 0U; i < 12U; i++) {
    hex_nonce[i * 2U] = nibble_to_hex((nonce[i] >> 4U) & 0x0FU);
    hex_nonce[(i * 2U) + 1U] = nibble_to_hex(nonce[i] & 0x0FU);
  }
  hex_nonce[24] = '\0';

  /* Reply: 234 XCRYPT <nonce-hex> */
  char reply_msg[64];
  (void)snprintf(reply_msg, sizeof(reply_msg), "XCRYPT %s", hex_nonce);
  ftp_error_t err =
      ftp_session_send_reply(session, FTP_REPLY_234_AUTH_OK, reply_msg);

  if (err == FTP_OK) {
    /* Activate encryption on this session */
    ftp_crypto_init(&session->crypto, session_key, nonce);
    ftp_log_session_event(session, "CRYPTO_ON", FTP_OK, 0U);
  }

  /* Scrub key material from stack */
  volatile uint8_t *vk = (volatile uint8_t *)session_key;
  for (size_t i = 0U; i < sizeof(session_key); i++) {
    vk[i] = 0U;
  }

  return err;
}

#endif /* FTP_ENABLE_CRYPTO */
