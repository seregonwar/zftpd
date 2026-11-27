# Changelog

All notable changes to zftpd. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and releases use
[Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

## [1.7.1] - 2026-11-27

Storage that the console reported as *Other* and never gave back: packages
staged by the web installer could stay on disk forever, invisible to the user.

### Fixed

- **Staged PKGs are removed when an install fails, not only when it succeeds.**
  Packages uploaded from the web UI land in `/data/zftpd/pkg/`, a directory
  nobody browses. On PS5 they were deleted only after a successful install, so
  an installer `ERROR`, a helper that never connected, an unreachable ELF loader
  or a refused request left the whole PKG behind. Any final outcome now discards
  it. A helper that vanishes without a verdict leaves the file alone, since the
  system installer may still be reading it.
- **Declining "Already installed, replace it?" no longer leaves the upload
  behind.** The web UI deletes the staged package whenever the install does not
  start.
- **Interrupted web uploads are removed.** *Stop sending*, a dropped connection
  or a write error used to leave a truncated file at the target path; it is now
  deleted, as an interrupted FTP `STOR` already was.
- **Startup sweep of stale staged packages.** Files in `/data/zftpd/pkg/` older
  than six hours are removed when the payload starts, which also gives back the
  space lost before this release. On PS5 the sweep only runs once the previous
  instance has actually stopped.
- `pal_file_close()` no longer retries `close()` after `EINTR`. FreeBSD/OrbisOS
  release the descriptor before `EINTR` can be reported, so the retry could
  close a descriptor another session had just opened (an upload, a socket).

### Notes

- Deleting files over FTP was already correct: `DELE`/`RMD` unlink immediately
  and no transfer path keeps a descriptor on a deleted file. If space still looks
  used after an FTP delete, check `/data/zftpd/pkg/` and whether the figure
  updates after a reboot.

## [1.7.0] - 2026-10-28

Closes the four issues opened against v1.6.0 (#13, #14, #17, #18) and picks up
the FTP protocol fixes from upstream PRs #15, #16 and #19.

### Added

- **PS5 firmware 14.00 and 14.10 support**, alongside every firmware supported so
  far (1.00 … 13.60).
- **Permission editing from the web UI** (#14): a *Permissions…* dialog in the
  file manager, the `POST /api/chmod` endpoint behind it and the `mode` field in
  `/api/list`, so the `rwx` bits can be read and changed without an FTP client.
- **SELF decryption pre-check** (#17): `GET /api/file/decrypt_check` reports
  whether a container can be decrypted, and a single-file download asks before
  saving one that would arrive still encrypted.
- **Native PS5 uninstall** through the embedded installer helper: uninstall no
  longer needs a reboot on 13.60.
- `WERROR=0` make knob, for host toolchains whose optimiser warnings are not
  actionable; release builds keep `-Werror`.

### Changed

- **Startup no longer dead-ends on a previous instance** (#18). `instance.pid`
  is a control identity, not a lock: the saved token authenticates the
  cooperative stop, stale/reused/malformed identities are ignored, and a payload
  that survives SIGTERM is left running instead of demanding a console reboot.
  The new instance starts beside it on the first free FTP port in 2120-2129 and
  the toast names the port that works; a busy HTTP port is reported instead of
  failing silently.
- **Downloads degrade to buffered I/O** (#13) instead of aborting. When
  `sendfile()` is unavailable (encrypted data channel, rate limiter) or fails
  mid-transfer on a mount whose vnode pager cannot serve it, RETR and
  `/api/file/get` continue with `pread`/`read` + send.
- **FEAT replies follow RFC 2389** and multiline replies follow RFC 959: numeric
  intermediate lines are prefixed with a space (PR #15, #19).
- `SIZE` answers `550` for directories, FIFOs and other non-regular files instead
  of their metadata size (PR #19).
- PS5 install and uninstall error codes are parsed as signed native values, so a
  failure is no longer reported as success (PR #15, #16).

### Fixed

- #18, *previous instance is stuck, restart console*: a stale `instance.pid`
  naming a reused or vanished pid no longer blocks startup.
- #13, *PS5 hangs on FTP download from an exFAT image mount*: buffered fallback
  in both the FTP and the HTTP download path.
- #17, *Decrypt protected files does not decrypt `/decid_update.elf`*: an
  undecryptable SELF now answers `422` with the reason, logs a
  `[self] cannot decrypt` line and is never served as the encrypted container
  without saying so.
- #14, *Allow permission editing*: `rwx` bits are editable from the web UI.
- A browser that cancels a download no longer kills the process: socket writes use
  `MSG_NOSIGNAL` and `sendfile()` blocks and drains `SIGPIPE` on Linux, so the
  guarantee also holds for the FFI library and test harnesses that install no
  signal handler.
- Two host tests no longer overflow a `realpath()` buffer under `_FORTIFY_SOURCE`
  (`test_http_api_common`, `test_http_response_stream`); `test_transfer_queue`
  skips cleanly when libcurl is absent.

### Tests

- `test_instance_control` covers stale, reused, missing, zombie and malformed
  identities plus both conflict paths.
- New coverage for the console `sendfile` fallback, encrypted RETR, `chmod` and
  `decrypt_check`.
- Suite: 37 passed, 0 failed, 3 skipped (the libcurl-only tests).

### Notes

- PS5 zhttp builds still need the PacBrew `ps5-payload-curl` and
  `ps5-payload-libnfs` portlibs; without them `make TARGET=ps5` stops with a clear
  message. `ENABLE_LIBCURL=0 ENABLE_LIBNFS=0` builds the payload without the
  download backends.
- FAT/exFAT mounts accept `chmod()` and keep their own bits: `/api/chmod` reports
  the effective mode with `"supported": false`, and the web UI repeats it.

[Unreleased]: https://github.com/seregonwar/zftpd/compare/v1.7.1...HEAD
[1.7.1]: https://github.com/seregonwar/zftpd/compare/v1.7.0...v1.7.1
[1.7.0]: https://github.com/seregonwar/zftpd/compare/v1.6.0...v1.7.0
