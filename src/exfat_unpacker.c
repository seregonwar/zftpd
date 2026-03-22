/*
 * Feature-test macros must appear before any system include.
 * _FILE_OFFSET_BITS=64: enables fseeko/ftello on 32-bit hosts.
 * _POSIX_C_SOURCE=200809L: exposes fseeko, fdopen, strcasecmp etc. in strict C99 mode.
 */
#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

/*
 * exfat_unpacker — exFAT filesystem image parser
 * Copyright (C) 2025 seregonwar
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 *
 * exfat_unpacker.c — read-only exFAT filesystem image parser
 *
 * Fixes applied over the original version:
 *   1. Boot sector: volume_flags uint32→uint16; bytes_per_sector_shift now
 *      correctly at 0x6C instead of 0x6E.
 *   2. File entry struct trimmed to exactly 32 bytes; phantom fields removed.
 *   3. Filename entry: name_length/name_hash moved to stream extension where
 *      they actually live; out-of-bounds reads eliminated.
 *   4. exfat_read_directory: follows the cluster chain (was reading only the
 *      first cluster, silently truncating large directories).
 *   5. exfat_parse_file_entry: gets NameLength from stream extension;
 *      accumulates chars across multiple filename entries; respects the
 *      secondary_count to avoid walking beyond the entry set.
 *   6. File extraction: uses a single stack buffer (EXFAT_IO_CHUNK_SIZE)
 *      instead of per-cluster malloc; respects the NoFatChain flag.
 *   7. Added exfat_extract_to_buffer() for in-memory extraction.
 *   8. fseeko() used for large-file offsets (> 2 GiB).
 */

#include "exfat_unpacker.h"
#include <errno.h>
#if defined(_MSC_VER)
#  include <io.h>
#  include <fcntl.h>
#  define dup    _dup
#  define fdopen _fdopen
#  define close  _close
#  define write  _write
#  define fseeko _fseeki64
#  define ftello _ftelli64
#  define lseek64 _lseeki64
#else
#  include <unistd.h>  /* write(), close() */
#  define lseek64 lseek
#endif

/* ── Boot sector ────────────────────────────────────────────────────────── */

int exfat_read_boot_sector(exfat_context_t *ctx) {
    if (!ctx || !ctx->image_file)
        return -1;

    if (fseeko(ctx->image_file, 0, SEEK_SET) != 0) {
        perror("[exFAT] fseeko boot sector");
        return -1;
    }
    if (fread(&ctx->boot_sector, sizeof(exfat_boot_sector_t), 1,
              ctx->image_file) != 1) {
        perror("[exFAT] fread boot sector");
        return -1;
    }
    if (exfat_validate_boot_sector(&ctx->boot_sector) != 0)
        return -1;

    /*
     * Derive working constants.
     * Shift values are validated by exfat_validate_boot_sector().
     */
    ctx->bytes_per_sector         = 1U << ctx->boot_sector.bytes_per_sector_shift;
    ctx->sectors_per_cluster      = 1U << ctx->boot_sector.sectors_per_cluster_shift;
    ctx->bytes_per_cluster        = ctx->bytes_per_sector * ctx->sectors_per_cluster;
    ctx->cluster_heap_offset_bytes =
        (uint64_t)ctx->boot_sector.cluster_heap_offset * ctx->bytes_per_sector;

    return 0;
}

int exfat_validate_boot_sector(const exfat_boot_sector_t *boot) {
    if (!boot)
        return -1;

    /* FileSystemName must be "EXFAT   " (with 3 trailing spaces) */
    if (memcmp(boot->fs_name, EXFAT_FS_NAME, EXFAT_FS_NAME_LEN) != 0) {
        fprintf(stderr, "[exFAT] invalid filesystem name\n");
        return -1;
    }
    if (boot->boot_signature != EXFAT_BOOT_SIGNATURE) {
        fprintf(stderr, "[exFAT] invalid boot signature: 0x%04X\n",
                boot->boot_signature);
        return -1;
    }
    /* Per spec §3.1.13: BytesPerSectorShift in [9, 12] */
    if (boot->bytes_per_sector_shift < 9 || boot->bytes_per_sector_shift > 12) {
        fprintf(stderr, "[exFAT] BytesPerSectorShift=%u out of range [9,12]\n",
                boot->bytes_per_sector_shift);
        return -1;
    }
    /* Per spec §3.1.14: BytesPerSectorShift + SectorsPerClusterShift <= 25 */
    if ((unsigned)boot->bytes_per_sector_shift +
        (unsigned)boot->sectors_per_cluster_shift > 25U) {
        fprintf(stderr,
                "[exFAT] shift sum %u+%u > 25 (cluster too large)\n",
                boot->bytes_per_sector_shift,
                boot->sectors_per_cluster_shift);
        return -1;
    }
    return 0;
}

