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
 * @file http_config.h
 * @brief HTTP server configuration
 */

#ifndef HTTP_CONFIG_H
#define HTTP_CONFIG_H

#include <stdint.h>

/* Compile-time feature toggle */
#ifndef ENABLE_ZHTTPD
#define ENABLE_ZHTTPD 1
#endif

#ifndef ENABLE_HTTP_GZIP
#define ENABLE_HTTP_GZIP 0
#endif

#ifndef HTTP_DEBUG_LOG_HEADERS
#define HTTP_DEBUG_LOG_HEADERS 0
#endif

/* Server configuration */
#define HTTP_DEFAULT_PORT 8080
#define HTTP_MAX_CONNECTIONS 100
#define HTTP_REQUEST_TIMEOUT 30
#define HTTP_KEEPALIVE_TIMEOUT 60

/* Buffer sizes */
#define HTTP_REQUEST_BUFFER_SIZE 8192
#define HTTP_RESPONSE_BUFFER_SIZE 8192
#define HTTP_URI_MAX_LENGTH 2048
#define HTTP_HEADER_MAX_COUNT 32
#define HTTP_HEADER_LINE_MAX 1024

/* File transfer */
#define HTTP_SENDFILE_CHUNK_SIZE 65536

/* Thread stack size (bytes) */
#ifndef HTTP_THREAD_STACK_SIZE
#define HTTP_THREAD_STACK_SIZE (512U * 1024U)
#endif

/* CSRF token length in hex characters (32 hex = 16 random bytes) */
#define HTTP_CSRF_TOKEN_LENGTH 32

/*---------------------------------------------------------------------------*
 * Upload feature toggle (disabled by default for security)
 *---------------------------------------------------------------------------*/
#ifndef ENABLE_WEB_UPLOAD
#define ENABLE_WEB_UPLOAD 0
#endif

#endif /* HTTP_CONFIG_H */
