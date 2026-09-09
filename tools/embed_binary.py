#!/usr/bin/env python3
"""Generate a small C header containing an embedded binary asset."""

from pathlib import Path
import argparse


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("source", type=Path)
    parser.add_argument("destination", type=Path)
    parser.add_argument("symbol")
    args = parser.parse_args()

    data = args.source.read_bytes()
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    guard = f"ZFTPD_GENERATED_{args.symbol.upper()}_H"
    with args.destination.open("w", encoding="ascii") as out:
        out.write(f"#ifndef {guard}\n#define {guard}\n\n")
        out.write("#include <stddef.h>\n\n")
        out.write(f"static unsigned char {args.symbol}[] = {{\n")
        for offset in range(0, len(data), 16):
            chunk = data[offset : offset + 16]
            values = ", ".join(f"0x{byte:02x}" for byte in chunk)
            out.write(f"  {values},\n")
        out.write("};\n")
        out.write(f"static const size_t {args.symbol}_len = sizeof({args.symbol});\n\n")
        out.write(f"#endif /* {guard} */\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
