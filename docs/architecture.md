# Architecture and repository layout

zftpd is organized by responsibility instead of by filename prefix.  A module
owns its implementation and headers, while generated code is kept out of the
source tree.

```text
src/
├── app/          process entry point and composition
├── ftp/          FTP protocol, sessions, commands and data path
├── http/         HTTP server, REST API, parsing and CSRF
├── transfer/     remote-to-local transfer manager and protocol backends
├── archive/      ZIP, PKG and exFAT/container readers
├── runtime/      event-loop implementations
└── platform/     platform abstraction layer
    └── ps5/      PS5-only networking/filter code

include/
├── ftp/
├── http/
├── transfer/
├── archive/
├── runtime/
└── platform/
    └── ps5/
```

## Dependency direction

The intended dependency direction is deliberately one-way:

```text
app
├── ftp ───────────────> platform
└── http
    ├── transfer ──────> external protocol libraries / POSIX
    ├── archive
    └── platform

runtime ───────────────> platform / operating-system event APIs
```

Lower-level modules must not import `app`.  `platform` must not depend on FTP
or HTTP policy.  Protocol-specific remote transfer code belongs in
`transfer/backends`, not in REST handlers or the PAL.

## Generated code

Generated files are build artifacts.  In particular, the embedded web UI and
PS5 CA bundle are generated under `build/<target>/<variant>/generated/` and are
never committed under `src/`.

This keeps source diffs reviewable and prevents a small web change from
creating tens of thousands of lines of generated C changes.

## External dependencies

Do not reimplement mature network protocols inside zftpd.  HTTP/HTTPS/FTP/FTPS
use libcurl and NFS uses libnfs.  PS5 consumes the PacBrew ports.  See
[`dependencies.md`](dependencies.md) for pinned versions and build details.

## Adding a feature

Choose the module by ownership, not by caller.  A new FTP command belongs in
`ftp/`; an HTTP route belongs in `http/`; a new remote source protocol belongs
in `transfer/`; a console syscall or compatibility layer belongs in
`platform/`.  If multiple modules need a facility, place it in the lowest
layer that can provide it without importing a higher-level policy module.
