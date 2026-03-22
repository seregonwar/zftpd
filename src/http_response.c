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
 * @file http_response.c
 * @brief HTTP response builder implementation
 */

#include "http_response.h"
#include "http_config.h"
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static http_response_t g_response_pool[HTTP_MAX_CONNECTIONS];
static unsigned char g_response_in_use[HTTP_MAX_CONNECTIONS];

/*===========================================================================*
 * STATUS CODE → TEXT MAPPING
 *===========================================================================*/

static const char *status_text(http_status_t status) {
  switch (status) {
  /* 2xx */
  case HTTP_STATUS_200_OK:
    return "OK";
  case HTTP_STATUS_201_CREATED:
    return "Created";
  case HTTP_STATUS_204_NO_CONTENT:
    return "No Content";
  /* 3xx */
  case HTTP_STATUS_301_MOVED:
    return "Moved Permanently";
  case HTTP_STATUS_304_NOT_MODIFIED:
    return "Not Modified";
  /* 4xx */
  case HTTP_STATUS_400_BAD_REQUEST:
    return "Bad Request";
  case HTTP_STATUS_403_FORBIDDEN:
    return "Forbidden";
  case HTTP_STATUS_404_NOT_FOUND:
    return "Not Found";
  case HTTP_STATUS_405_METHOD_NOT_ALLOWED:
    return "Method Not Allowed";
  case HTTP_STATUS_409_CONFLICT:
    return "Conflict";
  /* 5xx */
  case HTTP_STATUS_500_INTERNAL_ERROR:
    return "Internal Server Error";
  default:
    return "Unknown";
  }
}

/*===========================================================================*
 * CREATE / DESTROY
 *===========================================================================*/

http_response_t *http_response_create(http_status_t status) {
  http_response_t *resp = NULL;
  for (size_t i = 0; i < (size_t)HTTP_MAX_CONNECTIONS; i++) {
    if (g_response_in_use[i] == 0U) {
      g_response_in_use[i] = 1U;
      resp = &g_response_pool[i];
      break;
    }
  }
  if (resp == NULL) {
    return NULL;
  }

  memset(resp, 0, sizeof(*resp));
  resp->sendfile_fd = -1;
  resp->stream_dir = NULL;
  resp->mem_body = NULL;
  resp->mem_length = 0U;
  resp->mem_sent = 0U;
  resp->mem_seg_count = 0U;
  resp->mem_seg_index = 0U;
  resp->mem_seg_sent = 0U;

  resp->used = (size_t)snprintf(resp->data, sizeof(resp->data),
                                "HTTP/1.1 %d %s\r\n", (int)status,
                                status_text(status));
  if (resp->used >= sizeof(resp->data)) {
    http_response_destroy(resp);
    return NULL;
  }

  if (http_response_add_header(resp, "X-Content-Type-Options", "nosniff") !=
          0 ||
      http_response_add_header(resp, "X-Frame-Options", "DENY") != 0 ||
      http_response_add_header(resp, "Referrer-Policy", "no-referrer") != 0 ||
      http_response_add_header(resp, "Cache-Control", "no-store") != 0 ||
      http_response_add_header(resp, "Content-Security-Policy",
                               "default-src 'self'; "
                               "connect-src *; "
                               "img-src 'self' data: blob:; "
                               "style-src 'self' 'unsafe-inline' https://fonts.googleapis.com; "
                               "style-src-elem 'self' 'unsafe-inline' https://fonts.googleapis.com; "
                               "font-src 'self' data: https://fonts.gstatic.com; "
                               "script-src 'self' 'unsafe-inline' blob:; "
                               "script-src-elem 'self' 'unsafe-inline' blob:; "
                               "object-src 'none'; base-uri 'none'; frame-ancestors 'none'") !=
          0) {
    http_response_destroy(resp);
    return NULL;
  }

  return resp;
}

