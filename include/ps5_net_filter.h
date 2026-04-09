/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps5_net_filter.h
 * @brief PS5 kernel-level outbound connection filter
 *
 * PROBLEM AD DRESSED
 * -----------------
 * Sony system daemons (ScePatchChecker, PFAuthClient, SceConsoleFeatureFlagChecker,
 * SceHidConfigService, SceWorkaroundCtl, RNPS Curl) enter aggressive retry loops
 * when DNS blocks their servers.  Each failed attempt still generates outgoing
 * packets, saturating the bandwidth available to FTP transfers by up to 40%.
 *
 * ROOT CAUSE
 * ----------
 * DNS-level blocking returns NXDOMAIN or timeout, which the daemons treat as a
 * *transient* network failure and retry with exponential back-off that saturates
 * the wire.  The kernel itself never learns that the connection should be rejected.
 *
 * SOLUTION
 * --------
 * Intercept connect(2) at the kernel sysent table level.  When a non-zftpd process
 * attempts to reach a destination outside the local subnet, return ENETUNREACH
 * immediately.  The daemon interprets this as a *local* error and either backs off
 * aggressively or stops retrying altogether — no packet is ever generated.
 *
 *
 * ARCHITECTURE OVERVIEW
 * ---------------------
 *
 *  ┌──────────────────────────────────────────────────────────────┐
 *  │                     Userland (zftpd)                         │
 *  │                                                              │
 *  │  ps5_net_filter_install()                                    │
 *  │    │                                                         │
 *  │    ├─ [1] Locate sysent[SYS_CONNECT] via firmware table     │
 *  │    ├─ [2] Save original sy_call pointer                     │
 *  │    ├─ [3] Copy hook_sys_connect() bytes → kernel RWX page   │
 *  │    └─ [4] kernel_copyin(&hook_addr, &sysent[98].sy_call, 8) │
 *  │                                                              │
 *  └──────────────────────────────────────────────────────────────┘
 *
 *  ┌──────────────────────────────────────────────────────────────┐
 *  │                  Kernel space (PS5 FreeBSD 11)               │
 *  │                                                              │
 *  │  ScePatchChecker calls connect(sock, &sony_addr, len)        │
 *  │    │                                                         │
 *  │    └─► sysent[98].sy_call → hook_sys_connect()              │
 *  │              │                                               │
 *  │              ├─ td->td_proc->p_pid == zftpd_pid?            │
 *  │              │     YES → original_connect()  (FTP traffic)  │
 *  │              │                                               │
 *  │              └─ destination in SONY_IP_RANGES?              │
 *  │                    YES → return ENETUNREACH  (no packet!)   │
 *  │                    NO  → original_connect()                 │
 *  │                                                              │
 *  └──────────────────────────────────────────────────────────────┘
 *
 *
 * USAGE
 * -----
 *   // After ps5_jailbreak():
 *   ps5_net_filter_config_t cfg = PS5_NET_FILTER_CONFIG_DEFAULT;
 *   int rc = ps5_net_filter_install(&cfg);
 *   if (rc != 0) { // handle: filter not installed, FTP still works }
 *
 *   // At shutdown:
 *   ps5_net_filter_uninstall();
 *
 *
 * SAFETY PROPERTIES
 * -----------------
 *   - The hook ALWAYS passes through connections from zftpd's own PID.
 *   - The hook ALWAYS passes through connections to RFC-1918 addresses
 *     (10.0.0.0/8, 172.16.0.0/12, 192.168.0.0/16) and loopback.
 *   - The blocklist is a fixed compile-time array; no dynamic allocation
 *     occurs in kernel context.
 *   - If the sysent offset table does not contain an entry for the running
 *     firmware version, install() returns an error and the kernel is NOT
 *     modified.
 *   - uninstall() is idempotent.
 *
 *
 * FIRMWARE SUPPORT
 * ----------------
 *   Supported firmware versions (sysent offsets validated):
 *     4.03, 7.00, 7.61, 8.20, 8.60, 9.00, 9.40, 9.60, 10.00, 10.01, 10.50
 *
 *   Unsupported firmware: install() returns PS5_NET_FILTER_ERR_FW_UNSUPPORTED.
 *
 *
 * THREAD SAFETY
 * -------------
 *   - install() and uninstall() must be called from the main thread only.
 *   - Must be called after ps5_jailbreak() and before ftp_server_start().
 *   - The installed kernel hook itself is fully re-entrant (no mutable globals
 *     in the kernel-context code path).
 *
 *
 * @note  This module is compiled ONLY when PLATFORM_PS5 is defined.
 *        On all other platforms the header provides no-op stubs.
 *
 * @warning Modifies kernel sysent table.  Always call uninstall() before
 *          the payload exits, otherwise the kernel will jump to freed memory.
 */

#ifndef PS5_NET_FILTER_H
#define PS5_NET_FILTER_H

/*===========================================================================*
 * COMPILE GUARD — PS5 only
 *===========================================================================*/

#ifdef PLATFORM_PS5

#include <stdint.h>
#include <stdatomic.h>
#include <netinet/in.h>

#ifdef __cplusplus
extern "C" {
#endif

/*===========================================================================*
 * CONSTANTS
 *===========================================================================*/

/**
 * Maximum number of IP subnet rules in the block list.
 *
 * RATIONALE: Fixed compile-time limit prevents any dynamic allocation
 * in the kernel-context hot path.  32 entries covers all known Sony
 * CDN and PSN IP ranges with room to spare.
 */
#define PS5_NET_FILTER_MAX_RULES  32U

/** Maximum length of a daemon name string (including NUL). */
#define PS5_NET_FILTER_MAX_NAME   24U

/*===========================================================================*
 * ERROR CODES
 *===========================================================================*/

typedef enum {
    PS5_NET_FILTER_OK                  =  0, /**< Success */
    PS5_NET_FILTER_ERR_INVALID_PARAM   = -1, /**< NULL pointer or bad argument */
    PS5_NET_FILTER_ERR_FW_UNSUPPORTED  = -2, /**< Firmware version not in table */
    PS5_NET_FILTER_ERR_ALREADY_ACTIVE  = -3, /**< install() called twice */
    PS5_NET_FILTER_ERR_NOT_INSTALLED   = -4, /**< uninstall() without install() */
    PS5_NET_FILTER_ERR_KMAP_FAILED     = -5, /**< Could not map kernel executable page */
    PS5_NET_FILTER_ERR_KWRITE_FAILED   = -6, /**< kernel_copyin() failed */
    PS5_NET_FILTER_ERR_FW_DETECT       = -7, /**< Could not read firmware version */
    PS5_NET_FILTER_ERR_SYSENT_INVALID  = -8, /**< Sysent sanity check failed */
    PS5_NET_FILTER_ERR_EXTERNAL_HOOK   = -9, /**< Another payload already hooked sysent */
} ps5_net_filter_err_t;

/*===========================================================================*
 * IP SUBNET RULE
 *===========================================================================*/

/**
 * An IPv4 subnet to block (network byte order).
 *
 * A connection is blocked when:
 *   (dest_addr & mask) == (network & mask)
 */
typedef struct {
    uint32_t network; /**< Network address (network byte order) */
    uint32_t mask;    /**< Subnet mask (network byte order)     */
} ps5_net_filter_rule_t;

/*===========================================================================*
 * STATISTICS
 *===========================================================================*/

/**
 * Runtime statistics collected by the kernel hook.
 *
 * @note All fields updated with atomic increments; safe to read at any time.
 */
typedef struct {
    uint64_t blocked_total;     /**< Total connections blocked */
    uint64_t allowed_self;      /**< Connections allowed: zftpd's own PID */
    uint64_t allowed_local;     /**< Connections allowed: RFC-1918 / loopback */
    uint64_t allowed_other;     /**< Connections allowed: not in block list */
    uint64_t hook_calls_total;  /**< Total hook invocations */
} ps5_net_filter_stats_t;

/*===========================================================================*
 * CONFIGURATION
 *===========================================================================*/

/**
 * Filter configuration passed to ps5_net_filter_install().
 *
 * DESIGN: The caller builds the rule table in userland before install().
 * The table is copied into the kernel-accessible hook page alongside the
 * hook code itself, avoiding any userland pointer dereferences in kernel
 * context.
 */
typedef struct {
    /**
     * Array of IP subnets to block.
     *
     * If rule_count == 0, the default Sony IP table is used automatically.
     * To disable all blocking (test/debug), set rule_count = 0 and
     * use_default_rules = false — install() will still succeed but the
     * hook will be a passthrough.
     */
    ps5_net_filter_rule_t rules[PS5_NET_FILTER_MAX_RULES];

    /** Number of valid entries in rules[]. Range [0, PS5_NET_FILTER_MAX_RULES]. */
    uint32_t rule_count;

    /**
     * If true and rule_count == 0, fill rules[] with the built-in Sony
     * CDN/PSN table before installing.  Default: true.
     */
    uint8_t use_default_rules;

    /**
     * Also hook sendto(2) in addition to connect(2).
     *
     * Sony's Curl-based daemons (RNPS) use sendto() on UDP sockets for
     * some telemetry paths.  Enabling this intercepts those as well.
     *
     * Default: true.
     */
    uint8_t hook_sendto;

    /** Reserved for future use. Must be zero-initialised. */
    uint8_t _reserved[2];

} ps5_net_filter_config_t;

/**
 * Default configuration initialiser.
 *
 * Usage:
 *   ps5_net_filter_config_t cfg = PS5_NET_FILTER_CONFIG_DEFAULT;
 *   ps5_net_filter_install(&cfg);
 */
#define PS5_NET_FILTER_CONFIG_DEFAULT \
    { .rules = {{0, 0}}, .rule_count = 0U, .use_default_rules = 1, .hook_sendto = 1, ._reserved = {0, 0} }

/*===========================================================================*
 * PUBLIC API
 *===========================================================================*/

/**
 * @brief Install the outbound connection filter into the PS5 kernel.
 *
 * Steps performed:
 *   1. Detect running firmware version.
 *   2. Look up sysent[SYS_CONNECT] offset for that firmware.
 *   3. Populate cfg->rules[] with default Sony IP table if requested.
 *   4. Allocate a kernel-executable page and copy the hook code + rule
 *      table into it.
 *   5. Atomically replace sysent[98].sy_call (and optionally sysent[133])
 *      with the address of the installed hook.
 *   6. Save our own PID so the hook can fast-path our FTP traffic.
 *
 * @param[in] cfg  Filter configuration. May be NULL to use all defaults.
 *
 * @return PS5_NET_FILTER_OK on success, negative error code on failure.
 *
 * @retval PS5_NET_FILTER_OK               Hook installed successfully.
 * @retval PS5_NET_FILTER_ERR_FW_UNSUPPORTED  Firmware not in support table.
 * @retval PS5_NET_FILTER_ERR_ALREADY_ACTIVE  Called twice without uninstall.
 * @retval PS5_NET_FILTER_ERR_KMAP_FAILED     Kernel page allocation failed.
 * @retval PS5_NET_FILTER_ERR_KWRITE_FAILED   kernel_copyin() failed.
 *
 * @pre  ps5_jailbreak() has been called successfully (kernel r/w access).
 * @pre  Must be called from the main thread.
 * @post On success, sysent[98].sy_call points to the installed hook.
 *
 * @note Thread-safety: NOT thread-safe. Call from main thread only.
 * @note The install is reversible: always call ps5_net_filter_uninstall()
 *       before the payload exits.
 * @note Failure is non-fatal: FTP server can start and operate normally;
 *       bandwidth saturation will continue to occur.
 */
int ps5_net_filter_install(const ps5_net_filter_config_t *cfg);

/**
 * @brief Uninstall the connection filter and restore the original kernel state.
 *
 * Restores the original sysent[98].sy_call (and sysent[133] if hooked).
 * Frees the kernel-executable page.
 *
 * @return PS5_NET_FILTER_OK on success, negative error code on failure.
 *
 * @retval PS5_NET_FILTER_OK            Hook removed, kernel restored.
 * @retval PS5_NET_FILTER_ERR_NOT_INSTALLED  install() was never called.
 * @retval PS5_NET_FILTER_ERR_KWRITE_FAILED  kernel_copyin() failed.
 *
 * @note Idempotent: safe to call even if install() failed.
 * @note Thread-safety: NOT thread-safe. Call from main thread only.
 * @note WCET: Two kernel_copyin() calls + one munmap(). Bounded.
 */
int ps5_net_filter_uninstall(void);

/**
 * @brief Check whether the filter is currently installed.
 *
 * @return 1 if installed and active, 0 otherwise.
 *
 * @note Thread-safety: Safe (atomic load).
 */
int ps5_net_filter_is_active(void);

/**
 * @brief Read runtime statistics from the filter.
 *
 * @param[out] out  Destination buffer for statistics snapshot.
 *
 * @return PS5_NET_FILTER_OK on success, PS5_NET_FILTER_ERR_INVALID_PARAM
 *         if out is NULL.
 *
 * @pre  out != NULL
 * @note Thread-safety: Safe (atomic reads, snapshot may be slightly stale).
 */
int ps5_net_filter_get_stats(ps5_net_filter_stats_t *out);

/**
 * @brief Retrieve a human-readable description of an error code.
 *
 * @param[in] err  Error code returned by any ps5_net_filter_* function.
 *
 * @return Static string describing the error. Never returns NULL.
 *
 * @note Thread-safety: Safe (read-only static data).
 */
const char *ps5_net_filter_strerror(int err);

#ifdef __cplusplus
}
#endif

/*===========================================================================*
 * STUB IMPLEMENTATIONS — non-PS5 platforms
 *===========================================================================*/

#else /* !PLATFORM_PS5 */

/*
 * Provide no-op stubs so callers can compile on POSIX without ifdefs.
 * The compiler will inline and eliminate these entirely at -O2.
 */
typedef struct { int _dummy; } ps5_net_filter_config_t;
typedef struct { int _dummy; } ps5_net_filter_stats_t;

#define PS5_NET_FILTER_CONFIG_DEFAULT { 0 }
#define PS5_NET_FILTER_OK 0

static inline int ps5_net_filter_install(const ps5_net_filter_config_t *cfg)
    { (void)cfg; return 0; }
static inline int ps5_net_filter_uninstall(void) { return 0; }
static inline int ps5_net_filter_is_active(void) { return 0; }
static inline int ps5_net_filter_get_stats(ps5_net_filter_stats_t *s)
    { (void)s; return 0; }
static inline const char *ps5_net_filter_strerror(int e)
    { (void)e; return "not supported on this platform"; }

#endif /* PLATFORM_PS5 */

#endif /* PS5_NET_FILTER_H */
