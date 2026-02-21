# zftpd Internal APIs for Module Authors

This document summarizes the main internal facilities you can rely on when adding modules that need file/network I/O, memory management, logging, and optional crypto. Follow the existing safety/embedded style (C11, MISRA/CERT minded: explicit error codes, no unchecked pointers, defensive bounds checks, handle EINTR/EAGAIN, prefer zero-copy paths where available).

## Design Principles
- Single-threaded event-driven model; avoid blocking where possible.
- Return `ftp_error_t` (or `int` errno-style) and never leave partially initialized state.
- Validate inputs (null, length, ranges) before use.
- Keep allocations bounded and free/release buffers on all paths.
- Prefer platform abstraction layers (`pal_*`) instead of direct syscalls.

## Error Types
- `ftp_error_t` in `include/ftp_types.h` covers FTP-domain errors.
- Use `FTP_OK` for success; return specific error enums for callers to translate into FTP replies/logs.

## Memory Management
### Arena Scratch (`pal_scratch`)
- Fast temp buffers with rollback semantics.
- Typical use:
```c
#include "pal_scratch.h"

pal_scratch_t arena;
pal_scratch_mark_t mark;
pal_scratch_init(&arena, buffer, buffer_len);
pal_scratch_mark(&arena, &mark);
void *tmp = pal_scratch_alloc(&arena, needed, alignof(max_align_t));
/* ...use tmp... */
pal_scratch_reset(&arena, &mark); // frees all since mark
```

### Heap Alloc (`pal_alloc`)
- Thin wrappers for tracked malloc/free; prefer for persistent objects.
```c
#include "pal_alloc.h"
void *p = pal_malloc(size);
if (p == NULL) return FTP_ERR_NO_MEMORY;
/* ... */
pal_free(p);
```

### Buffer Pool (`ftp_buffer_pool`)
- Reusable I/O buffers sized for data path.
```c
#include "ftp_buffer_pool.h"
void *buf = ftp_buffer_acquire();
size_t sz = ftp_buffer_size();
/* use buf up to sz bytes */
ftp_buffer_release(buf);
```

## File I/O (`pal_fileio`)
- Portable wrappers for open/read/write/close/stat; handle EINTR internally where applicable.
- Sendfile fast path (Linux/FreeBSD/PS4/PS5) via `pal_sendfile`.
```c
#include "pal_fileio.h"
int fd = pal_file_open(path, O_RDONLY, 0);
struct stat st;
if (pal_file_fstat(fd, &st) != FTP_OK) { pal_file_close(fd); return FTP_ERR_FILE_STAT; }
ssize_t n = pal_file_read(fd, buf, len);
pal_file_close(fd);
```

## Virtual Filesystem (`pal_filesystem`, `pal_filesystem_psx`)
- `vfs_*` helpers abstract file nodes with optional PS4/PS5 self-handling.
```c
#include "pal_filesystem.h"
vfs_node_t node;
if (vfs_open(&node, "/data/pkg.bin") != FTP_OK) return FTP_ERR_FILE_OPEN;
ssize_t n = vfs_read(&node, buf, len);
vfs_close(&node);
```

## Path Utilities (`ftp_path`)
- Safe canonicalization and traversal checks, returns resolved paths under session root.
```c
#include "ftp_path.h"
char resolved[FTP_PATH_MAX];
ftp_error_t err = ftp_path_resolve(session, user_path, resolved, sizeof(resolved));
if (err != FTP_OK) return err; /* use resolved */
```

## Networking (`pal_network`)
- Socket creation, sockaddr helpers, byte-order macros; wraps platform differences.
```c
#include "pal_network.h"
struct sockaddr_in addr;
ftp_error_t err = pal_make_sockaddr("192.168.0.10", 2121, &addr);
int fd = PAL_SOCKET(AF_INET, SOCK_STREAM, 0);
if (PAL_CONNECT(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {/* handle */}
```

### Advanced: Scratch arena with fallback
```c
#include "pal_scratch.h"
#include "pal_alloc.h"

typedef struct {
    pal_scratch_t arena;
    uint8_t stack_buf[4096];
} my_ctx_t;

void my_ctx_init(my_ctx_t *ctx) {
    pal_scratch_init(&ctx->arena, ctx->stack_buf, sizeof(ctx->stack_buf));
}

void *my_tmp_alloc(my_ctx_t *ctx, size_t sz) {
    pal_scratch_mark_t mark;
    pal_scratch_mark(&ctx->arena, &mark);
    void *p = pal_scratch_alloc(&ctx->arena, sz, alignof(max_align_t));
    if (p != NULL) {
        pal_scratch_reset(&ctx->arena, &mark); // single alloc, safe to drop mark after use
        return p;
    }
    pal_scratch_reset(&ctx->arena, &mark);
    return pal_malloc(sz); // fallback to heap if arena exhausted
}
```

