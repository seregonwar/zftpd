#ifndef PKG_UNPACKER_H
#define PKG_UNPACKER_H
/*
 * GNU GPLv3 License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */
 
/*
 * pkg_unpacker — PS4 PKG archive metadata parser
 *
 * Provides read-only access to *unencrypted* metadata entries embedded in the
 * PS4 PKG entry table (e.g., param.sfo, icon0.png, pic1.png).  It does NOT
 * decrypt NPDRM-protected entries or the PFS filesystem image.
 *
 * Modelled after the exfat_unpacker.h API for consistency.
 *
 * ── Thread-safety ──────────────────────────────────────────────────────────
 * NONE.  All functions require external synchronisation when a context is
 * shared across threads.
 *
 * ── Re-entrancy ────────────────────────────────────────────────────────────
 * Functions are NOT re-entrant.
 *
 * ── Platform ───────────────────────────────────────────────────────────────
 * POSIX (_FILE_OFFSET_BITS=64) and Windows (_fseeki64 / _ftelli64).
 *
 * ── Format reference ───────────────────────────────────────────────────────
 * PS4 PKG specification: https://www.psdevwiki.com/ps4/PKG_files
 * shadPS4 implementation (pkg.h / pkg.cpp) cross-referenced for field offsets.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>  /* ssize_t */

#if defined(_MSC_VER)
#  ifndef ssize_t
     typedef SSIZE_T ssize_t;
#  endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Constants
 * ═════════════════════════════════════════════════════════════════════════*/

/** ASCII ".CNT" (big-endian) — used in PS4 PKGs. */
#define PKG_MAGIC_CNT               0x7F434E54U

/** ASCII ".PKG" (big-endian) — used in PS3/some mock PKGs. */
#define PKG_MAGIC_PKG               0x7F504B47U

/** Bytes read from the file start to cover all used header fields. */
#define PKG_HEADER_SIZE             0x1000U

/** On-disk byte length of the content_id field (no NUL in the file). */
#define PKG_CONTENT_ID_LEN          36U

/**
 * Hard cap on entry_count.  A PKG with more than this many entries is
 * treated as malformed.  Keeps allocation bounded and avoids amplification
 * attacks on untrusted files.
 */
#define PKG_MAX_ENTRY_COUNT         10000U

/**
 * Hard cap on the size of a single entry that may be extracted into memory.
 * Individual metadata entries (SFO, icons) are never this large in practice;
 * this guards against corrupt or crafted size fields.
 */
#define PKG_MAX_ENTRY_SIZE          (64U * 1024U * 1024U)   /* 64 MiB */

/**
 * The entry table must not overlap the header.
 * Any table_offset below this value is treated as corrupt.
 */
#define PKG_MIN_TABLE_OFFSET        PKG_HEADER_SIZE

/** Size in bytes of one serialised entry record in the file. */
#define PKG_ENTRY_RECORD_SIZE       32U

/* ── Common unencrypted entry IDs ────────────────────────────────────────── */
#define PKG_ENTRY_ID_PARAM_SFO      0x1000U
#define PKG_ENTRY_ID_ICON0_PNG      0x1200U
#define PKG_ENTRY_ID_ICON1_PNG      0x1210U
#define PKG_ENTRY_ID_PIC0_PNG       0x1220U
#define PKG_ENTRY_ID_PIC1_PNG       0x1230U

/**
 * Bit 31 of flags1.
 *   0 = entry data is plaintext; safe to extract.
 *   1 = entry data is encrypted (NPDRM / system key); extraction yields
 *       raw ciphertext, which is almost certainly not what the caller wants.
 *
 * Always test with pkg_entry_is_encrypted() before extracting.
 */
#define PKG_ENTRY_FLAG_ENCRYPTED    0x80000000U

/* ═══════════════════════════════════════════════════════════════════════════
 * Error codes
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * Return type for all pkg_* functions that can fail.
 * Values are negative so that a single `if (rc < 0)` test catches any error.
 */
typedef enum {
    PKG_OK               =  0,  /**< Success.                                   */
    PKG_ERR_PARAM        = -1,  /**< NULL or invalid argument.                  */
    PKG_ERR_IO           = -2,  /**< File I/O failure (check errno for detail). */
    PKG_ERR_FORMAT       = -3,  /**< File does not conform to the PKG format.   */
    PKG_ERR_RANGE        = -4,  /**< Value would overflow or is out of range.   */
    PKG_ERR_NOMEM        = -5,  /**< malloc() returned NULL.                    */
    PKG_ERR_ENCRYPTED    = -6,  /**< Entry is encrypted; cannot yield plaintext.*/
    PKG_ERR_BUFFER_SMALL = -7   /**< Caller buffer is smaller than entry->size. */
} pkg_error_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Data structures
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * Decoded fields from the on-disk PKG header.
 *
 * @note  content_id is always NUL-terminated even though the on-disk field
 *        has no NUL (PKG_CONTENT_ID_LEN bytes, no terminator).
 */
