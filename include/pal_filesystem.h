#ifndef PAL_FILESYSTEM_H
#define PAL_FILESYSTEM_H

#include "ftp_types.h"
#include <stddef.h>
#include <stdint.h>
#include <time.h>
#include <sys/types.h>

typedef enum {
    VFS_CAP_SENDFILE = 1U << 0,
    VFS_CAP_STREAM_ONLY = 1U << 1
} vfs_capability_t;

typedef struct {
    uint32_t mode;
    uint64_t size;
    int64_t mtime;
} vfs_stat_t;

typedef struct vfs_node {
    vfs_capability_t caps;
    void *private_ctx;
    int fd;
    uint64_t size;
    uint64_t offset;

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)
    struct {
        int self_fd;
        uint64_t elf_off;
        uint16_t num_entries;
        uint16_t phnum;
        uint64_t phoff;
        uint64_t file_size;
        uint32_t magic;
    } psx;
#endif
} vfs_node_t;

ftp_error_t vfs_stat(const char *path, vfs_stat_t *out);
ftp_error_t vfs_open(vfs_node_t *node, const char *path);
void vfs_close(vfs_node_t *node);
void vfs_set_offset(vfs_node_t *node, uint64_t offset);
ssize_t vfs_read(vfs_node_t *node, void *buffer, size_t length);

static inline vfs_capability_t vfs_get_caps(const vfs_node_t *node)
{
    return (node != NULL) ? node->caps : 0;
}

static inline uint64_t vfs_get_size(const vfs_node_t *node)
{
    return (node != NULL) ? node->size : 0U;
}

#endif
