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

/** @file control.c @brief FTP authentication, control and navigation commands. */
#include "ftp_commands.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_filesystem.h"
#include "pal_fileio.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>


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
  atomic_store(&session->state, FTP_STATE_AUTHENTICATED);
  return ftp_session_send_reply(session, FTP_REPLY_230_LOGGED_IN, NULL);
}

ftp_error_t cmd_PASS(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Already logged in from USER — accept PASS as harmless no-op */
  session->authenticated = 1U;
  session->auth_attempts = 0U;
  atomic_store(&session->state, FTP_STATE_AUTHENTICATED);
  return ftp_session_send_reply(session, FTP_REPLY_230_LOGGED_IN, NULL);
}

ftp_error_t cmd_QUIT(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_221_GOODBYE, NULL);
}

ftp_error_t cmd_NOOP(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, NULL);
}


ftp_error_t cmd_CWD(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err = ftp_path_resolve(session, args, resolved, sizeof(resolved));

  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  int is_dir = pal_path_is_directory(resolved);

  if (is_dir != 1) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Not a directory.");
  }

  size_t len = strlen(resolved);
  if (len >= sizeof(session->cwd)) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Path too long.");
  }

  memcpy(session->cwd, resolved, len + 1U);

  return ftp_session_send_reply(session, FTP_REPLY_250_FILE_ACTION_OK,
                                "Directory changed.");
}

ftp_error_t cmd_CDUP(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return cmd_CWD(session, "..");
}

ftp_error_t cmd_PWD(ftp_session_t *session, const char *args) {
  (void)args; /* Unused */

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  /* Format: 257 "pathname" — truncate path to fit the fixed-size reply buffer.
   * FTP_PATH_MAX (4096 on Linux) may exceed FTP_REPLY_BUFFER_SIZE (1024). */
  char reply[FTP_REPLY_BUFFER_SIZE];
  size_t cwd_len = strlen(session->cwd);
  size_t max_path = sizeof(reply) - 24; /* "..." is current directory. + NUL */
  if (cwd_len > max_path) { cwd_len = max_path; }
  int n = snprintf(reply, sizeof(reply), "\"%.*s\" is current directory.",
                   (int)cwd_len, session->cwd);

  if ((n < 0) || ((size_t)n >= sizeof(reply))) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_257_PATH_CREATED, reply);
}
