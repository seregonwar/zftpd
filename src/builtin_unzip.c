/* ══ BUILTIN UNZIP ═══════════════════════════════════════════════════════════
 * Self-contained ZIP extractor backed by the bundled miniz reader.
 *
 * No platform library dependencies — compiles and runs on PS4/PS5 without
 * libarchive or libz.
 *
 * Supported by miniz: Store (method 0), Deflate (method 8), Zip64, data
 * descriptors, and CRC validation.
 *
 * ZIP reference:  PKWARE APPNOTE.TXT v6.3.4
 * Deflate ref:    RFC 1951
 * ═════════════════════════════════════════════════════════════════════════ */

#include "builtin_unzip.h"
#include "ftp_types.h"
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define MINIZ_NO_ARCHIVE_WRITING_APIS
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wcast-align"
#pragma clang diagnostic ignored "-Wcast-qual"
#pragma clang diagnostic ignored "-Wconversion"
#pragma clang diagnostic ignored "-Wmissing-prototypes"
#pragma clang diagnostic ignored "-Wredundant-decls"
#pragma clang diagnostic ignored "-Wshadow"
#pragma clang diagnostic ignored "-Wsign-conversion"
#pragma clang diagnostic ignored "-Wstrict-prototypes"
#pragma clang diagnostic ignored "-Wundef"
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wwrite-strings"
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-align"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wmissing-prototypes"
#pragma GCC diagnostic ignored "-Wredundant-decls"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wsign-conversion"
#pragma GCC diagnostic ignored "-Wstrict-prototypes"
#pragma GCC diagnostic ignored "-Wundef"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wwrite-strings"
#endif
#include "../external/Itemzflow-main/itemzflow/include/zip/miniz.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ═════════════════════════════════════════════════════════════════════════
 * ZIP CONSTANTS
 * ═════════════════════════════════════════════════════════════════════════ */

#define ZIP_WRITE_BUF_SIZE    (256U * 1024U)  /* 256 KB write buffer */

#define BUILTIN_UNZIP_USE_MINIZ 1

#if !BUILTIN_UNZIP_USE_MINIZ
/* ═════════════════════════════════════════════════════════════════════════
 * MINIMAL INFLATE DECOMPRESSOR (RFC 1951)
 *
 * Fixed Huffman + dynamic Huffman.
 * No stored-block or window larger than 32 KB is common; we support the
 * full spec up to the standard 32 KB window.
 * ═════════════════════════════════════════════════════════════════════════ */

#define INFLATE_MAX_BITS      15
#define INFLATE_MAX_CODES     288     /* max symbol count (lit/len)     */
#define INFLATE_HUFF_TABLE    32768   /* 1<<15: direct-lookup table    */
#define INFLATE_WINDOW_SIZE   32768U

typedef struct {
  const uint8_t *in;         /* input buffer */
  size_t         in_size;
  size_t         in_pos;
  uint32_t       bit_buf;
  int            bit_count;
  uint8_t       *window;     /* 32 KB sliding window */
  size_t         win_pos;
} inflate_state_t;

/* Length base values for codes 257-285 */
static const uint16_t inflate_length_base[] = {
  3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
  35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258
};

/* Extra bits for length codes 257-285 */
static const uint8_t inflate_length_extra[] = {
  0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
  3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0
};

/* Distance base values for codes 0-29 */
static const uint16_t inflate_dist_base[] = {
  1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
  257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
  8193, 12289, 16385, 24577
};

/* Extra bits for distance codes 0-29 */
static const uint8_t inflate_dist_extra[] = {
  0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
  7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13
};

/* Fixed Huffman code lengths (RFC 1951 section 3.2.6).
 * Initialised once at first use — safe because builtin_unzip() runs on a
 * single background extraction thread per request. */
static uint8_t inflate_fixed_lit_len[288];
static uint8_t inflate_fixed_dist[32];
static int inflate_fixed_init_done = 0;

