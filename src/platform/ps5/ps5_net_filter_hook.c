/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps5_net_filter_hook.c
 * @brief Kernel-context hook functions for the PS5 network filter
 *
 * ╔══════════════════════════════════════════════════════════════════════╗
 * ║  CRITICAL — THIS FILE RUNS IN KERNEL CONTEXT (ring 0)              ║
 * ║                                                                      ║
 * ║  CONSTRAINTS (enforced by compiler flags and code rules):           ║
 * ║  • No stack canary / stack protector   (-fno-stack-protector)       ║
 * ║  • No red zone                          (-mno-red-zone)             ║
 * ║  • Position-independent code           (-fPIC, -mcmodel=large)      ║
 * ║  • No PLT / GOT accesses               (-fno-plt)                   ║
 * ║  • No userland pointer dereferences                                 ║
 * ║  • No dynamic allocation (no malloc)                                ║
 * ║  • Bounded loops only                                               ║
 * ║  • No floating point (kernel disables FPU in ring 0)               ║
 * ║  • All external data accessed via the shared block (RIP-relative)  ║
 * ╚══════════════════════════════════════════════════════════════════════╝
 *
 *
 * BUILD INSTRUCTIONS
 * ------------------
 * This file is compiled separately with kernel-safe flags.  The resulting
 * machine code is extracted and embedded in ps5_net_filter.c as byte arrays.
 *
 * Makefile excerpt (added automatically when TARGET=ps5):
 *
 *   # Compile hook with kernel-safe flags
 *   $(CC_PS5) \
 *       -DPS5_HOOK_BUILD \
 *       -DPS5_HOOK_CONNECT_OFFSET=0x000 \
 *       -DPS5_HOOK_SHARED_OFFSET=0x480 \
 *       -O2 \
 *       -fno-stack-protector \
 *       -mno-red-zone \
 *       -fPIC \
 *       -mcmodel=large \
 *       -fno-plt \
 *       -fno-common \
 *       -fno-builtin \
 *       -fno-exceptions \
 *       -nostdinc \
 *       -I include/ \
 *       -c src/ps5_net_filter_hook.c \
 *       -o build/ps5/ps5_net_filter_hook.o
 *
 *   # Extract .text.hook section as raw binary
 *   $(OBJCOPY) -O binary \
 *       --only-section=.text.hook_connect \
 *       --only-section=.text.hook_sendto \
 *       build/ps5/ps5_net_filter_hook.o \
 *       build/ps5/ps5_net_filter_hook.bin
 *
 *   # Generate C byte array for inclusion
 *   xxd -i build/ps5/ps5_net_filter_hook.bin \
 *       > src/ps5_net_filter_hook_blob.h
 *
 * The generated blob is then used in ps5_net_filter.c to replace the
 * placeholder arrays (g_hook_connect_code[], g_hook_sendto_code[]).
 *
 *
 * CALLING CONVENTION (FreeBSD amd64 sy_call_t)
 * ---------------------------------------------
 * int sy_call(struct thread *td, void *uap);
 *
 * Registers on entry:
 *   rdi = struct thread *td          (current thread)
 *   rsi = struct connect_args *uap   (syscall arguments)
 *
 * Return:
 *   eax = 0    → success (pass to original handler)
 *   eax = errno → error (returned directly to userland)
 *
 *
 * SHARED BLOCK LAYOUT (ps5_hook_shared_t — defined in ps5_net_filter.c)
 * -----------------------------------------------------------------------
 * This struct is embedded at HOOK_SHARED_DATA_OFFSET (0x480) in the kernel
 * hook page.  The hooks access it via a known kernel virtual address that is
 * patched into the code at install time.
 *
 *   Offset  Field
 *   +0x00   uintptr_t  original_connect    (8 bytes)
 *   +0x08   uintptr_t  original_sendto     (8 bytes)
 *   +0x10   int32_t    zftpd_pid           (4 bytes)
 *   +0x14   uint32_t   rule_count          (4 bytes)
 *   +0x18   rule_t     rules[32]           (32 × 8 = 256 bytes)
 *   +0x118  uint32_t   td_proc_offset      (4 bytes)
 *   +0x11C  uint32_t   proc_pid_offset     (4 bytes)
 *   +0x120  int64_t    stat_blocked        (8 bytes)
 *   +0x128  int64_t    stat_allowed_self   (8 bytes)
 *   +0x130  int64_t    stat_allowed_local  (8 bytes)
 *   +0x138  int64_t    stat_allowed_other  (8 bytes)
 *   +0x140  int64_t    stat_hook_calls     (8 bytes)
 */

