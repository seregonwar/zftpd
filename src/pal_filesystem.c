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
 * @file pal_filesystem.h
 * @brief Unified filesystem abstraction (PS4/PS5)
 * 
 * @author Seregon
 * @version 1.0.0
 * 
 * PLATFORMS: FreeBSD (PS4/PS5 kqueue), Linux (epoll)
 * DESIGN: Single-threaded, non-blocking I/O
 * 
 */
#include "pal_filesystem.h"

#include "pal_fileio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
#include <sys/mount.h> /* fstatfs, struct statfs */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
/* PS4/PS5 libkernel exports _fstatfs, not fstatfs */
extern int _fstatfs(int, struct statfs *);
#define pal_fstatfs _fstatfs
#else
#define pal_fstatfs fstatfs
#endif
#endif

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
int psx_vfs_try_open_self(vfs_node_t *node, const char *path);
ftp_error_t psx_vfs_stat(const char *path, vfs_stat_t *out);
ssize_t psx_vfs_read(vfs_node_t *node, void *buffer, size_t length);
#endif

ftp_error_t vfs_stat(const char *path, vfs_stat_t *out)
{
    if ((path == NULL) || (out == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    ftp_error_t psx_err = psx_vfs_stat(path, out);
    if (psx_err != FTP_ERR_UNKNOWN) {
        return psx_err;
    }
#endif

    struct stat st;
    ftp_error_t err = pal_file_stat(path, &st);
    if (err != FTP_OK) {
        return err;
    }

    out->mode = (uint32_t)st.st_mode;
    out->size = (uint64_t)st.st_size;
    out->mtime = (int64_t)st.st_mtime;
    return FTP_OK;
}

ftp_error_t vfs_open(vfs_node_t *node, const char *path)
{
    if ((node == NULL) || (path == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }

    memset(node, 0, sizeof(*node));
    node->fd = -1;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    int self_res = psx_vfs_try_open_self(node, path);
    if (self_res < 0) {
        return FTP_ERR_FILE_OPEN;
    }
    if (self_res > 0) {
        return FTP_OK;
    }
#endif

    int fd = pal_file_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return FTP_ERR_FILE_OPEN;
    }

    struct stat st;
    if (pal_file_fstat(fd, &st) != FTP_OK) {
        pal_file_close(fd);
        return FTP_ERR_FILE_STAT;
    }

    node->fd = fd;
    node->private_ctx = NULL;
    node->size = (uint64_t)st.st_size;
    node->offset = 0U;

    /*
     * SENDFILE SAFETY CHECK (PS4/PS5)
     *
     * DESIGN RATIONALE:
     *   FreeBSD's sendfile(2) uses the kernel VM pager to DMA file pages
     *   directly into the socket buffer (zero userspace copy). This requires
     *   the source vnode's pager to implement the vm_pager_ops interface.
     *
     *   On PS5's modified FreeBSD kernel, the exFAT (exfatfs) and FAT32
     *   (msdosfs) drivers used for USB storage do NOT implement this
     *   interface correctly. Calling sendfile() on a vnode backed by these
     *   filesystems dereferences a null/invalid pager function pointer,
     *   causing an immediate kernel panic (KP).
     *
     *   nullfs mirrors the underlying vnode — if the origin is exFAT, the
     *   nullfs vnode inherits the same broken pager ops.
     *
     *   Detection: fstatfs() on the open fd returns the filesystem type
     *   name without any additional syscall cost. For USB-backed filesystems
     *   we clear VFS_CAP_SENDFILE, forcing the buffered read/write path.
     *
     * @see pal_sendfile() in pal_fileio.c — callers must check this cap
     * @see https://github.com/seregonwar/zftpd — KP report from USB download
     */
#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    {
        struct statfs sfs;
        int sendfile_safe = 1; /* assume safe until proven otherwise */

        if (pal_fstatfs(fd, &sfs) == 0) {
            /*
             * Filesystems known to cause KP with sendfile() on PS4/PS5:
             *   - exfatfs : USB drives formatted as exFAT (most common)
             *   - msdosfs : USB drives formatted as FAT32
             *   - nullfs  : bind mount — inherits origin pager; unsafe if
             *               origin is exFAT/msdosfs (/mnt/usb* game mounts)
             *   - pfsmnt  : PlayStation FS mount (/user/av_contents, etc.)
             *               sendfile() on pfsmnt vnodes sends corrupt data
             *   - pfs     : raw PFS — same broken pager ops as pfsmnt
             *
             * Add new entries here if additional filesystems are identified.
             */
            if ((strcmp(sfs.f_fstypename, "exfatfs") == 0) ||
                (strcmp(sfs.f_fstypename, "msdosfs") == 0) ||
                (strcmp(sfs.f_fstypename,  "nullfs") == 0) ||
                (strcmp(sfs.f_fstypename, "pfsmnt")  == 0) ||
                (strcmp(sfs.f_fstypename,    "pfs")  == 0)) {
                sendfile_safe = 0;
            }
        }
        /* fstatfs failure: assume unsafe — tolerate the performance hit */
        else {
            sendfile_safe = 0;
        }

        node->caps = sendfile_safe ? VFS_CAP_SENDFILE : 0U;
    }
#else
    node->caps = VFS_CAP_SENDFILE;
#endif

    return FTP_OK;
}

void vfs_close(vfs_node_t *node)
{
    if (node == NULL) {
        return;
    }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    if ((node->caps & VFS_CAP_STREAM_ONLY) != 0U) {
        if (node->psx.self_fd >= 0) {
            pal_file_close(node->psx.self_fd);
        }
        node->psx.self_fd = -1;
        node->private_ctx = NULL;
        return;
    }
#endif

    if (node->fd >= 0) {
        pal_file_close(node->fd);
        node->fd = -1;
    }
}

void vfs_set_offset(vfs_node_t *node, uint64_t offset)
{
    if (node == NULL) {
        return;
    }

    node->offset = offset;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    if ((node->caps & VFS_CAP_STREAM_ONLY) != 0U) {
        return;
    }
#endif

    if (node->fd >= 0) {
        (void)pal_file_seek(node->fd, (off_t)offset, SEEK_SET);
    }
}

ssize_t vfs_read(vfs_node_t *node, void *buffer, size_t length)
{
    if ((node == NULL) || (buffer == NULL) || (length == 0U)) {
        errno = EINVAL;
        return -1;
    }

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    if ((node->caps & VFS_CAP_STREAM_ONLY) != 0U) {
        return psx_vfs_read(node, buffer, length);
    }
#endif

    if (node->fd < 0) {
        errno = EBADF;
        return -1;
    }

    ssize_t n = pal_file_read(node->fd, buffer, length);
    if (n > 0) {
        node->offset += (uint64_t)n;
    }
    return n;
}
