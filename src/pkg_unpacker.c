/*
 * GNU GPLv3 License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */
/*
 * pkg_unpacker.c — PS4 PKG archive metadata parser
 *
 * ── DESIGN RATIONALE ────────────────────────────────────────────────────────
 *
 * Big-endian parsing via read_be32():
 *   The original code used expressions of the form `(hdr_buf[N] << 24)`.
 *   When `char` is signed and the byte value >= 0x80, the uint8_t is promoted
 *   to `int`, and shifting a positive `int` into the sign bit is undefined
 *   behaviour (C11 §6.5.7 ¶4).  read_be32() casts every byte to uint32_t
 *   before shifting, eliminating the UB entirely.
 *
 * All offsets widened to uint64_t before arithmetic:
 *   PKG offsets and sizes are stored as uint32_t (big-endian) on disk.
 *   Before any addition or comparison against the 64-bit file size, they are
 *   widened with an explicit cast to prevent silent wrap-around.
 *
 * No dynamic allocation after pkg_init():
 *   pkg_extract_to_buffer() and pkg_extract_file_fd() use only the
 *   caller-supplied buffer or a stack buffer (COPY_BUF_SIZE bytes).
 *
 * No fprintf / perror in library code:
 *   Side-effects on stderr are inappropriate for a library.  All diagnostics
 *   are communicated exclusively through return values (pkg_error_t).
 *
 * EINTR handling in write(2) loop:
 *   A signal delivered between the kernel accepting bytes and write(2)
 *   returning causes it to return -1 / EINTR.  The inner write loop retries
 *   automatically rather than propagating a spurious error.
 *
 * Partial-read detection in pkg_extract_file_fd():
 *   The original code checked only `got == 0` as an fread error.  A short
 *   read (0 < got < to_read) silently corrupted the extracted data.  We now
 *   require `got == to_read` and return PKG_ERR_IO otherwise.
 *
 * Encryption guard on all extraction paths:
 *   flags1 bit 31 (PKG_ENTRY_FLAG_ENCRYPTED) was parsed in the original
 *   struct but never populated.  We now read flags1/flags2 from each entry
 *   record and refuse extraction of encrypted entries with PKG_ERR_ENCRYPTED.
 *
 * ── MISRA C:2012 deviations ─────────────────────────────────────────────────
 *   Rule 15.5  — Single return point: intentionally violated in validation
 *                sequences to avoid deeply nested if-else ladders that reduce
 *                readability without improving safety.
 *   Dir  4.9   — Function-like macros: not used; all helpers are functions.
 */

#ifndef _FILE_OFFSET_BITS
#  define _FILE_OFFSET_BITS 64
#endif
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif

#include "pkg_unpacker.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>     /* SSIZE_MAX */

#if defined(_MSC_VER)
#  include <io.h>
#  define fseeko    _fseeki64
#  define ftello    _ftelli64
#  define write     _write
#  define SSIZE_MAX _I64_MAX
#else
#  include <unistd.h>
#endif

/* ── Internal constants ─────────────────────────────────────────────────── */

/**
 * Stack-buffer size for the streaming copy in pkg_extract_file_fd().
 *
 * DESIGN RATIONALE — why 64 KiB:
 *   Matches one typical OS page-cache read-ahead quantum.  Large enough to
 *   amortise the per-fread overhead; small enough to live on the stack without
 *   risk of overflow on embedded targets with reduced stack sizes.
 */
#define COPY_BUF_SIZE   65536U

/*
 * Byte offsets of the fields we decode within the on-disk header buffer.
 * Cross-referenced against psdevwiki.com/ps4/PKG_files and shadPS4 pkg.h.
 *
 *  0x00  magic           (u32_be)
 *  0x04  type            (u32_be)
 *  0x08  unk_0x08        (u32_be)
 *  0x0C  file_count      (u32_be)
 *  0x10  entry_count     (u32_be)   ← HDR_OFF_ENTRY_COUNT
 *  0x14  sc_entry_count  (u16_be)
 *  0x16  entry_count_2   (u16_be)
 *  0x18  table_offset    (u32_be)   ← HDR_OFF_TABLE_OFFSET
 *  0x1C  entry_data_size (u32_be)
 *  0x20  body_offset     (u64_be)
 *  0x28  body_size       (u64_be)
 *  0x30  content_offset  (u64_be)
 *  0x38  content_size    (u64_be)
 *  0x40  content_id      (char[36]) ← HDR_OFF_CONTENT_ID
 */