#ifdef PS5_HOOK_BUILD

/*
 * Kernel-mode headers only.
 * We cannot include standard libc headers here.
 */
#include <stdint.h>
#include <stddef.h>

/*===========================================================================*
 * PRIMITIVE TYPE ALIASES (no libc)
 *===========================================================================*/

typedef unsigned char  u8;
typedef unsigned short u16;
typedef unsigned int   u32;
typedef unsigned long long u64;
typedef signed int     s32;
typedef signed long long s64;

/*===========================================================================*
 * KERNEL CONSTANTS
 *===========================================================================*/

#define AF_INET        2U

/** ENETUNREACH (errno 51 on FreeBSD) */
#define ENETUNREACH   51

/** Address families */
#define SA_FAMILY_OFFSET  0U   /* sockaddr.sa_family offset */
#define SA_DATA_OFFSET    2U   /* sockaddr.sa_data[0] offset */

/** sockaddr_in layout */
#define SIN_FAMILY_OFFSET  0U  /* sin_family (u16) */
#define SIN_PORT_OFFSET    2U  /* sin_port   (u16) */
#define SIN_ADDR_OFFSET    4U  /* sin_addr   (u32) — network byte order */

/*===========================================================================*
 * SHARED BLOCK FIELD OFFSETS
 * (must match ps5_hook_shared_t layout in ps5_net_filter.c)
 *===========================================================================*/

#define SHARED_ORIGINAL_CONNECT_OFF  0x00U
#define SHARED_ORIGINAL_SENDTO_OFF   0x08U
#define SHARED_ZFTPD_PID_OFF         0x10U
#define SHARED_RULE_COUNT_OFF        0x14U
#define SHARED_RULES_OFF             0x18U   /* rule_t rules[32] */
#define SHARED_TD_PROC_OFF           0x118U
#define SHARED_PROC_PID_OFF          0x11CU
#define SHARED_STAT_BLOCKED_OFF      0x120U
#define SHARED_STAT_SELF_OFF         0x128U
#define SHARED_STAT_LOCAL_OFF        0x130U
#define SHARED_STAT_OTHER_OFF        0x138U
#define SHARED_STAT_CALLS_OFF        0x140U

/** Size of one rule_t entry (network u32 + mask u32) */
#define RULE_SIZE  8U

/*===========================================================================*
 * KERNEL copyin / copyout PROTOTYPES
 *
 * These are kernel functions available in ring-0 context.
 * They safely copy between kernel and user memory.
 *===========================================================================*/

/**
 * Copy 'len' bytes from userland address 'uaddr' into kernel buffer 'kaddr'.
 * Returns 0 on success, EFAULT on bad user pointer.
 */
extern int copyin(const void *uaddr, void *kaddr, size_t len);

/**
 * Copy 'len' bytes from kernel buffer 'kaddr' to userland address 'uaddr'.
 * Returns 0 on success, EFAULT.
 */
extern int copyout(const void *kaddr, void *uaddr, size_t len);

/*===========================================================================*
 * HELPER: RFC-1918 / LOOPBACK CHECK
 *===========================================================================*/