static void inflate_init_fixed(void) {
  if (inflate_fixed_init_done) return;
  int i;
  for (i = 0; i <= 143; i++) inflate_fixed_lit_len[i] = 8;
  for (i = 144; i <= 255; i++) inflate_fixed_lit_len[i] = 9;
  for (i = 256; i <= 279; i++) inflate_fixed_lit_len[i] = 7;
  for (i = 280; i <= 287; i++) inflate_fixed_lit_len[i] = 8;
  for (i = 0; i <= 31; i++) inflate_fixed_dist[i] = 5;
  inflate_fixed_init_done = 1;
}

/* ── bit reader ── */

static int inflate_need_bits(inflate_state_t *s, int n) {
  while (s->bit_count < n) {
    if (s->in_pos >= s->in_size) return -1;
    s->bit_buf |= (uint32_t)s->in[s->in_pos++] << s->bit_count;
    s->bit_count += 8;
  }
  return 0;
}

static uint32_t inflate_peek_bits(inflate_state_t *s, int n) {
  return s->bit_buf & ((1U << n) - 1U);
}

static void inflate_drop_bits(inflate_state_t *s, int n) {
  s->bit_buf >>= n;
  s->bit_count -= n;
}

static int inflate_read_bits(inflate_state_t *s, int n, uint32_t *out) {
  if (inflate_need_bits(s, n) != 0) return -1;
  *out = inflate_peek_bits(s, n);
  inflate_drop_bits(s, n);
  return 0;
}

/* ── Huffman tree builder ── */

typedef struct {
  uint16_t counts[INFLATE_MAX_BITS + 1];
  uint16_t symbols[INFLATE_HUFF_TABLE]; /* 32768 entries: direct-lookup */
} inflate_huff_t;

static int inflate_build_huff(inflate_huff_t *h, const uint8_t *lengths,
                               int n) {
  int i, len;
  /* Count codes per length */
  for (i = 0; i <= INFLATE_MAX_BITS; i++) h->counts[i] = 0;
  for (i = 0; i < n; i++) {
    len = lengths[i];
    if (len > INFLATE_MAX_BITS) return -1;
    if (len > 0) h->counts[len]++;
  }
  h->counts[0] = 0;

  /* Assign canonical codes (MSB-first per RFC 1951 §3.2.1) */
  uint32_t code = 0;
  uint16_t next_code[INFLATE_MAX_BITS + 1];
  for (len = 1; len <= INFLATE_MAX_BITS; len++) {
    code = (code + h->counts[len - 1]) << 1;
    next_code[len] = (uint16_t)code;
  }

  /* Clear entire lookup table */
  for (i = 0; i < INFLATE_HUFF_TABLE; i++) h->symbols[i] = 0xFFFF;

  /* Build symbol table — bit-reverse canonical codes so the decoder
   * (which reads bits LSB-first) can use them as direct indices. */
  for (i = 0; i < n; i++) {
    len = lengths[i];
    if (len > 0) {
      uint16_t canon = next_code[len];          /* MSB-first canonical */
      /* Bit-reverse for LSB-first lookup */
      uint16_t rev = 0;
      int b;
      for (b = 0; b < len; b++) {
        rev = (uint16_t)((uint16_t)(rev << 1) | (uint16_t)(canon & 1U));
        canon = (uint16_t)(canon >> 1);
      }
      h->symbols[rev] = (uint16_t)i;
      next_code[len] = (uint16_t)(next_code[len] + 1);
    }
  }

  return 0;
}

/* Decode one symbol.  Reads bits LSB-first from the stream (per RFC 1951)
 * and uses the bit-reversed symbol table for O(1) lookup.
 *
 * The bit-by-bit loop avoids requesting more bits than actually needed,
 * which prevents EOF failures when fewer than 15 bits remain. */
