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
 * @file http_server.h
 * @brief HTTP server public API
 *
 * Usage:
 *   http_server_t *http = http_server_create(loop, 8080);
 *   // ... event loop runs ...
 *   http_server_destroy(http);
 */

#ifndef HTTP_SERVER_H
#define HTTP_SERVER_H

#include "event_loop.h"
#include <stdint.h>

/* Opaque handle */
typedef struct http_server http_server_t;

/**
 * @brief Create and start HTTP server on given port
 * @return Server handle, or NULL on failure
 */
http_server_t *http_server_create(event_loop_t *loop, uint16_t port);

/**
 * @brief Stop and destroy HTTP server
 */
void http_server_destroy(http_server_t *server);

#endif /* HTTP_SERVER_H */