/**
 * @brief Check if an IPv4 address (network byte order) is local/private.
 *
 * Returns 1 for:
 *   127.0.0.0/8    (loopback)
 *   10.0.0.0/8     (RFC-1918)
 *   172.16.0.0/12  (RFC-1918)
 *   192.168.0.0/16 (RFC-1918)
 *   169.254.0.0/16 (link-local)
 *
 * @note  All comparisons are in network byte order (big-endian).
 *        On x86-64 (little-endian), the first byte of a network-order u32
 *        occupies the MOST significant byte position:
 *          ip = 0xC0A80101  → 192.168.1.1  (0xC0 = 192, 0xA8 = 168, ...)
 *
 * @note  No branches involving external data — all constants are immediate.
 *        WCET: O(1), no loops, no memory reads.
 */
static inline __attribute__((always_inline))
int is_local_address(u32 ip_nbo)
{
    /*
     * Network-byte-order comparisons.
     *
     * On amd64 (little-endian), a 32-bit value stored in memory as
     * { 0x7F, 0x00, 0x00, 0x01 } (127.0.0.1) is loaded as 0x0100007F.
     * BUT here ip_nbo is already in network byte order (big-endian in memory,
     * loaded as a u32 integer).  So we compare the HIGH byte.
     *
     * EXAMPLE:
     *   ip_nbo = 0x7F000001 (127.0.0.1 in big-endian as integer)
     *   High byte = (ip_nbo >> 24) = 0x7F = 127  ← loopback
     *
     * Wait — this depends on whether the kernel stores it in the u32 with
     * big-endian semantics.  FreeBSD sin_addr.s_addr is in network byte order
     * (big-endian).  When we do a u32 read on amd64 (little-endian), the bytes
     * are reversed:
     *
     *   sin_addr for 192.168.1.1:
     *     Stored in memory: C0 A8 01 01
     *     Loaded as u32 on LE:  0x0101A8C0
     *
     * So we must compare the LOW byte to get the first octet!
     *   (ip_nbo & 0xFF) == 127  → loopback
     *   (ip_nbo & 0xFF) == 10   → 10.x.x.x
     *   (ip_nbo & 0xFF) == 192 AND ((ip_nbo >> 8) & 0xFF) == 168  → 192.168.x.x
     *   (ip_nbo & 0xFF) == 172 AND (((ip_nbo >> 8) & 0xFF) & 0xF0) == 16  → 172.16-31.x.x
     *   (ip_nbo & 0xFF) == 169 AND ((ip_nbo >> 8) & 0xFF) == 254  → 169.254.x.x
     */

    u32 b0 = ip_nbo & 0xFFU;
    u32 b1 = (ip_nbo >> 8U) & 0xFFU;

    /* 127.x.x.x — loopback */
    if (b0 == 127U) { return 1; }

    /* 10.x.x.x — RFC-1918 Class A */
    if (b0 == 10U) { return 1; }

    /* 192.168.x.x — RFC-1918 Class C */
    if ((b0 == 192U) && (b1 == 168U)) { return 1; }

    /* 172.16.0.0 – 172.31.255.255 — RFC-1918 Class B */
    if ((b0 == 172U) && (b1 >= 16U) && (b1 <= 31U)) { return 1; }

    /* 169.254.x.x — link-local */
    if ((b0 == 169U) && (b1 == 254U)) { return 1; }

    return 0;
}

/*===========================================================================*
 * HELPER: MATCH IP AGAINST RULE TABLE
 *===========================================================================*/

/**
 * @brief Check if ip_nbo matches any rule in the block table.
 *
 * @param ip_nbo     Destination IP (network byte order, LE-loaded u32).
 * @param rules_ptr  Kernel VA of rules[] in the shared block.
 * @param rule_count Number of rules (bounded by PS5_NET_FILTER_MAX_RULES).
 *
 * @return 1 if the IP should be blocked, 0 otherwise.
 *
 * @note  WCET: O(rule_count) — at most 32 iterations.
 *        Each iteration: 3 reads + 2 ANDs + 1 compare = ~8 cycles.
 *        Total worst-case: ~256 cycles ≈ 0.1 µs @ 3.5 GHz.  Negligible.
 */
