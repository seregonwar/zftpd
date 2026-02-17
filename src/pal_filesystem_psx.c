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

#if defined(PLATFORM_PS4) || defined(PLATFORM_PS5)

#include "ftp_types.h"
#include "pal_fileio.h"
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#ifndef MAP_SELF
#define MAP_SELF 0x80000
#endif

ftp_error_t psx_vfs_stat(const char *path, vfs_stat_t *out);
int psx_vfs_try_open_self(vfs_node_t *node, const char *path);
ssize_t psx_vfs_read(vfs_node_t *node, void *buffer, size_t length);

#if defined(PLATFORM_PS5) && defined(__has_include)
#if __has_include(<ps5/kernel.h>)
#include <ps5/kernel.h>
#define PS5_HAVE_KERNEL 1
#endif
#endif

#if defined(PLATFORM_PS5) && defined(PS5_HAVE_KERNEL)
#define PS5_SUPERPAGE_SIZE 0x200000U

static atomic_int g_ps5_pager_resolved = ATOMIC_VAR_INIT(0);
static intptr_t g_ps5_pager_table = 0;
static intptr_t g_ps5_pager_ops_vnode = 0;
static intptr_t g_ps5_pager_ops_self = 0;

static void ps5_resolve_pager_addresses(void)
{
    int expected = 0;
    if (!atomic_compare_exchange_strong(&g_ps5_pager_resolved, &expected, 1)) {
        return;
    }

    uint32_t fw = kernel_get_fw_version();
    switch (fw >> 16) {
        case 0x100:
        case 0x101:
        case 0x102:
        case 0x105:
        case 0x110:
        case 0x111:
        case 0x112:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xC27C40;
            break;
        case 0x113:
        case 0x114:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xC27CA0;
            break;
        case 0x200:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xC4EF60;
            break;
        case 0x220:
        case 0x225:
        case 0x226:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xC4EFA0;
            break;
        case 0x230:
        case 0x250:
        case 0x270:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xC4F120;
            break;
        case 0x300:
        case 0x310:
        case 0x320:
        case 0x321:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xCAF8C0;
            break;
        case 0x400:
        case 0x402:
        case 0x403:
        case 0x450:
        case 0x451:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xD20840;
            break;
        case 0x500:
        case 0x502:
        case 0x510:
        case 0x550:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xE0FEF0;
            break;
        case 0x600:
        case 0x602:
        case 0x650:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xE30410;
            break;
        case 0x700:
        case 0x701:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xE310C0;
            break;
        case 0x720:
        case 0x740:
        case 0x760:
        case 0x761:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xE31180;
            break;
        case 0x800:
        case 0x820:
        case 0x840:
        case 0x860:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xE31250;
            break;
        case 0x900:
        case 0x905:
        case 0x920:
        case 0x940:
        case 0x960:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xDE0420;
            break;
        case 0x1000:
        case 0x1001:
            g_ps5_pager_table = (intptr_t)KERNEL_ADDRESS_DATA_BASE + 0xDE04F0;
            break;
        default:
            g_ps5_pager_table = 0;
            return;
    }

    g_ps5_pager_ops_vnode = (intptr_t)kernel_getlong(g_ps5_pager_table + 2 * 8);
    g_ps5_pager_ops_self = (intptr_t)kernel_getlong(g_ps5_pager_table + 7 * 8);
}

static void *ps5_mmap_self(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    ps5_resolve_pager_addresses();

    if ((g_ps5_pager_table == 0) || (g_ps5_pager_ops_vnode == 0) || (g_ps5_pager_ops_self == 0)) {
        errno = ENOSYS;
        return MAP_FAILED;
    }

    if (kernel_setlong(g_ps5_pager_table + 2 * 8, (uint64_t)g_ps5_pager_ops_self) != 0) {
        return MAP_FAILED;
    }

    void *data = mmap(addr, len, prot, flags, fd, offset);

    if (kernel_setlong(g_ps5_pager_table + 2 * 8, (uint64_t)g_ps5_pager_ops_vnode) != 0) {
        return MAP_FAILED;
    }

    return data;
}
#endif

