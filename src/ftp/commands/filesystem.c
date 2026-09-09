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

/** @file filesystem.c @brief FTP filesystem mutation and asynchronous copy commands. */
#include "ftp_commands.h"
#include "ftp_log.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static ftp_error_t start_async_copy(ftp_session_t *session,
                                    const char *src_ftp_path,
                                    const char *dst_ftp_path, int is_move);

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
  size_t res_len = strlen(resolved);
  size_t max_path = sizeof(reply) - 14; /* "..." created. + NUL */
  if (res_len > max_path) { res_len = max_path; }
  int n = snprintf(reply, sizeof(reply), "\"%.*s\" created.",
                   (int)res_len, resolved);

  /* VULN-05 fix: check for truncation (same as cmd_PWD) */
  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED,
                                  "Directory created.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}

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

  if (pal_path_exists(resolved) != 1) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }

  size_t len = strlen(resolved);
  if (len >= sizeof(session->rename_from)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Path too long.");
  }

  memcpy(session->rename_from, resolved, len + 1U);

  return ftp_session_send_reply(session, FTP_REPLY_350_PENDING,
                                "Ready for RNTO.");
}

ftp_error_t cmd_RNTO(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

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

  err = pal_file_rename(session->rename_from, resolved);

  if (err == FTP_ERR_CROSS_DEVICE) {
    ftp_error_t async_err =
        start_async_copy(session, session->rename_from, args, 1);
    session->rename_from[0] = '\0';
    return async_err;
  }

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

  /* Use pal_file_copy_recursive_ex so the OS errno is captured and visible
   * in the failure log.  Previously pal_file_copy_recursive was called with
   * no errno output, causing all error logs to show errno=0. */
  int copy_errno = 0;
  ftp_error_t err = pal_file_copy_recursive_ex(task->src_path, task->dst_path,
                                               !task->is_move, NULL, NULL,
                                               &copy_errno);

  pthread_mutex_lock(&session->copy_mutex);
  session->copy_in_progress = 0;
  pthread_mutex_unlock(&session->copy_mutex);

  if (err == FTP_OK) {
    ftp_log_line(FTP_LOG_INFO, "[COPY] Background task completed successfully");
  } else {
    char msg[256];
    snprintf(msg, sizeof(msg), "[COPY] Background task failed: err=%d errno=%d",
             (int)err, copy_errno);
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