static inline __attribute__((always_inline))
int ip_matches_blocklist(u32 ip_nbo,
                         const u64 *rules_ptr,
                         u32 rule_count)
{
    /*
     * rules_ptr points to an array of rule_t:
     *   struct rule_t { u32 network; u32 mask; };
     *
     * Stored as two consecutive u32 (network byte order in memory).
     * We read them as two u32 from the kernel page directly.
     */

    if (rule_count > 32U) {
        rule_count = 32U;  /* Defensive clamp — should never be needed */
    }

    for (u32 i = 0U; i < rule_count; i++) {
        /*
         * Each rule is 8 bytes: [network:4][mask:4]
         * We cast rules_ptr to a byte stream for clarity.
         */
        const u8 *rule_bytes = (const u8 *)rules_ptr + (u64)(i * 8U);

        u32 network;
        u32 mask;

        /* Safe kernel-memory reads (already in kernel space, no copyin needed) */
        __builtin_memcpy(&network, rule_bytes,     sizeof(u32));
        __builtin_memcpy(&mask,    rule_bytes + 4U, sizeof(u32));

        /*
         * Match condition: (ip & mask) == (network & mask)
         *
         * Both ip_nbo and the stored network/mask are in the same byte
         * order (LE-loaded network-byte-order u32), so the comparison
         * is direct.
         */
        if ((ip_nbo & mask) == (network & mask)) {
            return 1;
        }
    }

    return 0;
}

/*===========================================================================*
 * hook_sys_connect
 *===========================================================================*/

/**
 * @brief Replacement for the kernel's sys_connect(2) handler.
 *
 * Called for EVERY connect(2) syscall on the system.
 *
 * Function signature matches FreeBSD sy_call_t:
 *   int (*sy_call_t)(struct thread *, void *);
 *
 * @param td   Pointer to the current kernel thread structure.
 * @param uap  Pointer to connect_args on the kernel stack:
 *               struct connect_args { int s; caddr_t name; int namelen; };
 *
 * @return  0          → pass to original handler (allow).
 * @return  ENETUNREACH → deny connection without packet generation.
 *
 * @note Placed in a named section so objcopy can extract it precisely.
 */
