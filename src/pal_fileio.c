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
 * @file pal_fileio.c
 * @brief Platform Abstraction Layer - File I/O Implementation
 * 
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-13
 * 
 */

#include "pal_fileio.h"
#include "pal_network.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>

/* Fallback buffer size for non-sendfile platforms */
#define FALLBACK_BUFFER_SIZE FTP_BUFFER_SIZE

#ifndef PAL_FILE_COPY_BUFFER_SIZE
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#define PAL_FILE_COPY_BUFFER_SIZE (1024U * 1024U)
#else
#define PAL_FILE_COPY_BUFFER_SIZE FTP_BUFFER_SIZE
#endif
#endif

/*===========================================================================*
 * ZERO-COPY FILE TRANSFER
 *===========================================================================*/

/**
 * @brief Send file data via socket (zero-copy)
 * 
 * DESIGN RATIONALE:
 * - sendfile() eliminates userspace copy (kernel-direct transfer)
 * - Fallback to buffered I/O maintains portability
 * - Performance difference: 2-3x throughput improvement
 */
ssize_t pal_sendfile(int sock_fd, int file_fd, off_t *offset, size_t count)
{
    /* Validate parameters */
    if ((sock_fd < 0) || (file_fd < 0)) {
        errno = EINVAL;
        return -1;
    }
    
    if ((offset == NULL) || (*offset < 0)) {
        errno = EINVAL;
        return -1;
    }
    
    if (count == 0U) {
        return 0;
    }
    
#if defined(__linux__)
    /*
     * Linux sendfile(2)
     * Signature: sendfile(out_fd, in_fd, offset, count)
     */
    ssize_t result = sendfile(sock_fd, file_fd, offset, count);
    return result;
    
#elif defined(__FreeBSD__) || defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    /*
     * FreeBSD sendfile(2)
     * Signature: sendfile(in_fd, out_fd, offset, count, hdtr, sbytes, flags)
     * 
     * IMPORTANT: Different parameter order than Linux!
     */
    off_t sbytes = 0;
    off_t start_offset = *offset;
    
    int ret = sendfile(file_fd, sock_fd, start_offset, count,
                       NULL, &sbytes, 0);
    
    /*
     * Update offset by bytes actually sent
     * FreeBSD updates sbytes even on EAGAIN (partial send)
     */
    if (sbytes > 0) {
        *offset += sbytes;
    }
    
    if (ret == 0) {
        /* Success: all bytes sent */
        return sbytes;
    } else if ((ret == -1) && (errno == EAGAIN)) {
        /* Non-blocking socket: partial send */
        return sbytes;
    } else {
        /* Error */
        return -1;
    }
    
#else
    /*
     * Fallback: Buffered read/write
     * Used on platforms without sendfile() support
     */
    static _Thread_local char buffer[FALLBACK_BUFFER_SIZE];
    
    /* Read from file at specified offset */
    ssize_t nread = pread(file_fd, buffer,
                          (count < FALLBACK_BUFFER_SIZE) ? count : FALLBACK_BUFFER_SIZE,
                          *offset);
    if (nread <= 0) {
        return nread; /* EOF or error */
    }
    
    /* Send via socket */
    ssize_t nsent = pal_send_all(sock_fd, buffer, (size_t)nread, 0);
    if (nsent < 0) {
        return -1;
    }
    *offset += nsent;
    return nsent;
#endif
}

/*===========================================================================*
 * FILE OPERATIONS
 *===========================================================================*/

