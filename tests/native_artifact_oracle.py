#!/usr/bin/env python3
"""Independent Luna Native proof-v1 framing and digest oracle."""

import argparse
import hashlib
import pathlib
import struct
from typing import List, Optional, Tuple


MAGIC = b"LUNANP1\0"
RECORD_SIZE = 504


def fixed_string(record: bytes, offset: int, size: int) -> str:
    field = record[offset : offset + size]
    end = field.find(b"\0")
    if end <= 0:
        raise ValueError("invalid fixed proof string")
    return field[:end].decode("utf-8")


def digest_list(values: List[str]) -> bytes:
    canonical = bytearray(struct.pack("<I", len(set(values))))
    for value in sorted(set(values)):
        encoded = value.encode("utf-8")
        canonical += struct.pack("<I", len(encoded)) + encoded
    return hashlib.sha256(canonical).digest()


def elf_dependencies(data: bytes) -> List[str]:
    is_64 = data[4] == 2
    endian = "<" if data[5] == 1 else ">"
    if is_64:
        phoff = struct.unpack_from(endian + "Q", data, 32)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 54)
    else:
        phoff = struct.unpack_from(endian + "I", data, 28)[0]
        phentsize, phnum = struct.unpack_from(endian + "HH", data, 42)
    loads: List[Tuple[int, int, int]] = []
    dynamic: Optional[Tuple[int, int]] = None
    for index in range(phnum):
        offset = phoff + index * phentsize
        kind = struct.unpack_from(endian + "I", data, offset)[0]
        if is_64:
            file_offset, address = struct.unpack_from(endian + "QQ", data, offset + 8)
            file_size = struct.unpack_from(endian + "Q", data, offset + 32)[0]
        else:
            file_offset, address = struct.unpack_from(endian + "II", data, offset + 4)
            file_size = struct.unpack_from(endian + "I", data, offset + 16)[0]
        if kind == 1:
            loads.append((address, file_offset, file_size))
        elif kind == 2:
            dynamic = (file_offset, file_size)
    if dynamic is None:
        raise ValueError("ELF image has no dynamic table")
    entry_format = endian + ("qQ" if is_64 else "iI")
    entry_size = struct.calcsize(entry_format)
    needed: List[int] = []
    string_address = 0
    string_size = 0
    for offset in range(dynamic[0], dynamic[0] + dynamic[1], entry_size):
        tag, value = struct.unpack_from(entry_format, data, offset)
        if tag == 0:
            break
        if tag == 1:
            needed.append(value)
        elif tag == 5:
            string_address = value
        elif tag == 10:
            string_size = value
    string_offset = None
    for address, file_offset, file_size in loads:
        if address <= string_address < address + file_size:
            string_offset = file_offset + string_address - address
            break
    if string_offset is None or string_size == 0:
        raise ValueError("cannot map ELF dynamic string table")
    result = []
    for relative in needed:
        end = data.find(b"\0", string_offset + relative,
                        string_offset + string_size)
        if end < 0:
            raise ValueError("unterminated ELF dependency")
        result.append(data[string_offset + relative : end].decode("utf-8"))
    return result


