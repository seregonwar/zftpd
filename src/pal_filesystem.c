#include "pal_filesystem.h"

#include "pal_fileio.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

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
    node->caps = VFS_CAP_SENDFILE;
    node->private_ctx = NULL;
    node->size = (uint64_t)st.st_size;
    node->offset = 0U;
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