typedef struct {
    uint32_t magic;
    uint32_t type;
    uint32_t file_count;
    uint32_t entry_count;
    uint32_t table_offset;
    char     content_id[PKG_CONTENT_ID_LEN + 1U]; /* +1 for '\0' */
} pkg_header_t;

/**
 * One entry decoded from the PKG entry table.
 *
 * DESIGN RATIONALE — why expose flags1 / flags2:
 *   Bit 31 of flags1 signals encryption (PKG_ENTRY_FLAG_ENCRYPTED).
 *   Callers must check this before extracting to avoid silently processing
 *   ciphertext.  Hiding these fields would force callers to call
 *   pkg_extract_* and check for PKG_ERR_ENCRYPTED after the seek — wasteful
 *   and harder to reason about.
 *
 * @note  Use pkg_entry_is_encrypted() rather than testing flags1 directly.
 */
typedef struct {
    uint32_t id;               /**< Entry type identifier (e.g. PKG_ENTRY_ID_*). */
    uint32_t filename_offset;  /**< Offset into the "entry_names" name table.    */
    uint32_t flags1;           /**< Bit 31 set → PKG_ENTRY_FLAG_ENCRYPTED.       */
    uint32_t flags2;           /**< Reserved / key index for encrypted entries.  */
    uint32_t offset;           /**< Absolute byte offset of data in the PKG file.*/
    uint32_t size;             /**< Byte length of the entry data.               */
} pkg_entry_t;

/**
 * Parser context.
 *
 * Treat as opaque: do not modify fields directly.  All internal invariants
 * are established by pkg_init() and held until pkg_cleanup().
 */
typedef struct {
    FILE         *file;         /**< Open file handle (owned by this context).  */
    uint64_t      file_size;    /**< Total PKG file size in bytes.              */
    pkg_header_t  header;       /**< Decoded header fields.                     */
    pkg_entry_t  *entries;      /**< Heap-allocated array [0, num_entries).     */
    size_t        num_entries;  /**< Element count of entries[].                */
} pkg_context_t;

/* ═══════════════════════════════════════════════════════════════════════════
 * Inline helpers
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Test whether an entry is flagged as encrypted.
 *
 * Always call this before pkg_extract_to_buffer() or pkg_extract_file_fd()
 * to avoid obtaining ciphertext instead of plaintext.
 *
 * @param[in] entry  Non-NULL pointer to an entry; behaviour undefined if NULL.
 * @return Non-zero if the entry is encrypted, zero if it is plaintext.
 *
 * @pre  entry != NULL
 * @note WCET: O(1), no I/O.
 */
static inline int pkg_entry_is_encrypted(const pkg_entry_t *entry)
{
    return (entry->flags1 & PKG_ENTRY_FLAG_ENCRYPTED) != 0U;
}

/* ═══════════════════════════════════════════════════════════════════════════
 * API
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief Open a PKG file and parse its header and entry table into ctx.
 *
 * On failure the context is left in a state that is safe to pass to
 * pkg_cleanup() — the caller MUST still call pkg_cleanup() to release any
 * partially-acquired resources (e.g., the file handle that was opened before
 * the header was validated).
 *
 * Validation performed:
 *  - Magic bytes == PKG_MAGIC.
 *  - entry_count in [1, PKG_MAX_ENTRY_COUNT].
 *  - table_offset >= PKG_MIN_TABLE_OFFSET.
 *  - table_offset + entry_count * PKG_ENTRY_RECORD_SIZE <= file_size  (no OOB).
 *  - Each entry's [offset, offset+size) is contained within the file.
 *
 * @param[out] ctx       Caller-allocated context; must not be NULL.
 * @param[in]  pkg_path  NUL-terminated path to the PKG file; must not be NULL
 *                       or empty.
 *
 * @return PKG_OK on success.
 * @retval PKG_ERR_PARAM    ctx or pkg_path is NULL / pkg_path is empty.
 * @retval PKG_ERR_IO       fopen, fread, fseeko, or ftello failed.
 * @retval PKG_ERR_FORMAT   File is too small, magic mismatch, entry_count
 *                           out of range, or an entry extends beyond EOF.
 * @retval PKG_ERR_RANGE    Internal arithmetic overflow (defensive).
 * @retval PKG_ERR_NOMEM    malloc() returned NULL.
 *
 * @pre  ctx != NULL
 * @pre  pkg_path != NULL && pkg_path[0] != '\0'
 * @post On PKG_OK: ctx->file != NULL, ctx->entries != NULL,
 *       ctx->num_entries == ctx->header.entry_count,
 *       ctx->header.content_id is NUL-terminated.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: Unbounded (disk I/O; up to PKG_MAX_ENTRY_COUNT entry reads).
 * @warning Do not call from interrupt or hard-real-time context.
 */