static int inflate_decode_huff(inflate_state_t *s, const inflate_huff_t *h,
                               int *symbol) {
  uint32_t code = 0;
  int len;
  for (len = 1; len <= INFLATE_MAX_BITS; len++) {
    if (inflate_need_bits(s, 1) != 0) return -1;
    code = (code << 1) | (inflate_peek_bits(s, 1) & 1U);
    inflate_drop_bits(s, 1);
    if (h->symbols[code] != 0xFFFF) {
      *symbol = h->symbols[code];
      return 0;
    }
  }
  return -1;
}

/* ── Output byte to decompressed stream ── */

typedef int (*unzip_write_cb)(void *userdata, const uint8_t *data, size_t len);

/* ── Main inflate routine ── */

static int inflate_block(inflate_state_t *s, unzip_write_cb write_cb,
                          void *userdata) {
  uint32_t bfinal, btype;

  if (inflate_read_bits(s, 1, &bfinal) != 0) return -1;
  if (inflate_read_bits(s, 2, &btype) != 0) return -1;

  if (btype == 0) {
    /* Stored (uncompressed) block — RFC 1951 §3.2.4 */
    inflate_drop_bits(s, s->bit_count & 7); /* align to byte boundary */
    uint32_t len_raw, nlen_raw;
    if (inflate_read_bits(s, 16, &len_raw) != 0) return -1;
    if (inflate_read_bits(s, 16, &nlen_raw) != 0) return -1;
    uint32_t len16 = len_raw & 0xFFFFU;
    uint32_t nlen16 = nlen_raw & 0xFFFFU;
    if ((len16 ^ nlen16) != 0xFFFFU) return -1; /* integrity check */
    for (uint32_t k = 0; k < len16; k++) {
      uint32_t b;
      if (inflate_read_bits(s, 8, &b) != 0) return -1;
      uint8_t byte = (uint8_t)b;
      s->window[s->win_pos & (INFLATE_WINDOW_SIZE - 1U)] = byte;
      s->win_pos++;
      if (write_cb(userdata, &byte, 1) != 0) return -1;
    }
    if (bfinal == 0) return inflate_block(s, write_cb, userdata);
    return 0;
  }

  /* Huffman tables are ~64KB each — use static allocation to avoid both
   * stack overflow (PS4/PS5) and malloc-free complexity. Safe because
   * extraction runs on a single background thread per request. */
  static inflate_huff_t lit_huff;
  static inflate_huff_t dist_huff;

  const uint8_t *lit_lens, *dist_lens;
  int lit_count, dist_count;

  if (btype == 1) {
    /* Fixed Huffman */
    inflate_init_fixed();
    lit_lens = inflate_fixed_lit_len;
    lit_count = 288;
    dist_lens = inflate_fixed_dist;
    dist_count = 32;
  } else if (btype == 2) {
    /* Dynamic Huffman */
    uint32_t hlit, hdist, hclen;
    if (inflate_read_bits(s, 5, &hlit) != 0) return -1;
    if (inflate_read_bits(s, 5, &hdist) != 0) return -1;
    if (inflate_read_bits(s, 4, &hclen) != 0) return -1;

    hlit += 257;
    hdist += 1;
    hclen += 4;

    /* Code length code order (RFC 1951) */
    static const int cl_order[] = {
      16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
    };
    uint8_t cl_lengths[19] = {0};
    static inflate_huff_t cl_huff;  /* 19 symbols, 64KB struct — static to avoid stack overflow */
    for (int i = 0; i < (int)hclen; i++) {
      uint32_t v;
      if (inflate_read_bits(s, 3, &v) != 0) return -1;
      cl_lengths[cl_order[i]] = (uint8_t)v;
    }

    if (inflate_build_huff(&cl_huff, cl_lengths, 19) != 0) return -1;

    /* Decode literal/length + distance code lengths */
    uint8_t dyn_lit_len[288 + 32];
    int total = (int)(hlit + hdist);
    int idx = 0;
    while (idx < total) {
      int sym;
      if (inflate_decode_huff(s, &cl_huff, &sym) != 0) return -1;
      if (sym < 16) {
        dyn_lit_len[idx++] = (uint8_t)sym;
      } else if (sym == 16) {
        uint32_t rep;
        if (inflate_read_bits(s, 2, &rep) != 0) return -1;
        rep += 3;
        if (idx == 0) return -1;
        uint8_t prev = dyn_lit_len[idx - 1];
        for (uint32_t r = 0; r < rep && idx < total; r++)
          dyn_lit_len[idx++] = prev;
      } else if (sym == 17) {
        uint32_t rep;
        if (inflate_read_bits(s, 3, &rep) != 0) return -1;
        rep += 3;
        for (uint32_t r = 0; r < rep && idx < total; r++)
          dyn_lit_len[idx++] = 0;
      } else if (sym == 18) {
        uint32_t rep;
        if (inflate_read_bits(s, 7, &rep) != 0) return -1;
        rep += 11;
        for (uint32_t r = 0; r < rep && idx < total; r++)
          dyn_lit_len[idx++] = 0;
      }
    }

    lit_lens = dyn_lit_len;
    lit_count = (int)hlit;
    dist_lens = dyn_lit_len + hlit;
    dist_count = (int)hdist;
  } else {
    return -1; /* reserved block type */
  }

  if (inflate_build_huff(&lit_huff, lit_lens, lit_count) != 0) return -1;
  if (inflate_build_huff(&dist_huff, dist_lens, dist_count) != 0) return -1;

  /* Decode symbols */
  for (;;) {
    int sym;
    if (inflate_decode_huff(s, &lit_huff, &sym) != 0) return -1;

    if (sym < 256) {
      /* Literal byte */
      uint8_t b = (uint8_t)sym;
      s->window[s->win_pos & (INFLATE_WINDOW_SIZE - 1U)] = b;
      s->win_pos++;
      if (write_cb(userdata, &b, 1) != 0) return -1;
    } else if (sym == 256) {
      /* End of block */
      break;
    } else {
      /* Length + distance */
      int len_idx = sym - 257;
      if (len_idx < 0 || len_idx >= 29) return -1;
      uint32_t length = inflate_length_base[len_idx];
      int extra_bits = inflate_length_extra[len_idx];
      if (extra_bits > 0) {
        uint32_t extra;
        if (inflate_read_bits(s, extra_bits, &extra) != 0) return -1;
        length += extra;
      }

      int dist_sym;
      if (inflate_decode_huff(s, &dist_huff, &dist_sym) != 0) return -1;
      if (dist_sym < 0 || dist_sym >= 30) return -1;
      uint32_t distance = inflate_dist_base[dist_sym];
      int dist_extra = inflate_dist_extra[dist_sym];
      if (dist_extra > 0) {
        uint32_t extra;
        if (inflate_read_bits(s, dist_extra, &extra) != 0) return -1;
        distance += extra;
      }

      /* Copy from window */
      if (distance > s->win_pos || distance == 0) return -1;
      size_t src = s->win_pos - (size_t)distance;
      for (uint32_t k = 0; k < length; k++) {
        uint8_t b = s->window[src & (INFLATE_WINDOW_SIZE - 1U)];
        s->window[s->win_pos & (INFLATE_WINDOW_SIZE - 1U)] = b;
        s->win_pos++;
        src++;
        if (write_cb(userdata, &b, 1) != 0) return -1;
      }
    }
  }

  /* Recurse if not final block */
  if (bfinal == 0) return inflate_block(s, write_cb, userdata);
  return 0;
}
#endif /* !BUILTIN_UNZIP_USE_MINIZ */

