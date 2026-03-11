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
 * @file http_api.h
 * @brief REST API handlers
 */

#ifndef HTTP_API_H
#define HTTP_API_H

#include "ftp_types.h"
#include "http_parser.h"
#include "http_response.h"

http_response_t *http_api_handle(const http_request_t *request);

/**
 * @brief Attach the FTP server context to the HTTP API layer.
 *
 * Required for the POST /api/network/reset endpoint (Fix #4).
 * Must be called after ftp_server_init() and before http_server_create().
 *
 * @param ctx  Initialized FTP server context, or NULL to detach.
 */
void http_api_set_server_ctx(ftp_server_context_t *ctx);

/**
 * @brief Set the root directory for HTTP API path confinement
 *
 * All HTTP file operations (list, download, upload, stats) will be
 * restricted to paths within this root.  Must be called before
 * http_server_create().
 *
 *   FTP path confinement:  ftp_path_resolve() -> ftp_path_is_within_root()
 *   HTTP path confinement: validate_path()    -> http_is_within_root()  [NEW]
 */
void http_api_set_root(const char *root);

/**
 * @brief Get the configured HTTP root directory
 * @return Pointer to static root buffer (empty string if unset)
 */
const char *http_api_get_root(void);

/**
 * @brief Recursively compute the total size of all regular files under a directory.
 *
 * @param path   Absolute path to the directory.
 * @param depth  Initial recursion depth (pass 0).
 * @return       Total bytes of all regular files, or 0 on error / empty dir.
 */
uint64_t http_dir_size_recursive(const char *path, int depth);

#endif /* HTTP_API_H */
