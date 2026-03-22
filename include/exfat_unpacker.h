#ifndef EXFAT_UNPACKER_H
#define EXFAT_UNPACKER_H
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
 * exfat_unpacker.h — exFAT filesystem image parser
 *
 * Provides read-only access to exFAT images: boot sector, FAT chain,
 * directory traversal and in-memory file extraction.
 *
 * THREAD SAFETY: NOT thread-safe. Callers that share an exfat_context_t
 * across threads must provide external serialisation.  In the NetMount
 * server each call to ensure_meta() holds the per-export meta_mutex, so
 * only one thread ever uses a context at a time — no additional locking
 * is needed.
 *
 * BUG HISTORY (original library, fixed here):
 *   1. exfat_boot_sector_t: volume_flags was uint32_t (should be uint16_t),
 *      shifting bytes_per_sector_power to 0x6E instead of the correct 0x6C.
 *   2. exfat_file_entry_t: phantom first_cluster/data_length fields made
 *      the struct 44 bytes instead of 32; tenth-of-second fields were
 *      uint16_t instead of uint8_t.
 *   3. exfat_filename_entry_t: name_length + name_hash overflowed beyond
 *      the 32-byte entry boundary, causing out-of-bounds reads.
 *   4. exfat_read_directory: read only one cluster, silently truncating
 *      large directories that span multiple clusters.
 *   5. exfat_extract_*: per-cluster malloc in hot path; NoFatChain flag
 *      (stream extension GeneralSecondaryFlags bit 1) ignored.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>  /* ssize_t */
#if defined(_MSC_VER)
#  include <BaseTsd.h>
#  ifndef ssize_t
typedef SSIZE_T ssize_t;
#  endif
#endif

#if defined(_MSC_VER)
#  pragma pack(push, 1)
#  define EXFAT_PACKED
#else
#  define EXFAT_PACKED __attribute__((packed))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ── Constants ──────────────────────────────────────────────────────────── */

#define EXFAT_SECTOR_SIZE           512
#define EXFAT_BOOT_SIGNATURE        0xAA55U
#define EXFAT_FS_NAME               "EXFAT   "   /* 8 bytes per spec */
#define EXFAT_FS_NAME_LEN           8
#define EXFAT_ENTRY_SIZE            32
#define EXFAT_MAX_FILENAME_LEN      255           /* spec max: 255 chars */
#define EXFAT_MAX_NAME_ENTRIES      17            /* ceil(255/15) */

/* Directory entry types */
#define EXFAT_ENTRY_TYPE_FILE_DIR   0x85U
#define EXFAT_ENTRY_TYPE_STREAM_EXT 0xC0U
#define EXFAT_ENTRY_TYPE_FILENAME   0xC1U
#define EXFAT_ENTRY_TYPE_BITMAP     0x81U
#define EXFAT_ENTRY_TYPE_UPCASE     0x82U
#define EXFAT_ENTRY_TYPE_VOLID      0x83U
#define EXFAT_ENTRY_TYPE_EOD        0x00U         /* end-of-directory */

/* GeneralSecondaryFlags (stream extension offset 0x01) */
#define EXFAT_FLAG_ALLOC_POSSIBLE   0x01U
#define EXFAT_FLAG_NO_FAT_CHAIN     0x02U         /* contiguous clusters */

/* File attributes */
#define EXFAT_ATTR_READ_ONLY        0x0001U
#define EXFAT_ATTR_HIDDEN           0x0002U
#define EXFAT_ATTR_SYSTEM           0x0004U
#define EXFAT_ATTR_VOLUME_LABEL     0x0008U
#define EXFAT_ATTR_DIRECTORY        0x0010U
#define EXFAT_ATTR_ARCHIVE          0x0020U

/* FAT special values */
#define EXFAT_FAT_FREE_CLUSTER      0x00000000U
#define EXFAT_FAT_MEDIA_DESCRIPTOR  0xFFFFFFF8U
#define EXFAT_FAT_END_OF_CHAIN      0xFFFFFFFFU
#define EXFAT_FAT_BAD_CLUSTER       0xFFFFFFF7U

