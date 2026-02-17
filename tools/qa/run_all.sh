#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"

TARGET="${TARGET:-macos}"
ENABLE_ZHTTPD="${ENABLE_ZHTTPD:-0}"

echo "[1/6] Unit tests + ASan/UBSan (debug)"
make -C "$ROOT_DIR" TARGET="$TARGET" BUILD_TYPE=debug ENABLE_ZHTTPD="$ENABLE_ZHTTPD" test

echo "[2/6] Clang Static Analyzer"
make -C "$ROOT_DIR" TARGET="$TARGET" BUILD_TYPE=release ENABLE_ZHTTPD="$ENABLE_ZHTTPD" analyze

echo "[3/6] Integration: FTP roundtrip checksums"
"$ROOT_DIR/tools/qa/ftp_roundtrip.sh" "TARGET=$TARGET" "BUILD_TYPE=debug" "FTP_PORT=21234" "PARALLEL=4" "FILE_COUNT=6" "FILE_SIZE_BYTES=$((16*1024*1024))"

echo "[4/6] Integration: FTP roundtrip (bigger, more parallel)"
"$ROOT_DIR/tools/qa/ftp_roundtrip.sh" "TARGET=$TARGET" "BUILD_TYPE=debug" "FTP_PORT=21235" "PARALLEL=8" "FILE_COUNT=8" "FILE_SIZE_BYTES=$((32*1024*1024))"

echo "[5/6] Valgrind (if available, Linux only)"
if command -v valgrind >/dev/null 2>&1; then
  if [[ "$TARGET" == "linux" ]]; then
    make -C "$ROOT_DIR" TARGET=linux BUILD_TYPE=release ENABLE_ZHTTPD="$ENABLE_ZHTTPD" clean
    make -C "$ROOT_DIR" TARGET=linux BUILD_TYPE=release ENABLE_ZHTTPD="$ENABLE_ZHTTPD" all
    for t in build/linux/release/tests/*; do
      if [[ -x "$t" ]]; then
        valgrind --leak-check=full --error-exitcode=99 "$t"
      fi
    done
  else
    echo "Skipping Valgrind: TARGET=$TARGET (Valgrind is typically run on Linux)."
  fi
else
  echo "Skipping Valgrind: not installed."
fi

echo "[6/6] Done"