/* ═════════════════════════════════════════════════════════════════════════
 * ZIP STREAMING HELPERS
 * ═════════════════════════════════════════════════════════════════════════ */

/* Write callback: append to output fd */
typedef struct {
  int     fd;
  uint8_t buf[ZIP_WRITE_BUF_SIZE];
  size_t  buf_pos;
} write_ctx_t;

static int write_flush(write_ctx_t *wc) {
  if (wc->buf_pos == 0) return 0;
  size_t written = 0;
  while (written < wc->buf_pos) {
    ssize_t n = write(wc->fd, wc->buf + written, wc->buf_pos - written);
    if (n < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    written += (size_t)n;
  }
  wc->buf_pos = 0;
  return 0;
}

static int write_byte(void *userdata, const uint8_t *data, size_t len) {
  write_ctx_t *wc = (write_ctx_t *)userdata;
  size_t off = 0U;
  while (off < len) {
    size_t space = ZIP_WRITE_BUF_SIZE - wc->buf_pos;
    size_t chunk = len - off;
    if (chunk > space) {
      chunk = space;
    }
    memcpy(wc->buf + wc->buf_pos, data + off, chunk);
    wc->buf_pos += chunk;
    off += chunk;
    if (wc->buf_pos >= ZIP_WRITE_BUF_SIZE) {
      if (write_flush(wc) != 0) return -1;
    }
  }
  return 0;
}

/* ═════════════════════════════════════════════════════════════════════════
 * PATH SANITIZATION
 * ═════════════════════════════════════════════════════════════════════════ */

static int sanitize_zip_path(const char *name, size_t len,
                             char *out, size_t out_size) {
  if (name == NULL || out == NULL || out_size == 0U || len == 0U) {
    return -1;
  }

  if (name[0] == '/' || name[0] == '\\') {
    return -1;
  }
  if (len >= 2U && isalpha((unsigned char)name[0]) && name[1] == ':') {
    return -1;
  }

  size_t wi = 0U;
  size_t comp_start = 0U;
  size_t comp_len = 0U;

  for (size_t i = 0U; i <= len; i++) {
    int at_end = (i == len);
    char c = at_end ? '/' : name[i];
    int sep = (c == '/' || c == '\\');

    if (!sep) {
      if ((unsigned char)c < 32U) {
        return -1;
      }
      if (comp_len == 0U) {
        comp_start = i;
      }
      comp_len++;
      continue;
    }

    if (comp_len == 0U) {
      return -1;
    }

    if (comp_len == 1U && name[comp_start] == '.') {
      return -1;
    }
    if (comp_len == 2U && name[comp_start] == '.' &&
        name[comp_start + 1U] == '.') {
      return -1;
    }

    if (wi > 0U) {
      if (wi + 1U >= out_size) {
        return -1;
      }
      out[wi++] = '/';
    }
    if (wi + comp_len >= out_size) {
      return -1;
    }
    memcpy(out + wi, name + comp_start, comp_len);
    wi += comp_len;
    comp_len = 0U;
  }

  if (wi == 0U || wi >= out_size) {
    return -1;
  }
  out[wi] = '\0';
  return 0;
}

static int build_dest_path(const char *dest_dir, const char *entry_name,
                           char *out, size_t out_size) {
  size_t dlen = strlen(dest_dir);
  size_t elen = strlen(entry_name);

  /* Strip trailing slash from dest_dir */
  while (dlen > 0 && dest_dir[dlen - 1] == '/') dlen--;

  if (dlen + 1 + elen + 1 > out_size) return -1;

  if (dlen == 0) {
    /* dest_dir is "/" or empty */
    out[0] = '/';
    memcpy(out + 1, entry_name, elen + 1);
  } else {
    memcpy(out, dest_dir, dlen);
    out[dlen] = '/';
    memcpy(out + dlen + 1, entry_name, elen + 1);
  }
  return 0;
}

static int mkdir_p(const char *path) {
  char tmp[1024];
  size_t len = strlen(path);
  if (len >= sizeof(tmp)) return -1;

  memcpy(tmp, path, len + 1);

  for (size_t i = 1; i < len; i++) {
    if (tmp[i] == '/') {
      tmp[i] = '\0';
      if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
      tmp[i] = '/';
    }
  }
  if (mkdir(tmp, 0777) != 0 && errno != EEXIST) return -1;
  return 0;
}

typedef struct {
  int fd;
} unzip_read_ctx_t;

static size_t unzip_miniz_read(void *opaque, mz_uint64 file_ofs,
                               void *buf, size_t len) {
  unzip_read_ctx_t *ctx = (unzip_read_ctx_t *)opaque;
  uint8_t *out = (uint8_t *)buf;
  size_t done = 0U;

  if (ctx == NULL || ctx->fd < 0 || buf == NULL) {
    return 0U;
  }

  off_t off = (off_t)file_ofs;
  if ((mz_uint64)off != file_ofs) {
    return 0U;
  }

  while (done < len) {
    ssize_t n = pread(ctx->fd, out + done, len - done, off + (off_t)done);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      break;
    }
    if (n == 0) {
      break;
    }
    done += (size_t)n;
  }

  return done;
}

