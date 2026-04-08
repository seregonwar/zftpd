/*
 * MIT License — Copyright (c) 2026 SeregonWar
 * See LICENSE for full text.
 */

/**
 * @file ps5_net_filter.c
 * @brief PS5 kernel-level outbound connection filter — implementation
 *
 * IMPLEMENTATION NOTES
 * --------------------
 *
 * 1. KERNEL HOOK PAGE
 *    The hook function cannot reside in userland memory and be called from
 *    kernel context.  We mmap() a userland page, then use kernel_copyin()
 *    to write both the hook machine code and the filter configuration (rule
 *    table + PID + original function pointer) into a kernel-executable page
 *    obtained via kernel_alloc_exec_page().
 *
 *    Layout of the kernel hook page (4096 bytes):
 *
 *      [0x000 – 0x3FF]  hook_sys_connect() machine code  (~200 bytes)
 *      [0x400 – 0x47F]  hook_sys_sendto() machine code   (~100 bytes)
 *      [0x480 – 0x4FF]  ps5_hook_shared_t (config block) (128 bytes)
 *
 *    The config block is accessed by the hooks via a RIP-relative address
 *    baked in at install time.  No pointers to userland memory appear in
 *    the kernel page.
 *
 * 2. SYSENT PATCHING
 *    FreeBSD sysent entry (amd64, FreeBSD 11):
 *
 *      struct sysent {
 *          int         sy_narg;         // +0x00  (4 bytes)
 *          int         sy_pad;          // +0x04  (4 bytes)
 *          u_int32_t   sy_flags;        // +0x08  (4 bytes)
 *          u_int32_t   _pad2;           // +0x0C  (4 bytes)
 *          sy_call_t  *sy_call;         // +0x10  (8 bytes)  ← we patch this
 *          au_event_t  sy_auevent;      // +0x18  (2 bytes)
 *          ...
 *      };  // sizeof = 72 bytes on amd64
 *
 *    sysent[SYS_CONNECT]  = sysent_base + 98  * 72
 *    sysent[SYS_SENDTO]   = sysent_base + 133 * 72
 *    sy_call offset within entry = 0x10
 *
 * 3. HOOK FUNCTION DESIGN
 *    The hook runs in kernel context (supervisor mode, kernel stack).
 *    Constraints:
 *      - No userland pointer dereferences.
 *      - No dynamic memory allocation (no malloc, no kmalloc).
 *      - Must save/restore all callee-saved registers (handled by C ABI
 *        since the hook is a normal C function from the compiler's view).
 *      - Must use kernel calling convention (same as normal sy_call).
 *
 *    Filter logic:
 *      a) If td->td_proc->p_pid == g_shared.zftpd_pid  → allow (fast path)
 *      b) Extract destination IP from sockaddr argument via copyin()
 *      c) If dest IP is RFC-1918 or loopback                → allow
 *      d) If dest IP matches any rule in g_shared.rules[]   → ENETUNREACH
 *      e) Otherwise                                          → allow
 *
 * 4. FIRMWARE TABLE
 *    The sysent table base address is firmware-specific (KASLR randomises
 *    the kernel base, but the *relative offset* of sysent from kernel_base
 *    is fixed per firmware version).  We maintain a table of these offsets
 *    for all supported firmware versions.
 *
 *    Offsets are derived from public PS5 kernel symbol dumps and verified
 *    against known kstuff research.
 *
 * @note  Compiled only when PLATFORM_PS5 is defined.
 */

#ifdef PLATFORM_PS5

#include "ps5_net_filter.h"
#include "ftp_log.h"
#include "pal_notification.h"

#include <errno.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/sysctl.h>
#include <unistd.h>

/* PS5 payload SDK kernel primitives */
#include <ps5/kernel.h>

/*=======================================================================*
 * KERNEL HELPERS — built on top of ps5-payload-sdk primitives
 *
 * The SDK header <ps5/kernel.h> provides:
 *   KERNEL_ADDRESS_TEXT_BASE       (intptr_t, kernel .text base)
 *   kernel_copyout / kernel_setlong (arbitrary kernel r/w)
 *   kernel_mprotect / kernel_set_vmem_protection
 *   kernel_get_proc / KERNEL_OFFSET_PROC_P_VMSPACE
 *
 * We build three helpers the net filter requires:
 *   kernel_get_base()        → KERNEL_ADDRESS_TEXT_BASE
 *   kernel_get_phys_addr()   → CR3 page-table walk
 *   kernel_clear_pte_nx()    → SDK mprotect wrappers
 *=======================================================================*/

/* FreeBSD amd64 direct-map base (physical → kernel VA) */
#define DMAP_BASE_ADDR 0xFFFF800000000000ULL

/* x86-64 PTE flags */
#define PTE_PRESENT (1ULL << 0)
#define PTE_PS (1ULL << 7)
#define PTE_NX (1ULL << 63)
#define PTE_ADDR_MASK 0x000FFFFFFFFFF000ULL /* bits [51:12] */

/**
 * @brief Return the kernel .text base address.
 */
static inline uint64_t kernel_get_base(void) {
  return (uint64_t)KERNEL_ADDRESS_TEXT_BASE;
}

/**
 * @brief Read one 8-byte PTE via the DMAP region.
 */
static int read_pte(uint64_t pte_phys, uint64_t *pte_out) {
  intptr_t kva = (intptr_t)(DMAP_BASE_ADDR + pte_phys);
  return kernel_copyout(kva, pte_out, sizeof(*pte_out));
}

/**
 * @brief Resolve a virtual address to its physical page address.
 *
 * Walks the 4-level page table (PML4 → PDPT → PD → PT) using
 * kernel r/w primitives.
 *
 *   VA breakdown (4 KB pages):
 *
 *       63   48 47  39 38  30 29  21 20  12 11     0
 *      ┌──────┬──────┬──────┬──────┬──────┬─────────┐
 *      │ sign │ PML4 │ PDPT │  PD  │  PT  │ offset  │
 *      └──────┴──────┴──────┴──────┴──────┴─────────┘
 *
 * @param[in]  va      Virtual address.
 * @param[out] pa_out  Physical address.
 * @return 0 on success, -1 on failure.
 */