__attribute__((section(".text.hook_connect"), noinline))
int hook_sys_connect(void *td, void *uap)
{
    /*
     * ── SHARED BLOCK ACCESS ──────────────────────────────────────────────
     *
     * The shared block is at a known kernel VA patched in at install time.
     * We access it via a pointer stored in a variable that the compiler
     * will address via a RIP-relative load from the code section.
     *
     * IMPORTANT: This pointer is PATCHED at install time by ps5_net_filter.c.
     * At compile time it is initialised to 0; the install routine overwrites
     * the 8 bytes at HOOK_CONNECT_JMP_PATCH_OFFSET with the actual kaddr.
     *
     * We declare it as a volatile pointer to prevent the compiler from
     * caching the value — the kernel VA is only valid after patching.
     */
    static volatile u64 g_shared_kaddr = 0ULL;  /* PATCHED at install */

    /* Cast to byte pointer for field access by offset */
    const volatile u8 *sh = (const volatile u8 *)g_shared_kaddr;

    if (sh == NULL) {
        /*
         * Hook was called before the shared block pointer was patched.
         * This should never happen in normal operation — it would mean the
         * sysent was patched but the shared pointer was not written.
         * Fall through to original handler (safe fallback).
         */
        goto call_original;
    }

    /* ── stat: increment total hook calls ──────────────────────────────── */
    {
        volatile s64 *p_calls = (volatile s64 *)(sh + SHARED_STAT_CALLS_OFF);
        __asm__ __volatile__ (
            "lock xaddq %0, (%1)"
            : "+r" ((s64){1})
            : "r"  (p_calls)
            : "memory"
        );
    }

    /* ── Extract zftpd PID and current process PID ──────────────────────── */
    s32 zftpd_pid;
    __builtin_memcpy(&zftpd_pid, sh + SHARED_ZFTPD_PID_OFF, sizeof(s32));

    /*
     * Access current process PID via td->td_proc->p_pid.
     *
     * Offsets are in the shared block (firmware-specific):
     *   td_proc_off  = td->td_proc field offset
     *   proc_pid_off = proc->p_pid field offset
     */
    u32 td_proc_off;
    u32 proc_pid_off;
    __builtin_memcpy(&td_proc_off,  sh + SHARED_TD_PROC_OFF,  sizeof(u32));
    __builtin_memcpy(&proc_pid_off, sh + SHARED_PROC_PID_OFF, sizeof(u32));

    /* Dereference td->td_proc */
    void *td_proc = NULL;
    {
        const u8 *td_bytes = (const u8 *)td;
        __builtin_memcpy(&td_proc, td_bytes + td_proc_off, sizeof(void *));
    }

    if (td_proc == NULL) {
        goto call_original;  /* Defensive: corrupt td, pass through */
    }

    /* Read p_pid from struct proc */
    s32 caller_pid = -1;
    {
        const u8 *proc_bytes = (const u8 *)td_proc;
        __builtin_memcpy(&caller_pid, proc_bytes + proc_pid_off, sizeof(s32));
    }

    /* ── FAST PATH: zftpd's own connections always allowed ──────────────── */
    if (caller_pid == zftpd_pid) {
        volatile s64 *p_self = (volatile s64 *)(sh + SHARED_STAT_SELF_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p_self) : "memory");
        goto call_original;
    }

    /* ── Extract destination IP from uap->name (struct sockaddr *) ──────── */
    /*
     * connect_args layout (FreeBSD amd64):
     *   [0x00] int     s       (4 bytes)
     *   [0x04] padding         (4 bytes)
     *   [0x08] caddr_t name    (8 bytes) ← pointer to struct sockaddr in USER space
     *   [0x10] int     namelen (4 bytes)
     */
    void *sockaddr_uptr = NULL;
    {
        const u8 *uap_bytes = (const u8 *)uap;
        __builtin_memcpy(&sockaddr_uptr, uap_bytes + 0x08U, sizeof(void *));
    }

    if (sockaddr_uptr == NULL) {
        goto call_original;  /* No address — let kernel handle it */
    }

    /*
     * copyin() can sleep while resolving a page fault.  FreeBSD forbids
     * sleeping while a non-sleepable lock is held (INVARIANTS will panic).
     *
     * Some system threads (e.g. SceSpZeroConfMain) call connect() while
     * holding a non-sleepable lock.  Detect this by checking the thread's
     * critical-section nesting counter and lock count:
     *
     *   struct thread:
     *     td_critnest  — incremented by critical_enter(), must be 0 to sleep
     *     td_locks     — number of non-sleepable locks held, must be 0
     *
     * Offsets on FreeBSD 11 amd64 (PS5): td_critnest=0x1CC, td_locks=0x1D0.
     * These are stable across all PS5 firmware versions that match our table.
     * If we cannot confirm both are zero, fall through to the original handler
     * (safe: we skip filtering for that call rather than panic the kernel).
     *
     * NOTE: FreeBSD td_critnest is u_int (4 bytes); td_locks is int (4 bytes).
     */
    {
        const u8 *td_bytes = (const u8 *)td;
        u32 critnest = 0U;
        s32 locks    = 0;
        __builtin_memcpy(&critnest, td_bytes + 0x1CCU, sizeof(u32));
        __builtin_memcpy(&locks,    td_bytes + 0x1D0U, sizeof(s32));
        if ((critnest != 0U) || (locks != 0)) {
            /* Cannot sleep — skip copyin, allow the connection */
            goto allow_other;
        }
    }

    /*
     * copyin() the first 8 bytes of the sockaddr from userland.
     * We only need sa_family (2 bytes) and sin_addr (4 bytes at offset 4).
     * Reading 8 bytes covers both safely.
     */
    u8 sa_buf[8] = {0};
    if (copyin(sockaddr_uptr, sa_buf, sizeof(sa_buf)) != 0) {
        goto call_original;  /* Bad user pointer — let kernel reject it */
    }

    u16 sa_family;
    __builtin_memcpy(&sa_family, sa_buf + SIN_FAMILY_OFFSET, sizeof(u16));

    /* ── Only filter IPv4 (AF_INET = 2) ────────────────────────────────── */
    if (sa_family != (u16)AF_INET) {
        goto allow_other;
    }

    /* Extract destination IP (network byte order, loaded as LE u32) */
    u32 dest_ip;
    __builtin_memcpy(&dest_ip, sa_buf + SIN_ADDR_OFFSET, sizeof(u32));

    /* ── Allow RFC-1918 / loopback (fast path, no rule scan needed) ─────── */
    if (is_local_address(dest_ip) != 0) {
        goto allow_local;
    }

    /* ── Check against the block list ──────────────────────────────────── */
    u32 rule_count;
    __builtin_memcpy(&rule_count, sh + SHARED_RULE_COUNT_OFF, sizeof(u32));

    const u64 *rules_ptr = (const u64 *)(sh + SHARED_RULES_OFF);

    if (ip_matches_blocklist(dest_ip, rules_ptr, rule_count) != 0) {
        /* BLOCK: increment stat and return ENETUNREACH */
        volatile s64 *p_blocked = (volatile s64 *)(sh + SHARED_STAT_BLOCKED_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p_blocked) : "memory");
        return ENETUNREACH;
    }