typedef struct self_head {
    uint32_t magic;
    uint8_t version;
    uint8_t mode;
    uint8_t endian;
    uint8_t attrs;
    uint32_t key_type;
    uint16_t header_size;
    uint16_t meta_size;
    uint64_t file_size;
    uint16_t num_entries;
    uint16_t flags;
} self_head_t;

typedef struct self_entry {
    struct __attribute__((packed)) {
        uint8_t is_ordered : 1;
        uint8_t is_encrypted : 1;
        uint8_t is_signed : 1;
        uint8_t is_compressed : 1;
        uint8_t unknown0 : 4;
        uint8_t window_bits : 3;
        uint8_t has_blocks : 1;
        uint8_t block_bits : 4;
        uint8_t has_digest : 1;
        uint8_t has_extents : 1;
        uint8_t unknown1 : 2;
        uint16_t segment_index : 16;
        uint32_t unknown2 : 28;
    } props;
    uint64_t offset;
    uint64_t enc_size;
    uint64_t dec_size;
} self_entry_t;

static const uint32_t SELF_PS4_MAGIC = 0x1D3D154FU;
static const uint32_t SELF_PS5_MAGIC = 0xEEF51454U;

static pthread_mutex_t g_self_map_lock = PTHREAD_MUTEX_INITIALIZER;

static int read_exact(int fd, void *buf, size_t size, off_t off)
{
    uint8_t *p = (uint8_t *)buf;
    size_t remaining = size;
    off_t cur = off;

    while (remaining > 0U) {
        ssize_t n = pread(fd, p, remaining, cur);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        if (n == 0) {
            errno = EIO;
            return -1;
        }
        p += (size_t)n;
        remaining -= (size_t)n;
        cur += (off_t)n;
    }

    return 0;
}

