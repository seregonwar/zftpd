/*
 * GNU GPLv3 License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */
/* ═══════════════════════════════════════════════════════════════════════════
 * PAL CURL — Complete libcurl shim for PS4/PS5
 *
 * Drop-in replacement for the libcurl subset used by http_api.c.
 * Implemented on top of BSD sockets + HTTP/1.1.  No TLS (HTTPS is
 * rejected with CURLE_UNSUPPORTED_PROTOCOL; the console uses plain HTTP
 * for all local-network transfers).
 *
 * Supported API surface:
 *   curl_global_init / curl_global_cleanup   (no-ops; for source compat)
 *   curl_easy_init / curl_easy_cleanup
 *   curl_easy_reset
 *   curl_easy_setopt
 *   curl_easy_perform
 *   curl_easy_getinfo
 *   curl_easy_strerror
 *   curl_slist_append / curl_slist_free_all
 *   curl_version
 *
 * Supported methods : GET, POST, HEAD (CURLOPT_NOBODY)
 * Transfer encodings: Content-Length and chunked (HTTP/1.1 §4.1)
 * Redirect          : 3xx with Location, including relative URLs
 * Timeouts          : connect and transfer, via select(2)
 * Speed guard       : CURLOPT_LOW_SPEED_LIMIT / LOW_SPEED_TIME
 * Progress          : CURLOPT_XFERINFOFUNCTION
 *
 * ── Thread-safety ──────────────────────────────────────────────────────────
 * NO function in this unit is thread-safe.  Each CURL handle must be
 * used from a single thread at a time.
 *
 * ── Build guard ────────────────────────────────────────────────────────────
 * Compiled only when ENABLE_LIBCURL=1 && (PS4 || PS5).
 * On desktop the real libcurl is linked; this file is excluded.
 * ═══════════════════════════════════════════════════════════════════════════ */

#ifndef PAL_CURL_H
#define PAL_CURL_H

#include <stddef.h>   /* size_t */
#include <stdint.h>   /* int64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ═══════════════════════════════════════════════════════════════════════════
 * Scalar types
 * ═════════════════════════════════════════════════════════════════════════*/

/** Error / result code (matches real libcurl values for ABI compat). */
typedef int CURLcode;

/** Option identifier passed to curl_easy_setopt(). */
typedef int CURLoption;

/** Info identifier passed to curl_easy_getinfo(). */
typedef int CURLINFO;

/** Signed 64-bit file-size type (matches real libcurl's curl_off_t). */
typedef long long curl_off_t;

/** Opaque handle type (allocated by curl_easy_init). */
typedef void CURL;

/** Size of the error message buffer supplied via CURLOPT_ERRORBUFFER. */
#define CURL_ERROR_SIZE  256

/* ═══════════════════════════════════════════════════════════════════════════
 * Error codes  (values match real libcurl for source-level compatibility)
 * ═════════════════════════════════════════════════════════════════════════*/
#define CURLE_OK                    0
#define CURLE_UNSUPPORTED_PROTOCOL  1
#define CURLE_URL_MALFORMAT         3
#define CURLE_COULDNT_RESOLVE_HOST  6
#define CURLE_COULDNT_CONNECT       7
#define CURLE_PARTIAL_FILE          18
#define CURLE_HTTP_RETURNED_ERROR   22
#define CURLE_WRITE_ERROR           23
#define CURLE_OUT_OF_MEMORY         27
#define CURLE_OPERATION_TIMEDOUT    28
#define CURLE_ABORTED_BY_CALLBACK   42
#define CURLE_TOO_MANY_REDIRECTS    47
#define CURLE_UNKNOWN_OPTION        48
#define CURLE_GOT_NOTHING           52
#define CURLE_SEND_ERROR            55
#define CURLE_RECV_ERROR            56

/* ═══════════════════════════════════════════════════════════════════════════
 * curl_slist — linked list of strings (for CURLOPT_HTTPHEADER etc.)
 * ═════════════════════════════════════════════════════════════════════════*/

typedef struct curl_slist {
    char              *data;
    struct curl_slist *next;
} curl_slist;

/* ═══════════════════════════════════════════════════════════════════════════
 * Callback signatures
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * Write callback.  Called by curl_easy_perform() for each received data
 * chunk.  Must return nmemb on success.  Returning CURL_WRITEFUNC_PAUSE
 * requests a brief pause (implementation re-invokes after a short delay;
 * up to PAL_PAUSE_MAX_RETRIES times, then aborts with
 * CURLE_ABORTED_BY_CALLBACK).
 */
