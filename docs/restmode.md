# Rest Mode Resilience

zftpd survives PlayStation Rest Mode cycles by recreating dead listen sockets
and exposing a stable process identity so clients can reconnect cleanly.

There is **no** “enter rest mode” protocol command — Rest Mode is an environmental
event (sockets die / network stack drops / payload may be re-injected).

## Does activity resume after wake?

**Yes, if the payload process survives Rest Mode** (typical on PS4/PS5 when the
process is only suspended):

1. Existing FTP/HTTP TCP sessions die (the stack drops them)
2. zftpd detects the dead listen socket (`EBADF` / `ENOTSOCK` / liveness probe)
3. It recreates `bind`/`listen` with exponential backoff while the network is down
4. On success it shows an on-screen notification (`Server resumed` / `HTTP resumed`)
5. New client connections work again; FTP clients and ZHTTP reconnect

**No automatic resume if the payload was terminated** during Rest Mode — you must
re-inject zftpd. Clients then see a new `instance_id`.

## Scenarios

| Case | Payload process | Listen sockets | zftpd behaviour |
|------|-----------------|----------------|-----------------|
| Terminated | Dies | Gone | Requires re-inject; new `instance_id` |
| Suspended | Survives | Invalid (`EBADF` / `ENOTSOCK`) | Recreate listener with backoff |
| Suspended + stale FD | Survives | FD looks open, stack down | Liveness probe → recreate |

## Server (FTP)

`pal_resilient_accept()` (`src/pal_resilient_server.c`):

1. Detects listener loss on `accept()` (`EBADF`, `ENOTSOCK`, `EINVAL`)
2. Periodically probes the listen FD (`getsockopt(SO_TYPE)` / `SO_ACCEPTCONN`)
3. Recreates `socket` → `SO_REUSEADDR` → `bind` → `listen` with backoff:
   `500 → 1000 → 2000 → 4000 → 8000 → 10000 ms` (indefinite while running)
4. Every few failed attempts calls `pal_network_reinit()`
5. Sends an on-screen notification: `zftpd: Server resumed on <ip>:<port>`

Existing FTP control/data connections are **not** preserved (TCP is dead).
Clients reconnect; file resume remains FTP `REST` + `STOR`/`RETR`.

## Server (HTTP / ZHTTP)

`http_server.c` mirrors the same model on the event-loop listener:

1. `EVENT_ERROR` / `EVENT_CLOSE` or lost-errno on `accept()` schedules recreate
2. A detached thread retries with the same backoff
3. A wake pipe hands the new FD back to the event-loop thread for
   `event_loop_add` (handler table is not thread-safe)
4. Notification: `zftpd: HTTP resumed on port <n>`

## Daemon identity

Process-lifetime values from `ftp_instance.c`:

| Field | Meaning |
|-------|---------|
| `daemon_instance_id` | Random `uint64`, stable until process exit |
| `daemon_start_monotonic_ns` | `CLOCK_MONOTONIC` at first generation |

Exposed to clients via:

- **HTTP** `GET /api/status` → JSON `instance_id`, `start_monotonic_ns`, `version`, `pid`
- **FTP** `FEAT` line `XZFTPD INSTANCE <hex>`
- **FTP** `STAT` reply includes the instance hex

| `instance_id` vs previous | Meaning | Client action |
|---------------------------|---------|---------------|
| Same | Payload survived | Keep UI state, refresh listings |
| Different | Re-injected / new process | Treat remote handles as invalid |
| Missing / zero (legacy) | Old build | Skip rotation check |

## ZHTTP client reconnect

`web/js/api.js`:

- Health poll of `/api/status` every 5s
- Two consecutive transport failures → `WaitingForWake`
- Exponential backoff probe until `/api/status` succeeds
- `remoteReady()` gates POST/upload while stale/reconnecting
- Toasts via `app.js` on reconnect / daemon rotation

## What is preserved vs invalidated

**Preserved (same process):** daemon heap, session pool slots free for new clients,
`instance_id`, filesystem, config.

**Invalidated always:** live TCP sessions, PASV ports, in-flight transfers.

**Client-side preserved across ZHTTP reconnect:** current path, UI view, theme.
Listings are refreshed after `onReconnected`.

## Tests

```sh
make test
# includes tests/test_instance.c — identity stability + backoff helpers
```

## Hardware validation (recommended)

1. Start zftpd on PS5, connect FTP + open ZHTTP
2. Enter Rest Mode → wait → wake
3. Confirm notification “Server resumed” / “HTTP resumed”
4. FTP client reconnects; ZHTTP shows “Connection restored”
5. `GET /api/status` returns the **same** `instance_id` if the payload survived
6. Kill/re-inject payload → `instance_id` changes; ZHTTP reports daemon restart