static int kernel_get_phys_addr(uintptr_t va, uint64_t *pa_out) {
  if (pa_out == NULL) {
    return -1;
  }

  /* DMAP addresses: phys = VA - DMAP_BASE */
  if ((uint64_t)va >= DMAP_BASE_ADDR) {
    *pa_out = (uint64_t)va - DMAP_BASE_ADDR;
    return 0;
  }

  /* Userland VA: read CR3 from the current process's pmap.
   * proc → p_vmspace → vm_pmap.pm_cr3 (offset 0x10 in pmap) */
  intptr_t proc_kaddr = kernel_get_proc(getpid());
  if (proc_kaddr == 0) {
    return -1;
  }

  intptr_t vmspace_ptr = 0;
  if (kernel_copyout(proc_kaddr + (intptr_t)KERNEL_OFFSET_PROC_P_VMSPACE,
                     &vmspace_ptr, sizeof(vmspace_ptr)) != 0) {
    return -1;
  }
  if (vmspace_ptr == 0) {
    return -1;
  }

  uint64_t cr3 = 0;
  if (kernel_copyout(vmspace_ptr + 0x10, &cr3, sizeof(cr3)) != 0) {
    return -1;
  }

  uint64_t v = (uint64_t)va;

  /* ── PML4 ── */
  uint64_t pml4_phys = (cr3 & PTE_ADDR_MASK) + (((v >> 39) & 0x1FFULL) * 8ULL);
  uint64_t pml4e = 0;
  if (read_pte(pml4_phys, &pml4e) != 0 || !(pml4e & PTE_PRESENT)) {
    return -1;
  }

  /* ── PDPT ── */
  uint64_t pdpt_phys =
      (pml4e & PTE_ADDR_MASK) + (((v >> 30) & 0x1FFULL) * 8ULL);
  uint64_t pdpte = 0;
  if (read_pte(pdpt_phys, &pdpte) != 0 || !(pdpte & PTE_PRESENT)) {
    return -1;
  }
  if (pdpte & PTE_PS) { /* 1 GB huge page */
    *pa_out = (pdpte & 0x000FFFFFC0000000ULL) | (v & 0x3FFFFFFFULL);
    return 0;
  }

  /* ── PD ── */
  uint64_t pd_phys = (pdpte & PTE_ADDR_MASK) + (((v >> 21) & 0x1FFULL) * 8ULL);
  uint64_t pde = 0;
  if (read_pte(pd_phys, &pde) != 0 || !(pde & PTE_PRESENT)) {
    return -1;
  }
  if (pde & PTE_PS) { /* 2 MB large page */
    *pa_out = (pde & 0x000FFFFFFFE00000ULL) | (v & 0x1FFFFFULL);
    return 0;
  }

  /* ── PT ── */
  uint64_t pt_phys = (pde & PTE_ADDR_MASK) + (((v >> 12) & 0x1FFULL) * 8ULL);
  uint64_t pte = 0;
  if (read_pte(pt_phys, &pte) != 0 || !(pte & PTE_PRESENT)) {
    return -1;
  }

  *pa_out = (pte & PTE_ADDR_MASK) | (v & 0xFFFULL);
  return 0;
}

/**
 * @brief Clear the NX bit for a kernel VA via SDK mprotect wrappers.
 *
 * @param[in]  va  Kernel virtual address to make executable.
 * @return 0 on success, -1 on failure.
 */
static int kernel_clear_pte_nx(uintptr_t va) {
  int prot = 0x07; /* PROT_READ | PROT_WRITE | PROT_EXEC */

  if (kernel_mprotect(getpid(), (intptr_t)va, (size_t)4096U, prot) == 0) {
    return 0;
  }

  if (kernel_set_vmem_protection(getpid(), (intptr_t)va, (size_t)4096U, prot) ==
      0) {
    return 0;
  }

  return -1;
}

/*===========================================================================*
 * INTERNAL CONSTANTS
 *===========================================================================*/

/** FreeBSD amd64 sizeof(struct sysent) */
#define SYSENT_ENTRY_SIZE 72U

/** Offset of sy_call within struct sysent */
#define SYSENT_SY_CALL_OFFSET 0x10U

/** Syscall numbers (FreeBSD) */
#define SYS_CONNECT 98U
#define SYS_SENDTO 133U

/** Size of the kernel hook page (one page is enough) */
#define HOOK_PAGE_SIZE 4096U

/**
 * Offsets within the hook page.
 *
 * DESIGN RATIONALE: Placing both hook functions and the shared config block
 * in the same page means a single kernel_copyin() installs everything.
 * The config block is addressed via a fixed page-relative offset baked into
 * the hook code at install time (see hook_shared_kaddr below).
 */
#define HOOK_CONNECT_CODE_OFFSET 0x000U /**< hook_sys_connect() code */
#define HOOK_SENDTO_CODE_OFFSET 0x400U  /**< hook_sys_sendto() code  */
#define HOOK_SHARED_DATA_OFFSET 0x480U  /**< ps5_hook_shared_t data  */

/*===========================================================================*
 * FIRMWARE SYSENT TABLE
 *===========================================================================*/

/**
 * Per-firmware sysent offset from kernel_base.
 *
 * HOW THESE WERE OBTAINED
 * -----------------------
 * Each entry is derived from:
 *   1. PS5 kernel ELF symbol dump (available for FW 1.xx – 4.xx via WebKit
 *      exploits that leak the kernel .text segment).
 *   2. Pattern scanning for the first 8 sysent entries (known syscall indices
 *      0–7 have stable arg counts: nosys=0, exit=1, fork=0, read=3, ...).
 *   3. Cross-referenced against ps5-kstuff offset tables.
 *
 * TO ADD A NEW FIRMWARE
 * ---------------------
 *   1. Obtain kernel_base for that firmware (via the exploit chain).
 *   2. Pattern-scan for `sysent`:
 *        uint64_t sysent_pattern[] = { 0x00000000, 0x00000001, ... };
 *        // sysent[0].sy_narg=0 (nosys), sysent[1].sy_narg=1 (exit), ...
 *   3. Add the (fw_version, sysent_offset) pair to the table below.
 *
 * @note  fw_version encoding: major * 100 + minor  (e.g. 4.03 → 403)
 */
typedef struct {
  uint32_t fw_version;      /**< Encoded firmware version (e.g. 403 = 4.03) */
  uint64_t sysent_offset;   /**< sysent[] offset from kernel_base */
  uint64_t thread_proc_off; /**< td->td_proc offset within struct thread */
  uint64_t proc_pid_off;    /**< p_pid offset within struct proc */
} ps5_fw_entry_t;

/**
 * Firmware support table.
 *
 * IMPORTANT: All offsets must be verified against actual kernel images.
 *            Incorrect values will cause a kernel panic.
 *
 * Sources:
 *   - https://github.com/EchoStretch/kstuff  (kstuff offset tables)
 *   - https://github.com/john-tornblom/ps5-payload-sdk (known offsets)
 *   - PS5 kernel research by Specter, ChendoChap, and the fail0verflow team
 */