typedef size_t (*curl_write_callback)(void *ptr, size_t size, size_t nmemb,
                                      void *userdata);

#define CURL_WRITEFUNC_PAUSE  0x10000001U

/**
 * Transfer-info / progress callback.  Called periodically during body
 * download if CURLOPT_NOPROGRESS is 0 (the default).
 * Return 0 to continue; any other value aborts with
 * CURLE_ABORTED_BY_CALLBACK.
 */
typedef int (*curl_xferinfo_callback)(void       *clientp,
                                      curl_off_t  dltotal,
                                      curl_off_t  dlnow,
                                      curl_off_t  ultotal,
                                      curl_off_t  ulnow);

/* ═══════════════════════════════════════════════════════════════════════════
 * CURLoption IDs  (values match real libcurl)
 * ═════════════════════════════════════════════════════════════════════════*/

/*
 * OBJECTPOINT options   (base 10000 in libcurl)
 *   CURLOPTTYPE_OBJECTPOINT = 10000
 */
#define CURLOPT_WRITEDATA           10001   /* void *  */
#define CURLOPT_URL                 10002   /* const char * */
#define CURLOPT_RANGE               10007   /* const char * e.g. "0-4095" */
#define CURLOPT_ERRORBUFFER         10010   /* char[CURL_ERROR_SIZE] */
#define CURLOPT_POSTFIELDS          10015   /* const char * (not copied) */
#define CURLOPT_USERAGENT           10018   /* const char * */
#define CURLOPT_HTTPHEADER          10023   /* curl_slist * */
#define CURLOPT_XFERINFODATA        10057   /* void * */

/*
 * LONG options   (base 0 in libcurl)
 */
#define CURLOPT_VERBOSE             41      /* long bool */
#define CURLOPT_NOPROGRESS          43      /* long bool (default 1) */
#define CURLOPT_NOBODY              44      /* long bool — HEAD request */
#define CURLOPT_POST                47      /* long bool */
#define CURLOPT_FOLLOWLOCATION      52      /* long bool */
#define CURLOPT_TIMEOUT             13      /* long seconds */
#define CURLOPT_LOW_SPEED_LIMIT     19      /* long bytes/s */
#define CURLOPT_LOW_SPEED_TIME      20      /* long seconds */
#define CURLOPT_SSL_VERIFYPEER      64      /* long — accepted, ignored */
#define CURLOPT_MAXREDIRS           68      /* long (-1 = unlimited) */
#define CURLOPT_POSTFIELDSIZE       60      /* long (-1 = use strlen) */
#define CURLOPT_CONNECTTIMEOUT      78      /* long seconds */
#define CURLOPT_TIMEOUT_MS          155     /* long milliseconds */
#define CURLOPT_CONNECTTIMEOUT_MS   156     /* long milliseconds */

/*
 * FUNCTIONPOINT options   (base 20000 in libcurl)
 */
#define CURLOPT_WRITEFUNCTION       20011   /* curl_write_callback */
#define CURLOPT_XFERINFOFUNCTION    20219   /* curl_xferinfo_callback */

/* ═══════════════════════════════════════════════════════════════════════════
 * CURLINFO IDs  (values match real libcurl)
 * ═════════════════════════════════════════════════════════════════════════*/

/*
 * LONG info   (base 0x200000)
 */
#define CURLINFO_RESPONSE_CODE              0x200002  /* long */

/*
 * DOUBLE info   (base 0x300000)
 */
#define CURLINFO_TOTAL_TIME                 0x300003  /* double (seconds) */
#define CURLINFO_SIZE_DOWNLOAD              0x300008  /* double (bytes) */
#define CURLINFO_SPEED_DOWNLOAD             0x30000B  /* double (bytes/s) */
#define CURLINFO_CONTENT_LENGTH_DOWNLOAD    0x30000F  /* double (-1 if unknown) */

/* ═══════════════════════════════════════════════════════════════════════════
 * API
 * ═════════════════════════════════════════════════════════════════════════*/

/**
 * @brief No-op stub for source-level compatibility with libcurl callers.
 *        Must be called before any other curl_* function (libcurl requires
 *        this; our implementation has nothing to initialise globally).
 *
 * @param flags  Ignored.
 * @return Always CURLE_OK.
 *
 * @note Thread-safety: safe to call from any thread.
 * @note WCET: O(1).
 */
CURLcode curl_global_init(long flags);

