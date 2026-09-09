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

/** @file transfer_params.c @brief FTP transfer parameters and optional AUTH XCRYPT. */
#include "ftp_commands.h"
#include "ftp_crypto.h"
#include "ftp_log.h"
#include "ftp_session.h"
#include "pal_fileio.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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

/* AUTH XCRYPT derives a per-session ChaCha20 key from the PSK and a 96-bit nonce. */

#if FTP_ENABLE_CRYPTO

static char nibble_to_hex(uint8_t n) {
  return (n < 10U) ? (char)('0' + n) : (char)('a' + (n - 10U));
}

static int generate_nonce(uint8_t *buf, size_t len) {
  int fd = pal_file_open("/dev/urandom", O_RDONLY, 0);
  if (fd < 0) return -1;

  size_t offset = 0U;
  while (offset < len) {
    ssize_t n = pal_file_read(fd, buf + offset, len - offset);
    if (n <= 0) {
      pal_file_close(fd);
      return -1;
    }
    offset += (size_t)n;
  }
  pal_file_close(fd);
  return 0;
}

ftp_error_t cmd_AUTH(ftp_session_t *session, const char *args) {
  if ((session == NULL) || (args == NULL)) {
    return FTP_ERR_INVALID_PARAM;
  }

  if ((strcmp(args, "XCRYPT") != 0) && (strcmp(args, "xcrypt") != 0)) {
    return ftp_session_send_reply(session, FTP_REPLY_504_NOT_IMPL_PARAM,
                                  "Unsupported AUTH mechanism.");
  }

  if (session->crypto.active != 0U) {
    return ftp_session_send_reply(session, FTP_REPLY_503_BAD_SEQUENCE,
                                  "Already encrypted.");
  }

  uint8_t nonce[12];
  if (generate_nonce(nonce, sizeof(nonce)) != 0) {
    return ftp_session_send_reply(session, FTP_REPLY_451_LOCAL_ERROR,
                                  "Secure random source unavailable.");
  }

  static const uint8_t psk[32] = FTP_CRYPTO_PSK;
  uint8_t session_key[32];
  ftp_crypto_derive_key(psk, nonce, session_key);

  char hex_nonce[25]; /* 24 hex chars + NUL */
  for (size_t i = 0U; i < 12U; i++) {
    hex_nonce[i * 2U] = nibble_to_hex((nonce[i] >> 4U) & 0x0FU);
    hex_nonce[(i * 2U) + 1U] = nibble_to_hex(nonce[i] & 0x0FU);
  }
  hex_nonce[24] = '\0';

  char reply_msg[64];
  (void)snprintf(reply_msg, sizeof(reply_msg), "XCRYPT %s", hex_nonce);
  ftp_error_t err =
      ftp_session_send_reply(session, FTP_REPLY_234_AUTH_OK, reply_msg);

  if (err == FTP_OK) {
    ftp_crypto_init(&session->crypto, session_key, nonce);
    ftp_log_session_event(session, "CRYPTO_ON", FTP_OK, 0U);
  }

  volatile uint8_t *vk = (volatile uint8_t *)session_key;
  for (size_t i = 0U; i < sizeof(session_key); i++) {
    vk[i] = 0U;
  }

  return err;
}

#endif /* FTP_ENABLE_CRYPTO */