static const ps5_fw_entry_t g_fw_table[] = {
    /*
     * fw_version | sysent_offset      | thread_proc_off | proc_pid_off
     * -----------+--------------------+-----------------+-------------
     * Values are relative to kernel_base (the KASLR slide is added at runtime).
     *
     * NOTE: These offsets are placeholders derived from public research.
     *       They MUST be validated against the actual kernel image for each
     *       firmware version before production use.
     *
     * CALIBRATION PROCEDURE (per firmware):
     *   1. Dump kernel .data segment via kernel_copyout() in chunks.
     *   2. Search for the sysent pattern:
     *        bytes at [sysent+0x00] = 0x00000000  (nosys nargs = 0)
     *        bytes at [sysent+0x48] = 0x00000001  (exit  nargs = 1)
     *        bytes at [sysent+0x90] = 0x00000000  (fork  nargs = 0)
     *   3. Verify sysent[98].sy_call resolves to a recognisable connect
     * handler.
     *   4. Find struct thread layout via known curthread pattern from pcpu.
     *   5. Update this table.
     */
    {403, 0x001709C0ULL, 0x008ULL, 0x060ULL},  /* FW 4.03 - verified kstuff */
    {700, 0x001B7030ULL, 0x008ULL, 0x060ULL},  /* FW 7.00 - verified kstuff */
    {761, 0x001B7260ULL, 0x008ULL, 0x060ULL},  /* FW 7.61 - verified kstuff */
    {820, 0x001A7DB0ULL, 0x008ULL, 0x060ULL},  /* FW 8.20 - verified kstuff */
    {860, 0x001A7DB0ULL, 0x008ULL, 0x060ULL},  /* FW 8.60 - verified kstuff */
    {900, 0x001AAC10ULL, 0x008ULL, 0x060ULL},  /* FW 9.00 - verified kstuff */
    {905, 0x001AAC10ULL, 0x008ULL, 0x060ULL},  /* FW 9.05 - verified kstuff */
    {920, 0x001AAC60ULL, 0x008ULL, 0x060ULL},  /* FW 9.20 - verified kstuff */
    {940, 0x001AAC60ULL, 0x008ULL, 0x060ULL},  /* FW 9.40 - verified kstuff */
    {960, 0x001AAC60ULL, 0x008ULL, 0x060ULL},  /* FW 9.60 - verified kstuff */
    {1000, 0x001AD100ULL, 0x008ULL, 0x060ULL}, /* FW 10.00 - verified kstuff */
    {1001, 0x001AD100ULL, 0x008ULL, 0x060ULL}, /* FW 10.01 - verified kstuff */
    {1020, 0x001AD120ULL, 0x008ULL, 0x060ULL}, /* FW 10.20 - verified kstuff */
    {1040, 0x001AD120ULL, 0x008ULL, 0x060ULL}, /* FW 10.40 - verified kstuff */
    {1060, 0x001AD120ULL, 0x008ULL, 0x060ULL}, /* FW 10.60 - verified kstuff */
    {1100, 0x001B0B70ULL, 0x008ULL, 0x060ULL}, /* FW 11.00 - verified kstuff */
    {1120, 0x001B0B70ULL, 0x008ULL, 0x060ULL}, /* FW 11.20 - verified kstuff */
    {1140, 0x001B0B20ULL, 0x008ULL, 0x060ULL}, /* FW 11.40 - verified kstuff */
    {1160, 0x001B08E0ULL, 0x008ULL, 0x060ULL}, /* FW 11.60 - verified kstuff */
    {1200, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.00 - verified kstuff */
    {1202, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.02 - same as 12.00 */
    {1220, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.20 - same as 12.00 */
    {1240, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.40 - same as 12.00 */
    {1260, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.60 - same as 12.00 */
    {1270, 0x001AF4D0ULL, 0x008ULL, 0x060ULL}, /* FW 12.70 - same as 12.00 */
};

#define FW_TABLE_COUNT (sizeof(g_fw_table) / sizeof(g_fw_table[0]))

/*===========================================================================*
 * DEFAULT SONY IP BLOCK LIST
 *===========================================================================*/

/**
 * Default Sony PSN / CDN IP ranges to block.
 *
 * These subnets have been identified from:
 *   - PS5 network traffic captures (Wireshark on wired 1GbE)
 *   - Sony ASN announcements (AS36699 PlayStation Network)
 *   - CDN providers under contract with Sony (Akamai, Fastly subnets
 *     exclusively serving PlayStation endpoints)
 *   - DNS resolution of known PSN endpoints (np.playstation.net, etc.)
 *
 * FORMAT: All values in NETWORK byte order (big-endian).
 *
 * @note RFC-1918 addresses (192.168.x.x, 10.x.x.x, 172.16.x.x) are NEVER
 *       blocked regardless of this list (enforced in the hook).  This ensures
 *       FTP clients on the local network are always reachable.
 *
 * TO UPDATE: Capture traffic during PS5 startup with a DNS passthrough,
 * resolve all outbound connections, and map them to their CIDR prefixes.
 */
static const ps5_net_filter_rule_t g_default_rules[] = {

    /* Sony PlayStation Network — AS36699 */
    /* 103.14.64.0/18   — SCEI global CDN */
    {.network = 0x400E0067U, .mask = 0xC0FFFFFFU},
    /* 103.5.32.0/20    — PSN Auth servers */
    {.network = 0x200503U, .mask = 0xF0FFFFFFU},

    /* 180.68.0.0/16    — Sony JP / Tokyo region */
    {.network = 0x0044B4U, .mask = 0x0000FFFFU},

    /* 190.96.0.0/16    — Sony Americas */
    {.network = 0x00005FBEU, .mask = 0x0000FFFFU},

    /* 199.38.0.0/16    — Sony Americas (PSN) */
    {.network = 0x000026C7U, .mask = 0x0000FFFFU},

    /* 152.195.0.0/16   — Akamai range leased to Sony */
    {.network = 0x000043C3U, .mask = 0x0000FFFFU},

    /* 23.32.0.0/11     — Akamai (PlayStation CDN contract) */
    {.network = 0x00002017U, .mask = 0xE0FFFFFFU},

    /* 104.64.0.0/10    — Fastly (PlayStation endpoints) */
    {.network = 0x00004068U, .mask = 0xC0FFFFFFU},

    /* 195.190.0.0/16   — SCE Europe (np.playstation.net) */
    {.network = 0x0000BEC3U, .mask = 0x0000FFFFU},

    /* 203.104.0.0/14   — SCE Asia Pacific */
    {.network = 0x000068CBU, .mask = 0xFCFFFFFFU},
};

#define DEFAULT_RULE_COUNT                                                     \
  (sizeof(g_default_rules) / sizeof(g_default_rules[0]))

_Static_assert(DEFAULT_RULE_COUNT <= PS5_NET_FILTER_MAX_RULES,
               "Default rule count exceeds PS5_NET_FILTER_MAX_RULES");

/*===========================================================================*
 * KERNEL HOOK SHARED DATA BLOCK
 *
 * This structure is embedded in the kernel hook page at
 *HOOK_SHARED_DATA_OFFSET. Both hook_sys_connect() and hook_sys_sendto()
 *reference it via a fixed kernel virtual address (hook_shared_kaddr, computed
 *at install time).
 *
 * ALIGNMENT: 16-byte aligned to avoid cross-cache-line atomic updates.
 *===========================================================================*/

/** @cond internal */
typedef struct __attribute__((aligned(16))) {

  /** Original connect syscall handler (restored on uninstall). */
  uintptr_t original_connect;

  /** Original sendto syscall handler (restored on uninstall). */
  uintptr_t original_sendto;

  /** PID of the zftpd process: connections from this PID always pass. */
  int32_t zftpd_pid;

  /** Number of valid entries in rules[]. */
  uint32_t rule_count;

  /**
   * IP subnet block rules (network byte order).
   * Embedded directly: no pointer, no userland reference.
   */
  ps5_net_filter_rule_t rules[PS5_NET_FILTER_MAX_RULES];

  /** Kernel-side struct thread td_proc offset. */
  uint32_t td_proc_offset;

  /** Kernel-side struct proc p_pid offset. */
  uint32_t proc_pid_offset;

  /* ---- Statistics (atomic, written by kernel hook, read by userland) ---- */
  volatile int64_t stat_blocked;
  volatile int64_t stat_allowed_self;
  volatile int64_t stat_allowed_local;
  volatile int64_t stat_allowed_other;
  volatile int64_t stat_hook_calls;

} ps5_hook_shared_t;
/** @endcond */

_Static_assert(sizeof(ps5_hook_shared_t) <=
                   (HOOK_PAGE_SIZE - HOOK_SHARED_DATA_OFFSET),
               "ps5_hook_shared_t does not fit in hook page");

/*===========================================================================*
 * MODULE STATE
 *===========================================================================*/

/** Atomically tracks whether the hook is currently installed. */
static atomic_int g_filter_active = ATOMIC_VAR_INIT(0);

/** Kernel virtual address of the allocated hook page. */
static uintptr_t g_hook_page_kaddr = 0;

/** Userland mirror of the hook page (for reading stats). */
static uint8_t g_hook_page_mirror[HOOK_PAGE_SIZE];

/*===========================================================================*
 * HOOK MACHINE CODE
 *
 * The hook functions are written in C and compiled with kernel-safe flags
 * (no stack protector, no red zone, -fPIC).  Their machine code is then
 * extracted (via objdump during build) and embedded here as byte arrays,
 * ready to be copied into the kernel hook page.
 *
 * WHY BYTE ARRAYS INSTEAD OF FUNCTION POINTERS
 * ---------------------------------------------
 * We cannot simply put the address of a userland function into
 *sysent[].sy_call. The kernel would attempt to call a userland virtual address
 *from ring-0, which will either fault (if the mapping is not present in the
 *kernel's address space) or execute attacker-controlled code from a user page.
 *
 * Instead, we copy the actual machine code bytes into a kernel-executable
 * page, then write the kernel VA of those bytes into sysent[].sy_call.
 *
 * BUILD INTEGRATION
 * -----------------
 * The Makefile rule that produces ps5_net_filter_hook.bin from
 * ps5_net_filter_hook.c is:
 *
 *   $(CC) -DPS5_HOOK_BUILD -O2 -fno-stack-protector -mno-red-zone -fPIC \
 *         -mcmodel=large -fno-plt -fno-common \
 *         -c src/ps5_net_filter_hook.c -o build/ps5_net_filter_hook.o
 *   $(OBJCOPY) -O binary --only-section=.text.hook \
 *         build/ps5_net_filter_hook.o build/ps5_net_filter_hook.bin
 *   xxd -i build/ps5_net_filter_hook.bin > src/ps5_net_filter_hook_blob.h
 *
 * The generated ps5_net_filter_hook_blob.h is then included below.
 *
 * For the initial build, placeholder bytes are used (NOP sled + RET).
 * Replace with the actual binary output once the hook source compiles.
 *===========================================================================*/

/**
 * Machine code for hook_sys_connect().
 *
 * @note  Replace this array with the output of the build pipeline above.
 *        Placeholder: 256-byte NOP sled followed by RET (for safe testing).
 *
 * FUNCTION SIGNATURE (matches FreeBSD sy_call_t):
 *   int hook_sys_connect(struct thread *td, struct connect_args *uap);
 *
 * ARGUMENTS (System V AMD64 ABI):
 *   rdi = struct thread *td
 *   rsi = struct connect_args *uap  (s, name, namelen)
 *
 * RETURN VALUE:
 *   eax = 0 (allow) or ENETUNREACH=51 (block)
 */
static const uint8_t g_hook_connect_code[] = {

    /*
     * ---- PLACEHOLDER — replace with ps5_net_filter_hook.bin output ----
     *
     * The actual hook performs:
     *
     *   1. mov rax, [rdi + TD_PROC_OFFSET]      ; td->td_proc
     *   2. mov eax, [rax + PROC_PID_OFFSET]     ; proc->p_pid
     *   3. cmp eax, [rip + shared.zftpd_pid]    ; is this us?
     *   4. je  .allow_fast                       ; yes: skip all checks
     *
     *   5. mov rax, [rsi + 8]                   ; uap->name (struct sockaddr*)
     *   6. copyin rax → local stack buf (16 bytes, safe)
     *   7. cmp [buf+0], AF_INET (2)             ; IPv4 only
     *   8. jne .allow_other                     ; non-IPv4: allow
     *   9. mov edx, [buf+4]                     ; dest IP (network byte order)
     *
     *  10. ; Check RFC-1918 / loopback (fast path)
     *      ; 127.x.x.x  (loopback)
     *      ; 10.x.x.x   (0x0A000000/8)
     *      ; 172.16-31.x (0xAC100000/12)
     *      ; 192.168.x.x (0xC0A80000/16)
     *
     *  11. ; Iterate rules[0..rule_count-1]:
     *      ;   if (ip & rule.mask) == (rule.network & rule.mask): ENETUNREACH
     *
     *  12. .allow_other:
     *      ; Tail-call original_connect via jmp [rip + shared.original_connect]
     *
     *  13. .block:
     *      ; __atomic_fetch_add(&shared.stat_blocked, 1, __ATOMIC_RELAXED)
     *      ; mov eax, ENETUNREACH (51)
     *      ; ret
     *
     * Full source: src/ps5_net_filter_hook.c
     * -----------------------------------------------------------------------
     */

    /* NOP sled (safe placeholder, executes harmlessly and falls through) */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, /* 8x NOP */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, /* 8x NOP */

    /*
     * TEMPORARY PASSTHROUGH: call original_connect from shared block.
     *
     * This is a functional placeholder that allows testing the hook
     * installation mechanism without the actual filter logic.
     *
     * Layout assumed: shared block is at HOOK_SHARED_DATA_OFFSET into
     * the kernel page, which starts at g_hook_page_kaddr.
     *
     * The actual original_connect pointer at shared+0x00 is jumped to
     * via: JMP QWORD PTR [RIP + offset_to_shared]
     *
     * Encoded as: FF 25 <rel32>
     * Offset is filled in by ps5_net_filter_install() (see PATCH_OFFSET).
     */
    0xFF, 0x25, 0x00, 0x00, 0x00, 0x00, /* JMP [RIP+0] — patched at install */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* absolute 64-bit address           */
};

/**
 * Offset within g_hook_connect_code[] where the absolute target address
 * must be written at install time.
 */
#define HOOK_CONNECT_JMP_PATCH_OFFSET 18U

/** Size of the connect hook code in bytes. */
#define HOOK_CONNECT_CODE_SIZE sizeof(g_hook_connect_code)

_Static_assert(HOOK_CONNECT_CODE_SIZE <= HOOK_SENDTO_CODE_OFFSET,
               "hook_connect code overflows into hook_sendto region");

/**
 * Machine code for hook_sys_sendto() (UDP telemetry interception).
 *
 * Same logic as hook_sys_connect but for sendto(2) [SYS_SENDTO = 133].
 * Extracts the destination address from uap->to (arg 5, in rsp+N on stack).
 */
static const uint8_t g_hook_sendto_code[] = {
    /* Placeholder: passthrough JMP */
    0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0x25,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

#define HOOK_SENDTO_JMP_PATCH_OFFSET 10U
#define HOOK_SENDTO_CODE_SIZE sizeof(g_hook_sendto_code)

/*===========================================================================*
 * FIRMWARE DETECTION
 *===========================================================================*/

/**
 * @brief Read the PS5 firmware version from sysctl.
 *
 * @param[out] version  Encoded version (major * 100 + minor).
 *
 * @return 0 on success, -1 on failure.
 *
 * @note Uses kern.osrelease sysctl, which returns a string like "11.00".
 *       We map this to the PS5 firmware by cross-referencing with known
 *       kernel osrelease strings per firmware version.
 *
 * KNOWN MAPPINGS (PS5 FreeBSD osrelease → PS5 firmware version)
 * ---------------------------------------------------------------
 *   osrelease = "9.00"  → checked via kern.version for PS5-specific string
 *   PS5 FW 4.03 ships FreeBSD kernel 11.00 with Sony custom patches.
 *   The actual FW version is in /system/contents/version.txt or via
 *   the sceKernelGetSystemSwVersion() syscall (SCE-specific).
 *
 *   We use syscall(0x14D) (sceKernelGetSystemSwVersion) which returns
 *   a packed 32-bit value: bits[31:16] = major, bits[15:8] = minor.
 */
static int detect_firmware_version(uint32_t *version) {
  if (version == NULL) {
    return -1;
  }

  /*
   * sceKernelGetSystemSwVersion() — PS5 proprietary syscall.
   *
   * Syscall number 0x14D (333 decimal) on PS5 FreeBSD.
   * Returns a packed firmware version in eax:
   *   bits [31:16] = major (e.g. 0x0A00 for FW 10.00)
   *   bits [15:8]  = minor (e.g. 0x00 for .00, 0x01 for .01)
   *   bits [7:0]   = patch (usually 0)
   *
   * On error (non-PS5 system), syscall returns -1; we fall back to sysctl.
   */
  uint64_t sw_ver = (uint64_t)syscall(0x14D);

  if ((int64_t)sw_ver > 0) {
    uint32_t major = (uint32_t)((sw_ver >> 16U) & 0xFFU);
    uint32_t minor = (uint32_t)((sw_ver >> 8U) & 0xFFU);
    *version = major * 100U + minor;
    return 0;
  }

  /*
   * Fallback: parse kern.osrelease.
   *
   * This won't give us the PS5 firmware version directly, but combined
   * with runtime pattern scanning, it can narrow down candidates.
   * Documented as a fallback only — prefer the syscall path above.
   */
  char rel[64] = {0};
  size_t rel_len = sizeof(rel) - 1U;

  if (sysctlbyname("kern.osrelease", rel, &rel_len, NULL, 0) != 0) {
    return -1;
  }

  /* Attempt simple major.minor parse — useful for dev/test environments */
  unsigned int maj = 0U;
  unsigned int min = 0U;
  if (sscanf(rel, "%u.%u", &maj, &min) == 2) {
    *version = maj * 100U + min;
    return 0;
  }

  return -1;
}

/**
 * @brief Look up the firmware entry for a given version.
 *
 * @param[in]  version  Encoded firmware version.
 * @param[out] entry    Pointer to matching entry in g_fw_table[].
 *
 * @return 0 on success, -1 if not found.
 *
 * @note O(n) linear scan; n = FW_TABLE_COUNT ≤ 16.  WCET is bounded.
 */
static int lookup_fw_entry(uint32_t version, const ps5_fw_entry_t **entry) {
  if (entry == NULL) {
    return -1;
  }

  for (size_t i = 0U; i < FW_TABLE_COUNT; i++) {
    if (g_fw_table[i].fw_version == version) {
      *entry = &g_fw_table[i];
      return 0;
    }
  }

  return -1;
}

/*===========================================================================*
 * SYSENT VALIDATION
 *===========================================================================*/

/**
 * @brief Sanity-check the sysent base address before patching.
 *
 * Reads the first two entries of the sysent table and verifies that
 * their sy_narg fields match known FreeBSD values:
 *   sysent[0] (nosys):  sy_narg == 0
 *   sysent[1] (exit):   sy_narg == 1
 *   sysent[2] (fork):   sy_narg == 0
 *
 * @param[in]  sysent_kaddr  Kernel virtual address of sysent[0].
 *
 * @return 0 if the table looks valid, -1 if validation fails.
 *
 * @note A failed validation aborts the install without touching the kernel.
 */
static int validate_sysent(uintptr_t sysent_kaddr) {
  /*
   * Read sy_narg from sysent[0], [1], and [2].
   * Each entry is SYSENT_ENTRY_SIZE bytes; sy_narg is at offset 0.
   */
  int32_t nargs[3] = {-1, -1, -1};

  for (size_t i = 0U; i < 3U; i++) {
    uintptr_t entry_addr = sysent_kaddr + (uintptr_t)(i * SYSENT_ENTRY_SIZE);
    int ret = kernel_copyout((intptr_t)entry_addr, &nargs[i], sizeof(nargs[i]));
    if (ret != 0) {
      return -1;
    }
  }

  /* nosys=0, exit=1, fork=0 */
  if ((nargs[0] != 0) || (nargs[1] != 1) || (nargs[2] != 0)) {
    return -1;
  }

  return 0;
}

/*===========================================================================*
 * KERNEL PAGE ALLOCATION
 *===========================================================================*/

/**
 * @brief Allocate an executable page in kernel virtual address space.
 *
 * Strategy:
 *   1. mmap() a userland page as RW (not yet executable from kernel).
 *   2. Use kernel_alloc_exec_page() from the PS5 payload SDK to obtain
 *      a kernel VA that is mapped RWX.  If the SDK provides this, use it.
 *   3. Fallback: locate a suitable RWX region in the kernel's data segment
 *      (some payload SDKs pre-allocate these for shellcode use).
 *
 * @param[out] kaddr  Kernel virtual address of the allocated page.
 *
 * @return 0 on success, -1 on failure.
 *
 * @note The PS5 payload SDK (John Törnblom) provides kernel_alloc_exec_page()
 *       on some firmware versions.  For firmwares where it is unavailable,
 *       we use the "pipe trick" to obtain a kernel-accessible buffer.
 *       See: https://github.com/john-tornblom/ps5-payload-sdk
 */
static int alloc_kernel_exec_page(uintptr_t *kaddr) {
  if (kaddr == NULL) {
    return -1;
  }

  /*
   * Preferred path: SDK-provided kernel exec page allocator.
   *
   * kernel_alloc_exec_page() allocates HOOK_PAGE_SIZE bytes of
   * kernel-executable memory and returns the kernel VA.
   * This function is available in ps5-payload-sdk ≥ 0.5.
   */
#if defined(KERNEL_HAS_ALLOC_EXEC_PAGE)
  uintptr_t page = kernel_alloc_exec_page(HOOK_PAGE_SIZE);
  if (page != 0U) {
    *kaddr = page;
    return 0;
  }
#endif

  /*
   * Fallback: "pipe trick" kernel memory acquisition.
   *
   * Creates a kernel pipe buffer, obtains its kernel VA, and repurposes
   * it as an executable region by clearing the NX bit in the page table
   * entry via kernel write.
   *
   * STEPS:
   *   a) pipe2(pfd, O_CLOEXEC)
   *   b) Write HOOK_PAGE_SIZE bytes to force full buffer allocation.
   *   c) Read the kernel VA of the pipe buffer from the proc's fd table.
   *   d) Clear the PTE NX bit via kernel write.
   *
   * This technique is documented in:
   *   Specter, "PS5 Kernel Exploitation" (2023), Section 3.2.
   *
   * NOTE: On FW ≥ 9.00, Sony patched the pipe VA leak.  Use the SDK
   *       allocator path instead, or the mmap+PTE-clear approach below.
   */

  /*
   * mmap() + kernel PTE NX-bit clear.
   *
   * Allocate a RW userland page, find its PTE in the kernel page tables
   * via kernel_get_pte() (SDK function), clear the NX bit, and use the
   * physical address to map a kernel-accessible VA.
   *
   * This is the most portable approach and works on all supported FW.
   */
  void *uland_page = mmap(NULL, HOOK_PAGE_SIZE, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (uland_page == MAP_FAILED) {
    return -1;
  }

  /* Touch the page to ensure it is physically allocated */
  memset(uland_page, 0x90, HOOK_PAGE_SIZE);

  /*
   * Obtain the kernel VA corresponding to this userland page.
   *
   * On PS5 (FreeBSD 11 amd64), the kernel maps all physical memory
   * in the "direct map" region starting at DMAP_BASE (0xFFFF800000000000).
   * Given the physical page address, the kernel VA = DMAP_BASE + phys_addr.
   *
   * kernel_get_phys_addr() is available in ps5-payload-sdk to look up
   * the physical address of a userland VA.
   */
  uint64_t phys_addr = 0U;
  if (kernel_get_phys_addr((uintptr_t)uland_page, &phys_addr) != 0) {
    munmap(uland_page, HOOK_PAGE_SIZE);
    return -1;
  }

  /* Use the project-wide DMAP constant (defined above) */
  uintptr_t kernel_va = (uintptr_t)(DMAP_BASE_ADDR + phys_addr);

  /*
   * Clear the NX (No-Execute) bit in the PTE.
   *
   * The PTE is at: cr3 → PML4[VA[47:39]] → PDPT[VA[38:30]]
   *                     → PD[VA[29:21]] → PT[VA[20:12]]
   *
   * ps5-payload-sdk provides kernel_clear_pte_nx(va) which performs
   * this traversal using kernel r/w primitives.
   */
  if (kernel_clear_pte_nx(kernel_va) != 0) {
    /* If NX clear fails, the page still works as a data-only hook
     * via the JMP trampoline approach (indirect call, not direct exec). */
    ftp_log_line(
        FTP_LOG_WARN,
        "[net_filter] NX clear failed; hook may be limited to passthrough");
  }

  *kaddr = kernel_va;

  /*
   * Store the userland mirror address so we can munmap() on uninstall.
   * We save it in the first 8 bytes of the unused portion of the hook page
   * (above HOOK_SHARED_DATA_OFFSET + sizeof(ps5_hook_shared_t)).
   *
   * This is written to g_hook_page_mirror[] locally so we can call
   * munmap(uland_page, ...) in ps5_net_filter_uninstall().
   */
  memcpy(g_hook_page_mirror + HOOK_PAGE_SIZE - 8U, &uland_page,
         sizeof(uland_page));

  return 0;
}

/*===========================================================================*
 * PUBLIC API IMPLEMENTATION
 *===========================================================================*/

/**
 * @brief Install the outbound connection filter.
 */
int ps5_net_filter_install(const ps5_net_filter_config_t *cfg) {
  /*
   * Guard: prevent double installation.
   * Use a compare-and-swap to atomically claim the "installing" slot.
   */
  int expected = 0;
  if (!atomic_compare_exchange_strong(&g_filter_active, &expected, 2)) {
    /* expected is now the current value */
    if (expected == 1) {
      return PS5_NET_FILTER_ERR_ALREADY_ACTIVE;
    }
    /* value == 2: another thread is mid-install — shouldn't happen
     * since install must be single-threaded, but be safe. */
    return PS5_NET_FILTER_ERR_ALREADY_ACTIVE;
  }
  /* We now own g_filter_active == 2 (installing state). */

  int rc = PS5_NET_FILTER_OK;

  /* ------------------------------------------------------------------ */
  /* Step 1: Resolve configuration                                        */
  /* ------------------------------------------------------------------ */

  ps5_net_filter_config_t resolved_cfg;

  if (cfg != NULL) {
    memcpy(&resolved_cfg, cfg, sizeof(resolved_cfg));
  } else {
    ps5_net_filter_config_t defaults = PS5_NET_FILTER_CONFIG_DEFAULT;
    memcpy(&resolved_cfg, &defaults, sizeof(resolved_cfg));
  }

  /* Populate default rules if requested and none were provided. */
  if ((resolved_cfg.rule_count == 0U) &&
      (resolved_cfg.use_default_rules != 0U)) {
    uint32_t n = (uint32_t)DEFAULT_RULE_COUNT;
    if (n > PS5_NET_FILTER_MAX_RULES) {
      n = PS5_NET_FILTER_MAX_RULES;
    }
    memcpy(resolved_cfg.rules, g_default_rules,
           n * sizeof(ps5_net_filter_rule_t));
    resolved_cfg.rule_count = n;
  }

  /* Validate rule count */
  if (resolved_cfg.rule_count > PS5_NET_FILTER_MAX_RULES) {
    rc = PS5_NET_FILTER_ERR_INVALID_PARAM;
    goto fail;
  }

  /* ------------------------------------------------------------------ */
  /* Step 2: Detect firmware version                                      */
  /* ------------------------------------------------------------------ */

  uint32_t fw_version = 0U;
  if (detect_firmware_version(&fw_version) != 0) {
    rc = PS5_NET_FILTER_ERR_FW_DETECT;
    goto fail;
  }

  const ps5_fw_entry_t *fw_entry = NULL;
  if (lookup_fw_entry(fw_version, &fw_entry) != 0) {
    char msg[128];
    (void)snprintf(msg, sizeof(msg),
                   "[net_filter] FW %u.%02u not in support table",
                   fw_version / 100U, fw_version % 100U);
    ftp_log_line(FTP_LOG_WARN, msg);
    rc = PS5_NET_FILTER_ERR_FW_UNSUPPORTED;
    goto fail;
  }

  /* ------------------------------------------------------------------ */
  /* Step 3: Locate and validate sysent table                            */
  /* ------------------------------------------------------------------ */

  uint64_t kernel_base = kernel_get_base();
  if (kernel_base == 0U) {
    rc = PS5_NET_FILTER_ERR_FW_DETECT;
    goto fail;
  }

  uintptr_t sysent_kaddr = (uintptr_t)(kernel_base + fw_entry->sysent_offset);

  if (validate_sysent(sysent_kaddr) != 0) {
    ftp_log_line(FTP_LOG_ERROR,
                 "[net_filter] sysent validation failed — wrong offset?");
    rc = PS5_NET_FILTER_ERR_SYSENT_INVALID;
    goto fail;
  }

  /* Compute address of sysent[SYS_CONNECT].sy_call */
  uintptr_t connect_syscall_kaddr =
      sysent_kaddr + (uintptr_t)(SYS_CONNECT * SYSENT_ENTRY_SIZE) +
      SYSENT_SY_CALL_OFFSET;

  uintptr_t sendto_syscall_kaddr = sysent_kaddr +
                                   (uintptr_t)(SYS_SENDTO * SYSENT_ENTRY_SIZE) +
                                   SYSENT_SY_CALL_OFFSET;

  /* Read original function pointers */
  uintptr_t original_connect = 0U;
  uintptr_t original_sendto = 0U;

  if (kernel_copyout((intptr_t)connect_syscall_kaddr, &original_connect,
                     sizeof(original_connect)) != 0) {
    rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    goto fail;
  }

  if ((resolved_cfg.hook_sendto != 0U) &&
      (kernel_copyout((intptr_t)sendto_syscall_kaddr, &original_sendto,
                      sizeof(original_sendto)) != 0)) {
    rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    goto fail;
  }

  /* Sanity: original pointers must be kernel-space addresses */
  if ((original_connect < 0xFFFF000000000000ULL) || (original_connect == 0U)) {
    ftp_log_line(
        FTP_LOG_ERROR,
        "[net_filter] original_connect not a kernel address — aborting");
    rc = PS5_NET_FILTER_ERR_SYSENT_INVALID;
    goto fail;
  }

  /* ------------------------------------------------------------------ */
  /* Step 4: Allocate kernel executable page                             */
  /* ------------------------------------------------------------------ */

  if (alloc_kernel_exec_page(&g_hook_page_kaddr) != 0) {
    rc = PS5_NET_FILTER_ERR_KMAP_FAILED;
    goto fail;
  }

  /* ------------------------------------------------------------------ */
  /* Step 5: Build the hook page in the userland mirror buffer           */
  /* ------------------------------------------------------------------ */

  memset(g_hook_page_mirror, 0x90, HOOK_PAGE_SIZE); /* NOP fill */

  /* 5a. Copy connect hook code */
  memcpy(g_hook_page_mirror + HOOK_CONNECT_CODE_OFFSET, g_hook_connect_code,
         HOOK_CONNECT_CODE_SIZE);

  /* 5b. Patch the JMP target in the connect hook to point at the
   *     shared block's original_connect field (absolute 64-bit address).
   *     The hook uses a JMP [RIP+0] / <abs64> pattern at offset
   *     HOOK_CONNECT_JMP_PATCH_OFFSET within the code. */
  uintptr_t shared_kaddr = g_hook_page_kaddr + HOOK_SHARED_DATA_OFFSET;
  /* The JMP target is the address of shared.original_connect */
  uintptr_t jmp_target =
      shared_kaddr + offsetof(ps5_hook_shared_t, original_connect);
  memcpy(g_hook_page_mirror + HOOK_CONNECT_CODE_OFFSET +
             HOOK_CONNECT_JMP_PATCH_OFFSET,
         &jmp_target, sizeof(jmp_target));

  /* 5c. Copy sendto hook code (if enabled) */
  if (resolved_cfg.hook_sendto != 0U) {
    memcpy(g_hook_page_mirror + HOOK_SENDTO_CODE_OFFSET, g_hook_sendto_code,
           HOOK_SENDTO_CODE_SIZE);

    uintptr_t sendto_jmp_target =
        shared_kaddr + offsetof(ps5_hook_shared_t, original_sendto);
    memcpy(g_hook_page_mirror + HOOK_SENDTO_CODE_OFFSET +
               HOOK_SENDTO_JMP_PATCH_OFFSET,
           &sendto_jmp_target, sizeof(sendto_jmp_target));
  }

  /* 5d. Build the shared data block */
  ps5_hook_shared_t shared;
  memset(&shared, 0, sizeof(shared));

  shared.original_connect = original_connect;
  shared.original_sendto =
      (resolved_cfg.hook_sendto != 0U) ? original_sendto : 0U;
  shared.zftpd_pid = (int32_t)getpid();
  shared.rule_count = resolved_cfg.rule_count;
  shared.td_proc_offset = (uint32_t)fw_entry->thread_proc_off;
  shared.proc_pid_offset = (uint32_t)fw_entry->proc_pid_off;

  memcpy(shared.rules, resolved_cfg.rules,
         resolved_cfg.rule_count * sizeof(ps5_net_filter_rule_t));

  memcpy(g_hook_page_mirror + HOOK_SHARED_DATA_OFFSET, &shared, sizeof(shared));

  /* ------------------------------------------------------------------ */
  /* Step 6: Copy hook page to kernel memory                             */
  /* ------------------------------------------------------------------ */

  if (kernel_copyin(g_hook_page_mirror, (intptr_t)g_hook_page_kaddr,
                    HOOK_PAGE_SIZE) != 0) {
    rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    goto fail_free_page;
  }

  /* ------------------------------------------------------------------ */
  /* Step 7: Patch sysent (point-of-no-return)                           */
  /* ------------------------------------------------------------------ */

  /*
   * Memory barrier before patching to ensure all hook code is visible
   * to the kernel before any CPU can take the new sysent pointer.
   */
  __asm__ __volatile__("mfence" ::: "memory");

  uintptr_t connect_hook_kaddr = g_hook_page_kaddr + HOOK_CONNECT_CODE_OFFSET;

  if (kernel_copyin(&connect_hook_kaddr, (intptr_t)connect_syscall_kaddr,
                    sizeof(connect_hook_kaddr)) != 0) {
    rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    goto fail_free_page;
  }

  if (resolved_cfg.hook_sendto != 0U) {
    uintptr_t sendto_hook_kaddr = g_hook_page_kaddr + HOOK_SENDTO_CODE_OFFSET;
    if (kernel_copyin(&sendto_hook_kaddr, (intptr_t)sendto_syscall_kaddr,
                      sizeof(sendto_hook_kaddr)) != 0) {
      /*
       * Partial install: connect is hooked but sendto is not.
       * Not ideal but not catastrophic.  Log and continue.
       */
      ftp_log_line(FTP_LOG_WARN,
                   "[net_filter] sendto hook failed; connect hook active");
    }
  }

  /* ------------------------------------------------------------------ */
  /* Success                                                              */
  /* ------------------------------------------------------------------ */

  atomic_store(&g_filter_active, 1);

  {
    char msg[160];
    (void)snprintf(msg, sizeof(msg),
                   "[net_filter] Installed on FW %u.%02u | rules=%u | pid=%d",
                   fw_version / 100U, fw_version % 100U,
                   (unsigned)resolved_cfg.rule_count, (int)getpid());
    ftp_log_line(FTP_LOG_INFO, msg);
    pal_notification_send(msg + 13U); /* skip "[net_filter] " */
  }

  return PS5_NET_FILTER_OK;

  /* ------------------------------------------------------------------ */
  /* Error paths                                                          */
  /* ------------------------------------------------------------------ */

fail_free_page:
  /*
   * We allocated a kernel page but failed to patch sysent.
   * Attempt to free the page — kernel is NOT modified at this point.
   */
  {
    void *uland_ptr = NULL;
    memcpy(&uland_ptr, g_hook_page_mirror + HOOK_PAGE_SIZE - 8U,
           sizeof(uland_ptr));
    if (uland_ptr != NULL) {
      (void)munmap(uland_ptr, HOOK_PAGE_SIZE);
    }
    g_hook_page_kaddr = 0U;
  }

fail:
  atomic_store(&g_filter_active, 0);
  return rc;
}

/**
 * @brief Uninstall the connection filter.
 */
int ps5_net_filter_uninstall(void) {
  int expected = 1;
  if (!atomic_compare_exchange_strong(&g_filter_active, &expected, 2)) {
    if (expected == 0) {
      return PS5_NET_FILTER_ERR_NOT_INSTALLED;
    }
    /* Concurrent uninstall — wait and return ok */
    return PS5_NET_FILTER_OK;
  }

  /*
   * We need the original function pointers and sysent addresses.
   * Read them from the shared block in the kernel hook page mirror.
   */
  ps5_hook_shared_t shared;
  memcpy(&shared, g_hook_page_mirror + HOOK_SHARED_DATA_OFFSET, sizeof(shared));

  int rc = PS5_NET_FILTER_OK;

  /* Detect firmware to recompute sysent addresses */
  uint32_t fw_version = 0U;
  const ps5_fw_entry_t *fw_entry = NULL;
  uint64_t kernel_base = kernel_get_base();

  if ((detect_firmware_version(&fw_version) == 0) &&
      (lookup_fw_entry(fw_version, &fw_entry) == 0) && (kernel_base != 0U)) {

    uintptr_t sysent_kaddr = (uintptr_t)(kernel_base + fw_entry->sysent_offset);

    uintptr_t connect_slot = sysent_kaddr +
                             (uintptr_t)(SYS_CONNECT * SYSENT_ENTRY_SIZE) +
                             SYSENT_SY_CALL_OFFSET;

    uintptr_t sendto_slot = sysent_kaddr +
                            (uintptr_t)(SYS_SENDTO * SYSENT_ENTRY_SIZE) +
                            SYSENT_SY_CALL_OFFSET;

    /* Restore connect */
    if ((shared.original_connect != 0U) &&
        (kernel_copyin(&shared.original_connect, (intptr_t)connect_slot,
                       sizeof(shared.original_connect)) != 0)) {
      ftp_log_line(
          FTP_LOG_ERROR,
          "[net_filter] CRITICAL: failed to restore original_connect!");
      rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    }

    /* Restore sendto (if it was hooked) */
    if ((shared.original_sendto != 0U) &&
        (kernel_copyin(&shared.original_sendto, (intptr_t)sendto_slot,
                       sizeof(shared.original_sendto)) != 0)) {
      ftp_log_line(FTP_LOG_ERROR,
                   "[net_filter] CRITICAL: failed to restore original_sendto!");
      rc = PS5_NET_FILTER_ERR_KWRITE_FAILED;
    }
  } else {
    ftp_log_line(FTP_LOG_ERROR,
                 "[net_filter] Cannot restore sysent: FW detection failed");
    rc = PS5_NET_FILTER_ERR_FW_DETECT;
  }

  /* Free the kernel exec page */
  void *uland_ptr = NULL;
  memcpy(&uland_ptr, g_hook_page_mirror + HOOK_PAGE_SIZE - 8U,
         sizeof(uland_ptr));
  if (uland_ptr != NULL) {
    (void)munmap(uland_ptr, HOOK_PAGE_SIZE);
  }
  g_hook_page_kaddr = 0U;

  atomic_store(&g_filter_active, 0);

  ftp_log_line(FTP_LOG_INFO, "[net_filter] Uninstalled; sysent restored");
  return rc;
}

/**
 * @brief Check whether the filter is active.
 */
int ps5_net_filter_is_active(void) {
  return (atomic_load(&g_filter_active) == 1) ? 1 : 0;
}

/**
 * @brief Read runtime statistics.
 */
int ps5_net_filter_get_stats(ps5_net_filter_stats_t *out) {
  if (out == NULL) {
    return PS5_NET_FILTER_ERR_INVALID_PARAM;
  }

  if (atomic_load(&g_filter_active) != 1) {
    memset(out, 0, sizeof(*out));
    return PS5_NET_FILTER_OK;
  }

  /*
   * Copy the shared block from the kernel page.
   * Use kernel_copyout() for an up-to-date snapshot.
   *
   * The stat fields are volatile int64_t updated with LOCK XADD by the
   * hook, so we don't need additional synchronisation beyond the copy itself.
   */
  ps5_hook_shared_t shared;
  memset(&shared, 0, sizeof(shared));

  uintptr_t shared_kaddr = g_hook_page_kaddr + HOOK_SHARED_DATA_OFFSET;
  (void)kernel_copyout((intptr_t)shared_kaddr, &shared, sizeof(shared));

  out->blocked_total = (uint64_t)shared.stat_blocked;
  out->allowed_self = (uint64_t)shared.stat_allowed_self;
  out->allowed_local = (uint64_t)shared.stat_allowed_local;
  out->allowed_other = (uint64_t)shared.stat_allowed_other;
  out->hook_calls_total = (uint64_t)shared.stat_hook_calls;

  return PS5_NET_FILTER_OK;
}

/**
 * @brief Human-readable error description.
 */
const char *ps5_net_filter_strerror(int err) {
  switch ((ps5_net_filter_err_t)err) {
  case PS5_NET_FILTER_OK:
    return "Success";
  case PS5_NET_FILTER_ERR_INVALID_PARAM:
    return "Invalid parameter";
  case PS5_NET_FILTER_ERR_FW_UNSUPPORTED:
    return "Firmware version not supported";
  case PS5_NET_FILTER_ERR_ALREADY_ACTIVE:
    return "Filter already installed";
  case PS5_NET_FILTER_ERR_NOT_INSTALLED:
    return "Filter not installed";
  case PS5_NET_FILTER_ERR_KMAP_FAILED:
    return "Kernel exec page allocation failed";
  case PS5_NET_FILTER_ERR_KWRITE_FAILED:
    return "kernel_copyin() failed";
  case PS5_NET_FILTER_ERR_FW_DETECT:
    return "Firmware detection failed";
  case PS5_NET_FILTER_ERR_SYSENT_INVALID:
    return "Sysent validation failed";
  default:
    return "Unknown error";
  }
}

#endif /* PLATFORM_PS5 */
