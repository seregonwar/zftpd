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
#include "ftp_log.h"
#include "pal_fileio.h"
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

ftp_error_t psx_vfs_stat(const char *path, vfs_stat_t *out);
int psx_vfs_try_open_self(vfs_node_t *node, const char *path);
ssize_t psx_vfs_read(vfs_node_t *node, void *buffer, size_t length);
void psx_vfs_release_map(vfs_node_t *node);

#if defined(PLATFORM_PS5) && defined(__has_include)
#if __has_include(<ps5/kernel.h>)
#include <ps5/kernel.h>
#define PS5_HAVE_KERNEL 1
#endif
#endif

#if defined(PLATFORM_PS5) && defined(PS5_HAVE_KERNEL)
#include "ps5_fw_offsets.h"

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

    const ps5_fw_offsets_t *offsets = ps5_fw_offsets_lookup(kernel_get_fw_version());
    if ((offsets == NULL) || (offsets->pager_table_off == 0U)) {
        g_ps5_pager_table = 0;
        return;
    }

    g_ps5_pager_table =
        (intptr_t)KERNEL_ADDRESS_DATA_BASE + (intptr_t)offsets->pager_table_off;

    g_ps5_pager_ops_vnode = (intptr_t)kernel_getlong(g_ps5_pager_table + 2 * 8);
    g_ps5_pager_ops_self = (intptr_t)kernel_getlong(g_ps5_pager_table + 7 * 8);
}

void psx_pager_debug(uintptr_t *table, uintptr_t *ops_vnode, uintptr_t *ops_self)
{
#if defined(PLATFORM_PS5) && defined(PS5_HAVE_KERNEL)
    ps5_resolve_pager_addresses();
    if (table != NULL) *table = (uintptr_t)g_ps5_pager_table;
    if (ops_vnode != NULL) *ops_vnode = (uintptr_t)g_ps5_pager_ops_vnode;
    if (ops_self != NULL) *ops_self = (uintptr_t)g_ps5_pager_ops_self;
#else
    if (table != NULL) *table = 0U;
    if (ops_vnode != NULL) *ops_vnode = 0U;
    if (ops_self != NULL) *ops_self = 0U;
#endif
}