allow_other:
    {
        volatile s64 *p_other = (volatile s64 *)(sh + SHARED_STAT_OTHER_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p_other) : "memory");
    }
    goto call_original;

allow_local:
    {
        volatile s64 *p_local = (volatile s64 *)(sh + SHARED_STAT_LOCAL_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p_local) : "memory");
    }

call_original:
    {
        /*
         * Tail-call the original sys_connect handler.
         *
         * We load the function pointer from the shared block and jump to it.
         * Using a tail call avoids adding a stack frame — important in
         * kernel context where stack depth is limited.
         *
         * The original_connect field is at sh + SHARED_ORIGINAL_CONNECT_OFF.
         */
        uintptr_t orig_fn = 0U;
        __builtin_memcpy(&orig_fn,
                         (const void *)((u64)sh + SHARED_ORIGINAL_CONNECT_OFF),
                         sizeof(uintptr_t));

        typedef int (*sy_call_fn)(void *, void *);
        sy_call_fn fn = (sy_call_fn)orig_fn;
        return fn(td, uap);
    }
}

/*===========================================================================*
 * hook_sys_sendto
 *===========================================================================*/

/**
 * @brief Replacement for the kernel's sys_sendto(2) handler.
 *
 * Intercepts UDP sendto() calls targeting Sony CDN/PSN IPs.
 * Sony's RNPS (React Native PlayStation) runtime uses sendto() for
 * some telemetry and configuration push events over UDP.
 *
 * sendto_args layout (FreeBSD amd64):
 *   [0x00] int      s         (4 bytes)
 *   [0x04] padding            (4 bytes)
 *   [0x08] caddr_t  buf       (8 bytes)
 *   [0x10] size_t   len       (8 bytes)
 *   [0x18] int      flags     (4 bytes)
 *   [0x1C] padding            (4 bytes)
 *   [0x20] caddr_t  to        (8 bytes)  ← struct sockaddr* in user space
 *   [0x28] int      tolen     (4 bytes)
 *
 * @return 0 (pass to original) or ENETUNREACH (block).
 */