static int self_parse_headers(int fd, self_head_t *head, uint64_t *elf_off, Elf64_Ehdr *ehdr)
{
    if (read_exact(fd, head, sizeof(*head), 0) != 0) {
        return -1;
    }

    if ((head->magic != SELF_PS4_MAGIC) && (head->magic != SELF_PS5_MAGIC)) {
        errno = EINVAL;
        return -1;
    }

    uint64_t off = (uint64_t)sizeof(*head) + (uint64_t)head->num_entries * (uint64_t)sizeof(self_entry_t);
    if (elf_off != NULL) {
        *elf_off = off;
    }

    if (read_exact(fd, ehdr, sizeof(*ehdr), (off_t)off) != 0) {
        return -1;
    }

    if ((ehdr->e_ident[EI_MAG0] != ELFMAG0) || (ehdr->e_ident[EI_MAG1] != ELFMAG1) ||
        (ehdr->e_ident[EI_MAG2] != ELFMAG2) || (ehdr->e_ident[EI_MAG3] != ELFMAG3)) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

static int self_find_entry(int fd, uint16_t num_entries, uint16_t segment_index, uint64_t entry_table_off,
                           self_entry_t *out)
{
    for (uint16_t i = 0; i < num_entries; i++) {
        self_entry_t ent;
        off_t off = (off_t)(entry_table_off + (uint64_t)i * (uint64_t)sizeof(self_entry_t));
        if (read_exact(fd, &ent, sizeof(ent), off) != 0) {
            return -1;
        }
        if ((ent.props.segment_index == segment_index) && (ent.props.has_blocks != 0U)) {
            *out = ent;
            return 0;
        }
    }

    errno = ENOENT;
    return -1;
}

static void *self_map_segment(int fd, const Elf64_Phdr *phdr, uint16_t ind)
{
    if (phdr->p_filesz == 0U) {
        errno = EINVAL;
        return NULL;
    }

    int flags = MAP_PRIVATE | MAP_SELF;
#ifdef MAP_ALIGNED
    if (phdr->p_align != 0U) {
        flags |= MAP_ALIGNED((int)phdr->p_align);
    }
#endif

    off_t off = (off_t)((uint64_t)ind << 32);

#if defined(PLATFORM_PS5) && defined(PS5_HAVE_KERNEL)
    if (kernel_get_fw_version() >= 0x9000000U) {
        uint64_t aligned_vaddr = (uint64_t)phdr->p_vaddr;
        if (phdr->p_align != 0U) {
            aligned_vaddr &= ~((uint64_t)phdr->p_align - 1U);
        }
        off |= (off_t)(aligned_vaddr & (PS5_SUPERPAGE_SIZE - 1U));
    }

    void *p = ps5_mmap_self(NULL, (size_t)phdr->p_filesz, PROT_READ, flags, fd, off);
#else
    void *p = mmap(NULL, (size_t)phdr->p_filesz, PROT_READ, flags, fd, off);
#endif
    if (p == MAP_FAILED) {
        return NULL;
    }

    return p;
}

static uint64_t self_compute_elf_size(int fd, uint64_t elf_off, const Elf64_Ehdr *ehdr)
{
    uint64_t max_end = 0U;

    for (uint16_t i = 0; i < (uint16_t)ehdr->e_phnum; i++) {
        Elf64_Phdr phdr;
        off_t off = (off_t)(elf_off + (uint64_t)ehdr->e_phoff + (uint64_t)i * (uint64_t)sizeof(phdr));
        if (read_exact(fd, &phdr, sizeof(phdr), off) != 0) {
            return 0U;
        }

        if (phdr.p_filesz == 0U) {
            continue;
        }

        uint64_t end = (uint64_t)phdr.p_offset + (uint64_t)phdr.p_filesz;
        if (end > max_end) {
            max_end = end;
        }
    }

    return max_end;
}

ftp_error_t psx_vfs_stat(const char *path, vfs_stat_t *out)
{
    if ((path == NULL) || (out == NULL)) {
        return FTP_ERR_INVALID_PARAM;
    }

    struct stat st;
    ftp_error_t err = pal_file_stat(path, &st);
    if (err != FTP_OK) {
        return err;
    }

    out->mode = (uint32_t)st.st_mode;
    out->mtime = (int64_t)st.st_mtime;
    out->size = (uint64_t)st.st_size;

    int fd = pal_file_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return FTP_OK;
    }

    self_head_t head;
    uint64_t elf_off = 0U;
    Elf64_Ehdr ehdr;
    if (self_parse_headers(fd, &head, &elf_off, &ehdr) == 0) {
        uint64_t elf_size = self_compute_elf_size(fd, elf_off, &ehdr);
        if (elf_size > 0U) {
            out->size = elf_size;
        }
    }

    pal_file_close(fd);
    return FTP_OK;
}

int psx_vfs_try_open_self(vfs_node_t *node, const char *path)
{
    if ((node == NULL) || (path == NULL)) {
        errno = EINVAL;
        return -1;
    }

    int fd = pal_file_open(path, O_RDONLY, 0);
    if (fd < 0) {
        return -1;
    }

    self_head_t head;
    uint64_t elf_off = 0U;
    Elf64_Ehdr ehdr;

    if (self_parse_headers(fd, &head, &elf_off, &ehdr) != 0) {
        pal_file_close(fd);
        return 0;
    }

    uint64_t elf_size = self_compute_elf_size(fd, elf_off, &ehdr);
    if (elf_size == 0U) {
        pal_file_close(fd);
        return -1;
    }

    node->caps = VFS_CAP_STREAM_ONLY;
    node->fd = -1;
    node->size = elf_size;
    node->offset = 0U;
    node->private_ctx = &node->psx;
    node->psx.self_fd = fd;
    node->psx.elf_off = elf_off;
    node->psx.num_entries = head.num_entries;
    node->psx.phnum = (uint16_t)ehdr.e_phnum;
    node->psx.phoff = (uint64_t)ehdr.e_phoff;
    node->psx.file_size = head.file_size;
    node->psx.magic = head.magic;

    return 1;
}