int pkg_init(pkg_context_t *ctx, const char *pkg_path);

/**
 * @brief Release all resources owned by ctx.
 *
 * Closes the file handle and frees the entries array.  Safe to call on a
 * partially-initialised context (e.g., after a failed pkg_init).
 * Idempotent: safe to call more than once on the same ctx.
 *
 * @param[in,out] ctx  Context to clean up.  Ignored if NULL.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1), no I/O.
 */
void pkg_cleanup(pkg_context_t *ctx);

/**
 * @brief Find an entry in the table by its numeric ID.
 *
 * Performs a linear scan over the parsed entry array.  For the typical
 * entry counts found in real PKG files (<200) this is faster than a hash
 * map due to cache locality.
 *
 * @param[in] ctx  Initialised context.
 * @param[in] id   Entry ID to search for (e.g., PKG_ENTRY_ID_PARAM_SFO).
 *
 * @return Pointer into ctx->entries[] for the first matching entry, or NULL
 *         if not found or if ctx / ctx->entries is NULL.
 *
 * @warning The returned pointer is invalidated by pkg_cleanup().
 *          Do not free or retain it beyond the lifetime of ctx.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(num_entries).
 */
const pkg_entry_t *pkg_find_entry_by_id(const pkg_context_t *ctx, uint32_t id);

/**
 * @brief Extract a plaintext entry's raw bytes into a caller-supplied buffer.
 *
 * Seeks to entry->offset and reads entry->size bytes into buf.
 * The call is rejected if the entry is encrypted (flags1 bit 31 set).
 *
 * @param[in,out] ctx       Initialised context (file position is modified).
 * @param[in]     entry     Entry to extract; must not be NULL.
 * @param[out]    buf       Destination buffer; must not be NULL.
 * @param[in]     buf_size  Usable bytes in buf.
 *
 * @return Bytes written (>= 0) on success, or a negative pkg_error_t value.
 * @retval PKG_ERR_PARAM        A required argument is NULL or buf_size == 0.
 * @retval PKG_ERR_ENCRYPTED    Entry is encrypted; call refused.
 * @retval PKG_ERR_RANGE        entry->size > PKG_MAX_ENTRY_SIZE or > SSIZE_MAX.
 * @retval PKG_ERR_BUFFER_SMALL buf_size < entry->size.
 * @retval PKG_ERR_IO           fseeko or fread failed, or short read occurred.
 *
 * @pre  (entry->flags1 & PKG_ENTRY_FLAG_ENCRYPTED) == 0
 * @pre  buf_size >= entry->size
 * @post On success, buf[0..entry->size) contains the raw entry data.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: Proportional to entry->size; dominated by disk I/O.
 */
ssize_t pkg_extract_to_buffer(pkg_context_t *ctx, const pkg_entry_t *entry,
                               uint8_t *buf, size_t buf_size);

/**
 * @brief Stream a plaintext entry's raw bytes to an open file descriptor.
 *
 * Uses a fixed-size stack buffer (COPY_BUF_SIZE bytes) internally; never
 * allocates from the heap.  EINTR-interrupted write(2) calls are retried
 * automatically.
 *
 * @param[in,out] ctx        Initialised context (file position is modified).
 * @param[in]     entry      Entry to extract; must not be NULL.
 * @param[in]     output_fd  Open, writable, blocking file descriptor (>= 0).
 *
 * @return PKG_OK on success.
 * @retval PKG_ERR_PARAM     A required argument is NULL or output_fd < 0.
 * @retval PKG_ERR_ENCRYPTED Entry is encrypted; call refused.
 * @retval PKG_ERR_RANGE     entry->size > PKG_MAX_ENTRY_SIZE.
 * @retval PKG_ERR_IO        fseeko, fread, or write(2) failed, or the read
 *                            returned fewer bytes than entry->size (truncated).
 *
 * @pre  (entry->flags1 & PKG_ENTRY_FLAG_ENCRYPTED) == 0
 * @pre  output_fd is open for writing in blocking mode.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: Proportional to entry->size; dominated by disk I/O.
 */
int pkg_extract_file_fd(pkg_context_t *ctx, const pkg_entry_t *entry,
                        int output_fd);

#ifdef __cplusplus
}
#endif

#endif /* PKG_UNPACKER_H */