/* I/O chunk size for file extraction (avoids per-cluster malloc) */
#define EXFAT_IO_CHUNK_SIZE         (256U * 1024U)  /* 256 KiB */

/* ── Boot Sector (512 bytes, Microsoft exFAT spec §3.1) ─────────────────── */
/*
 * Accurate field layout with correct offsets.  Previously volume_flags was
 * uint32_t (4 bytes) which shifted bytes_per_sector_power from 0x6C to 0x6E.
 *
 * Offset  Field                       Size
 * ──────  ─────────────────────────── ────
 * 0x00    JumpBoot                       3
 * 0x03    FileSystemName ("EXFAT   ")    8
 * 0x0B    MustBeZero                    53
 * 0x40    PartitionOffset (uint64)       8
 * 0x48    VolumeLength    (uint64)       8
 * 0x50    FatOffset       (uint32)       4
 * 0x54    FatLength       (uint32)       4
 * 0x58    ClusterHeapOffset (uint32)     4
 * 0x5C    ClusterCount    (uint32)       4
 * 0x60    FirstClusterOfRootDir (u32)    4
 * 0x64    VolumeSerialNumber (uint32)    4
 * 0x68    FileSystemRevision (uint16)    2
 * 0x6A    VolumeFlags        (uint16)    2   ← was uint32_t (BUG)
 * 0x6C    BytesPerSectorShift (uint8)    1   ← was at 0x6E (BUG)
 * 0x6D    SectorsPerClusterShift (u8)    1   ← was at 0x6F (BUG)
 * 0x6E    NumberOfFats    (uint8)        1
 * 0x6F    DriveSelect     (uint8)        1
 * 0x70    PercentInUse    (uint8)        1
 * 0x71    Reserved                       7
 * 0x78    BootCode                     390
 * 0x1FE   BootSignature   (uint16)       2
 * ──────────────────────────────────────
 *         Total                        512
 */
typedef struct EXFAT_PACKED {
    uint8_t  jump_boot[3];              /* 0x00 */
    uint8_t  fs_name[8];                /* 0x03  "EXFAT   " */
    uint8_t  must_be_zero[53];          /* 0x0B */
    uint64_t partition_offset;          /* 0x40  sectors from media start */
    uint64_t volume_length;             /* 0x48  sectors in volume */
    uint32_t fat_offset;                /* 0x50  sectors from VBR start */
    uint32_t fat_length;                /* 0x54  sectors */
    uint32_t cluster_heap_offset;       /* 0x58  sectors from VBR start */
    uint32_t total_clusters;            /* 0x5C */
    uint32_t root_dir_first_cluster;    /* 0x60 */
    uint32_t volume_serial;             /* 0x64 */
    uint16_t fs_revision;               /* 0x68  e.g. 0x0100 = 1.00 */
    uint16_t volume_flags;              /* 0x6A  FIXED: was uint32_t */
    uint8_t  bytes_per_sector_shift;    /* 0x6C  2^n bytes/sector [9,12] */
    uint8_t  sectors_per_cluster_shift; /* 0x6D  2^n sectors/cluster */
    uint8_t  num_fats;                  /* 0x6E  usually 1 */
    uint8_t  drive_select;              /* 0x6F */
    uint8_t  percent_in_use;            /* 0x70  0xFF = unknown */
    uint8_t  reserved[7];               /* 0x71 */
    uint8_t  boot_code[390];            /* 0x78 */
    uint16_t boot_signature;            /* 0x1FE  must be 0xAA55 */
} exfat_boot_sector_t;

/* Compile-time size assertion — catches any future struct changes. */
typedef char exfat_boot_sector_size_check[
    (sizeof(exfat_boot_sector_t) == 512) ? 1 : -1];

