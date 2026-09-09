#ifndef ZFTPD_PLATFORM_FILEIO_INTERNAL_H
#define ZFTPD_PLATFORM_FILEIO_INTERNAL_H

#include "pal_fileio.h"
#include <stdint.h>

#define PAL_MOVE_MAX_DEPTH 64U
#define PAL_LOG_PATH_CHARS 120
#define PAL_LOG_PATH_PAIR_CHARS 72

ftp_error_t fileio_error_from_errno(int error, ftp_error_t fallback);
ftp_error_t fileio_parent_path(const char *path, char *out, size_t out_size);
ftp_error_t fileio_join_path(const char *parent, const char *name, char *out,
                             size_t out_size);

ftp_error_t pal_file_copy_atomic_ex(const char *src_path, const char *dst_path,
                                    pal_copy_progress_cb_t cb, void *user_data,
                                    uint64_t *cumulative, int *out_errno);

#endif