void exfat_print_boot_sector(const exfat_boot_sector_t *boot) {
    if (!boot) return;
    printf("=== exFAT Boot Sector ===\n");
    printf("  FileSystemName      : %.8s\n",        boot->fs_name);
    printf("  FatOffset           : %u sectors\n",  boot->fat_offset);
    printf("  FatLength           : %u sectors\n",  boot->fat_length);
    printf("  ClusterHeapOffset   : %u sectors\n",  boot->cluster_heap_offset);
    printf("  TotalClusters       : %u\n",           boot->total_clusters);
    printf("  RootDirFirstCluster : %u\n",           boot->root_dir_first_cluster);
    printf("  VolumeSerialNumber  : 0x%08X\n",       boot->volume_serial);
    printf("  FileSystemRevision  : 0x%04X\n",       boot->fs_revision);
    printf("  BytesPerSectorShift : %u  (= %u bytes/sector)\n",
           boot->bytes_per_sector_shift, 1U << boot->bytes_per_sector_shift);
    printf("  SectorsPerClusterShift: %u  (= %u sectors/cluster)\n",
           boot->sectors_per_cluster_shift,
           1U << boot->sectors_per_cluster_shift);
    printf("  NumberOfFats        : %u\n",           boot->num_fats);
    printf("  PercentInUse        : %u%%\n",         boot->percent_in_use);
    printf("\n");
}

/* ── FAT ────────────────────────────────────────────────────────────────── */

int exfat_read_fat(exfat_context_t *ctx) {
    if (!ctx || !ctx->image_file)
        return -1;

    uint64_t fat_offset_bytes =
        (uint64_t)ctx->boot_sector.fat_offset * ctx->bytes_per_sector;
    uint64_t fat_size_bytes =
        (uint64_t)ctx->boot_sector.fat_length * ctx->bytes_per_sector;

    /* Sanity: FAT must cover at least total_clusters+2 entries */
    size_t min_entries = (size_t)ctx->boot_sector.total_clusters + 2U;
    size_t available   = (size_t)(fat_size_bytes / sizeof(uint32_t));
    if (available < min_entries) {
        fprintf(stderr,
                "[exFAT] FAT too small: %zu entries, need %zu\n",
                available, min_entries);
        return -1;
    }

    ctx->fat_table = (uint32_t *)malloc(fat_size_bytes);
    if (!ctx->fat_table) {
        perror("[exFAT] malloc FAT");
        return -1;
    }
    ctx->fat_entries = available;

    if (fseeko(ctx->image_file, (off_t)fat_offset_bytes, SEEK_SET) != 0) {
        perror("[exFAT] fseeko FAT");
        free(ctx->fat_table);
        ctx->fat_table = NULL;
        return -1;
    }
    if (fread(ctx->fat_table, fat_size_bytes, 1, ctx->image_file) != 1) {
        perror("[exFAT] fread FAT");
        free(ctx->fat_table);
        ctx->fat_table = NULL;
        return -1;
    }
    return 0;
}

uint32_t exfat_get_next_cluster(const exfat_context_t *ctx, uint32_t cluster) {
    if (!ctx || !ctx->fat_table)
        return EXFAT_FAT_END_OF_CHAIN;
    if (cluster >= (uint32_t)ctx->fat_entries)
        return EXFAT_FAT_END_OF_CHAIN;
    return ctx->fat_table[cluster];
}

void exfat_free_fat(exfat_context_t *ctx) {
    if (ctx && ctx->fat_table) {
        free(ctx->fat_table);
        ctx->fat_table  = NULL;
        ctx->fat_entries = 0;
    }
}