/* ── File Directory Entry (0x85, 32 bytes) ──────────────────────────────── */
/*
 * FIXED: removed phantom first_cluster / data_length fields that do not
 * exist in the 0x85 entry (they belong to the stream extension 0xC0).
 * Also fixed: tenth-of-second fields changed from uint16_t to uint8_t.
 *
 * 0x00  EntryType          1
 * 0x01  SecondaryCount     1
 * 0x02  SetChecksum        2
 * 0x04  FileAttributes     2
 * 0x06  Reserved1          2
 * 0x08  CreateTimestamp    4
 * 0x0C  LastModTimestamp   4
 * 0x10  LastAccTimestamp   4
 * 0x14  Create10ms         1
 * 0x15  LastMod10ms        1
 * 0x16  CreateUtcOffset    1
 * 0x17  LastModUtcOffset   1
 * 0x18  LastAccUtcOffset   1
 * 0x19  Reserved2          7
 * ─────────────────────── ──
 *                         32
 */
typedef struct EXFAT_PACKED {
    uint8_t  entry_type;         /* 0x00  0x85 */
    uint8_t  secondary_count;    /* 0x01  number of secondary entries */
    uint16_t set_checksum;       /* 0x02 */
    uint16_t file_attributes;    /* 0x04  EXFAT_ATTR_* */
    uint16_t reserved1;          /* 0x06 */
    uint32_t create_time;        /* 0x08  DOS timestamp */
    uint32_t last_modified_time; /* 0x0C  DOS timestamp */
    uint32_t last_accessed_time; /* 0x10  DOS timestamp */
    uint8_t  create_10ms;        /* 0x14  FIXED: was uint16_t */
    uint8_t  last_mod_10ms;      /* 0x15  FIXED: was uint16_t */
    uint8_t  create_utc_offset;  /* 0x16 */
    uint8_t  last_mod_utc_offset;/* 0x17 */
    uint8_t  last_acc_utc_offset;/* 0x18 */
    uint8_t  reserved2[7];       /* 0x19 */
} exfat_file_entry_t;

typedef char exfat_file_entry_size_check[
    (sizeof(exfat_file_entry_t) == 32) ? 1 : -1];

/* ── Stream Extension Entry (0xC0, 32 bytes) ────────────────────────────── */
/*
 * 0x00  EntryType                1
 * 0x01  GeneralSecondaryFlags    1  bit0=AllocPossible, bit1=NoFatChain
 * 0x02  Reserved1                1
 * 0x03  NameLength               1  filename length in chars (max 255)
 * 0x04  NameHash                 2
 * 0x06  Reserved2                2
 * 0x08  ValidDataLength          8
 * 0x10  Reserved3                4
 * 0x14  FirstCluster             4
 * 0x18  DataLength               8
 * ─────────────────────────────────
 *                               32
 */
typedef struct EXFAT_PACKED {
    uint8_t  entry_type;         /* 0x00  0xC0 */
    uint8_t  flags;              /* 0x01  GeneralSecondaryFlags */
    uint8_t  reserved1;          /* 0x02 */
    uint8_t  name_length;        /* 0x03  total filename chars */
    uint16_t name_hash;          /* 0x04 */
    uint16_t reserved2;          /* 0x06 */
    uint64_t valid_data_length;  /* 0x08 */
    uint32_t reserved3;          /* 0x10 */
    uint32_t first_cluster;      /* 0x14 */
    uint64_t data_length;        /* 0x18 */
} exfat_stream_ext_t;

typedef char exfat_stream_ext_size_check[
    (sizeof(exfat_stream_ext_t) == 32) ? 1 : -1];

/* ── Filename Entry (0xC1, 32 bytes) ────────────────────────────────────── */
/*
 * FIXED: removed name_length + name_hash fields that were placed beyond the
 * 32-byte boundary, causing out-of-bounds reads in the original code.
 * NameLength lives in the stream extension; each filename entry carries
 * exactly 15 UTF-16LE code units.
 *
 * 0x00  EntryType                 1
 * 0x01  GeneralSecondaryFlags     1
 * 0x02  FileName[15] (UTF-16LE)  30
 * ──────────────────────────────────
 *                                32
 */
typedef struct EXFAT_PACKED {
    uint8_t  entry_type;      /* 0x00  0xC1 */
    uint8_t  flags;           /* 0x01  GeneralSecondaryFlags */
    uint16_t file_name[15];   /* 0x02  up to 15 UTF-16LE chars */
} exfat_filename_entry_t;

typedef char exfat_filename_entry_size_check[
    (sizeof(exfat_filename_entry_t) == 32) ? 1 : -1];