#define HDR_OFF_MAGIC           0x000U
#define HDR_OFF_ENTRY_COUNT     0x010U
#define HDR_OFF_TABLE_OFFSET    0x018U
#define HDR_OFF_CONTENT_ID      0x040U

/*
 * Byte offsets within one 32-byte on-disk entry record.
 *
 *  +00  id               (u32_be)
 *  +04  filename_offset  (u32_be)
 *  +08  flags1           (u32_be)   bit 31 = PKG_ENTRY_FLAG_ENCRYPTED
 *  +12  flags2           (u32_be)
 *  +16  offset           (u32_be)
 *  +20  size             (u32_be)
 *  +24  padding          (u64_be)   unused
 */
#define EOFF_ID               0U
#define EOFF_FILENAME_OFFSET  4U
#define EOFF_FLAGS1           8U
#define EOFF_FLAGS2          12U
#define EOFF_OFFSET          16U
#define EOFF_SIZE            20U

/* ── Endianness helper ───────────────────────────────────────────────────── */

/**
 * @brief Read a big-endian uint32_t from an unaligned byte buffer.
 *
 * Each byte is cast to uint32_t before shifting to prevent two sources of
 * undefined behaviour in the original code:
 *  1. `uint8_t` is implicitly promoted to `int` (which may be 32-bit signed).
 *  2. Shifting a signed integer into or past the sign bit is UB (C11 §6.5.7).
 *
 * @param[in] p  Pointer to 4 bytes; must not be NULL (guaranteed by callers).
 * @return The decoded value.
 *
 * @note Thread-safety: pure function, no shared state.
 * @note WCET: O(1), no I/O, no branches.
 */
static uint32_t read_be32(const uint8_t *p)
{
    return (((uint32_t)p[0]) << 24U) |
           (((uint32_t)p[1]) << 16U) |
           (((uint32_t)p[2]) <<  8U) |
            ((uint32_t)p[3]);
}

/* ── File-size helper ────────────────────────────────────────────────────── */

/**
 * @brief Determine total byte size of an open file without disturbing the
 *        current file position.
 *
 * Saves the current position with ftello(), seeks to SEEK_END, queries the
 * position, then restores the saved position.
 *
 * @param[in]  fp   Open file handle; must not be NULL.
 * @param[out] out  Receives the file size on PKG_OK; must not be NULL.
 *
 * @return PKG_OK on success, PKG_ERR_IO if any stdio call fails.
 *
 * @note Thread-safety: NOT thread-safe (modifies fp's position transiently).
 * @note WCET: O(1) on most OS/FS implementations (no I/O read required).
 */
