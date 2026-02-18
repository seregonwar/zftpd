# zftpd - Zero-copy FTP Daemon compatible with all POSIX systems

[![C11](https://img.shields.io/badge/std-C11-blue.svg)](https://en.cppreference.com/w/c/11)
[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
<img src="https://img.shields.io/github/downloads/seregonwar/zftpd/total?style=flat-square&color=success" alt="Downloads"/><br/>
Multi-platform FTP server designed to run both as a POSIX binary (Linux/macOS) and as a console payload (PS3/PS4/PS5).

## Key Features

- Robust TCP I/O (handling partial sends, EINTR, backpressure)
- Control/data channel timeouts + session idle timeout
- Path hardening (no traversal + best-effort canonicalization)
- Structured logging per session/command
- Transfer rate limiting (optional, compile-time)
- On-screen notifications on PS4/PS5 (IP/port and status)

## Build

Artifacts are placed in `build/<target>/<build_type>/`.

### Requirements

- C11 Toolchain (gcc/clang) + `make`
- To generate `.bin`: `objcopy` (binutils or llvm-objcopy). Alternatively, use SDK wrappers:
  - PS4: `OBJCOPY=orbis-objcopy`
  - PS5: `OBJCOPY=prospero-objcopy`
- PS4: `PS4_PAYLOAD_SDK` set
- PS5: `PS5_PAYLOAD_SDK` set

### Commands

```bash
# Linux
make TARGET=linux

# macOS
make TARGET=macos

# PS4
make TARGET=ps4

# PS5
make TARGET=ps5

# Debug build
make TARGET=linux BUILD_TYPE=debug
```

### Output (what to use)

Artifacts are versioned and platform-tagged (examples):

- Linux: `build/linux/release/zftpd-linux-<arch>-v<version>.elf`
- macOS: `build/macos/release/zftpd-macos-<arch>-v<version>`
- PS4: `build/ps4/release/zftpd-ps4-v<version>.bin` + `zftpd-ps4-v<version>.elf`
- PS5: `build/ps5/release/zftpd-ps5-v<version>.bin` + `zftpd-ps5-v<version>.elf`

## Running (Daemon) by Platform

### Linux

```bash
./build/linux/release/zftpd-linux-<arch>-v<version>.elf
```

Useful options (POSIX only): `-p <port>` and `-d <root>`.

### macOS

```bash
./build/macos/release/zftpd-macos-<arch>-v<version>
```

### PS4

- Requires a *payload loader* (e.g. WebKit/PPPwn/Netcat/GoldHEN). `zftpd` does not require a resident "HEN", but must be launched by a loader/exploit.
- If the loader expects a `.bin` payload: send `build/ps4/release/zftpd-ps4-v<version>.bin`.
- If the loader accepts ELF: you can choose between `build/ps4/release/zftpd-ps4-v<version>.bin` and `build/ps4/release/zftpd-ps4-v<version>.elf`.

On startup, it shows a notification with IP and port.

### PS5

- Requires a *payload loader* (etaHEN/Netcat/equivalent loader).
- If the loader expects a `.bin` payload: send `build/ps5/release/zftpd-ps5-v<version>.bin`.
- If the loader accepts ELF: you can choose between `build/ps5/release/zftpd-ps5-v<version>.bin` and `build/ps5/release/zftpd-ps5-v<version>.elf`.

On startup, it shows "started" notifications + `FTP: <ip>:<port>`.

## Configuration

Compile-time configuration is in [ftp_config.h](include/ftp_config.h). Useful macros:

- `FTP_DEFAULT_PORT` (PS4/PS5 default 2122, POSIX default 2121)
- `FTP_MAX_SESSIONS`
- `FTP_SESSION_TIMEOUT`
- `FTP_TRANSFER_RATE_LIMIT_BPS` / `FTP_TRANSFER_RATE_BURST_BYTES`
- `FTP_LOG_COMMANDS`

## Notes

- If you see "payload already loaded" on PS4/PS5, it means an instance is already active (dedup). The new daemon will try to terminate the old instance and start a new one on port `FTP_DEFAULT_PORT:2122`. If that fails, it will try the next port `FTP_DEFAULT_PORT+1:2123` up to a maximum of 9 subsequent ports.
- For host testing: `make TARGET=linux test` or `make TARGET=macos test`.

## Support

If you find `zftpd` useful and would like to support its ongoing development, consider making a donation. Any contribution is greatly appreciated!

[![Donate PayPal](https://img.shields.io/badge/Donate-PayPal-00457C?style=flat-square&logo=paypal&logoColor=white)](https://www.paypal.com/paypalme/seregonwar)
[![Support Ko-fi](https://img.shields.io/badge/Support-Ko--fi-F16061?style=flat-square&logo=ko-fi&logoColor=white)](https://ko-fi.com/seregon)
[![Sponsor GitHub](https://img.shields.io/badge/Sponsor-GitHub-EA4AAA?style=flat-square&logo=github&logoColor=white)](https://github.com/sponsors/seregonwar)

## License

MIT — see [LICENSE](LICENSE).
