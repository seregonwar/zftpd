/*
 * pal_tls.h — TLS session abstraction (mbedTLS backend)
 *
 * Thin opaque layer used by pal_curl for HTTPS.
 * Wraps mbedTLS state; exposes send/recv primitives that match pal_curl's I/O model.
 *
 * Certificate verification is DISABLED by default (no system CA store on PS4/PS5).
 * Enable it by supplying a PEM CA bundle in pal_tls_cfg_t.ca_chain_pem.
 *
 * ── mbedTLS version ──────────────────────────────────────────────────────
 * Targets mbedTLS 3.x.  For mbedTLS 2.x define PAL_MBEDTLS_2X before including.
 *
 * ── Thread-safety ────────────────────────────────────────────────────────
 * NOT thread-safe.  Call pal_tls_global_init() once from the main thread
 * before spawning threads that use TLS.
 */

#ifndef PAL_TLS_H
#define PAL_TLS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Error codes ─────────────────────────────────────────────────────── */
#define PAL_TLS_OK             0
#define PAL_TLS_ERR_PARAM     -1
#define PAL_TLS_ERR_NOMEM     -2
#define PAL_TLS_ERR_HANDSHAKE -3
#define PAL_TLS_ERR_SEND      -4
#define PAL_TLS_ERR_RECV      -5
#define PAL_TLS_ERR_CERT      -6
#define PAL_TLS_ERR_INIT      -7

/* ── Configuration ───────────────────────────────────────────────────── */
typedef struct {
    const char *hostname;     /**< SNI + cert CN; must not be NULL.           */
    const char *ca_chain_pem; /**< PEM CA bundle; NULL disables verification. */
    int         verify_peer;  /**< Non-zero to require cert validation.       */
} pal_tls_cfg_t;

/* ── Opaque session ──────────────────────────────────────────────────── */
typedef struct pal_tls_session pal_tls_session_t;

/* ── Global lifecycle (one call per process) ─────────────────────────── */

/**
 * @brief Seed the global CSPRNG used by all TLS sessions.
 *
 * Must complete before the first pal_tls_connect().  Idempotent.
 *
 * @return PAL_TLS_OK or PAL_TLS_ERR_INIT.
 *
 * @note Thread-safety: NOT thread-safe; call before spawning TLS threads.
 */
int  pal_tls_global_init(void);

/** Counterpart to pal_tls_global_init(). */
void pal_tls_global_cleanup(void);

/* ── Session lifecycle ───────────────────────────────────────────────── */

/**
 * @brief Perform a TLS client handshake on an existing connected TCP socket.
 *
 * Caller retains ownership of fd; does NOT close it on failure.
 *
 * @param[in] fd   Connected, blocking TCP socket.
 * @param[in] cfg  Non-NULL config; cfg->hostname must not be NULL.
 *
 * @return New session on success, NULL on any failure.
 *
 * @pre  fd is connected and blocking.
 * @post On success the session is ready for pal_tls_send/recv.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: unbounded (network).
 */
pal_tls_session_t *pal_tls_connect(int fd, const pal_tls_cfg_t *cfg);

/**
 * @brief Send exactly len bytes through a TLS session.
 *
 * Retries on MBEDTLS_ERR_SSL_WANT_WRITE.
 *
 * @return Bytes written (> 0), or negative PAL_TLS_ERR_*.
 *
 * @note Thread-safety: NOT thread-safe.
 */
int pal_tls_send(pal_tls_session_t *sess, const void *buf, size_t len);

/**
 * @brief Receive up to len bytes from a TLS session.
 *
 * Returns 0 on clean peer closure.
 *
 * @note Thread-safety: NOT thread-safe.
 */
int pal_tls_recv(pal_tls_session_t *sess, void *buf, size_t len);

/**
 * @brief Send close_notify and free all session resources.
 *
 * Does NOT close the underlying fd.  Safe on NULL.
 *
 * @note Thread-safety: NOT thread-safe.
 */
void pal_tls_close(pal_tls_session_t *sess);

#ifdef __cplusplus
}
#endif
#endif /* PAL_TLS_H */
