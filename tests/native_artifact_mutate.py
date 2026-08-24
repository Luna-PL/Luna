#!/usr/bin/env python3
"""Create one deterministic mutation inside an embedded Native proof."""

import pathlib
import sys


def main() -> int:
    if len(sys.argv) not in (3, 4, 6):
        return 2
    source = pathlib.Path(sys.argv[1]).read_bytes()
    magic = b"LUNANP1\0"
    offset = source.find(magic)
    if offset < 0 or source.find(magic, offset + 1) >= 0:
        return 3
    mutated = bytearray(source)
    field = sys.argv[3] if len(sys.argv) == 4 else "export"
    field_offset = {"export": 56, "dependency": 88}.get(field)
    if field_offset is None:
        return 4
    mutated[offset + field_offset] ^= 0x01
    pathlib.Path(sys.argv[2]).write_bytes(mutated)
    if len(sys.argv) == 6:
        trust_fields = pathlib.Path(sys.argv[4]).read_text(
            encoding="utf-8").rstrip("\n").split("\t")
        if len(trust_fields) != 7:
            return 5
        trust_fields[1] = bytes(
            mutated[offset + 56:offset + 88]).hex()
        pathlib.Path(sys.argv[5]).write_text(
            "\t".join(trust_fields) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
