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
 * @file ftp_crypto.c
 * @brief ChaCha20 stream cipher — pure C, no external dependencies
 *
 * @author SeregonWar
 * @version 1.0.0
 * @date 2026-02-19
 *
 * REFERENCE: RFC 7539 — ChaCha20 and Poly1305 for IETF Protocols
 *
 * PERFORMANCE NOTES:
 *   The inner loop is 20 rounds of ARX (Add-Rotate-XOR) on 32-bit words.
 *   Modern compilers vectorize this well. Typical throughput:
 *     x86-64 (Zen2/Intel):  ~3 GB/s
 *     ARM (Cortex-A76):     ~1 GB/s
 *   This far exceeds gigabit Ethernet (~125 MB/s), so encryption
 *   adds effectively zero overhead to FTP transfers.
 */

#include "ftp_crypto.h"

#if FTP_ENABLE_CRYPTO

#include <string.h>

/*===========================================================================*
 * ChaCha20 CORE
 *
 *  The ChaCha20 state is a 4x4 matrix of 32-bit words:
 *
 *     ┌──────────┬──────────┬──────────┬──────────┐
 *     │ "expa"   │ "nd 3"   │ "2-by"   │ "te k"   │  Constants
 *     ├──────────┼──────────┼──────────┼──────────┤
 *     │ key[0]   │ key[1]   │ key[2]   │ key[3]   │  Key (256-bit)
 *     ├──────────┼──────────┼──────────┼──────────┤
 *     │ key[4]   │ key[5]   │ key[6]   │ key[7]   │
 *     ├──────────┼──────────┼──────────┼──────────┤
 *     │ counter  │ nonce[0] │ nonce[1] │ nonce[2] │  Counter + Nonce
 *     └──────────┴──────────┴──────────┴──────────┘
 *
 *===========================================================================*/

/**
 * Rotate left 32-bit (compiler intrinsic on most platforms)
 */
static inline uint32_t rotl32(uint32_t v, unsigned int n) {
  return (v << n) | (v >> (32U - n));
}

/**
 * ChaCha20 quarter round — the fundamental ARX operation
 *
 *   a += b;  d ^= a;  d <<<= 16;
 *   c += d;  b ^= c;  b <<<= 12;
 *   a += b;  d ^= a;  d <<<= 8;
 *   c += d;  b ^= c;  b <<<= 7;
 */
static inline void quarter_round(uint32_t *a, uint32_t *b, uint32_t *c,
                                 uint32_t *d) {
  *a += *b;
  *d ^= *a;
  *d = rotl32(*d, 16U);
  *c += *d;
  *b ^= *c;
  *b = rotl32(*b, 12U);
  *a += *b;
  *d ^= *a;
  *d = rotl32(*d, 8U);
  *c += *d;
  *b ^= *c;
  *b = rotl32(*b, 7U);
}

/**
 * Load 32-bit little-endian word from byte array
 */
static inline uint32_t load32_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8U) | ((uint32_t)p[2] << 16U) |
         ((uint32_t)p[3] << 24U);
}

/**
 * Store 32-bit little-endian word to byte array
 */
static inline void store32_le(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v);
  p[1] = (uint8_t)(v >> 8U);
  p[2] = (uint8_t)(v >> 16U);
  p[3] = (uint8_t)(v >> 24U);
}

/**
 * Generate one 64-byte ChaCha20 keystream block
 *
 * Performs 20 rounds (10 column rounds + 10 diagonal rounds),
 * then adds the original state back (prevents state recovery).
 */
static void chacha20_block(const uint32_t state[16], uint8_t out[64]) {
  uint32_t x[16];
  memcpy(x, state, sizeof(x));

  /* 20 rounds = 10 double-rounds */
  for (unsigned int i = 0U; i < 10U; i++) {
    /* Column rounds */
    quarter_round(&x[0], &x[4], &x[8], &x[12]);
    quarter_round(&x[1], &x[5], &x[9], &x[13]);
    quarter_round(&x[2], &x[6], &x[10], &x[14]);
    quarter_round(&x[3], &x[7], &x[11], &x[15]);
    /* Diagonal rounds */
    quarter_round(&x[0], &x[5], &x[10], &x[15]);
    quarter_round(&x[1], &x[6], &x[11], &x[12]);
    quarter_round(&x[2], &x[7], &x[8], &x[13]);
    quarter_round(&x[3], &x[4], &x[9], &x[14]);
  }

  /* Add original state (prevents inverting the permutation) */
  for (unsigned int i = 0U; i < 16U; i++) {
    x[i] += state[i];
  }

  /* Serialize to little-endian bytes */
  for (unsigned int i = 0U; i < 16U; i++) {
    store32_le(&out[i * 4U], x[i]);
  }
}

/*===========================================================================*
 * PUBLIC API
 *===========================================================================*/

