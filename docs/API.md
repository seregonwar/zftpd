# zhttpd REST API Documentation

## Endpoints

### GET /api/status

Identity and build switches of the running daemon. The web interface polls it
for the connection state and to know which features the payload was compiled
with, so views can hide what the build does not have.

```json
{"ok":true,"version":"1.6.0","instance_id":"15114b8a2c31d9f0",
 "start_monotonic_ns":463980095296000,"pid":74,"platform":"ps4",
 "features":{"pkg_install":false}}
```

- `instance_id` changes when the payload is re-injected: clients treat it as a
  restart and drop their server-side handles.
- `features.pkg_install` mirrors the `ENABLE_PKG_INSTALL` build flag. When it is
  `false` the package installer endpoints answer `409 PKG installation is
  disabled for this build` and the UI hides the installer entirely.

### GET /api/list

List directory contents.

**Query Parameters:**
- `path` (required): Directory path

**Response:**
```json
{
  "path": "/home",
  "entries": [
    {
      "name": "file.txt",
      "type": "file",
      "size": 1024,
      "mode": 420
    },
    {
      "name": "subdir",
      "type": "directory",
      "size": 0,
      "mode": 493
    }
  ]
}
```

`mode` carries the permission bits (`st_mode & 0777`, so `420` = `0644` and
`493` = `0755`); the file manager uses it to pre-fill the *Permissions…* dialog
without a second request.

**Errors:**
- `404`: Directory not found
- `500`: Internal error

### GET /api/download

Download file (TODO).

### POST /api/chmod

Changes the Unix permission bits of one file or directory — the HTTP counterpart
of `SITE CHMOD`, used by the file manager's *Permissions…* dialog.

**Query Parameters:**
- `path` (required): file or folder, confined to the HTTP root.
- `mode` (required): octal mask, `000`–`777` (three digits, or four with a
  leading zero). Values above `777` — setuid/setgid/sticky — are rejected.

```text
POST /api/chmod?path=/data/homebrew/tool.elf&mode=755
```

```json
{"ok":true,"requested":"755","mode":"755","supported":true}
```

`mode` is read back from the filesystem after the call. FAT/exFAT mounts accept
`chmod()` and keep their own bits, in which case `supported` is `false` and
`mode` shows what the mount actually reports. A filesystem that refuses the call
answers `500`.

**Errors:**
- `400`: missing/invalid `mode` or missing `path`
- `403`: path outside the HTTP root
- `404`: path not found
- `405`: method is not `POST`
- `500`: the filesystem does not support permissions

### GET /api/mounts

Lists the removable volumes that are **really** connected (`/mnt/usb0`…`/mnt/usb7`,
`/mnt/ext0`, `/mnt/ext1`). The console creates those directories even with
nothing plugged in, so zftpd checks whether a filesystem is mounted on them
(different `st_dev` from the parent directory) and reports size and free space.
The file manager's Places list is built from this, so empty slots no longer
appear as drives.

```json
{"ok":true,"mounts":[{"path":"/mnt/usb0","name":"usb0","kind":"usb","total":32007397376,"free":12561055744}]}
```

### GET /api/archive/zip

Streams a ZIP archive of the selection — the bulk download used by the web UI
for "Download as ZIP". Entries are produced while the response is sent (no
temporary archive on the console), stored uncompressed, and folders are walked
recursively. ZIP64 records are emitted automatically for entries, offsets or
counts past the 32-bit limits, so multi-gigabyte selections stay valid.

The response announces the exact `Content-Length` (no chunked encoding), so
console browsers show progress and complete the download reliably.

**Query Parameters:**
- `path` (required, repeatable): file or folder, confined to the HTTP root.
- `name` (optional): download name, default `zftpd-<timestamp>.zip`.

Up to 64 paths per request; unreadable or unconfined entries are skipped, and
when the selection exceeds the entry limit the response carries
`X-Zftpd-Truncated: entry-limit`.

```text
GET /api/archive/zip?path=/data/games&path=/data/saves/config.bin&name=backup.zip
```

### GET /api/file/get

Streams a file to the client. By default the raw on-disk bytes are sent
(zero-copy `sendfile`), exactly like every other transfer path.

**Query Parameters:**
- `path` (required): file to send, confined to the HTTP root.
- `decrypt` (optional): `1` asks for the **decrypted** content when the file is
  a SELF container (PS4/PS5). The kernel pager is swapped in for the transfer
  (MAP_SELF), so the client receives the plain executable instead of the
  encrypted container. Other files are always sent untouched, and the response
  carries `X-Zftpd-Decrypted: self` plus chunked encoding.

The web UI exposes this as *Settings → Downloads → Decrypt protected files*,
which appends the flag to every download and preview link.

When the file is recognised as a SELF container but its payload cannot be
decrypted (no loadable segments, unsupported layout, unreadable tail), the
request is refused with `422 Unprocessable Content` and a JSON body naming the
reason, instead of quietly streaming the encrypted container. Plain files are
unaffected — they are always sent as they are.

```text
GET /api/file/get?path=/mnt/disc/CUSA00001/sce_sys/eboot.bin&decrypt=1
```

### GET /api/file/decrypt_check

Answers whether `decrypt=1` would produce the decrypted ELF, so a client can ask
before starting the transfer (the web UI checks single-file downloads).

**Query Parameters:**
- `path` (required): file to inspect, confined to the HTTP root.

```json
{"ok":true,"self":true,"decryptable":false,"size":12582912,
 "reason":"SELF container has no loadable segments to decrypt"}
```

`self` is `false` for ordinary files (nothing to decrypt, the original bytes are
what you want) and `decryptable` is only `true` when the pager accepted the
container. The per-attempt detail (`magic`, entry count, `errno`) goes to the
daemon log as a `[self] cannot decrypt …` line.

