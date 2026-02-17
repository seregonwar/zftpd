#!/usr/bin/env bash
set -euo pipefail

for arg in "$@"; do
  case "$arg" in
    *=*) export "$arg" ;;
  esac
done

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_TYPE="${BUILD_TYPE:-debug}"
TARGET="${TARGET:-macos}"
FTP_PORT="${FTP_PORT:-21234}"
PARALLEL="${PARALLEL:-4}"
FILE_COUNT="${FILE_COUNT:-8}"
FILE_SIZE_BYTES="${FILE_SIZE_BYTES:-33554432}"
USE_APPE="${USE_APPE:-0}"
USE_REST="${USE_REST:-0}"

if [[ "$TARGET" == "macos" ]]; then
  if [[ -x "$ROOT_DIR/build/$TARGET/$BUILD_TYPE/zftpd" ]]; then
    FTPD_BIN="$ROOT_DIR/build/$TARGET/$BUILD_TYPE/zftpd"
  elif [[ -x "$ROOT_DIR/build/$TARGET/$BUILD_TYPE/ftpd" ]]; then
    FTPD_BIN="$ROOT_DIR/build/$TARGET/$BUILD_TYPE/ftpd"
  else
    FTPD_BIN="$(ls -1t "$ROOT_DIR/build/$TARGET/$BUILD_TYPE"/zftpd-macos-*-v* 2>/dev/null | head -n 1 || true)"
  fi
else
  if [[ -x "$ROOT_DIR/build/$TARGET/$BUILD_TYPE/zftpd.elf" ]]; then
    FTPD_BIN="$ROOT_DIR/build/$TARGET/$BUILD_TYPE/zftpd.elf"
  elif [[ -x "$ROOT_DIR/build/$TARGET/$BUILD_TYPE/ftpd.elf" ]]; then
    FTPD_BIN="$ROOT_DIR/build/$TARGET/$BUILD_TYPE/ftpd.elf"
  else
    FTPD_BIN="$(ls -1t "$ROOT_DIR/build/$TARGET/$BUILD_TYPE"/zftpd-"$TARGET"-v*.elf 2>/dev/null | head -n 1 || true)"
  fi
fi

if [[ ! -x "$FTPD_BIN" ]]; then
  echo "error: server binary not found for TARGET=$TARGET BUILD_TYPE=$BUILD_TYPE" >&2
  exit 1
fi

if command -v md5sum >/dev/null 2>&1; then
  md5cmd() { md5sum "$1" | awk '{print $1}'; }
elif command -v md5 >/dev/null 2>&1; then
  md5cmd() { md5 -q "$1"; }
else
  md5cmd() { python3 - "$1" <<'PY'
import hashlib,sys
p=sys.argv[1]
h=hashlib.md5()
with open(p,'rb') as f:
  for b in iter(lambda:f.read(1024*1024), b''):
    h.update(b)
print(h.hexdigest())
PY
  }
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/zftpd-qa.XXXXXXXX")"
cleanup() {
  if [[ -n "${FTPD_PID:-}" ]]; then
    kill "$FTPD_PID" 2>/dev/null || true
    wait "$FTPD_PID" 2>/dev/null || true
  fi
  if [[ "${KEEP_WORK:-0}" != "1" ]]; then
    rm -rf "$WORK"
  else
    echo "KEEP_WORK=1 workdir=$WORK"
  fi
}
trap cleanup EXIT

pushd "$ROOT_DIR" >/dev/null
"$FTPD_BIN" -p "$FTP_PORT" >"$WORK/ftpd.log" 2>&1 &
FTPD_PID=$!
popd >/dev/null

sleep 0.2
if ! kill -0 "$FTPD_PID" 2>/dev/null; then
  echo "error: ftpd exited early" >&2
  sed -n '1,200p' "$WORK/ftpd.log" >&2 || true
  exit 2
fi

python3 - "$FTP_PORT" <<'PY'
import socket,sys,time
port=int(sys.argv[1])
deadline=time.time()+5
while time.time()<deadline:
  s=socket.socket()
  s.settimeout(0.3)
  try:
    s.connect(("127.0.0.1", port))
    s.close()
    sys.exit(0)
  except Exception:
    time.sleep(0.1)
  finally:
    try: s.close()
    except Exception: pass