void http_response_destroy(http_response_t *resp) {
  if (resp == NULL) {
    return;
  }

  if (resp->sendfile_fd >= 0) {
    close(resp->sendfile_fd);
    resp->sendfile_fd = -1;
  }
  if (resp->stream_dir != NULL) {
    closedir((DIR *)resp->stream_dir);
    resp->stream_dir = NULL;
  }
  if (resp->mem_body_owned && resp->mem_body != NULL) {
    void *tmp;
    memcpy(&tmp, &resp->mem_body, sizeof(tmp));
    free(tmp);
    resp->mem_body = NULL;
    resp->mem_body_owned = 0;
  }

  if ((resp >= &g_response_pool[0]) &&
      (resp < &g_response_pool[HTTP_MAX_CONNECTIONS])) {
    size_t idx = (size_t)(resp - &g_response_pool[0]);
    g_response_in_use[idx] = 0U;
  }
}

/*===========================================================================*
 * HEADERS
 *===========================================================================*/

/**
 * Grow the response buffer so that `extra` more bytes fit.
 */
static int ensure_space(http_response_t *resp, size_t extra) {
  if (resp == NULL) {
    return -1;
  }
  if (extra > (sizeof(resp->data) - resp->used)) {
    return -1;
  }
  return 0;
}

int http_response_add_header(http_response_t *resp, const char *name,
                             const char *value) {
  if (resp == NULL || name == NULL || value == NULL) {
    return -1;
  }

  int needed = snprintf(NULL, 0, "%s: %s\r\n", name, value);
  if (needed < 0) {
    return -1;
  }
  if (ensure_space(resp, (size_t)needed + 1U) < 0) {
    return -1;
  }

  resp->used +=
      (size_t)snprintf(resp->data + resp->used,
                       sizeof(resp->data) - resp->used,
                       "%s: %s\r\n", name, value);

#if HTTP_DEBUG_LOG_HEADERS
  printf("DEBUG: Added header %s: %s\n", name, value);
#endif
  return 0;
}

/*===========================================================================*
 * BODY
 *===========================================================================*/

int http_response_set_body(http_response_t *resp, const void *body,
                           size_t length) {
  if (resp == NULL || body == NULL) {
    return -1;
  }

  char len_str[32];
  int len_n = snprintf(len_str, sizeof(len_str), "%zu", length);
  if (len_n < 0) {
    return -1;
  }
  if ((size_t)len_n >= sizeof(len_str)) {
    return -1;
  }

  int hdr_needed = snprintf(NULL, 0, "Content-Length: %s\r\n", len_str);
  if (hdr_needed < 0) {
    return -1;
  }

  size_t needed = (size_t)hdr_needed + 2U + length;
  if (ensure_space(resp, needed) < 0) {
    return -1;
  }

  if (http_response_add_header(resp, "Content-Length", len_str) != 0) {
    return -1;
  }

  memcpy(resp->data + resp->used, "\r\n", 2);
  resp->used += 2;

  memcpy(resp->data + resp->used, body, length);
  resp->used += length;

  return 0;
}

int http_response_set_body_owned(http_response_t *resp, void *body,
                                 size_t length) {
  if (resp == NULL || body == NULL || length == 0U) {
    return -1;
  }
  if (resp->mem_body != NULL) {
    return -1;
  }

  char len_str[32];
  int len_n = snprintf(len_str, sizeof(len_str), "%zu", length);
  if (len_n < 0 || (size_t)len_n >= sizeof(len_str)) {
    return -1;
  }

  int hdr_needed = snprintf(NULL, 0, "Content-Length: %s\r\n", len_str);
  if (hdr_needed < 0) {
    return -1;
  }
  if (ensure_space(resp, (size_t)hdr_needed + 2U) < 0) {
    return -1;
  }
  if (http_response_add_header(resp, "Content-Length", len_str) != 0) {
    return -1;
  }
  /* Blank line terminating headers */
  resp->data[resp->used++] = '\r';
  resp->data[resp->used++] = '\n';

  resp->mem_body   = body;
  resp->mem_length = length;
  resp->mem_sent   = 0U;
  resp->mem_body_owned = 1;
  return 0;
}

