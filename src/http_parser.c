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
 * @file http_parser.c
 * @brief Minimal HTTP/1.1 request parser (no strtok, no strncpy)
 *
 * Rewrites the original parser to avoid:
 *   - strtok() : hidden global state, not reentrant
 *   - strncpy(): no guaranteed NUL termination
 *
 * Uses bounded pointer arithmetic with explicit length checks.
 * The input buffer is mutated in-place (NUL inserted at delimiters)
 * so header name/value pointers remain valid for the buffer's lifetime.
 */

#include "http_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

/*===========================================================================*
 *  HELPERS
 *
 *   find_char()  — bounded strchr (stays within [p, end))
 *   find_crlf()  — bounded strstr for "\r\n"
 *===========================================================================*/

/**
 * @brief Find first occurrence of c in [p, end)
 * @return Pointer to c, or NULL if not found
 */
static char *find_char(char *p, const char *end, char c) {
  while (p < end) {
    if (*p == c) {
      return p;
    }
    p++;
  }
  return NULL;
}

/**
 * @brief Find "\r\n" in [p, end)
 * @return Pointer to the '\r', or NULL if not found
 */
static char *find_crlf(char *p, const char *end) {
  while ((p + 1) < end) {
    if (p[0] == '\r' && p[1] == '\n') {
      return p;
    }
    p++;
  }
  return NULL;
}

/*===========================================================================*
 *  PARSE REQUEST LINE
 *
 *   "GET /index.html HTTP/1.1\r\n"
 *    ^method  ^uri       ^version
 *
 *   Splits on spaces with bounded search, no strtok.
 *===========================================================================*/

/**
 * @brief Parse the request line: METHOD SP URI SP HTTP/x.y
 *
 * @param[in,out] line     Start of request line (NUL-terminated by caller)
 * @param[in]     line_len Length of the line (excluding NUL)
 * @param[out]    request  Populated with method, uri, version
 *
 * @return 0 on success, -1 on malformed request
 */
static int parse_request_line(char *line, size_t line_len,
                              http_request_t *request) {
  const char *end = line + line_len;

  /*-- METHOD --*/
  char *sp1 = find_char(line, end, ' ');
  if (sp1 == NULL) {
    return -1;
  }
  *sp1 = '\0';

  if (strcmp(line, "GET") == 0) {
    request->method = HTTP_METHOD_GET;
  } else if (strcmp(line, "POST") == 0) {
    request->method = HTTP_METHOD_POST;
  } else if (strcmp(line, "HEAD") == 0) {
    request->method = HTTP_METHOD_HEAD;
  } else {
    request->method = HTTP_METHOD_UNKNOWN;
  }

  /*-- URI --*/
  char *uri_start = sp1 + 1;
  if (uri_start >= end) {
    return -1;
  }
  char *sp2 = find_char(uri_start, end, ' ');
  if (sp2 == NULL) {
    return -1;
  }
  *sp2 = '\0';

  /* Bounded copy into fixed-size uri field */
  size_t uri_len = (size_t)(sp2 - uri_start);
  if (uri_len >= HTTP_URI_MAX_LENGTH) {
    uri_len = HTTP_URI_MAX_LENGTH - 1U;
  }
  memcpy(request->uri, uri_start, uri_len);
  request->uri[uri_len] = '\0';

  /*-- VERSION  "HTTP/x.y" --*/
  char *ver_start = sp2 + 1;
  if (ver_start >= end) {
    return -1;
  }
  if (sscanf(ver_start, "HTTP/%d.%d", &request->version_major,
             &request->version_minor) != 2) {
    return -1;
  }

  return 0;
}

/*===========================================================================*
 *  PARSE HEADERS
 *
 *   "Content-Type: application/json\r\n"
 *    ^name        ^colon  ^value
 *
 *   Each header line is NUL-terminated at the \r\n boundary.
 *   Colon is replaced with NUL to split name/value in-place.
 *   Leading whitespace on value is skipped.
 *===========================================================================*/

/**
 * @brief Parse a single header line into request->headers[]
 *
 * @param[in,out] line    Header line, already NUL-terminated
 * @param[out]    request Target request struct
 */
static void parse_header_line(char *line, http_request_t *request) {
  if (request->num_headers >= HTTP_HEADER_MAX_COUNT) {
    return; /* silently drop excess headers */
  }

  char *colon = strchr(line, ':');
  if (colon == NULL) {
    return; /* malformed header, skip */
  }

  *colon = '\0';
  char *value = colon + 1;

  /* Skip leading whitespace (OWS per RFC 7230 §3.2.6) */
  while (*value == ' ' || *value == '\t') {
    value++;
  }

  request->headers[request->num_headers].name = line;
  request->headers[request->num_headers].value = value;
  request->num_headers++;
}

/*===========================================================================*
 *  PUBLIC API
 *===========================================================================*/

/**
 * @brief Parse a complete HTTP/1.1 request from a mutable buffer
 *
 * The buffer is mutated in-place: NUL bytes are inserted at
 * line boundaries and colon separators so that request->uri,
 * header name/value, and body pointers reference the buffer
 * directly with zero-copy semantics.
 *
 * @param[in,out] buffer  Raw HTTP request data (mutable)
 * @param[in]     length  Number of valid bytes in buffer
 * @param[out]    request Parsed result
 *
 * @return 0 on success, negative on parse error
 *   -1: NULL input or malformed request line
 *   -2: missing CRLF (incomplete request)
 */
int http_parse_request(char *buffer, size_t length, http_request_t *request) {
  if ((buffer == NULL) || (request == NULL) || (length == 0U)) {
    return -1;
  }

  memset(request, 0, sizeof(*request));

  const char *buf_end = buffer + length;

  /*-- Request line --*/
  char *crlf = find_crlf(buffer, buf_end);
  if (crlf == NULL) {
    return -2;
  }
  *crlf = '\0'; /* NUL-terminate request line */

  size_t line_len = (size_t)(crlf - buffer);
  if (parse_request_line(buffer, line_len, request) != 0) {
    return -1;
  }

  /*-- Headers --*/
  char *line = crlf + 2; /* skip past \r\n */
  while (line < buf_end) {
    crlf = find_crlf(line, buf_end);
    if (crlf == NULL) {
      break; /* truncated headers, stop */
    }
    *crlf = '\0';

    /* Empty line = end of headers, body follows */
    if (line[0] == '\0') {
      char *body_start = crlf + 2;
      if (body_start < buf_end) {
        request->body = body_start;
        request->body_length = (size_t)(buf_end - body_start);
      }
      break;
    }

    parse_header_line(line, request);
    line = crlf + 2;
  }

  return 0;
}

/**
 * @brief Look up a header value by name (case-insensitive)
 *
 * @param request Parsed request
 * @param name    Header name to search for
 *
 * @return Header value string, or NULL if not found
 */
const char *http_get_header(const http_request_t *request, const char *name) {
  if ((request == NULL) || (name == NULL)) {
    return NULL;
  }

  for (size_t i = 0; i < request->num_headers; i++) {
    if (strcasecmp(request->headers[i].name, name) == 0) {
      return request->headers[i].value;
    }
  }

  return NULL;
}