static ftp_error_t pal_file_copy_atomic(const char *src_path, const char *dst_path)
{
    if ((src_path == NULL) || (dst_path == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }

    struct stat st;
    if (stat(src_path, &st) < 0) {
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            default:
                return FTP_ERR_FILE_STAT;
        }
    }

    if ((st.st_mode & S_IFMT) != S_IFREG) {
        return FTP_ERR_INVALID_PARAM;
    }

    int src_fd = -1;
    int dst_fd = -1;
    ftp_error_t out_err = FTP_ERR_FILE_WRITE;

    static atomic_uint_fast32_t g_tmp_counter = ATOMIC_VAR_INIT(0U);
    uint_fast32_t counter = atomic_fetch_add(&g_tmp_counter, 1U);

    char tmp_path[FTP_PATH_MAX];
    int n = snprintf(tmp_path, sizeof(tmp_path), "%s.zftpd-tmp-%lu-%lu",
                     dst_path, (unsigned long)getpid(), (unsigned long)counter);
    if ((n < 0) || ((size_t)n >= sizeof(tmp_path))) {
        return FTP_ERR_PATH_TOO_LONG;
    }

    src_fd = open(src_path, O_RDONLY);
    if (src_fd < 0) {
        switch (errno) {
            case ENOENT:
                out_err = FTP_ERR_NOT_FOUND;
                break;
            case EACCES:
            case EPERM:
                out_err = FTP_ERR_PERMISSION;
                break;
            default:
                out_err = FTP_ERR_FILE_OPEN;
                break;
        }
        goto cleanup;
    }

    mode_t mode = (mode_t)(st.st_mode & 0777);
    dst_fd = open(tmp_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (dst_fd < 0) {
        switch (errno) {
            case EACCES:
            case EPERM:
                out_err = FTP_ERR_PERMISSION;
                break;
            default:
                out_err = FTP_ERR_FILE_OPEN;
                break;
        }
        goto cleanup;
    }

    static _Thread_local uint8_t copy_buf[PAL_FILE_COPY_BUFFER_SIZE];

    for (;;) {
        ssize_t r = read(src_fd, copy_buf, (size_t)PAL_FILE_COPY_BUFFER_SIZE);
        if (r > 0) {
            ssize_t w = pal_file_write_all(dst_fd, copy_buf, (size_t)r);
            if (w != r) {
                out_err = FTP_ERR_FILE_WRITE;
                goto cleanup;
            }
            continue;
        }
        if (r == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        out_err = FTP_ERR_FILE_READ;
        goto cleanup;
    }

    if (rename(tmp_path, dst_path) < 0) {
        out_err = FTP_ERR_FILE_WRITE;
        goto cleanup;
    }

    out_err = FTP_OK;

cleanup:
    if (dst_fd >= 0) {
        (void)close(dst_fd);
    }
    if (src_fd >= 0) {
        (void)close(src_fd);
    }
    if (out_err != FTP_OK) {
        (void)unlink(tmp_path);
    }
    return out_err;
}

/**
 * @brief Safe file open
 */
int pal_file_open(const char *path, int flags, mode_t mode)
{
    if (path == NULL) {
        errno = EINVAL;
        return FTP_ERR_INVALID_PARAM;
    }
    
    /* Check path length */
    size_t len = strlen(path);
    if (len >= FTP_PATH_MAX) {
        errno = ENAMETOOLONG;
        return FTP_ERR_PATH_TOO_LONG;
    }
    
    /* Open file */
    int fd = open(path, flags, mode);
    if (fd < 0) {
        /* Map errno to FTP error code */
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            case EMFILE:
            case ENFILE:
                return FTP_ERR_OUT_OF_MEMORY;
            default:
                return FTP_ERR_FILE_OPEN;
        }
    }
    
    return fd;
}

/**
 * @brief Safe file close
 */
ftp_error_t pal_file_close(int fd)
{
    if (fd < 0) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (close(fd) < 0) {
        return FTP_ERR_FILE_WRITE; /* Generic error */
    }
    
    return FTP_OK;
}

/**
 * @brief Get file status
 */
ftp_error_t pal_file_stat(const char *path, struct stat *st)
{
    if ((path == NULL) || (st == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (stat(path, st) < 0) {
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
                return FTP_ERR_PERMISSION;
            default:
                return FTP_ERR_FILE_STAT;
        }
    }
    
    return FTP_OK;
}

/**
 * @brief Get file status from descriptor
 */
ftp_error_t pal_file_fstat(int fd, struct stat *st)
{
    if ((fd < 0) || (st == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (fstat(fd, st) < 0) {
        return FTP_ERR_FILE_STAT;
    }
    
    return FTP_OK;
}

/**
 * @brief Read file data
 */
ssize_t pal_file_read(int fd, void *buffer, size_t count)
{
    if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
        errno = EINVAL;
        return -1;
    }
    
    return read(fd, buffer, count);
}

/**
 * @brief Write file data
 */
ssize_t pal_file_write(int fd, const void *buffer, size_t count)
{
    if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
        errno = EINVAL;
        return -1;
    }
    
    return write(fd, buffer, count);
}

ssize_t pal_file_write_all(int fd, const void *buffer, size_t count)
{
    if ((fd < 0) || (buffer == NULL) || (count == 0U)) {
        errno = EINVAL;
        return -1;
    }

    const uint8_t *p = (const uint8_t *)buffer;
    size_t total = 0U;

    while (total < count) {
        ssize_t n = write(fd, p + total, count - total);
        if (n > 0) {
            total += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        if (errno == EINTR) {
            continue;
        }
        return -1;
    }

    return (ssize_t)total;
}

/**
 * @brief Seek to file position
 */
off_t pal_file_seek(int fd, off_t offset, int whence)
{
    if (fd < 0) {
        errno = EINVAL;
        return -1;
    }
    
    return lseek(fd, offset, whence);
}

/**
 * @brief Truncate file
 */
ftp_error_t pal_file_truncate(int fd, off_t len)
{
    if ((fd < 0) || (len < 0)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
#ifdef PLATFORM_PS4
    /* PS4: Use syscall directly */
    if (syscall(480, fd, len) < 0) {
        return FTP_ERR_FILE_WRITE;
    }
#else
    /* POSIX: Standard ftruncate */
    if (ftruncate(fd, len) < 0) {
        return FTP_ERR_FILE_WRITE;
    }
#endif
    
    return FTP_OK;
}

/**
 * @brief Delete file
 */
ftp_error_t pal_file_delete(const char *path)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (unlink(path) < 0) {
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            case EISDIR:
                return FTP_ERR_INVALID_PARAM; /* Use rmdir for directories */
            default:
                return FTP_ERR_FILE_WRITE;
        }
    }
    
    return FTP_OK;
}

/**
 * @brief Rename/move file
 */
ftp_error_t pal_file_rename(const char *old_path, const char *new_path)
{
    if ((old_path == NULL) || (new_path == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (rename(old_path, new_path) < 0) {
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            case EXDEV:
                {
                    ftp_error_t copy_err = pal_file_copy_atomic(old_path, new_path);
                    if (copy_err != FTP_OK) {
                        return copy_err;
                    }
                    if (unlink(old_path) < 0) {
                        return FTP_ERR_FILE_WRITE;
                    }
                    return FTP_OK;
                }
            default:
                return FTP_ERR_FILE_WRITE;
        }
    }
    
    return FTP_OK;
}

/*===========================================================================*
 * DIRECTORY OPERATIONS
 *===========================================================================*/

/**
 * @brief Create directory
 */
ftp_error_t pal_dir_create(const char *path, mode_t mode)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (mkdir(path, mode) < 0) {
        switch (errno) {
            case EEXIST:
                return FTP_ERR_INVALID_PARAM; /* Already exists */
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            case ENOENT:
                return FTP_ERR_NOT_FOUND; /* Parent doesn't exist */
            default:
                return FTP_ERR_FILE_WRITE;
        }
    }
    
    return FTP_OK;
}

/**
 * @brief Remove directory
 */
ftp_error_t pal_dir_remove(const char *path)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    if (rmdir(path) < 0) {
        switch (errno) {
            case ENOENT:
                return FTP_ERR_NOT_FOUND;
            case EACCES:
            case EPERM:
                return FTP_ERR_PERMISSION;
            case ENOTEMPTY:
                return FTP_ERR_INVALID_PARAM; /* Directory not empty */
            case ENOTDIR:
                return FTP_ERR_INVALID_PARAM; /* Not a directory */
            default:
                return FTP_ERR_FILE_WRITE;
        }
    }
    
    return FTP_OK;
}

/**
 * @brief Check if path exists
 */
int pal_path_exists(const char *path)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    struct stat st;
    if (stat(path, &st) == 0) {
        return 1; /* Exists */
    }
    
    if (errno == ENOENT) {
        return 0; /* Does not exist */
    }
    
    return FTP_ERR_FILE_STAT; /* Other error */
}

/**
 * @brief Check if path is a directory
 */
int pal_path_is_directory(const char *path)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    struct stat st;
    if (stat(path, &st) < 0) {
        return FTP_ERR_FILE_STAT;
    }
    
    return S_ISDIR(st.st_mode) ? 1 : 0;
}

/**
 * @brief Check if path is a regular file
 */
int pal_path_is_file(const char *path)
{
    if (path == NULL) {
        return FTP_ERR_INVALID_PARAM;
    }
    
    struct stat st;
    if (stat(path, &st) < 0) {
        return FTP_ERR_FILE_STAT;
    }
    
    return S_ISREG(st.st_mode) ? 1 : 0;
}