def pe_dependencies(data: bytes) -> List[str]:
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", data, optional)[0]
    directory = optional + (112 if magic == 0x20B else 96)
    image_base = struct.unpack_from(
        "<Q" if magic == 0x20B else "<I", data,
        optional + (24 if magic == 0x20B else 28))[0]
    section_table = optional + optional_size
    mappings = []
    for index in range(sections):
        offset = section_table + index * 40
        virtual_size, address, raw_size, raw_offset = struct.unpack_from(
            "<IIII", data, offset + 8)
        mappings.append((address, max(virtual_size, raw_size), raw_offset))

    def rva_offset(rva: int) -> int:
        for address, size, raw_offset in mappings:
            if address <= rva < address + size:
                return raw_offset + rva - address
        raise ValueError("PE RVA is outside every section")

    result = []
    import_rva = struct.unpack_from("<I", data, directory + 8)[0]
    if import_rva:
        cursor = rva_offset(import_rva)
        while any(data[cursor : cursor + 20]):
            name_rva = struct.unpack_from("<I", data, cursor + 12)[0]
            name_offset = rva_offset(name_rva)
            end = data.index(0, name_offset)
            result.append(data[name_offset:end].decode("ascii"))
            cursor += 20
    delay_rva = struct.unpack_from("<I", data, directory + 13 * 8)[0]
    if delay_rva:
        cursor = rva_offset(delay_rva)
        while any(data[cursor : cursor + 32]):
            attributes, name_value = struct.unpack_from("<II", data, cursor)
            name_rva = name_value if attributes & 1 else name_value - image_base
            name_offset = rva_offset(name_rva)
            end = data.index(0, name_offset)
            result.append(data[name_offset:end].decode("ascii"))
            cursor += 32
    return result


def macho_dependencies(data: bytes) -> List[str]:
    magic = data[:4]
    if magic in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe"):
        endian = "<"
        is_64 = magic[0] == 0xCF
    elif magic in (b"\xfe\xed\xfa\xcf", b"\xfe\xed\xfa\xce"):
        endian = ">"
        is_64 = magic[-1] == 0xCF
    else:
        raise ValueError("unsupported Mach-O magic")
    commands = struct.unpack_from(endian + "I", data, 16)[0]
    cursor = 32 if is_64 else 28
    dylib_commands = {0xC, 0x80000018, 0x8000001F, 0x20, 0x80000023}
    result = []
    for _ in range(commands):
        command, size = struct.unpack_from(endian + "II", data, cursor)
        if command in dylib_commands:
            name_offset = struct.unpack_from(endian + "I", data, cursor + 8)[0]
            end = data.index(0, cursor + name_offset, cursor + size)
            result.append(data[cursor + name_offset : end].decode("utf-8"))
        cursor += size
    return result


def platform_dependencies(data: bytes) -> List[str]:
    if data.startswith(b"\x7fELF"):
        return elf_dependencies(data)
    if data.startswith(b"MZ"):
        return pe_dependencies(data)
    return macho_dependencies(data)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("artifact", type=pathlib.Path)
    parser.add_argument("trust", type=pathlib.Path)
    args = parser.parse_args()

    original = args.artifact.read_bytes()
    artifact = bytearray(original)
    offset = artifact.find(MAGIC)
    if offset < 0 or artifact.find(MAGIC, offset + 1) >= 0:
        raise ValueError("expected exactly one Native proof record")
    record = bytes(artifact[offset : offset + RECORD_SIZE])
    abi, size, algorithm, reserved = struct.unpack_from("<IIII", record, 8)
    if (abi, size, algorithm, reserved) != (1, RECORD_SIZE, 1, 0):
        raise ValueError("invalid Native proof framing")

    artifact_digest = record[24:56]
    export_digest = record[56:88]
    dependency_digest = record[88:120]
    package_id = fixed_string(record, 120, 128)
    package_version = fixed_string(record, 248, 32)
    target_abi = fixed_string(record, 280, 128)
    compiler_identity = fixed_string(record, 408, 96)
    artifact[offset : offset + RECORD_SIZE] = bytes(RECORD_SIZE)
    actual = hashlib.sha256(artifact).digest()
    if actual != artifact_digest:
        raise ValueError("independent artifact digest mismatch")
    if digest_list(platform_dependencies(original)) != dependency_digest:
        raise ValueError("independent dynamic-dependency digest mismatch")

    expected = "\t".join(
        (
            actual.hex(),
            export_digest.hex(),
            dependency_digest.hex(),
            compiler_identity,
            package_id,
            package_version,
            target_abi,
        )
    )
    records = args.trust.read_text(encoding="utf-8").splitlines()
    if records.count(expected) != 1:
        raise ValueError("independent trust-record mismatch")
    print(f"verified {package_id}@{package_version} {actual.hex()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
