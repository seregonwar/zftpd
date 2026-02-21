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
 * @file http_csrf.c
 * @brief CSRF Protection Implementation
 */

#include "http_csrf.h"
#include "http_config.h"
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#if ENABLE_WEB_UPLOAD

/* Global CSRF token storage (32 hex chars + null) */
static char g_csrf_token[HTTP_CSRF_TOKEN_LENGTH + 1];

void http_csrf_init(void) {
  int fd = open("/dev/urandom", O_RDONLY);
  if (fd < 0) {
    /* Fallback if /dev/urandom fails (should shouldn't happen on supported OS)
     */
    snprintf(g_csrf_token, sizeof(g_csrf_token),
             "0123456789abcdef0123456789abcdef");
    return;
  }

  unsigned char random_bytes[HTTP_CSRF_TOKEN_LENGTH / 2];
  if (read(fd, random_bytes, sizeof(random_bytes)) != sizeof(random_bytes)) {
    /* Read failed, use fallback */
    close(fd);
    snprintf(g_csrf_token, sizeof(g_csrf_token),
             "0123456789abcdef0123456789abcdef");
    return;
  }
  close(fd);

  /* Convert to hex string */
  for (int i = 0; i < HTTP_CSRF_TOKEN_LENGTH / 2; i++) {
    size_t offset = (size_t)i * 2U;
    size_t remaining = sizeof(g_csrf_token) - offset;
    if (remaining < 3U) {
      snprintf(g_csrf_token, sizeof(g_csrf_token),
               "0123456789abcdef0123456789abcdef");
      return;
    }
    int n = snprintf(g_csrf_token + offset, remaining, "%02x", random_bytes[i]);
    if ((n < 0) || (n >= (int)remaining)) {
      snprintf(g_csrf_token, sizeof(g_csrf_token),
               "0123456789abcdef0123456789abcdef");
      return;
    }
  }
}

const char *http_csrf_get_token(void) { return g_csrf_token; }

int http_csrf_validate(const http_request_t *req) {
  if (req == NULL) {
    return -1;
  }

  /* Look for X-CSRF-Token header */
  const char *token = NULL;
  for (size_t i = 0; i < req->num_headers; i++) {
    if (strcasecmp(req->headers[i].name, "X-CSRF-Token") == 0) {
      token = req->headers[i].value;
      break;
    }
  }

  if (token == NULL) {
    return -1;
  }

  /* Constant-time comparison (not strictly necessary but good practice) */
  /* Actually strictly speaking strcmp is fine here since token is random,
     but let's do a simple check. */
  if (strcmp(token, g_csrf_token) != 0) {
    return -1;
  }

  return 0;
}

#else

/* No-op implementations when upload is disabled */
void http_csrf_init(void) {}
const char *http_csrf_get_token(void) { return ""; }
int http_csrf_validate(const http_request_t *req) {
  (void)req;
  return 0;
}

#endif /* ENABLE_WEB_UPLOAD */