/**
 * @brief No-op stub counterpart to curl_global_init().
 *
 * @note Thread-safety: safe to call from any thread.
 * @note WCET: O(1).
 */
void curl_global_cleanup(void);

/**
 * @brief Allocate and initialise a new easy handle.
 *
 * Default values after init:
 *   max_redirs       = 10
 *   connect_timeout  = 30 s
 *   noprogress       = 1 (progress callback disabled)
 *
 * @return New handle on success, NULL on allocation failure.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1), one calloc.
 */
CURL *curl_easy_init(void);

/**
 * @brief Reset all options on an existing handle to their defaults.
 *
 * The handle remains valid and may be re-used.  The file handle and
 * response info fields are also cleared.
 *
 * @param handle  Non-NULL easy handle.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1).
 */
void curl_easy_reset(CURL *handle);

/**
 * @brief Release all resources owned by handle and free it.
 *
 * After this call the handle pointer is invalid.  Idempotent on NULL.
 *
 * @param handle  Easy handle or NULL (ignored).
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1).
 */
void curl_easy_cleanup(CURL *handle);

/**
 * @brief Set an option on a handle.
 *
 * The third argument type depends on the option:
 *   OBJECTPOINT  options expect  const char * or void * or curl_slist *
 *   FUNCTIONPOINT options expect a function pointer
 *   LONG         options expect  long
 *
 * @param handle  Non-NULL easy handle.
 * @param option  CURLOPT_* constant.
 * @param ...     Option value (type depends on option).
 *
 * @return CURLE_OK on success.
 * @retval CURLE_UNKNOWN_OPTION  handle is NULL or option is unrecognised.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1) for all options except CURLOPT_HTTPHEADER (no copy made).
 */
CURLcode curl_easy_setopt(CURL *handle, CURLoption option, ...);

/**
 * @brief Perform the transfer described by the handle's options.
 *
 * Executes: DNS resolve → TCP connect → HTTP request → response stream.
 * Redirects (3xx) are followed up to max_redirs times if
 * CURLOPT_FOLLOWLOCATION is set.
 *
 * Response info fields (CURLINFO_*) are populated on return, regardless
 * of success or failure.
 *
 * @param handle  Fully configured easy handle.
 *
 * @return CURLE_OK on a completed transfer (HTTP status < 400).
 *         Other CURLcode values on error.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: Unbounded (network I/O).
 * @warning Must not be called from an interrupt context.
 */
CURLcode curl_easy_perform(CURL *handle);

/**
 * @brief Retrieve information about the last completed transfer.
 *
 * @param handle  Easy handle (must have completed at least one perform).
 * @param info    CURLINFO_* constant.
 * @param ...     Pointer to receive the value (type depends on info).
 *
 * @return CURLE_OK on success.
 * @retval CURLE_UNKNOWN_OPTION  handle is NULL or info is unrecognised.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(1).
 */
CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...);

/**
 * @brief Map a CURLcode to a human-readable English string.
 *
 * The returned pointer is to a string literal; do not free or modify it.
 *
 * @param code  Any CURLcode value.
 * @return NUL-terminated string; never NULL.
 *
 * @note Thread-safety: safe (returns a pointer to a constant literal).
 * @note WCET: O(1).
 */
const char *curl_easy_strerror(CURLcode code);

/**
 * @brief Return a brief version identifier string.
 *
 * @return Pointer to a string literal. Never NULL.
 *
 * @note Thread-safety: safe.
 * @note WCET: O(1).
 */
const char *curl_version(void);

/**
 * @brief Append a copy of data to a slist, or create a new slist.
 *
 * If allocation fails the original list is returned unchanged (the caller
 * should check whether the list grew by comparing pointers or list length).
 *
 * @param list  Existing slist or NULL to create a new one.
 * @param data  NUL-terminated string to append; must not be NULL.
 *
 * @return Updated (or newly created) slist head, or the original list on
 *         allocation failure.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(n) for traversal to the tail.
 */
curl_slist *curl_slist_append(curl_slist *list, const char *data);

/**
 * @brief Free all nodes in a slist and their string data.
 *
 * Safe to call with NULL. The caller must not use the list after this call.
 *
 * @param list  Head of the slist, or NULL.
 *
 * @note Thread-safety: NOT thread-safe.
 * @note WCET: O(n).
 */
void curl_slist_free_all(curl_slist *list);

#ifdef __cplusplus
}
#endif

#endif /* PAL_CURL_H */