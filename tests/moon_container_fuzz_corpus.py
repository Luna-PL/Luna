#!/usr/bin/env python3
"""Generate a reproducible Moon Container libFuzzer seed corpus.

The generator preserves any coverage-discovered files already present in the
output directory and only refreshes its stable ``seed-*`` entries.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import subprocess


MAGIC = b"\x89MOON\r\n\x1a"
HEADER_SIZE = 80
DIRECTORY_ENTRY_SIZE = 32
DIGEST_OFFSET = 48


def align_eight(value: int) -> int:
    return (value + 7) & ~7


def digest(encoded: bytes) -> bytes:
    material = bytearray(encoded)
    material[DIGEST_OFFSET : DIGEST_OFFSET + 32] = bytes(32)
    return hashlib.sha256(material).digest()


def authenticate(encoded: bytearray) -> bytes:
    encoded[DIGEST_OFFSET : DIGEST_OFFSET + 32] = bytes(32)
    encoded[DIGEST_OFFSET : DIGEST_OFFSET + 32] = digest(encoded)
    return bytes(encoded)


def encode_framing(sections: list[tuple[int, bytes]]) -> bytes:
    sections = sorted(sections)
    directory_end = HEADER_SIZE + len(sections) * DIRECTORY_ENTRY_SIZE
    cursor = align_eight(directory_end)
    rows: list[tuple[int, int, bytes]] = []
    for section_id, payload in sections:
        rows.append((section_id, cursor, payload))
        cursor = align_eight(cursor + len(payload))
    file_size = rows[-1][1] + len(rows[-1][2]) if rows else HEADER_SIZE
    encoded = bytearray(file_size)
    encoded[:8] = MAGIC
    struct.pack_into("<IIIIIIQQ", encoded, 8,
                     1, 0, HEADER_SIZE, 0, len(rows), 0,
                     HEADER_SIZE, file_size)
    for index, (section_id, offset, payload) in enumerate(rows):
        base = HEADER_SIZE + index * DIRECTORY_ENTRY_SIZE
        struct.pack_into("<IIQQQ", encoded, base,
                         section_id, 0, offset, len(payload), len(payload))
        encoded[offset : offset + len(payload)] = payload
    return authenticate(encoded)


def directory(encoded: bytes) -> list[tuple[int, int, int]]:
    count = struct.unpack_from("<I", encoded, 24)[0]
    result = []
    for index in range(count):
        base = HEADER_SIZE + index * DIRECTORY_ENTRY_SIZE
        section_id, _, offset, length, _ = struct.unpack_from(
            "<IIQQQ", encoded, base)
        result.append((section_id, offset, length))
    return result


def authenticated_payload_flip(encoded: bytes, section_id: int) -> bytes:
    mutated = bytearray(encoded)
    for candidate, offset, length in directory(encoded):
        if candidate == section_id and length:
            mutated[offset + length // 2] ^= 0x80
            return authenticate(mutated)
    raise RuntimeError(f"section {section_id} has no payload")


def write_seed(output: pathlib.Path, name: str, payload: bytes) -> None:
    (output / name).write_bytes(payload)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--compiler", required=True, type=pathlib.Path)
    parser.add_argument("--package", required=True, type=pathlib.Path)
    parser.add_argument("--output-dir", required=True, type=pathlib.Path)
    args = parser.parse_args()

    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    (output / "artifacts").mkdir(exist_ok=True)

    full = output / "seed-valid-application.moon"
    subprocess.run(
        [str(args.compiler.resolve()), "build", str(args.package.resolve()),
         "-t", "moon", "-o", str(full)],
        check=True, capture_output=True, text=True)
    full_bytes = full.read_bytes()

    empty_sections = [(section_id, b"") for section_id in range(1, 9)]
    framing = encode_framing(empty_sections)
    optional = encode_framing(
        empty_sections + [(0x80000011, b"optional-fuzz-seed")])
    write_seed(output, "seed-valid-empty-framing.moon", framing)
    write_seed(output, "seed-valid-optional-framing.moon", optional)
    write_seed(output, "seed-truncated-header.moon", full_bytes[:79])

    corrupt_magic = bytearray(full_bytes)
    corrupt_magic[0] ^= 1
    write_seed(output, "seed-corrupt-magic.moon", bytes(corrupt_magic))
    corrupt_digest = bytearray(full_bytes)
    corrupt_digest[DIGEST_OFFSET] ^= 1
    write_seed(output, "seed-corrupt-digest.moon", bytes(corrupt_digest))
    for section_id, label in ((1, "manifest"), (2, "types"), (5, "code")):
        write_seed(
            output, f"seed-authenticated-{label}-flip.moon",
            authenticated_payload_flip(full_bytes, section_id))

    missing_required = bytearray(framing)
    final_row = HEADER_SIZE + 7 * DIRECTORY_ENTRY_SIZE
    struct.pack_into("<I", missing_required, final_row, 0x80000012)
    write_seed(
        output, "seed-authenticated-missing-required.moon",
        authenticate(missing_required))
    write_seed(output, "seed-random-prefix", b"MOONFUZZ\x00\xff\x7f")

    seeds = sorted(path.name for path in output.glob("seed-*"))
    print(f"moon-container-fuzz-corpus: {len(seeds)} seeds in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