/* ── Main context ───────────────────────────────────────────────────────── */

typedef struct {
    FILE    *image_file;              /* open for reading; owned by context */
    exfat_boot_sector_t boot_sector;
    uint32_t bytes_per_sector;
    uint32_t sectors_per_cluster;
    uint32_t bytes_per_cluster;
    uint64_t cluster_heap_offset_bytes; /* absolute byte offset in image */
    uint32_t *fat_table;
    size_t    fat_entries;
} exfat_context_t;

/* ── Parsed file/directory descriptor ──────────────────────────────────── */

typedef struct {
    char     filename[EXFAT_MAX_FILENAME_LEN + 1];
    uint16_t attributes;
    uint32_t first_cluster;
    uint64_t data_length;
    uint32_t create_time;
    uint32_t last_modified_time;
    uint32_t last_accessed_time;
    int      is_directory;
    int      no_fat_chain; /* 1 = contiguous clusters, FAT not needed */
} exfat_file_info_t;

/* ── API ────────────────────────────────────────────────────────────────── */

/* Context lifecycle */
int  exfat_init(exfat_context_t *ctx, const char *image_path);
void exfat_cleanup(exfat_context_t *ctx);

/* Boot sector */
int  exfat_read_boot_sector(exfat_context_t *ctx);
int  exfat_validate_boot_sector(const exfat_boot_sector_t *boot);
void exfat_print_boot_sector(const exfat_boot_sector_t *boot);

/* FAT */
int      exfat_read_fat(exfat_context_t *ctx);
uint32_t exfat_get_next_cluster(const exfat_context_t *ctx, uint32_t cluster);
void     exfat_free_fat(exfat_context_t *ctx);

/* Cluster I/O */
uint64_t exfat_get_cluster_offset(const exfat_context_t *ctx, uint32_t cluster);
ssize_t  exfat_read_cluster(exfat_context_t *ctx, uint32_t cluster,
                             uint8_t *buffer, size_t size);

/* Directory */
int exfat_read_directory(exfat_context_t *ctx, uint32_t first_cluster,
                          exfat_file_info_t *entries, int max_entries);
int exfat_parse_file_entry(const uint8_t *data, size_t offset, size_t data_len,
                            exfat_file_info_t *info);
int exfat_utf16_to_utf8(const uint16_t *utf16, int nchars,
                         char *utf8, size_t utf8_size);

/* File extraction */
int     exfat_extract_file(exfat_context_t *ctx, const exfat_file_info_t *info,
                            const char *output_path);
int     exfat_extract_file_fd(exfat_context_t *ctx, const exfat_file_info_t *info,
                               int output_fd);
ssize_t exfat_extract_to_buffer(exfat_context_t *ctx, const exfat_file_info_t *info,
                                 uint8_t *buf, size_t buf_size);

/* Utilities */
void   exfat_print_file_info(const exfat_file_info_t *info);
time_t exfat_dos_time_to_unix(uint32_t dos_time);
void   exfat_format_size(uint64_t size, char *buffer, size_t buf_size);

/* ── Inline helpers ─────────────────────────────────────────────────────── */

static inline int exfat_is_valid_cluster(uint32_t c) {
    return c >= 2U && c < EXFAT_FAT_BAD_CLUSTER;
}
static inline int exfat_is_end_of_chain(uint32_t c) {
    return c >= EXFAT_FAT_BAD_CLUSTER;
}
static inline int exfat_is_directory(uint16_t attr) {
    return (attr & EXFAT_ATTR_DIRECTORY) != 0;
}
static inline uint16_t exfat_read_le16(const uint8_t *p) {
    return (uint16_t)((unsigned)p[0] | ((unsigned)p[1] << 8));
}
static inline uint32_t exfat_read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline uint64_t exfat_read_le64(const uint8_t *p) {
    return (uint64_t)exfat_read_le32(p) |
           ((uint64_t)exfat_read_le32(p + 4) << 32);
}

#ifdef __cplusplus
}
#endif

#if defined(_MSC_VER)
#  pragma pack(pop)
#endif
#undef EXFAT_PACKED

#endif /* EXFAT_UNPACKER_H */
