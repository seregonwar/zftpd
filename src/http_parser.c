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
#include "http_parser.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

int http_parse_request(char *buffer, size_t length, http_request_t *request) {
  if (!buffer || !request || length == 0)
    return -1;

  memset(request, 0, sizeof(*request));

  char *line = buffer;
  char *line_end = strstr(line, "\r\n");
  if (!line_end)
    return -2;
  *line_end = '\0';

  char *method_str = strtok(line, " ");
  char *uri_str = strtok(NULL, " ");
  char *version_str = strtok(NULL, " ");

  if (!method_str || !uri_str || !version_str)
    return -1;

  if (strcmp(method_str, "GET") == 0)
    request->method = HTTP_METHOD_GET;
  else if (strcmp(method_str, "POST") == 0)
    request->method = HTTP_METHOD_POST;
  else if (strcmp(method_str, "HEAD") == 0)
    request->method = HTTP_METHOD_HEAD;
  else
    request->method = HTTP_METHOD_UNKNOWN;

  strncpy(request->uri, uri_str, HTTP_URI_MAX_LENGTH - 1);

  if (sscanf(version_str, "HTTP/%d.%d", &request->version_major,
             &request->version_minor) != 2) {
    return -1;
  }

  line = line_end + 2;
  while ((line_end = strstr(line, "\r\n")) != NULL) {
    *line_end = '\0';

    if (line[0] == '\0') {
      request->body = line_end + 2;
      request->body_length = length - (size_t)(request->body - buffer);
      break;
    }

    char *colon = strchr(line, ':');
    if (colon && request->num_headers < HTTP_HEADER_MAX_COUNT) {
      *colon = '\0';
      char *value = colon + 1;
      while (*value == ' ')
        value++;

      request->headers[request->num_headers].name = line;
      request->headers[request->num_headers].value = value;
      request->num_headers++;
    }

    line = line_end + 2;
  }

  return 0;
}

const char *http_get_header(const http_request_t *request, const char *name) {
  if (!request || !name)
    return NULL;

  for (size_t i = 0; i < request->num_headers; i++) {
    if (strcasecmp(request->headers[i].name, name) == 0) {
      return request->headers[i].value;
    }
  }

  return NULL;
}