/* ── Cluster I/O ────────────────────────────────────────────────────────── */

uint64_t exfat_get_cluster_offset(const exfat_context_t *ctx, uint32_t cluster) {
    if (!ctx) return 0;
    return ctx->cluster_heap_offset_bytes +
           (uint64_t)(cluster - 2U) * ctx->bytes_per_cluster;
}

ssize_t exfat_read_cluster(exfat_context_t *ctx, uint32_t cluster,
                            uint8_t *buffer, size_t size) {
    if (!ctx || !ctx->image_file || !buffer)
        return -1;
    if (!exfat_is_valid_cluster(cluster)) {
        fprintf(stderr, "[exFAT] invalid cluster %u\n", cluster);
        return -1;
    }
    uint64_t offset    = exfat_get_cluster_offset(ctx, cluster);
    size_t bytes_to_read = (size < ctx->bytes_per_cluster) ?
                            size : ctx->bytes_per_cluster;

    if (fseeko(ctx->image_file, (off_t)offset, SEEK_SET) != 0) {
        perror("[exFAT] fseeko cluster");
        return -1;
    }
    size_t got = fread(buffer, 1, bytes_to_read, ctx->image_file);
    return (ssize_t)got;
}

/* ── Directory parsing ──────────────────────────────────────────────────── */

int exfat_utf16_to_utf8(const uint16_t *utf16, int nchars,
                         char *utf8, size_t utf8_size) {
    if (!utf16 || !utf8 || utf8_size == 0)
        return -1;

    size_t out = 0;
    for (int i = 0; i < nchars && out < utf8_size - 1U; i++) {
        uint16_t ch = utf16[i];
        if (ch == 0) break;

        if (ch < 0x80U) {
            utf8[out++] = (char)ch;
        } else if (ch < 0x800U) {
            if (out + 2 >= utf8_size) break;
            utf8[out++] = (char)(0xC0U | (ch >> 6));
            utf8[out++] = (char)(0x80U | (ch & 0x3FU));
        } else {
            if (out + 3 >= utf8_size) break;
            utf8[out++] = (char)(0xE0U | (ch >> 12));
            utf8[out++] = (char)(0x80U | ((ch >> 6) & 0x3FU));
            utf8[out++] = (char)(0x80U | (ch & 0x3FU));
        }
    }
    utf8[out] = '\0';
    return 0;
}

/*
 * Parse one complete directory entry set (0x85 + secondaries).
 *
 * @param data      flat buffer containing one or more 32-byte entries
 * @param offset    byte offset of the 0x85 entry within data[]
 * @param data_len  total valid bytes in data[]
 * @param info      output
 *
 * @return  total bytes consumed by this entry set (= (1+secondary_count)*32),
 *          or -1 if the entry is not a 0x85 file/dir entry or data is short.
 */