int http_response_set_body_ref(http_response_t *resp, const void *body,
                               size_t length) {
  if (resp == NULL || body == NULL) {
    return -1;
  }
  if (resp->mem_body != NULL) {
    return -1;
  }
  if (length == 0U) {
    return -1;
  }

  char len_str[32];
  int len_n = snprintf(len_str, sizeof(len_str), "%zu", length);
  if (len_n < 0) {
    return -1;
  }
  if ((size_t)len_n >= sizeof(len_str)) {
    return -1;
  }

  int hdr_needed = snprintf(NULL, 0, "Content-Length: %s\r\n", len_str);
  if (hdr_needed < 0) {
    return -1;
  }

  if (ensure_space(resp, (size_t)hdr_needed + 2U) < 0) {
    return -1;
  }
  if (http_response_add_header(resp, "Content-Length", len_str) != 0) {
    return -1;
  }
  if (http_response_finalize(resp) != 0) {
    return -1;
  }

  resp->mem_body = body;
  resp->mem_length = length;
  resp->mem_sent = 0U;
  return 0;
}

static int size_add_checked(size_t a, size_t b, size_t *out) {
  if (out == NULL) {
    return -1;
  }
  if (a > (SIZE_MAX - b)) {
    return -1;
  }
  *out = a + b;
  return 0;
}

int http_response_set_body_splice(http_response_t *resp, const void *prefix,
                                  size_t prefix_len, const void *insert,
                                  size_t insert_len, const void *suffix,
                                  size_t suffix_len) {
  if ((resp == NULL) || (prefix == NULL) || (suffix == NULL)) {
    return -1;
  }
  if (resp->mem_seg_count != 0U) {
    return -1;
  }
  if ((insert_len > 0U) && (insert == NULL)) {
    return -1;
  }
  if (insert_len >= sizeof(resp->mem_inline)) {
    return -1;
  }

  size_t total = 0U;
  if (size_add_checked(prefix_len, insert_len, &total) != 0) {
    return -1;
  }
  if (size_add_checked(total, suffix_len, &total) != 0) {
    return -1;
  }
  if (total == 0U) {
    return -1;
  }

  if (insert_len > 0U) {
    memcpy(resp->mem_inline, insert, insert_len);
  }
  resp->mem_inline[insert_len] = '\0';

  char len_str[32];
  int len_n = snprintf(len_str, sizeof(len_str), "%zu", total);
  if (len_n < 0) {
    return -1;
  }
  if ((size_t)len_n >= sizeof(len_str)) {
    return -1;
  }

  int hdr_needed = snprintf(NULL, 0, "Content-Length: %s\r\n", len_str);
  if (hdr_needed < 0) {
    return -1;
  }

  if (ensure_space(resp, (size_t)hdr_needed + 2U) < 0) {
    return -1;
  }
  if (http_response_add_header(resp, "Content-Length", len_str) != 0) {
    return -1;
  }
  if (http_response_finalize(resp) != 0) {
    return -1;
  }

  resp->mem_segs[0] = prefix;
  resp->mem_lens[0] = prefix_len;
  resp->mem_segs[1] = (insert_len > 0U) ? (const void *)resp->mem_inline : NULL;
  resp->mem_lens[1] = insert_len;
  resp->mem_segs[2] = suffix;
  resp->mem_lens[2] = suffix_len;
  resp->mem_seg_count = 3U;
  resp->mem_seg_index = 0U;
  resp->mem_seg_sent = 0U;
  return 0;
}

int http_response_append_raw(http_response_t *resp, const void *data,
                             size_t length) {
  if (resp == NULL || data == NULL)
    return -1;
  if (ensure_space(resp, length) < 0)
    return -1;
  memcpy(resp->data + resp->used, data, length);
  resp->used += length;
  return 0;
}

/*===========================================================================*
 * FINALIZE (when body is already appended or there is no body)
 *===========================================================================*/

int http_response_finalize(http_response_t *resp) {
  if (resp == NULL) {
    return -1;
  }

  /* Append the blank line that terminates headers */
  if (ensure_space(resp, 2) < 0) {
    return -1;
  }

  memcpy(resp->data + resp->used, "\r\n", 2);
  resp->used += 2;

  return 0;
}
