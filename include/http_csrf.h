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
 * @file http_csrf.h
 * @brief CSRF Protection Module
 */

#ifndef HTTP_CSRF_H
#define HTTP_CSRF_H

#include "http_parser.h"
#include "http_server.h"

/**
 * @brief Initialize CSRF protection (generate random token)
 */
void http_csrf_init(void);

/**
 * @brief Get the current CSRF token
 * @return 32-character hex token string
 */
const char *http_csrf_get_token(void);

/**
 * @brief Validate CSRF token in request headers
 * @param req HTTP request to check
 * @return 0 if valid, -1 if missing or invalid
 */
int http_csrf_validate(const http_request_t *req);

#endif /* HTTP_CSRF_H */