int exfat_parse_file_entry(const uint8_t *data, size_t offset, size_t data_len,
                            exfat_file_info_t *info) {
    if (!data || !info || offset >= data_len)
        return -1;

    const exfat_file_entry_t *fe =
        (const exfat_file_entry_t *)(data + offset);

    if (fe->entry_type != EXFAT_ENTRY_TYPE_FILE_DIR)
        return -1;

    uint8_t secondary_count = fe->secondary_count;
    size_t  total_bytes     = (size_t)(1 + secondary_count) * EXFAT_ENTRY_SIZE;

    if (offset + total_bytes > data_len)
        return -1;   /* entry set extends beyond buffer */

    memset(info, 0, sizeof(exfat_file_info_t));
    info->attributes        = fe->file_attributes;
    info->create_time       = fe->create_time;
    info->last_modified_time= fe->last_modified_time;
    info->last_accessed_time= fe->last_accessed_time;
    info->is_directory      = exfat_is_directory(fe->file_attributes);

    /* Walk secondary entries ─────────────────────────────────────────── */
    int      name_chars_collected = 0;  /* chars accumulated so far */
    int      name_length          = 0;  /* from stream extension */
    char     name_buf[EXFAT_MAX_FILENAME_LEN * 4 + 1];  /* worst-case UTF-8 */
    size_t   name_buf_used        = 0;

    for (int s = 0; s < (int)secondary_count; s++) {
        const uint8_t *sec = data + offset + (size_t)(s + 1) * EXFAT_ENTRY_SIZE;
        uint8_t stype = sec[0];

        if (stype == EXFAT_ENTRY_TYPE_STREAM_EXT) {
            const exfat_stream_ext_t *sx = (const exfat_stream_ext_t *)sec;
            info->first_cluster = sx->first_cluster;
            info->data_length   = sx->data_length;
            info->no_fat_chain  = (sx->flags & EXFAT_FLAG_NO_FAT_CHAIN) ? 1 : 0;
            name_length         = (int)sx->name_length;  /* total chars */

        } else if (stype == EXFAT_ENTRY_TYPE_FILENAME) {
            const exfat_filename_entry_t *fn =
                (const exfat_filename_entry_t *)sec;

            /*
             * Each filename entry carries up to 15 UTF-16LE code units.
             * Collect until we reach name_length chars (from stream ext).
             * This correctly handles filenames longer than 15 chars that
             * span multiple filename entries.
             */
            int chars_remaining = name_length - name_chars_collected;
            if (chars_remaining <= 0)
                break;

            int chars_here = (chars_remaining < 15) ? chars_remaining : 15;

            /*
             * Copy the packed uint16_t array to a local aligned buffer before
             * passing to exfat_utf16_to_utf8().  Taking the address of a packed
             * struct member directly causes -Waddress-of-packed-member and
             * potential UB on architectures that require aligned 16-bit reads.
             */
            uint16_t name_copy[15];
            memcpy(name_copy, fn->file_name, (size_t)chars_here * sizeof(uint16_t));

            char chunk[15 * 4 + 1];
            exfat_utf16_to_utf8(name_copy, chars_here,
                                 chunk, sizeof(chunk));

            size_t chunk_len = strlen(chunk);
            if (name_buf_used + chunk_len < sizeof(name_buf)) {
                memcpy(name_buf + name_buf_used, chunk, chunk_len);
                name_buf_used         += chunk_len;
                name_chars_collected  += chars_here;
            }
        }
    }

    name_buf[name_buf_used] = '\0';
    /* Copy into the fixed-size filename field; snprintf ensures NUL */
    snprintf(info->filename, sizeof(info->filename), "%s", name_buf);

    return (int)total_bytes;
}

/*
 * Read all directory entries from a cluster chain.
 *
 * FIX: the original only read the first cluster.  This implementation
 * follows the FAT chain (or advances contiguously for NoFatChain dirs)
 * until the chain ends or max_entries is reached.
 *
 * @note  Directories themselves never set NoFatChain in practice, but we
 *        handle it for correctness.
 */
int exfat_read_directory(exfat_context_t *ctx, uint32_t first_cluster,
                          exfat_file_info_t *entries, int max_entries) {
    if (!ctx || !entries || max_entries <= 0)
        return -1;

    uint8_t *cluster_buf = (uint8_t *)malloc(ctx->bytes_per_cluster);
    if (!cluster_buf) {
        perror("[exFAT] malloc directory buffer");
        return -1;
    }

    int      entry_count = 0;
    uint32_t cluster     = first_cluster;

    while (exfat_is_valid_cluster(cluster) && entry_count < max_entries) {
        ssize_t got = exfat_read_cluster(ctx, cluster, cluster_buf,
                                          ctx->bytes_per_cluster);
        if (got <= 0)
            break;

        size_t offset = 0;
        while (offset + EXFAT_ENTRY_SIZE <= (size_t)got &&
               entry_count < max_entries) {

            uint8_t etype = cluster_buf[offset];

            if (etype == EXFAT_ENTRY_TYPE_EOD)
                goto done;   /* end-of-directory — stop scanning */

            if (etype != EXFAT_ENTRY_TYPE_FILE_DIR) {
                offset += EXFAT_ENTRY_SIZE;
                continue;
            }

            int consumed = exfat_parse_file_entry(
                cluster_buf, offset, (size_t)got,
                &entries[entry_count]);

            if (consumed <= 0) {
                offset += EXFAT_ENTRY_SIZE;
                continue;
            }

            /* Only count entries with a non-empty filename */
            if (entries[entry_count].filename[0] != '\0')
                entry_count++;

            offset += (size_t)consumed;
        }

        /* Advance to next cluster */
        uint32_t next = exfat_get_next_cluster(ctx, cluster);
        if (exfat_is_end_of_chain(next))
            break;
        cluster = next;
    }

done:
    free(cluster_buf);
    return entry_count;
}

