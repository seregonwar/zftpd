/* ══ BUILTIN UNZIP ═══════════════════════════════════════════════════════════
 * Self-contained ZIP extractor — no external libraries required.
 * Supports Store (0) and Deflate (8) compression methods.
 * Works on all platforms including PS4/PS5 without libarchive.
 *
 * API:
 *   builtin_unzip(const char *zip_path, const char *dest_dir,
 *                 volatile int *cancelled,
 *                 char *error_msg, size_t error_msg_size)
 *     → 0 on success, -1 on error
 *
 * ZIP format reference: PKWARE APPNOTE.TXT v6.3.4
 * Deflate reference:    RFC 1951
 * ═════════════════════════════════════════════════════════════════════════ */

#ifndef BUILTIN_UNZIP_H
#define BUILTIN_UNZIP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Extract a ZIP archive to a destination directory.
 *
 * @param zip_path         Path to the ZIP file.
 * @param dest_dir         Destination directory (must exist).
 * @param cancelled        Pointer to a volatile flag — set to 1 to abort.
 * @param error_msg        Buffer for error message on failure.
 * @param error_msg_size   Size of error_msg buffer.
 * @return 0 on success, -1 on failure (error_msg is populated).
 */
int builtin_unzip(const char *zip_path, const char *dest_dir,
                  volatile int *cancelled,
                  char *error_msg, size_t error_msg_size);

#ifdef __cplusplus
}
#endif

#endif /* BUILTIN_UNZIP_H */