static void *ps5_mmap_self(void *addr, size_t len, int prot, int flags, int fd, off_t offset)
{
    ps5_resolve_pager_addresses();

    if ((g_ps5_pager_table == 0) || (g_ps5_pager_ops_vnode == 0) || (g_ps5_pager_ops_self == 0)) {
        errno = ENOSYS;
        return MAP_FAILED;
    }

    if (kernel_setlong(g_ps5_pager_table + 2 * 8, (uint64_t)g_ps5_pager_ops_self) != 0) {
        errno = EIO;
        return MAP_FAILED;
    }

    void *data = mmap(addr, len, prot, flags, fd, offset);
    int map_errno = errno;

    if (kernel_setlong(g_ps5_pager_table + 2 * 8, (uint64_t)g_ps5_pager_ops_vnode) != 0) {
        if (data != MAP_FAILED) {
            (void)munmap(data, len);
        }
        errno = EIO;
        return MAP_FAILED;
    }

    if (data == MAP_FAILED) {
        errno = map_errno;
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

/* Parses the SELF container header. Returns 0 when parsed, 1 when the file is
 * not a SELF at all, -1 on error (errno set). *is_self is only set when the
 * magic matched, so callers can tell a plain file from a SELF they cannot use. */
static int self_parse_headers(int fd, self_head_t *head, uint64_t *elf_off,
                              Elf64_Ehdr *ehdr, int *is_self)
{
    if (is_self != NULL) {
        *is_self = 0;
    }

    if (read_exact(fd, head, sizeof(*head), 0) != 0) {
        return -1;
    }

    if ((head->magic != SELF_PS4_MAGIC) && (head->magic != SELF_PS5_MAGIC)) {
        return 1;
    }

    if (is_self != NULL) {
        *is_self = 1;
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

/* One log line per failed attempt: otherwise the user only sees that the file
 * came out undecrypted, with nothing to report. */
static void self_log_failure(const char *path, const self_head_t *head,
                             int is_self, uint64_t elf_size, int error)
{
    const char *name = path != NULL ? strrchr(path, '/') : NULL;
    name = (name != NULL) ? name + 1 : path;
    char msg[320];
    (void)snprintf(msg, sizeof(msg),
                   "[self] cannot decrypt %.120s: self=%d magic=0x%08x "
                   "entries=%u size=%llu elf_size=%llu errno=%d",
                   (name != NULL) ? name : "?", is_self,
                   (head != NULL) ? (unsigned)head->magic : 0U,
                   (head != NULL) ? (unsigned)head->num_entries : 0U,
                   (head != NULL) ? (unsigned long long)head->file_size : 0ULL,
                   (unsigned long long)elf_size, error);
    ftp_log_line(FTP_LOG_WARN, msg);
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

static void *self_map_segment(int fd, const Elf64_Phdr *phdr, uint16_t ind,
                              uint64_t delta, size_t *mapped_length)
{
    if (phdr->p_filesz == 0U || delta >= phdr->p_filesz || mapped_length == NULL) {
        errno = EINVAL;
        return NULL;
    }

    size_t map_len = (size_t)(phdr->p_filesz - delta);
#if defined(PLATFORM_PS4)
    /* PS4's SELF mmap uses the program-header index in the high 32 bits and
     * the segment offset in the low bits.  Keep each mapping small, as the
     * reference app dumper does, to avoid locking huge game segments. */
    if (map_len > 0x100000U) map_len = 0x100000U;
#endif

    /* The temporary pager-table swap selects the SELF pager. 0x80000 is
     * MAP_32BIT on PS5 and must not be passed as a separate MAP_SELF flag. */
    int flags = MAP_PRIVATE;
#if defined(PLATFORM_PS4)
    /* The PS4 SELF mapping flag used by the original app dumper. */
    flags |= 0x80000;
#endif
#if defined(MAP_ALIGNED) && !defined(PLATFORM_PS4)
    long page_size = sysconf(_SC_PAGESIZE);
    if (phdr->p_align != 0U &&
        (page_size <= 0 || phdr->p_align > (uint64_t)page_size)) {
        /* FreeBSD expects log2(alignment), not the ELF alignment in bytes. */
        uint64_t align = (uint64_t)phdr->p_align;
        if ((align & (align - 1U)) != 0U) {
            errno = EINVAL;
            return NULL;
        }
        unsigned align_log2 = 0U;
        while (align > 1U) {
            align >>= 1U;
            align_log2++;
        }
        if (align_log2 > 30U) {
            errno = EINVAL;
            return NULL;
        }
        flags |= MAP_ALIGNED(align_log2);
    }
#endif

    off_t off = (off_t)(((uint64_t)ind << 32) | delta);

#if defined(PLATFORM_PS5) && defined(PS5_HAVE_KERNEL)
    if (kernel_get_fw_version() >= 0x9000000U) {
        uint64_t aligned_vaddr = (uint64_t)phdr->p_vaddr;
        if (phdr->p_align != 0U) {
            aligned_vaddr &= ~((uint64_t)phdr->p_align - 1U);
        }
        off |= (off_t)(aligned_vaddr & (PS5_SUPERPAGE_SIZE - 1U));
    }

    void *p = ps5_mmap_self(NULL, map_len, PROT_READ, flags, fd, off);
#else
    void *p = mmap(NULL, map_len, PROT_READ, flags, fd, off);
#endif
    if (p == MAP_FAILED) {
        int saved_errno = errno;
        char msg[256];
        (void)snprintf(msg, sizeof(msg),
                       "[self] mmap failed seg=%u len=%llu off=0x%llx align=0x%llx flags=0x%x errno=%d",
                       (unsigned)ind, (unsigned long long)map_len,
                       (unsigned long long)(uint64_t)off,
                       (unsigned long long)phdr->p_align, flags, saved_errno);
        ftp_log_line(FTP_LOG_ERROR, msg);
        errno = saved_errno;
        return NULL;
    }

    /* PS5's temporary pager swap needs the fault within this call.  PS4's
     * installed mmap patch remains active and its mapped window is small. */
#if defined(PLATFORM_PS5)
    if (mlock(p, map_len) != 0) {
        int saved_errno = errno;
        char msg[192];
        (void)snprintf(msg, sizeof(msg),
                       "[self] mlock failed seg=%u len=%llu errno=%d",
                       (unsigned)ind, (unsigned long long)map_len,
                       saved_errno);
        ftp_log_line(FTP_LOG_ERROR, msg);
        (void)munmap(p, map_len);
        errno = saved_errno;
        return NULL;
    }
#endif

    *mapped_length = map_len;
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

    out->mode  = (uint32_t)st.st_mode;
    out->mtime = (int64_t)st.st_mtime;
    out->size  = (uint64_t)st.st_size;

    /*
     * DESIGN RATIONALE — why we do NOT override size with self_compute_elf_size()
     *
     * Previous code opened every file here and, when it detected a SELF header
     * (PS4/PS5 encrypted executable), replaced out->size with the logical ELF
     * size computed from the program-header table.
     *
     * This caused a critical file-transfer bug:
     *
     *   - A SELF container may be 12 GB on disk but its embedded ELF segments
     *     occupy only ~419 MB (the rest is SELF metadata, signatures, and
     *     encrypted padding).
     *
     *   - FTP clients use the size reported by SIZE / MLSD to determine how
     *     many bytes constitute a complete file.  When they see 419 MB they
     *     stop reading after 419 MB, even though the actual file is 12 GB.
     *
     *   - vfs_open() then opened the same file via psx_vfs_try_open_self
     *     (MAP_SELF path), sending 419 MB of DECRYPTED ELF bytes instead of
     *     the raw on-disk content.  The client receives a truncated, decrypted
     *     file — useless for backup or copying.
     *
     * zftpd is a file-transfer daemon: it must report and transfer the ACTUAL
     * bytes present on disk (st_size), not the logical ELF payload.  Anyone
     * who needs to inspect the ELF structure should use platform tools.
     *
     * The self_parse_headers / self_compute_elf_size machinery is kept for
     * psx_vfs_try_open_self (the MAP_SELF execution path) but must not
     * influence size reporting for transfer purposes.
     */
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
    int is_self = 0;

    int parsed = self_parse_headers(fd, &head, &elf_off, &ehdr, &is_self);
    if (parsed == 1) {
        pal_file_close(fd);
        return 0;
    }
    if (parsed != 0) {
        int saved_errno = errno;
        if (is_self == 0) {
            /* Unreadable magic: nothing says this is a SELF, so send it raw. */
            pal_file_close(fd);
            return 0;
        }
        self_log_failure(path, &head, is_self, 0U, saved_errno);
        pal_file_close(fd);
        errno = saved_errno;
        return -1;
    }

    uint64_t elf_size = self_compute_elf_size(fd, elf_off, &ehdr);
    if (elf_size == 0U) {
        /* SELF with no loadable segments: the decrypted image would be empty. */
        self_log_failure(path, &head, 1, 0U, ENOTSUP);
        pal_file_close(fd);
        errno = ENOTSUP;
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
    node->psx.mapped_segment = NULL;
    node->psx.mapped_offset = 0U;
    node->psx.mapped_length = 0U;

    return 1;
}

void psx_vfs_release_map(vfs_node_t *node)
{
    if (node == NULL || node->psx.mapped_segment == NULL) return;
    (void)munmap(node->psx.mapped_segment, node->psx.mapped_length);
    node->psx.mapped_segment = NULL;
    node->psx.mapped_offset = 0U;
    node->psx.mapped_length = 0U;
}

static int find_covering_phdr(int fd, const vfs_node_t *node, uint64_t offset,
                              Elf64_Phdr *out, uint16_t *out_index,
                              uint64_t *next_offset)
{
    uint64_t elf_off = node->psx.elf_off;
    uint64_t phoff = node->psx.phoff;
    uint16_t phnum = node->psx.phnum;

    uint64_t best_next = UINT64_MAX;

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
        }
    }

    if (next_offset != NULL) {
        *next_offset = best_next;
    }
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

        /* mmap/mlock of an entire SELF segment is expensive. Reuse the
         * populated mapping across successive reads of the same segment. */
        if (node->psx.mapped_segment != NULL) {
            uint64_t start = node->psx.mapped_offset;
            uint64_t end = start + (uint64_t)node->psx.mapped_length;
            if (cur_off >= start && cur_off < end) {
                size_t chunk = to_read - done;
                if ((uint64_t)chunk > end - cur_off) chunk = (size_t)(end - cur_off);
                memcpy(dst + done, (const uint8_t *)node->psx.mapped_segment +
                       (size_t)(cur_off - start), chunk);
                done += chunk;
                continue;
            }
            psx_vfs_release_map(node);
        }

        Elf64_Phdr phdr;
        uint16_t seg_index = 0U;
        uint64_t next_segment = UINT64_MAX;
        int phdr_res = find_covering_phdr(node->psx.self_fd, node, cur_off,
                                          &phdr, &seg_index, &next_segment);

        if (phdr_res < 0) {
            return -1;
        }

        /* The ELF header and program headers live in the unencrypted SELF
         * prefix.  They must be present even when no PT_LOAD covers offset 0. */
        uint64_t header_end = sizeof(Elf64_Ehdr);
        uint64_t phdr_end = node->psx.phoff +
                            (uint64_t)node->psx.phnum * sizeof(Elf64_Phdr);
        if (phdr_end > header_end) {
            header_end = phdr_end;
        }
        if (cur_off < header_end) {
            size_t chunk = to_read - done;
            if ((uint64_t)chunk > header_end - cur_off) {
                chunk = (size_t)(header_end - cur_off);
            }
            if (read_exact(node->psx.self_fd, dst + done, chunk,
                           (off_t)(node->psx.elf_off + cur_off)) != 0) {
                return -1;
            }
            done += chunk;
            continue;
        }

        if (phdr_res == 0) {
            size_t zero_len = to_read - done;
            if (next_segment != UINT64_MAX &&
                (uint64_t)zero_len > next_segment - cur_off) {
                zero_len = (size_t)(next_segment - cur_off);
            }
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
            if (read_exact(node->psx.self_fd, dst + done, chunk, src_off) == 0) {
                done += chunk;
                continue;
            }
            /* Version metadata lives outside the data area on some builds:
             * zero it rather than aborting the whole transfer. */
            memset(dst + done, 0, chunk);
            done += chunk;
            continue;
        }

        /*
         * Loadable segment types: only those carry data.  Anything else is
         * padding in the ELF image.
         */
        if ((phdr.p_type != 0x1U) &&          /* PT_LOAD          */
            (phdr.p_type != 0x61000000U) &&   /* PT_SCE_DYNLIBDATA */
            (phdr.p_type != 0x61000010U) &&   /* PT_SCE_RELRO      */
            (phdr.p_type != 0x6fffff00U)) {   /* PT_SCE_COMMENT    */
            memset(dst + done, 0, chunk);
            done += chunk;
            continue;
        }

        self_entry_t ent;
        int have_entry = (self_find_entry(node->psx.self_fd, node->psx.num_entries,
                                          seg_index, entry_table_off, &ent) == 0);

        /*
         * Always go through the self pager: the kernel decrypts whatever needs
         * decrypting, so a mis-read segment table cannot route the read into
         * ciphertext.  The entry is only used as a fallback when the mapping
         * itself fails.
         */
        pthread_mutex_lock(&g_self_map_lock);
        size_t mapped_length = 0U;
        void *map = self_map_segment(node->psx.self_fd, &phdr, seg_index,
                                     delta, &mapped_length);
        pthread_mutex_unlock(&g_self_map_lock);

        if (map != NULL) {
            node->psx.mapped_segment = map;
            node->psx.mapped_offset = cur_off;
            node->psx.mapped_length = mapped_length;
            if (chunk > mapped_length) chunk = mapped_length;
            memcpy(dst + done, map, chunk);
            done += chunk;
            continue;
        }

        if (have_entry && (ent.props.is_encrypted == 0U) && (ent.props.is_compressed == 0U)) {
            off_t src_off = (off_t)((uint64_t)ent.offset + delta);
            if (read_exact(node->psx.self_fd, dst + done, chunk, src_off) == 0) {
                done += chunk;
                continue;
            }
        }

        /* A missing decrypted segment is a failed dump.  Its advertised size
         * must not be fulfilled with fabricated zero bytes. */
        return -1;
    }

    node->offset += (uint64_t)done;
    return (ssize_t)done;
}

#endif