/* ── File extraction helpers ────────────────────────────────────────────── */

/*
 * Core read loop shared by all three extraction functions.
 *
 * Reads the file described by `info` in EXFAT_IO_CHUNK_SIZE blocks and
 * calls write_cb(write_arg, buf, len) for each.  Returns 0 on success.
 *
 * NoFatChain support: if info->no_fat_chain is set, advance clusters
 * sequentially instead of following the FAT.
 */
typedef int (*exfat_write_cb_t)(void *arg, const uint8_t *buf, size_t len);

static int exfat_read_file_data(exfat_context_t *ctx,
                                 const exfat_file_info_t *info,
                                 exfat_write_cb_t write_cb, void *write_arg) {
    uint8_t  io_buf[EXFAT_IO_CHUNK_SIZE];  /* fixed stack buffer, no malloc */
    uint64_t remaining    = info->data_length;
    uint32_t cluster      = info->first_cluster;

    if (!exfat_is_valid_cluster(cluster) && remaining > 0) {
        fprintf(stderr, "[exFAT] invalid first cluster %u\n", cluster);
        return -1;
    }

    while (remaining > 0 && exfat_is_valid_cluster(cluster)) {
        uint64_t cluster_off = exfat_get_cluster_offset(ctx, cluster);

        /* How much of this cluster do we still need? */
        uint64_t want = (remaining < ctx->bytes_per_cluster) ?
                         remaining : ctx->bytes_per_cluster;

        /* Read in EXFAT_IO_CHUNK_SIZE pieces within the cluster */
        uint64_t cluster_done = 0;
        while (cluster_done < want) {
            uint64_t chunk = want - cluster_done;
            if (chunk > EXFAT_IO_CHUNK_SIZE)
                chunk = EXFAT_IO_CHUNK_SIZE;

            uint64_t abs_off = cluster_off + cluster_done;
            if (fseeko(ctx->image_file, (off_t)abs_off, SEEK_SET) != 0) {
                perror("[exFAT] fseeko file data");
                return -1;
            }
            size_t got = fread(io_buf, 1, (size_t)chunk, ctx->image_file);
            if (got == 0)
                return -1;  /* short read */

            if (write_cb(write_arg, io_buf, got) != 0)
                return -1;

            cluster_done += got;
        }

        remaining -= want;
        if (remaining == 0)
            break;

        /* Advance to next cluster */
        if (info->no_fat_chain) {
            cluster++;  /* contiguous layout */
        } else {
            uint32_t next = exfat_get_next_cluster(ctx, cluster);
            if (exfat_is_end_of_chain(next)) {
                if (remaining > 0)
                    fprintf(stderr, "[exFAT] chain ended with %llu bytes left\n",
                            (unsigned long long)remaining);
                break;
            }
            cluster = next;
        }
    }
    return 0;
}

/* write_cb for FILE* output */
static int write_cb_file(void *arg, const uint8_t *buf, size_t len) {
    FILE *f = (FILE *)arg;
    return (fwrite(buf, 1, len, f) == len) ? 0 : -1;
}

/* write_cb for fd output */
static int write_cb_fd(void *arg, const uint8_t *buf, size_t len) {
    int fd = *(int *)arg;
    while (len > 0) {
        ssize_t n = write(fd, buf, len);
        if (n <= 0) { perror("[exFAT] write"); return -1; }
        buf += n;
        len -= (size_t)n;
    }
    return 0;
}

/* write_cb for in-memory output */
typedef struct { uint8_t *buf; size_t pos; size_t cap; } mem_ctx_t;
static int write_cb_mem(void *arg, const uint8_t *buf, size_t len) {
    mem_ctx_t *m = (mem_ctx_t *)arg;
    if (m->pos + len > m->cap) {
        size_t can_copy = m->cap - m->pos;
        memcpy(m->buf + m->pos, buf, can_copy);
        m->pos += can_copy;
        return -1;  /* buffer full */
    }
    memcpy(m->buf + m->pos, buf, len);
    m->pos += len;
    return 0;
}

