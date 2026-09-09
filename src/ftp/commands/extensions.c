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

/** @file extensions.c @brief FTP OPTS, SITE and client-extension commands. */
#include "ftp_commands.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
/* RFC 2389 options used by modern clients. */

ftp_error_t cmd_OPTS(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "OPTS requires an argument.");
  }

  char upper[64];
  size_t len = strlen(args);
  if (len >= sizeof(upper)) {
    len = sizeof(upper) - 1U;
  }
  for (size_t i = 0U; i < len; i++) {
    upper[i] = (char)toupper((unsigned char)args[i]);
  }
  upper[len] = '\0';

  if ((len >= 4U) && (strncmp(upper, "UTF8", 4) == 0) &&
      (len == 4U || strcmp(upper + 4, " ON") == 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                  "UTF8 mode enabled.");
  }

  if (strncmp(upper, "MLST", 4) == 0) {
    return ftp_session_send_reply(session, FTP_REPLY_200_OK, "MLST OPTS set.");
  }

  return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                "Option not recognized.");
}

/* SITE CHMOD reports the effective mode because some console mounts ignore chmod bits. */

ftp_error_t cmd_SITE(ftp_session_t *session, const char *args) {
  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  if (args == NULL || args[0] == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE requires a command.");
  }

  char verb[8];
  size_t vi = 0U;
  while ((args[vi] != '\0') && (args[vi] != ' ') && (args[vi] != '\t') &&
         (vi + 1U < sizeof(verb))) {
    verb[vi] = (char)toupper((unsigned char)args[vi]);
    vi++;
  }
  verb[vi] = '\0';

  if (strcmp(verb, "CHMOD") != 0) {
    return ftp_session_send_reply(session, FTP_REPLY_502_NOT_IMPLEMENTED,
                                  "SITE command not supported.");
  }

  const char *p = args + vi;
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  if (*p == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE CHMOD requires mode and path.");
  }

  char *end = NULL;
  errno = 0;
  unsigned long mode_ul = strtoul(p, &end, 8);
  if ((end == p) || (errno == ERANGE) || (mode_ul > 07777UL)) {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "Invalid CHMOD mode (octal 0-7777).");
  }

  while ((*end == ' ') || (*end == '\t')) {
    end++;
  }
  if (*end == '\0') {
    return ftp_session_send_reply(session, FTP_REPLY_501_SYNTAX_ARGS,
                                  "SITE CHMOD requires a path.");
  }

  const char *path_arg = end;
  size_t path_len = strlen(path_arg);
  char path_buf[FTP_PATH_MAX];
  if ((path_len >= 2U) &&
      (((path_arg[0] == '"') && (path_arg[path_len - 1U] == '"')) ||
       ((path_arg[0] == '\'') && (path_arg[path_len - 1U] == '\'')))) {
    if (path_len - 2U >= sizeof(path_buf)) {
      return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                    "Path too long.");
    }
    memcpy(path_buf, path_arg + 1, path_len - 2U);
    path_buf[path_len - 2U] = '\0';
    path_arg = path_buf;
  }

  char resolved[FTP_PATH_MAX];
  ftp_error_t err =
      ftp_path_resolve(session, path_arg, resolved, sizeof(resolved));
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Invalid path.");
  }

  mode_t mode = (mode_t)(mode_ul & 07777UL);
  err = pal_file_chmod(resolved, mode);
  if (err == FTP_ERR_NOT_FOUND) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "File not found.");
  }
  if (err == FTP_ERR_PERMISSION) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Permission denied or CHMOD unsupported "
                                  "on this filesystem.");
  }
  if (err != FTP_OK) {
    return ftp_session_send_reply(session, FTP_REPLY_550_FILE_ERROR,
                                  "Cannot change permissions.");
  }

  struct stat st;
  if (pal_file_stat(resolved, &st) == FTP_OK) {
    mode_t applied = (mode_t)(st.st_mode & 07777);
    if (applied != mode) {
      char msg[96];
      (void)snprintf(msg, sizeof(msg),
                     "CHMOD accepted but filesystem reports %04o "
                     "(requested %04o).",
                     (unsigned)applied, (unsigned)mode);
      return ftp_session_send_reply(session, FTP_REPLY_200_OK, msg);
    }
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK,
                                "CHMOD command successful.");
}

ftp_error_t cmd_CLNT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_200_OK, "Noted.");
}
