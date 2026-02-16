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
 * @file http_response.h
 * @brief HTTP response builder
 *
 * STATUS CODES:
 *   2xx  Success       (200, 201, 204)
 *   3xx  Redirection   (301, 304)
 *   4xx  Client error  (400, 403, 404, 405)
 *   5xx  Server error  (500)
 */

#ifndef HTTP_RESPONSE_H
#define HTTP_RESPONSE_H

#include "http_config.h"
#include <stddef.h>
#include <sys/types.h>

/*===========================================================================*
 * HTTP STATUS CODES
 *===========================================================================*/

typedef enum {
  /*  2xx ── Success  */
  HTTP_STATUS_200_OK = 200,
  HTTP_STATUS_201_CREATED = 201,
  HTTP_STATUS_204_NO_CONTENT = 204,

  /*  3xx ── Redirection  */
  HTTP_STATUS_301_MOVED = 301,
  HTTP_STATUS_304_NOT_MODIFIED = 304,

  /*  4xx ── Client Error  */
  HTTP_STATUS_400_BAD_REQUEST = 400,
  HTTP_STATUS_403_FORBIDDEN = 403,
  HTTP_STATUS_404_NOT_FOUND = 404,
  HTTP_STATUS_405_METHOD_NOT_ALLOWED = 405,

  /*  5xx ── Server Error  */
  HTTP_STATUS_500_INTERNAL_ERROR = 500,
} http_status_t;

/*===========================================================================*
 * RESPONSE BUFFER
 *===========================================================================*/

typedef struct {
  char data[HTTP_RESPONSE_BUFFER_SIZE];
  size_t used;

  /* sendfile() support for zero-copy file downloads */
  int sendfile_fd;       /**< File fd (-1 = not used)           */
  off_t sendfile_offset; /**< Current offset in file            */
  size_t sendfile_count; /**< Remaining bytes to send           */

  /* Chunked directory streaming (for /api/list) */
  void *stream_dir;       /**< DIR* — NULL = not streaming       */
  char stream_path[1024]; /**< Base path for stat() calls        */
} http_response_t;

/*===========================================================================*
 * PUBLIC API
 *===========================================================================*/

http_response_t *http_response_create(http_status_t status);
int http_response_add_header(http_response_t *resp, const char *name,
                             const char *value);
int http_response_set_body(http_response_t *resp, const void *body,
                           size_t length);
int http_response_append_raw(http_response_t *resp, const void *data,
                             size_t length);
int http_response_finalize(http_response_t *resp);
void http_response_destroy(http_response_t *resp);

#endif /* HTTP_RESPONSE_H */