static int find_covering_phdr(int fd, const vfs_node_t *node, uint64_t offset, Elf64_Phdr *out, uint16_t *out_index)
{
    uint64_t elf_off = node->psx.elf_off;
    uint64_t phoff = node->psx.phoff;
    uint16_t phnum = node->psx.phnum;

    uint64_t best_next = UINT64_MAX;
    int found = 0;

    for (uint16_t i = 0; i < phnum; i++) {
        Elf64_Phdr phdr;
        off_t off = (off_t)(elf_off + phoff + (uint64_t)i * (uint64_t)sizeof(phdr));
        if (read_exact(fd, &phdr, sizeof(phdr), off) != 0) {
            return -1;
        }

        uint64_t start = (uint64_t)phdr.p_offset;
        uint64_t end = start + (uint64_t)phdr.p_filesz;

        if ((phdr.p_filesz != 0U) && (offset >= start) && (offset < end)) {
            *out = phdr;
            if (out_index != NULL) {
                *out_index = i;
            }
            return 1;
        }

        if ((phdr.p_filesz != 0U) && (start > offset) && (start < best_next)) {
            best_next = start;
            found = 0;
        }
    }

    (void)found;
    return 0;
}

ssize_t psx_vfs_read(vfs_node_t *node, void *buffer, size_t length)
{
    if ((node == NULL) || (buffer == NULL) || (length == 0U)) {
        errno = EINVAL;
        return -1;
    }

    if (node->psx.self_fd < 0) {
        errno = EBADF;
        return -1;
    }

    uint64_t pos = node->offset;
    if (pos >= node->size) {
        return 0;
    }

    size_t to_read = length;
    if ((uint64_t)to_read > (node->size - pos)) {
        to_read = (size_t)(node->size - pos);
    }

    uint8_t *dst = (uint8_t *)buffer;
    size_t done = 0U;

    const uint64_t entry_table_off = (uint64_t)sizeof(self_head_t);

    while (done < to_read) {
        uint64_t cur_off = pos + (uint64_t)done;

        Elf64_Phdr phdr;
        uint16_t seg_index = 0U;
        int phdr_res = find_covering_phdr(node->psx.self_fd, node, cur_off, &phdr, &seg_index);

        if (phdr_res < 0) {
            return -1;
        }

        if (phdr_res == 0) {
            size_t zero_len = to_read - done;
            memset(dst + done, 0, zero_len);
            done += zero_len;
            continue;
        }

        uint64_t seg_start = (uint64_t)phdr.p_offset;
        uint64_t seg_end = seg_start + (uint64_t)phdr.p_filesz;
        size_t seg_avail = (size_t)(seg_end - cur_off);
        size_t chunk = to_read - done;
        if (chunk > seg_avail) {
            chunk = seg_avail;
        }

        uint64_t delta = cur_off - seg_start;

        if (phdr.p_type == 0x6fffff01U) {
            off_t src_off = (off_t)((uint64_t)node->psx.file_size + delta);
            if (read_exact(node->psx.self_fd, dst + done, chunk, src_off) != 0) {
                return -1;
            }
            done += chunk;
            continue;
        }

        self_entry_t ent;
        if (self_find_entry(node->psx.self_fd, node->psx.num_entries, seg_index, entry_table_off, &ent) != 0) {
            memset(dst + done, 0, chunk);
            done += chunk;
            continue;
        }

        int encrypted = (ent.props.is_encrypted != 0U) || (ent.props.is_compressed != 0U);
        if (!encrypted) {
            off_t src_off = (off_t)((uint64_t)ent.offset + delta);
            if (read_exact(node->psx.self_fd, dst + done, chunk, src_off) != 0) {
                return -1;
            }
            done += chunk;
            continue;
        }

        pthread_mutex_lock(&g_self_map_lock);
        void *map = self_map_segment(node->psx.self_fd, &phdr, seg_index);
        if (map == NULL) {
            pthread_mutex_unlock(&g_self_map_lock);
            return -1;
        }

        memcpy(dst + done, (const uint8_t *)map + delta, chunk);
        (void)munmap(map, (size_t)phdr.p_filesz);
        pthread_mutex_unlock(&g_self_map_lock);

        done += chunk;
    }

    node->offset += (uint64_t)done;
    return (ssize_t)done;
}

#endif