typedef struct {
  write_ctx_t   wc;
  volatile int *cancelled;
} unzip_write_ctx_t;

static size_t unzip_miniz_write(void *opaque, mz_uint64 file_ofs,
                                const void *buf, size_t len) {
  unzip_write_ctx_t *ctx = (unzip_write_ctx_t *)opaque;
  (void)file_ofs;

  if (ctx == NULL || buf == NULL) {
    return 0U;
  }
  if (ctx->cancelled != NULL && *ctx->cancelled) {
    return 0U;
  }

  return (write_byte(&ctx->wc, (const uint8_t *)buf, len) == 0) ? len : 0U;
}

/* ═════════════════════════════════════════════════════════════════════════
 * PUBLIC API
 * ═════════════════════════════════════════════════════════════════════════ */

int builtin_unzip(const char *zip_path, const char *dest_dir,
                  volatile int *cancelled,
                  char *error_msg, size_t error_msg_size) {

  if (error_msg && error_msg_size > 0) error_msg[0] = '\0';

  int zip_fd = open(zip_path, O_RDONLY);
  if (zip_fd < 0) {
    if (error_msg) snprintf(error_msg, error_msg_size,
                            "Cannot open ZIP: %s", strerror(errno));
    return -1;
  }

  struct stat st;
  if (fstat(zip_fd, &st) != 0 || st.st_size < 0) {
    if (error_msg) snprintf(error_msg, error_msg_size, "Cannot stat ZIP: %s",
                            strerror(errno));
    close(zip_fd);
    return -1;
  }

  mz_zip_archive zip;
  mz_zip_zero_struct(&zip);
  unzip_read_ctx_t read_ctx;
  read_ctx.fd = zip_fd;
  zip.m_pRead = unzip_miniz_read;
  zip.m_pIO_opaque = &read_ctx;

  if (!mz_zip_reader_init(&zip, (mz_uint64)st.st_size, 0)) {
    const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
    if (error_msg) snprintf(error_msg, error_msg_size, "Invalid ZIP: %s",
                            mz_err ? mz_err : "unknown error");
    close(zip_fd);
    return -1;
  }

  int result = 0;
  mz_uint num_files = mz_zip_reader_get_num_files(&zip);

  for (mz_uint i = 0; i < num_files; i++) {
    if (cancelled && *cancelled) {
      if (error_msg) snprintf(error_msg, error_msg_size, "Cancelled");
      result = -1;
      break;
    }

    char entry_name[512];
    mz_uint name_need = mz_zip_reader_get_filename(&zip, i, NULL, 0);
    if (name_need == 0U || name_need > (mz_uint)sizeof(entry_name)) {
      if (error_msg) snprintf(error_msg, error_msg_size,
                              "ZIP entry path too long");
      result = -1;
      break;
    }
    (void)mz_zip_reader_get_filename(&zip, i, entry_name,
                                     (mz_uint)sizeof(entry_name));

    mz_zip_archive_file_stat file_stat;
    if (!mz_zip_reader_file_stat(&zip, i, &file_stat)) {
      const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
      if (error_msg) snprintf(error_msg, error_msg_size,
                              "ZIP metadata error: %s",
                              mz_err ? mz_err : "unknown error");
      result = -1;
      break;
    }

    size_t name_len = strlen(entry_name);
    if (name_len == 0U) {
      continue;
    }

    if (file_stat.m_is_directory ||
        entry_name[name_len - 1U] == '/' ||
        entry_name[name_len - 1U] == '\\') {
      while (name_len > 0U &&
             (entry_name[name_len - 1U] == '/' ||
              entry_name[name_len - 1U] == '\\')) {
        name_len--;
      }
      if (name_len == 0U) {
        continue;
      }

      char safe_name[512];
      if (sanitize_zip_path(entry_name, name_len,
                            safe_name, sizeof(safe_name)) != 0) {
        continue;
      }

      char dest_full[1024];
      if (build_dest_path(dest_dir, safe_name, dest_full, sizeof(dest_full)) == 0) {
        (void)mkdir_p(dest_full);
      }
      continue;
    }

    if (!file_stat.m_is_supported) {
      if (error_msg && file_stat.m_is_encrypted) {
        snprintf(error_msg, error_msg_size,
                 "Encrypted ZIP entry is not supported: %s", entry_name);
      } else if (error_msg) {
        snprintf(error_msg, error_msg_size,
                 "Unsupported ZIP compression method %u in: %s",
                 (unsigned)file_stat.m_method, entry_name);
      }
      result = -1;
      break;
    }

    char safe_name[512];
    if (sanitize_zip_path(entry_name, name_len,
                          safe_name, sizeof(safe_name)) != 0) {
      continue; /* path traversal attempt — skip */
    }

    char dest_full[1024];
    if (build_dest_path(dest_dir, safe_name, dest_full, sizeof(dest_full)) != 0) {
      continue;
    }

    {
      char parent[1024];
      const char *last_slash = strrchr(dest_full, '/');
      if (last_slash != NULL && last_slash != dest_full) {
        size_t plen = (size_t)(last_slash - dest_full);
        memcpy(parent, dest_full, plen);
        parent[plen] = '\0';
        if (mkdir_p(parent) != 0) {
          if (error_msg) snprintf(error_msg, error_msg_size,
                                  "Failed to create directory: %s", parent);
          result = -1;
          break;
        }
      }
    }

    int out_fd = open(dest_full, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (out_fd < 0) {
      if (error_msg) snprintf(error_msg, error_msg_size,
                              "Cannot create %s: %s", dest_full,
                              strerror(errno));
      result = -1;
      break;
    }

    unzip_write_ctx_t write_ctx;
    write_ctx.wc.fd = out_fd;
    write_ctx.wc.buf_pos = 0U;
    write_ctx.cancelled = cancelled;

    if (!mz_zip_reader_extract_to_callback(&zip, i, unzip_miniz_write,
                                           &write_ctx, 0) ||
        write_flush(&write_ctx.wc) != 0) {
      const char *mz_err = mz_zip_get_error_string(mz_zip_get_last_error(&zip));
      if (error_msg && cancelled && *cancelled) {
        snprintf(error_msg, error_msg_size, "Cancelled");
      } else if (error_msg) {
        snprintf(error_msg, error_msg_size, "Failed to extract %s: %s",
                 safe_name, mz_err ? mz_err : strerror(errno));
      }
      close(out_fd);
      (void)unlink(dest_full);
      result = -1;
      break;
    }

    close(out_fd);
  }

  (void)mz_zip_reader_end(&zip);
  close(zip_fd);
  return result;
}