/* ChaCha20 magic constant: "expand 32-byte k" in little-endian */
static const uint32_t SIGMA[4] = {
    0x61707865U, /* "expa" */
    0x3320646EU, /* "nd 3" */
    0x79622D32U, /* "2-by" */
    0x6B206574U  /* "te k" */
};

void ftp_crypto_init(ftp_crypto_ctx_t *ctx, const uint8_t key[32],
                     const uint8_t nonce[12]) {
  if ((ctx == NULL) || (key == NULL) || (nonce == NULL)) {
    return;
  }

  memset(ctx, 0, sizeof(*ctx));

  /* Row 0: constants */
  ctx->state[0] = SIGMA[0];
  ctx->state[1] = SIGMA[1];
  ctx->state[2] = SIGMA[2];
  ctx->state[3] = SIGMA[3];

  /* Row 1-2: 256-bit key (8 x 32-bit words) */
  for (unsigned int i = 0U; i < 8U; i++) {
    ctx->state[4U + i] = load32_le(&key[i * 4U]);
  }

  /* Row 3: counter(0) + 96-bit nonce */
  ctx->state[12] = 0U; /* block counter starts at 0 */
  ctx->state[13] = load32_le(&nonce[0]);
  ctx->state[14] = load32_le(&nonce[4]);
  ctx->state[15] = load32_le(&nonce[8]);

  ctx->counter = 0U;
  ctx->ks_offset = 64U; /* Force first block generation on next xor */
  ctx->active = 1U;
}

void ftp_crypto_xor(ftp_crypto_ctx_t *ctx, void *data, size_t len) {
  if ((ctx == NULL) || (data == NULL) || (len == 0U)) {
    return;
  }

  uint8_t *p = (uint8_t *)data;
  size_t remaining = len;

  while (remaining > 0U) {
    /* Generate new keystream block if current one is exhausted */
    if (ctx->ks_offset >= 64U) {
      ctx->state[12] = ctx->counter;
      chacha20_block(ctx->state, ctx->keystream);
      ctx->counter++;
      ctx->ks_offset = 0U;
    }

    /* XOR available keystream bytes with data */
    size_t avail = 64U - (size_t)ctx->ks_offset;
    size_t chunk = (remaining < avail) ? remaining : avail;

    for (size_t i = 0U; i < chunk; i++) {
      p[i] ^= ctx->keystream[ctx->ks_offset + (uint8_t)i];
    }

    p += chunk;
    remaining -= chunk;
    ctx->ks_offset += (uint8_t)chunk;
  }
}

void ftp_crypto_reset(ftp_crypto_ctx_t *ctx) {
  if (ctx == NULL) {
    return;
  }

  /* Secure erase: volatile prevents compiler from optimizing away */
  volatile uint8_t *p = (volatile uint8_t *)ctx;
  for (size_t i = 0U; i < sizeof(*ctx); i++) {
    p[i] = 0U;
  }
}

void ftp_crypto_derive_key(const uint8_t psk[32], const uint8_t nonce[12],
                           uint8_t out_key[32]) {
  if ((psk == NULL) || (nonce == NULL) || (out_key == NULL)) {
    return;
  }

  /*
   * Key derivation: ChaCha20-based KDF
   *
   *   Use the PSK as a ChaCha20 key with nonce to generate
   *   64 bytes of keystream, then take the first 32 bytes
   *   as the derived session key.
   *
   *   This ensures each session gets a unique key even with
   *   the same PSK, because the nonce is random per session.
   *
   *   PSK ──┐
   *         ├──► ChaCha20(counter=0) ──► 64B keystream
   *  nonce ─┘                              │
   *                                    first 32B = session key
   */
  uint32_t kdf_state[16];

  kdf_state[0] = SIGMA[0];
  kdf_state[1] = SIGMA[1];
  kdf_state[2] = SIGMA[2];
  kdf_state[3] = SIGMA[3];

  for (unsigned int i = 0U; i < 8U; i++) {
    kdf_state[4U + i] = load32_le(&psk[i * 4U]);
  }

  kdf_state[12] = 0U;
  kdf_state[13] = load32_le(&nonce[0]);
  kdf_state[14] = load32_le(&nonce[4]);
  kdf_state[15] = load32_le(&nonce[8]);

  uint8_t block[64];
  chacha20_block(kdf_state, block);

  memcpy(out_key, block, 32U);

  /* Scrub temporary key material from stack */
  volatile uint8_t *vb = (volatile uint8_t *)block;
  for (size_t i = 0U; i < sizeof(block); i++) {
    vb[i] = 0U;
  }
  volatile uint32_t *vs = (volatile uint32_t *)kdf_state;
  for (size_t i = 0U; i < 16U; i++) {
    vs[i] = 0U;
  }
}

#endif /* FTP_ENABLE_CRYPTO */