print("port not ready", port, file=sys.stderr)
sys.exit(1)
PY

python3 - "$WORK" "$FTP_PORT" "$PARALLEL" "$FILE_COUNT" "$FILE_SIZE_BYTES" "$USE_APPE" "$USE_REST" <<'PY'
import os, sys, threading, time
from ftplib import FTP, error_perm

work=sys.argv[1]
port=int(sys.argv[2])
parallel=int(sys.argv[3])
file_count=int(sys.argv[4])
file_size=int(sys.argv[5])
use_appe=int(sys.argv[6])!=0
use_rest=int(sys.argv[7])!=0

host="127.0.0.1"
remote_dir="qa"

def randfile(path, n):
  with open(path, "wb") as f:
    left=n
    while left>0:
      chunk=min(left, 1024*1024)
      f.write(os.urandom(chunk))
      left-=chunk

def ftp_mkdir_p(ftp, path):
  parts=[p for p in path.split("/") if p]
  cur=""
  for p in parts:
    cur = p if not cur else (cur + "/" + p)
    try:
      ftp.mkd(cur)
    except error_perm:
      pass

def upload_download_one(i):
  src=os.path.join(work, f"src_{i}.bin")
  dst=os.path.join(work, f"dst_{i}.bin")
  randfile(src, file_size)

  ftp=FTP()
  ftp.connect(host, port, timeout=30)
  ftp.login("anonymous","x")
  ftp.voidcmd("TYPE I")
  ftp_mkdir_p(ftp, remote_dir)
  remote_path=f"{remote_dir}/file_{i}.bin"

  with open(src, "rb") as f:
    if use_appe:
      try:
        ftp.delete(remote_path)
      except Exception:
        pass
      ftp.storbinary("STOR "+remote_path, f, blocksize=64*1024)
      f.seek(0)
      ftp.storbinary("APPE "+remote_path, f, blocksize=64*1024)
    else:
      ftp.storbinary("STOR "+remote_path, f, blocksize=64*1024)

  with open(dst, "wb") as out:
    ftp.retrbinary("RETR "+remote_path, out.write, blocksize=64*1024)

  if use_rest:
    off=max(1, file_size//2)
    rest_out=os.path.join(work, f"rest_{i}.bin")
    with open(rest_out, "wb") as out:
      ftp.retrbinary("RETR "+remote_path, out.write, blocksize=64*1024, rest=off)
    with open(src, "rb") as fsrc, open(rest_out, "rb") as frest:
      fsrc.seek(off)
      if fsrc.read(1024*1024) != frest.read(1024*1024):
        raise RuntimeError("REST mismatch")

  ftp.quit()
  return src, dst

jobs=list(range(file_count))
lock=threading.Lock()
results=[]
errors=[]

def worker():
  while True:
    with lock:
      if not jobs:
        return
      i=jobs.pop()
    try:
      results.append(upload_download_one(i))
    except Exception as e:
      errors.append((i, repr(e)))

threads=[threading.Thread(target=worker) for _ in range(parallel)]
for t in threads: t.start()
for t in threads: t.join()

if errors:
  print("errors:", errors)
  sys.exit(2)

print("ok", len(results))
PY

ok=0
for ((i=0;i<FILE_COUNT;i++)); do
  SRC="$WORK/src_${i}.bin"
  DST="$WORK/dst_${i}.bin"
  if [[ ! -f "$SRC" || ! -f "$DST" ]]; then
    echo "error: missing $SRC or $DST" >&2
    exit 3
  fi
  A="$(md5cmd "$SRC")"
  B="$(md5cmd "$DST")"
  if [[ "$A" != "$B" ]]; then
    echo "error: checksum mismatch for $i ($A != $B)" >&2
    exit 4
  fi
  ok=$((ok+1))
done

echo "ftp_roundtrip_ok files=$ok parallel=$PARALLEL size=$FILE_SIZE_BYTES port=$FTP_PORT"
