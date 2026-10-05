#!/usr/bin/env python3
"""XOR a payload so nested ELFs are not trivially carved from the injector."""
import sys

KEY = bytes((0xA7, 0x3C, 0x91, 0x5E))


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: mkblob.py <in> <out>", file=sys.stderr)
        return 2
    data = bytearray(open(sys.argv[1], "rb").read())
    if not data:
        print("empty blob", file=sys.stderr)
        return 1
    for i, b in enumerate(data):
        data[i] = b ^ KEY[i & 3]
    open(sys.argv[2], "wb").write(data)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