### Game dump

Decrypted dump of an installed title, read from the running title's sandbox
mount (`/mnt/sandbox/<TITLEID>-app0[-patch0-union]` on PS4, `_000/app0` and
friends under `/mnt/sandbox` on PS5), where the kernel exposes the application
in the clear; SELF/SPRX entries are decrypted through the self pager while they
are read. There is no fallback to the encrypted `/user/app` image, so a title
that is not running is launched first and the job waits for its sandbox to
mount. One dump runs at a time.

```text
POST /api/dump/start
{"title_id":"CUSA00001","target":"download","format":"zip","decrypt":1}
{"title_id":"CUSA00001","target":"local","dest":"/mnt/usb0/homebrew","format":"files"}

GET  /api/dump/status          # state, source, entries, size, bytes_done
GET  /api/dump?id=N            # streams the prepared archive (Content-Length)
POST /api/dump/cancel          # {"id":N} optional; the single slot is implied
```

`target` is `download` (default; a streamed ZIP the client then fetches with
`GET /api/dump?id=N`) or `local`. `format` is `zip` (default) or `files`
(console targets only — a streamed dump is always a ZIP). `decrypt` defaults
to `1`; `0` keeps the raw SELF containers.

`POST /api/dump/start` answers immediately with the slot id and the initial
state; `source`, `entries` and `size` become available through the status as
soon as the job worker has walked the source tree:

```json
{"ok":true,"id":1,"state":"launching","title_id":"CUSA00001","target":"download"}
```

`GET /api/dump/status` (the response the web UI polls):

```json
{"ok":true,"active":true,"id":1,"state":"running","title_id":"CUSA00001",
 "source":"/mnt/sandbox/CUSA00001_000/app0","entries":1234,"size":32212254720,
 "bytes_done":1073741824,"started":1790778983,"to_console":false,
 "cancelled":false,"message":""}
```

`state` is `launching`, `running`, `ready` (streamed dump prepared, waiting for
the download), `done` or `failed`; `message` carries the failure reason
(`Cancelled` after a cancel request). A slot that stops advancing is retired by
its deadline and reported as `failed`, so a broken dump never blocks the next
one.

`POST /api/dump/cancel` stops the job — the local writer gives up at the next
chunk, a prepared archive is dropped, a streamed download ends early:

```json
{"ok":true,"id":1,"state":"running","cancelled":true}
```

### POST /api/system/eject

Ejects the Blu-ray disc from the console's optical drive (PS4/PS5). The drive
is a FreeBSD `cd(4)` device: zftpd sends `CDIOCALLOW` + `CDIOCEJECT` to
`/dev/cd0` and shows a toast on success.

```json
{"ok":true,"status":"ok","message":"Disc ejected","code":0}
```

Failures answer HTTP 200 with `ok:false` and the driver reason, so a client can
tell a Digital Edition console (`ENOENT`) from a busy drive (`EBUSY`):

```json
{"ok":false,"status":"error","message":"Drive busy: a disc game is running","code":-16}
```

### GET /api/system/disc

Reports whether the console has an optical drive, so the UI can hide the action
on Digital Edition consoles.

```json
{"ok":true,"present":true}
```

### GET|POST /api/notify

Show a custom system notification on PS4/PS5 (toast). On non-console builds the
message is forwarded to syslog. Designed for local-network automation such as
Home Assistant.

**Query Parameters:**
- `text` (required): Notification message (URL-encoded). Max 1023 bytes after decode.
- `icon` (optional): PS notification texture suffix, default `icon_system`.
  Allowed characters: `[A-Za-z0-9_]`.

**Examples:**
```
GET /api/notify?text=Washing%20machine%20is%20done
GET /api/notify?text=Doorbell&icon=icon_system
```

**Home Assistant (`rest_command`):**
```yaml
rest_command:
  ps5_notify:
    url: "http://{{ states('sensor.ps5_ip') }}:2121/api/notify?text={{ text | urlencode }}"
    method: GET
```

**Response:**
```json
{
  "ok": true,
  "text": "Washing machine is done",
  "icon": "icon_system"
}
```

**Errors:**
- `400`: Missing/empty `text`, control characters, or invalid `icon`
- `405`: Method other than GET/POST

> Prefer **GET** for automation clients. POST is subject to CSRF validation when
> web upload support is enabled.

### Static Files

- `GET /` → `index.html`
- `GET /style.css` → CSS
- `GET /app.js` → JavaScript

## Adding Custom Endpoints

Edit `src/http/http_api.c`:

```c
http_response_t* http_api_handle(const http_request_t *request) {
    if (strncmp(request->uri, "/api/custom", 11) == 0) {
        return handle_custom(request);
    }
    // ... existing handlers
}
```

## Package installation

`GET|POST /api/admin/games/install`

| Parameter | Meaning |
| --------- | ------- |
| `path` | Package already on console storage. |
| `url` | Package the console downloads by itself (http/https). |
| `dest` | `internal` (default) or `extended`. |
| `overwrite` | `1` replaces an existing installation of the same title. |

Exactly one of `path` and `url` is required. When the package is already
installed the request answers `409` with
`{"error":"already_installed","title_id":…,"can_overwrite":true}` and nothing is
changed; the caller repeats it with `overwrite=1` to replace the installation.

`GET /api/admin/games/install_status` reports the running task:

```json
{"ok":true,"active":true,"task_id":-1,"progress":42,"error":0,
 "length":0,"transferred":0,"title_id":"","path":"https://…/package.pkg"}
```

On PS5 the installation is driven by a helper program that zftpd delivers to the
loader on loopback, because the system installer API is only usable from a
process the loader spawned. On PS4 the download service performs it directly.
