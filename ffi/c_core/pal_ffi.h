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
 * @file pal_ffi.h
 * @brief C-Core Foreign Function Interface (FFI) for ZFTPD
 *
 * This header defines a stable, C ABI intended for consumption by
 * high-level languages (Java, Rust, Python, Go, Zig).
 * It uses opaque contexts (`void*`) and integer error codes to remain strictly
 * decoupled from the internal data structures of zftpd.
 */

#ifndef PAL_FFI_H
#define PAL_FFI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Visibility macros for shared library export */
#if defined(_WIN32) || defined(__CYGWIN__)
#define PAL_FFI_EXPORT __declspec(dllexport)
#else
#define PAL_FFI_EXPORT __attribute__((visibility("default")))
#endif

/*===========================================================================*
 * ERROR CODES
 * Copied from ftp_types.h to keep FFI consumers independent
 *===========================================================================*/
#define PAL_FFI_OK 0
#define PAL_FFI_ERR_INVALID_PARAM -1
#define PAL_FFI_ERR_OUT_OF_MEMORY -2
#define PAL_FFI_ERR_UNKNOWN -99

/*===========================================================================*
 * MEMORY ALLOCATOR (pal_alloc.h)
 *===========================================================================*/

/**
 * @brief Initialize the global default allocator
 * @return PAL_FFI_OK on success, negative error code on failure
 */
PAL_FFI_EXPORT int pal_ffi_alloc_init_default(void);

/**
 * @brief Allocate memory using the PAL allocator
 * @param size Number of bytes to allocate
 * @return Pointer to allocated memory or NULL if failed
 */
PAL_FFI_EXPORT void *pal_ffi_malloc(size_t size);

/**
 * @brief Free memory using the PAL allocator
 * @param ptr Pointer to memory to free
 */
PAL_FFI_EXPORT void pal_ffi_free(void *ptr);

/**
 * @brief Allocate zero-initialized memory using the PAL allocator
 * @param nmemb Number of elements
 * @param size Size of each element
 * @return Pointer to allocated memory or NULL if failed
 */
PAL_FFI_EXPORT void *pal_ffi_calloc(size_t nmemb, size_t size);

/**
 * @brief Reallocate memory using the PAL allocator
 * @param ptr Pointer to existing memory
 * @param size New size
 * @return Pointer to new memory or NULL if failed
 */
PAL_FFI_EXPORT void *pal_ffi_realloc(void *ptr, size_t size);

/**
 * @brief Allocate aligned memory using the PAL allocator
 * @param alignment Alignment boundary
 * @param size Size of memory to allocate
 * @return Pointer to allocated memory or NULL if failed
 */
PAL_FFI_EXPORT void *pal_ffi_aligned_alloc(size_t alignment, size_t size);

/**
 * @brief Get total free bytes in the arena (approximate)
 * @return Free bytes
 */
PAL_FFI_EXPORT size_t pal_ffi_alloc_arena_free_approx(void);

/*===========================================================================*
 * EVENT LOOP (event_loop.h)
 *===========================================================================*/

/**
 * @brief Create an Event Loop context
 * @return Opaque pointer to the event loop, or NULL on failure
 */
PAL_FFI_EXPORT void *pal_ffi_event_loop_create(void);

/**
 * @brief Run the Event Loop (blocking)
 * @param loop Opaque pointer to the event loop
 * @return 0 on clean exit, negative on error
 */
PAL_FFI_EXPORT int pal_ffi_event_loop_run(void *loop);

/**
 * @brief Stop the Event Loop
 * @param loop Opaque pointer to the event loop
 */
PAL_FFI_EXPORT void pal_ffi_event_loop_stop(void *loop);

/**
 * @brief Destroy the Event Loop and free resources
 * @param loop Opaque pointer to the event loop
 */
PAL_FFI_EXPORT void pal_ffi_event_loop_destroy(void *loop);

/*===========================================================================*
 * FTP SERVER (ftp_server.h)
 *===========================================================================*/

/**
 * @brief Create and initialize a new FTP server context
 * @param bind_ip IP address to bind to ("0.0.0.0" for all)
 * @param port Port number
 * @param root_path Server root directory path
 * @return Opaque pointer to the FTP server context, or NULL on failure
 */
PAL_FFI_EXPORT void *pal_ffi_ftp_server_create(const char *bind_ip,
                                               uint16_t port,
                                               const char *root_path);

/**
 * @brief Start the FTP server (begin listening)
 * @param server Opaque pointer to the FTP server context
 * @return PAL_FFI_OK on success, negative error code on failure
 */
PAL_FFI_EXPORT int pal_ffi_ftp_server_start(void *server);

/**
 * @brief Check if the FTP server is running
 * @param server Opaque pointer to the FTP server context
 * @return 1 if running, 0 if not
 */
PAL_FFI_EXPORT int pal_ffi_ftp_server_is_running(const void *server);

/**
 * @brief Get the number of active FTP sessions
 * @param server Opaque pointer to the FTP server context
 * @return Number of active sessions
 */
PAL_FFI_EXPORT uint32_t
pal_ffi_ftp_server_get_active_sessions(const void *server);

/**
 * @brief Stop the FTP server (graceful shutdown)
 * @param server Opaque pointer to the FTP server context
 */
PAL_FFI_EXPORT void pal_ffi_ftp_server_stop(void *server);

/**
 * @brief Destroy the FTP server context and clean up resources
 * @param server Opaque pointer to the FTP server context
 */
PAL_FFI_EXPORT void pal_ffi_ftp_server_destroy(void *server);

/*===========================================================================*
 * HTTP SERVER (http_server.h)
 *===========================================================================*/

/**
 * @brief Create and start an HTTP server on the given port
 * @param loop Opaque pointer to the event loop
 * @param port Port number
 * @return Opaque pointer to the HTTP server context, or NULL on failure
 */
PAL_FFI_EXPORT void *pal_ffi_http_server_create(void *loop, uint16_t port);

/**
 * @brief Stop and destroy the HTTP server
 * @param server Opaque pointer to the HTTP server context
 */
PAL_FFI_EXPORT void pal_ffi_http_server_destroy(void *server);

#ifdef __cplusplus
}
#endif

#endif /* PAL_FFI_H */