static int get_file_size(FILE *fp, uint64_t *out)
{
    off_t saved = ftello(fp);
    if (saved == (off_t)-1) {
        return PKG_ERR_IO;
    }

    if (fseeko(fp, (off_t)0, SEEK_END) != 0) {
        return PKG_ERR_IO;
    }

    off_t end = ftello(fp);

    /* Restore position before any return, even on error. */
    if (fseeko(fp, saved, SEEK_SET) != 0) {
        return PKG_ERR_IO;
    }

    if (end < (off_t)0) {
        return PKG_ERR_IO;
    }

    *out = (uint64_t)end;
    return PKG_OK;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Context lifecycle
 * ═════════════════════════════════════════════════════════════════════════*/

int pkg_init(pkg_context_t *ctx, const char *pkg_path)
{
    if ((ctx == NULL) || (pkg_path == NULL) || (pkg_path[0] == '\0')) {
        return PKG_ERR_PARAM;
    }

    /*
     * Zero-initialise first.  This ensures pkg_cleanup() is safe to call
     * on ctx at any point below, even before all fields are populated.
     */
    (void)memset(ctx, 0, sizeof(*ctx));

    /* ── Open file ──────────────────────────────────────────────────────── */
    ctx->file = fopen(pkg_path, "rb");
    if (ctx->file == NULL) {
        /* errno is set by fopen; caller may inspect it. */
        return PKG_ERR_IO;
    }

    /* ── Determine file size ────────────────────────────────────────────── */
    {
        int rc = get_file_size(ctx->file, &ctx->file_size);
        if (rc != PKG_OK) {
            pkg_cleanup(ctx);
            return rc;
        }
    }

    if (ctx->file_size < (uint64_t)PKG_HEADER_SIZE) {
        pkg_cleanup(ctx);
        return PKG_ERR_FORMAT;
    }

    /* ── Read the header block ──────────────────────────────────────────── */
    uint8_t hdr_buf[PKG_HEADER_SIZE];
    if (fread(hdr_buf, 1U, sizeof(hdr_buf), ctx->file) != sizeof(hdr_buf)) {
        pkg_cleanup(ctx);
        return PKG_ERR_IO;
    }

    /* ── Validate magic ─────────────────────────────────────────────────── */
    uint32_t magic = read_be32(hdr_buf + HDR_OFF_MAGIC);
    if ((magic != PKG_MAGIC_CNT) && (magic != PKG_MAGIC_PKG)) {
        fprintf(stderr, "[PKG] invalid magic: 0x%08X (expected 0x%08X or 0x%08X)\n", 
                magic, PKG_MAGIC_CNT, PKG_MAGIC_PKG);
        fprintf(stderr, "[PKG] Dump: %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X\n",
                hdr_buf[0], hdr_buf[1], hdr_buf[2], hdr_buf[3],
                hdr_buf[4], hdr_buf[5], hdr_buf[6], hdr_buf[7],
                hdr_buf[8], hdr_buf[9], hdr_buf[10], hdr_buf[11],
                hdr_buf[12], hdr_buf[13], hdr_buf[14], hdr_buf[15]);
        pkg_cleanup(ctx);
        return PKG_ERR_FORMAT;
    }
    ctx->header.magic = magic;

    /* ── Decode and validate entry_count ────────────────────────────────── */
    uint32_t entry_count = read_be32(hdr_buf + HDR_OFF_ENTRY_COUNT);
    if ((entry_count == 0U) || (entry_count > PKG_MAX_ENTRY_COUNT)) {
        fprintf(stderr, "[PKG] invalid entry count: %u\n", entry_count);
        pkg_cleanup(ctx);
        return PKG_ERR_FORMAT;
    }
    ctx->header.entry_count = entry_count;

    /* ── Decode and validate table_offset ──────────────────────────────── */
    uint32_t table_offset = read_be32(hdr_buf + HDR_OFF_TABLE_OFFSET);

    if (table_offset < PKG_MIN_TABLE_OFFSET) {
        /*
         * The entry table overlaps the header — either the file is corrupt
         * or this is not a real PKG.
         */
        pkg_cleanup(ctx);
        return PKG_ERR_FORMAT;
    }

    /*
     * Verify the complete table fits inside the file.
     *
     * All values widened to uint64_t before multiplication and addition to
     * prevent uint32_t wrap-around (e.g., table_offset=0xFFFFFF00 +
     * entry_count*32 could overflow a u32 silently).
     */
    uint64_t table_bytes = (uint64_t)entry_count *
                           (uint64_t)PKG_ENTRY_RECORD_SIZE;
    uint64_t table_end   = (uint64_t)table_offset + table_bytes;

    if ((table_end < (uint64_t)table_offset) || /* addition overflowed u64 */
        (table_end > ctx->file_size)) {
        pkg_cleanup(ctx);
        return PKG_ERR_FORMAT;
    }
    ctx->header.table_offset = table_offset;

    /* ── Copy content_id; NUL-terminate explicitly ──────────────────────── */
    /*
     * The on-disk field is exactly PKG_CONTENT_ID_LEN bytes with no NUL.
     * The struct provides PKG_CONTENT_ID_LEN+1 bytes; zero-init (above)
     * already placed a '\0' at [PKG_CONTENT_ID_LEN], but we write it
     * explicitly for clarity and MISRA Rule 9.1 compliance.
     */
    (void)memcpy(ctx->header.content_id,
                 hdr_buf + HDR_OFF_CONTENT_ID,
                 PKG_CONTENT_ID_LEN);
    ctx->header.content_id[PKG_CONTENT_ID_LEN] = '\0';

    /* ── Allocate entry array ───────────────────────────────────────────── */
    /*
     * Overflow analysis:
     *   entry_count <= PKG_MAX_ENTRY_COUNT == 10 000
     *   sizeof(pkg_entry_t) == 24 bytes
     *   max allocation: 240 000 bytes — well within any sane size_t.
     *   The division check below is a defensive invariant assertion.
     */
    size_t alloc_bytes = (size_t)entry_count * sizeof(pkg_entry_t);
    if ((alloc_bytes / sizeof(pkg_entry_t)) != (size_t)entry_count) {
        /* Should never trigger given the guard above. */
        pkg_cleanup(ctx);
        return PKG_ERR_RANGE;
    }

    ctx->entries = (pkg_entry_t *)malloc(alloc_bytes);
    if (ctx->entries == NULL) {
        pkg_cleanup(ctx);
        return PKG_ERR_NOMEM;
    }

    /* ── Parse each entry record ────────────────────────────────────────── */
    if (fseeko(ctx->file, (off_t)table_offset, SEEK_SET) != 0) {
        pkg_cleanup(ctx);
        return PKG_ERR_IO;
    }

    for (uint32_t i = 0U; i < entry_count; i++) {
        uint8_t e_buf[PKG_ENTRY_RECORD_SIZE];

        if (fread(e_buf, 1U, sizeof(e_buf), ctx->file) != sizeof(e_buf)) {
            pkg_cleanup(ctx);
            return PKG_ERR_IO;
        }

        pkg_entry_t *e = &ctx->entries[i];

        e->id              = read_be32(e_buf + EOFF_ID);
        e->filename_offset = read_be32(e_buf + EOFF_FILENAME_OFFSET);
        e->flags1          = read_be32(e_buf + EOFF_FLAGS1);
        e->flags2          = read_be32(e_buf + EOFF_FLAGS2);
        e->offset          = read_be32(e_buf + EOFF_OFFSET);
        e->size            = read_be32(e_buf + EOFF_SIZE);

        /*
         * Validate that this entry's data is fully contained in the file.
         * Widen to uint64_t before addition to prevent uint32_t wrap.
         *
         * NOTE: We validate all entries on init rather than at extraction
         * time to fail fast on corrupt files and to avoid redundant checks
         * on every call to pkg_extract_*.
         */
        uint64_t entry_end = (uint64_t)e->offset + (uint64_t)e->size;
        if ((entry_end < (uint64_t)e->offset) || /* addition overflow guard */
            (entry_end > ctx->file_size)) {
            pkg_cleanup(ctx);
            return PKG_ERR_FORMAT;
        }
    }

    ctx->num_entries = (size_t)entry_count;
    return PKG_OK;
}

void pkg_cleanup(pkg_context_t *ctx)
{
    if (ctx == NULL) {
        return;
    }

    if (ctx->file != NULL) {
        (void)fclose(ctx->file);
        ctx->file = NULL;
    }

    if (ctx->entries != NULL) {
        free(ctx->entries);
        ctx->entries = NULL;
    }

    ctx->num_entries = 0U;
    ctx->file_size   = 0U;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Entry retrieval
 * ═════════════════════════════════════════════════════════════════════════*/

const pkg_entry_t *pkg_find_entry_by_id(const pkg_context_t *ctx, uint32_t id)
{
    if ((ctx == NULL) || (ctx->entries == NULL)) {
        return NULL;
    }

    for (size_t i = 0U; i < ctx->num_entries; i++) {
        if (ctx->entries[i].id == id) {
            return &ctx->entries[i];
        }
    }
    return NULL;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * Extraction
 * ═════════════════════════════════════════════════════════════════════════*/

ssize_t pkg_extract_to_buffer(pkg_context_t *ctx, const pkg_entry_t *entry,
                               uint8_t *buf, size_t buf_size)
{
    if ((ctx == NULL) || (ctx->file == NULL) ||
        (entry == NULL) || (buf == NULL) || (buf_size == 0U)) {
        return (ssize_t)PKG_ERR_PARAM;
    }

    if (pkg_entry_is_encrypted(entry)) {
        return (ssize_t)PKG_ERR_ENCRYPTED;
    }

    /*
     * Bound the in-memory extraction size.  This is a defence-in-depth check;
     * pkg_init() already validated that entry->offset + entry->size <=
     * file_size, so a corrupt entry->size cannot point past the file.
     * However, without this guard a 4 GiB size field would cause a
     * 4 GiB stack or heap demand on the caller's side.
     */
    if (entry->size > PKG_MAX_ENTRY_SIZE) {
        return (ssize_t)PKG_ERR_RANGE;
    }

    if ((size_t)entry->size > buf_size) {
        return (ssize_t)PKG_ERR_BUFFER_SMALL;
    }

    /*
     * ssize_t range note:
     *   entry->size <= PKG_MAX_ENTRY_SIZE (64 MiB) is guaranteed by the
     *   check above.  SSIZE_MAX is at minimum INT32_MAX (~2 GiB) on all
     *   supported platforms, so the cast on the return statement below is
     *   always safe.  No additional guard is required.
     */

    if (entry->size == 0U) {
        return (ssize_t)0;
    }

    if (fseeko(ctx->file, (off_t)entry->offset, SEEK_SET) != 0) {
        return (ssize_t)PKG_ERR_IO;
    }

    size_t got = fread(buf, 1U, (size_t)entry->size, ctx->file);
    if (got != (size_t)entry->size) {
        /*
         * A short read indicates either an I/O error or that the file was
         * truncated between pkg_init() and this call.  Either way, the
         * buffer content is incomplete and must not be used.
         */
        return (ssize_t)PKG_ERR_IO;
    }

    return (ssize_t)got;
}

int pkg_extract_file_fd(pkg_context_t *ctx, const pkg_entry_t *entry,
                        int output_fd)
{
    if ((ctx == NULL) || (ctx->file == NULL) ||
        (entry == NULL) || (output_fd < 0)) {
        return PKG_ERR_PARAM;
    }

    if (pkg_entry_is_encrypted(entry)) {
        return PKG_ERR_ENCRYPTED;
    }

    if (entry->size > PKG_MAX_ENTRY_SIZE) {
        return PKG_ERR_RANGE;
    }

    if (fseeko(ctx->file, (off_t)entry->offset, SEEK_SET) != 0) {
        return PKG_ERR_IO;
    }

    /* Stack-allocated copy buffer — no heap allocation in this function. */
    uint8_t copy_buf[COPY_BUF_SIZE];
    uint32_t remaining = entry->size;

    while (remaining > 0U) {
        size_t to_read = (remaining < (uint32_t)COPY_BUF_SIZE)
                         ? (size_t)remaining
                         : (size_t)COPY_BUF_SIZE;

        size_t got = fread(copy_buf, 1U, to_read, ctx->file);

        if (got == 0U) {
            /* EOF or hard read error before we consumed entry->size bytes. */
            return PKG_ERR_IO;
        }

        if (got != to_read) {
            /*
             * Partial read: the file is shorter than entry->size claimed.
             * The original code silently continued with a smaller chunk,
             * producing a corrupt output file.  We fail fast instead.
             */
            return PKG_ERR_IO;
        }

        /*
         * Write the full buffer to output_fd.
         *
         * DESIGN RATIONALE — inner retry loop for EINTR:
         *   A signal delivered while write(2) is in progress causes it to
         *   return -1 with errno == EINTR.  This is not a true error; we
         *   simply retry the write from where we left off.  Without this,
         *   callers running under a debugger or with timers would see
         *   spurious PKG_ERR_IO failures.
         */
        const uint8_t *p   = copy_buf;
        size_t         len = got;

        while (len > 0U) {
            ssize_t n = write(output_fd, p, len);

            if (n < 0) {
                if (errno == EINTR) {
                    continue;   /* Signal interrupted; retry this write. */
                }
                return PKG_ERR_IO;
            }

            if (n == 0) {
                /*
                 * write(2) returning 0 without error is not specified by
                 * POSIX for regular files; treat it as a non-recoverable
                 * error to avoid an infinite spin.
                 */
                return PKG_ERR_IO;
            }

            /* Safe casts: 0 < n <= (ssize_t)len, len is a size_t. */
            p   += (size_t)n;
            len -= (size_t)n;
        }

        /*
         * Safe subtraction: got == to_read <= remaining (ensured above),
         * so remaining - got >= 0 and no uint32_t underflow is possible.
         */
        remaining -= (uint32_t)got;
    }

    return PKG_OK;
}