### Advanced: Custom HTTP endpoint (ZHTTP)
```c
// http_api.c
#include "http_api.h"
#include "http_response.h"
#include "pal_fileio.h"

static http_response_t *handle_ping(const http_request_t *req) {
    (void)req;
    return http_response_json(200, "{\"ok\":true}\n");
}

http_response_t *http_api_handle(const http_request_t *req) {
    if (strncmp(req->uri, "/api/ping", 9) == 0) {
        return handle_ping(req);
    }
    // existing handlers...
    return http_response_not_found();
}
```
- Use `pal_socket_set_reuseaddr`, `pal_socket_set_nonblock`, `pal_network_get_primary_ip` when needed.

## Logging (`ftp_log`)
- Session-aware structured logging; prefer to emit events instead of printf.
```c
#include "ftp_log.h"
ftp_log_session_event(session, "MYMODULE", FTP_OK, bytes_processed);
```

## Notifications (PS4/PS5) (`pal_notification`)
- Lightweight wrapper to display toast messages on consoles.
```c
#include "pal_notification.h"
pal_notify("zftpd", "Started on 192.168.0.10:2122");
```

## Crypto (optional) (`ftp_crypto`)
- ChaCha20 PSK via `AUTH XCRYPT`; keep disabled unless explicitly required.
```c
#include "ftp_crypto.h"
uint8_t key[32] = { /* PSK */ };
uint8_t nonce[12] = {0};
ftp_crypto_derive_key(key, nonce, out_key);
```
- Never hardcode secrets in source destined for payloads; load from secure source when possible.

## Rate Limiting
- Token-bucket per session in `ftp_commands.c` (`ftp_rate_limit_wait`); reuse pattern for new data paths.

## HTTP/ZHTTP Modules
- When `ENABLE_ZHTTPD=1`, HTTP server modules live in `src/http_*.c`; add endpoints in `http_api.c`.
```c
// http_api.c
if (strncmp(req->uri, "/api/custom", 11) == 0) {
    return handle_custom(req);
}
```

## Quality Bar (Embedded-grade)
- Check all return values; handle `EINTR`, `EAGAIN`, and short I/O.
- Keep stack usage bounded; avoid VLAs and unbounded recursion.
- Zero-init structs before use; clear sensitive buffers after use (e.g., keys).
- Respect compile-time toggles: `ENABLE_ZHTTPD`, `ENABLE_WEB_UPLOAD`, `FTP_ENABLE_CRYPTO`, `FTP_ENABLE_MLST`, `FTP_ENABLE_UTF8`, rate-limit macros.
- Use `PAL_*` macros for portability (sockets, htons/ntohl, close) instead of raw syscalls.
- Prefer `snprintf` with size checks; guard against path overflows (see `ftp_path_resolve`).

## Minimal Example: Reading a file and sending over data FD
```c
#include "pal_filesystem.h"
#include "pal_network.h"
#include "ftp_buffer_pool.h"

ftp_error_t send_file(int data_fd, const char *path) {
    vfs_node_t node;
    if (vfs_open(&node, path) != FTP_OK) return FTP_ERR_FILE_OPEN;

    void *buf = ftp_buffer_acquire();
    size_t sz = ftp_buffer_size();
    ftp_error_t status = FTP_OK;

    while (status == FTP_OK) {
        ssize_t n = vfs_read(&node, buf, sz);
        if (n == 0) break; // EOF
        if (n < 0) { status = FTP_ERR_FILE_READ; break; }
        ssize_t sent = pal_send(data_fd, buf, (size_t)n, 0);
        if (sent != n) { status = FTP_ERR_NET_SEND; break; }
    }

    ftp_buffer_release(buf);
    vfs_close(&node);
    return status;
}
```

## Where to Look
- Headers: `include/pal_*.h`, `include/ftp_*.h`
- Implementations: `src/pal_*`, `src/ftp_commands.c`, `src/http_*.c`, `src/ftp_crypto.c`
- The PAL layers are usable to build standalone apps (e.g., custom services like a lightweight game server or tools unrelated to FTP). Reuse `pal_*` for portability, `ftp_buffer_pool` for I/O buffers, and add your own protocol handlers atop the same evented model.

Keep modules small, defensive, and consistent with existing patterns.