int exfat_extract_file(exfat_context_t *ctx, const exfat_file_info_t *info,
                        const char *output_path) {
    if (!ctx || !info || !output_path)
        return -1;
    FILE *f = fopen(output_path, "wb");
    if (!f) { perror("[exFAT] fopen output"); return -1; }
    int rc = exfat_read_file_data(ctx, info, write_cb_file, f);
    fclose(f);
    return rc;
}

int exfat_extract_file_fd(exfat_context_t *ctx, const exfat_file_info_t *info,
                           int output_fd) {
    if (!ctx || !info || output_fd < 0)
        return -1;
    return exfat_read_file_data(ctx, info, write_cb_fd, &output_fd);
}

/*
 * Extract file content directly into a caller-provided buffer.
 *
 * @return  number of bytes written on success, -1 on error (including
 *          truncation when buf_size < info->data_length).
 */
ssize_t exfat_extract_to_buffer(exfat_context_t *ctx,
                                  const exfat_file_info_t *info,
                                  uint8_t *buf, size_t buf_size) {
    if (!ctx || !info || !buf || buf_size == 0)
        return -1;
    if (info->data_length > buf_size) {
        fprintf(stderr, "[exFAT] buffer too small: need %llu, have %zu\n",
                (unsigned long long)info->data_length, buf_size);
        return -1;
    }
    mem_ctx_t m = { buf, 0, buf_size };
    if (exfat_read_file_data(ctx, info, write_cb_mem, &m) != 0)
        return -1;
    return (ssize_t)m.pos;
}

/* ── Context lifecycle ──────────────────────────────────────────────────── */

int exfat_init(exfat_context_t *ctx, const char *image_path) {
    if (!ctx || !image_path)
        return -1;

    memset(ctx, 0, sizeof(exfat_context_t));
    ctx->image_file = fopen(image_path, "rb");
    if (!ctx->image_file) {
        perror("[exFAT] fopen image");
        return -1;
    }
    if (exfat_read_boot_sector(ctx) != 0 || exfat_read_fat(ctx) != 0) {
        exfat_cleanup(ctx);
        return -1;
    }
    return 0;
}

void exfat_cleanup(exfat_context_t *ctx) {
    if (!ctx) return;
    if (ctx->image_file) {
        fclose(ctx->image_file);
        ctx->image_file = NULL;
    }
    exfat_free_fat(ctx);
}

/* ── Utility ────────────────────────────────────────────────────────────── */

void exfat_print_file_info(const exfat_file_info_t *info) {
    if (!info) return;
    char sz[32];
    exfat_format_size(info->data_length, sz, sizeof(sz));
    printf("%s  %-40s  %10s  cluster=%u%s\n",
           info->is_directory ? "DIR " : "FILE",
           info->filename, sz, info->first_cluster,
           info->no_fat_chain ? "  [contiguous]" : "");
}

time_t exfat_dos_time_to_unix(uint32_t dos_time) {
    struct tm t;
    memset(&t, 0, sizeof(t));
    t.tm_sec  = (int)((dos_time & 0x1FU) * 2);
    t.tm_min  = (int)((dos_time >>  5) & 0x3FU);
    t.tm_hour = (int)((dos_time >> 11) & 0x1FU);
    t.tm_mday = (int)((dos_time >> 16) & 0x1FU);
    t.tm_mon  = (int)(((dos_time >> 21) & 0x0FU) - 1U);
    t.tm_year = (int)(((dos_time >> 25) & 0x7FU) + 80U);
    t.tm_isdst = -1;
    return mktime(&t);
}

void exfat_format_size(uint64_t size, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return;
    static const char *units[] = { "B", "KB", "MB", "GB", "TB" };
    double d = (double)size;
    int u = 0;
    while (d >= 1024.0 && u < 4) { d /= 1024.0; u++; }
    snprintf(buf, buf_size, "%.2f %s", d, units[u]);
}
