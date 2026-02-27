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

#include "pal_ffi.h"

#include "../../include/event_loop.h"
#include "../../include/ftp_server.h"
#include "../../include/http_server.h"
#include "../../include/pal_alloc.h"

#include <stdlib.h>
#include <string.h>

/*===========================================================================*
 * MEMORY ALLOCATOR
 *===========================================================================*/

int pal_ffi_alloc_init_default(void) { return pal_alloc_init_default(); }

void *pal_ffi_malloc(size_t size) { return pal_malloc(size); }

void pal_ffi_free(void *ptr) { pal_free(ptr); }

void *pal_ffi_calloc(size_t nmemb, size_t size) {
  return pal_calloc(nmemb, size);
}

void *pal_ffi_realloc(void *ptr, size_t size) { return pal_realloc(ptr, size); }

void *pal_ffi_aligned_alloc(size_t alignment, size_t size) {
  return pal_aligned_alloc(alignment, size);
}

size_t pal_ffi_alloc_arena_free_approx(void) {
  return pal_alloc_arena_free_approx();
}

/*===========================================================================*
 * EVENT LOOP
 *===========================================================================*/

void *pal_ffi_event_loop_create(void) { return (void *)event_loop_create(); }

int pal_ffi_event_loop_run(void *loop) {
  if (loop == NULL)
    return PAL_FFI_ERR_INVALID_PARAM;
  return event_loop_run((event_loop_t *)loop);
}

void pal_ffi_event_loop_stop(void *loop) {
  if (loop != NULL) {
    event_loop_stop((event_loop_t *)loop);
  }
}

void pal_ffi_event_loop_destroy(void *loop) {
  if (loop != NULL) {
    event_loop_destroy((event_loop_t *)loop);
  }
}

/*===========================================================================*
 * FTP SERVER
 *===========================================================================*/

void *pal_ffi_ftp_server_create(const char *bind_ip, uint16_t port,
                                const char *root_path) {
  if (bind_ip == NULL || root_path == NULL)
    return NULL;

  ftp_server_context_t *ctx =
      (ftp_server_context_t *)pal_malloc(sizeof(ftp_server_context_t));
  if (ctx == NULL)
    return NULL;

  ftp_error_t err = ftp_server_init(ctx, bind_ip, port, root_path);
  if (err != FTP_OK) {
    pal_free(ctx);
    return NULL;
  }

  return (void *)ctx;
}

int pal_ffi_ftp_server_start(void *server) {
  if (server == NULL)
    return PAL_FFI_ERR_INVALID_PARAM;
  return (int)ftp_server_start((ftp_server_context_t *)server);
}

int pal_ffi_ftp_server_is_running(const void *server) {
  if (server == NULL)
    return 0;
  return ftp_server_is_running((const ftp_server_context_t *)server);
}

uint32_t pal_ffi_ftp_server_get_active_sessions(const void *server) {
  if (server == NULL)
    return 0;
  return ftp_server_get_active_sessions((const ftp_server_context_t *)server);
}

void pal_ffi_ftp_server_stop(void *server) {
  if (server != NULL) {
    ftp_server_stop((ftp_server_context_t *)server);
  }
}

void pal_ffi_ftp_server_destroy(void *server) {
  if (server != NULL) {
    ftp_server_cleanup((ftp_server_context_t *)server);
    pal_free(server);
  }
}

/*===========================================================================*
 * HTTP SERVER
 *===========================================================================*/

#if defined(ENABLE_ZHTTPD) && (ENABLE_ZHTTPD == 1)
void *pal_ffi_http_server_create(void *loop, uint16_t port) {
  if (loop == NULL)
    return NULL;
  return (void *)http_server_create((event_loop_t *)loop, port);
}

void pal_ffi_http_server_destroy(void *server) {
  if (server != NULL) {
    http_server_destroy((http_server_t *)server);
  }
}
#else
void *pal_ffi_http_server_create(void *loop, uint16_t port) {
  (void)loop;
  (void)port;
  return NULL;
}

void pal_ffi_http_server_destroy(void *server) { (void)server; }
#endif
