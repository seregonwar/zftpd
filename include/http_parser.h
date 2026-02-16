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
 * @file http_parser.h
 * @brief Minimal HTTP/1.1 parser
 */

#ifndef HTTP_PARSER_H
#define HTTP_PARSER_H

#include "http_config.h"
#include <stddef.h>

typedef enum {
    HTTP_METHOD_GET,
    HTTP_METHOD_POST,
    HTTP_METHOD_HEAD,
    HTTP_METHOD_UNKNOWN,
} http_method_t;

typedef struct {
    char *name;
    char *value;
} http_header_t;

typedef struct {
    http_method_t method;
    char uri[HTTP_URI_MAX_LENGTH];
    int version_major;
    int version_minor;
    http_header_t headers[HTTP_HEADER_MAX_COUNT];
    size_t num_headers;
    char *body;
    size_t body_length;
} http_request_t;

int http_parse_request(char *buffer, size_t length, http_request_t *request);
const char* http_get_header(const http_request_t *request, const char *name);

#endif /* HTTP_PARSER_H */