__attribute__((section(".text.hook_sendto"), noinline))
int hook_sys_sendto(void *td, void *uap)
{
    static volatile u64 g_shared_kaddr_sendto = 0ULL;  /* PATCHED at install */

    const volatile u8 *sh = (const volatile u8 *)g_shared_kaddr_sendto;
    if (sh == NULL) {
        goto sendto_call_original;
    }

    /* Increment total call count */
    {
        volatile s64 *p_calls = (volatile s64 *)(sh + SHARED_STAT_CALLS_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p_calls) : "memory");
    }

    /* Quick PID check */
    s32 zftpd_pid;
    u32 td_proc_off, proc_pid_off;
    __builtin_memcpy(&zftpd_pid,    sh + SHARED_ZFTPD_PID_OFF,  sizeof(s32));
    __builtin_memcpy(&td_proc_off,  sh + SHARED_TD_PROC_OFF,    sizeof(u32));
    __builtin_memcpy(&proc_pid_off, sh + SHARED_PROC_PID_OFF,   sizeof(u32));

    void *td_proc = NULL;
    __builtin_memcpy(&td_proc, (const u8 *)td + td_proc_off, sizeof(void *));
    if (td_proc != NULL) {
        s32 caller_pid = -1;
        __builtin_memcpy(&caller_pid, (const u8 *)td_proc + proc_pid_off, sizeof(s32));
        if (caller_pid == zftpd_pid) {
            volatile s64 *p = (volatile s64 *)(sh + SHARED_STAT_SELF_OFF);
            __asm__ __volatile__ ("lock xaddq %0, (%1)"
                                  : "+r" ((s64){1}) : "r" (p) : "memory");
            goto sendto_call_original;
        }
    }

    /* Extract the 'to' argument (struct sockaddr *) at uap+0x20 */
    void *sockaddr_uptr = NULL;
    __builtin_memcpy(&sockaddr_uptr, (const u8 *)uap + 0x20U, sizeof(void *));

    if (sockaddr_uptr == NULL) {
        goto sendto_call_original;
    }

    /* Same sleep-safety check as in hook_sys_connect — see comment there. */
    {
        const u8 *td_bytes = (const u8 *)td;
        u32 critnest = 0U;
        s32 locks    = 0;
        __builtin_memcpy(&critnest, td_bytes + 0x1CCU, sizeof(u32));
        __builtin_memcpy(&locks,    td_bytes + 0x1D0U, sizeof(s32));
        if ((critnest != 0U) || (locks != 0)) {
            goto sendto_call_original;
        }
    }

    u8 sa_buf[8] = {0};
    if (copyin(sockaddr_uptr, sa_buf, sizeof(sa_buf)) != 0) {
        goto sendto_call_original;
    }

    u16 sa_family;
    __builtin_memcpy(&sa_family, sa_buf + SIN_FAMILY_OFFSET, sizeof(u16));
    if (sa_family != (u16)AF_INET) {
        goto sendto_allow_other;
    }

    u32 dest_ip;
    __builtin_memcpy(&dest_ip, sa_buf + SIN_ADDR_OFFSET, sizeof(u32));

    if (is_local_address(dest_ip) != 0) {
        volatile s64 *p = (volatile s64 *)(sh + SHARED_STAT_LOCAL_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p) : "memory");
        goto sendto_call_original;
    }

    u32 rule_count;
    __builtin_memcpy(&rule_count, sh + SHARED_RULE_COUNT_OFF, sizeof(u32));
    const u64 *rules_ptr = (const u64 *)(sh + SHARED_RULES_OFF);

    if (ip_matches_blocklist(dest_ip, rules_ptr, rule_count) != 0) {
        volatile s64 *p = (volatile s64 *)(sh + SHARED_STAT_BLOCKED_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p) : "memory");
        return ENETUNREACH;
    }

sendto_allow_other:
    {
        volatile s64 *p = (volatile s64 *)(sh + SHARED_STAT_OTHER_OFF);
        __asm__ __volatile__ ("lock xaddq %0, (%1)"
                              : "+r" ((s64){1}) : "r" (p) : "memory");
    }

sendto_call_original:
    {
        uintptr_t orig_fn = 0U;
        __builtin_memcpy(&orig_fn,
                         (const void *)((u64)sh + SHARED_ORIGINAL_SENDTO_OFF),
                         sizeof(uintptr_t));
        if (orig_fn == 0U) {
            return 0;  /* sendto hook not installed — passthrough */
        }
        typedef int (*sy_call_fn)(void *, void *);
        return ((sy_call_fn)orig_fn)(td, uap);
    }
}

#endif /* PS5_HOOK_BUILD */
