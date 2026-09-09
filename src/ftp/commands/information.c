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

/** @file information.c @brief FTP metadata, feature and help commands. */
#include "ftp_commands.h"
#include "ftp_instance.h"
#include "ftp_path.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include "pal_filesystem.h"
#include <stdio.h>
#include <sys/stat.h>
#include <time.h>

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

ftp_error_t cmd_STAT(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  {
    char msg[128];
    (void)snprintf(msg, sizeof(msg),
                   "Server status OK. Instance %016llx",
                   (unsigned long long)ftp_daemon_instance_id());
    return ftp_session_send_reply(session, FTP_REPLY_211_SYSTEM_STATUS, msg);
  }
}

ftp_error_t cmd_SYST(ftp_session_t *session, const char *args) {
  (void)args;

  if (session == NULL) {
    return FTP_ERR_INVALID_PARAM;
  }

  return ftp_session_send_reply(session, FTP_REPLY_215_SYSTEM_TYPE, NULL);
}

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
   *   XZFTPD INSTANCE <hex>
   *  211 End
   */
  char feat_instance[48];
  (void)snprintf(feat_instance, sizeof(feat_instance),
                 " XZFTPD INSTANCE %016llx",
                 (unsigned long long)ftp_daemon_instance_id());

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
                            " MLST type*;size*;modify*;unix.mode*;",
#endif
#if FTP_ENABLE_CRYPTO
                            " XCRYPT",
#endif
                            " CPFR",
                            " CPTO",
                            " COPY",
                            " SITE CHMOD",
                            feat_instance,
                            "End"};

  return ftp_session_send_multiline_reply(
      session, FTP_REPLY_211_SYSTEM_STATUS, features,
      sizeof(features) / sizeof(features[0]));
}

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
                         " SITE CHMOD",
                         "End"};

  return ftp_session_send_multiline_reply(session, FTP_REPLY_214_HELP, lines,
                                          sizeof(lines) / sizeof(lines[0]));
}
