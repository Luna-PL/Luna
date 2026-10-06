#!/usr/bin/env python3
"""Independent raw platform consumer for the Native artifact fixture."""

import ctypes
import hashlib
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import time


class ExportDescriptor(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("declaration_kind", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("symbol_id", ctypes.c_char_p),
        ("contract_id", ctypes.c_char_p),
        ("linkage_name", ctypes.c_char_p),
        ("entry", ctypes.c_void_p),
    ]


class LibraryDescriptor(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("reserved_zero", ctypes.c_uint32),
        ("package_id", ctypes.c_char_p),
        ("package_version", ctypes.c_char_p),
        ("target_abi", ctypes.c_char_p),
        ("compiler_identity", ctypes.c_char_p),
        ("export_count", ctypes.c_uint64),
        ("exports", ctypes.POINTER(ExportDescriptor)),
    ]


class ExportDescriptorV2(ctypes.Structure):
    _fields_ = [
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("declaration_kind", ctypes.c_uint32),
        ("flags", ctypes.c_uint32),
        ("entry_abi", ctypes.c_uint32),
        ("reserved_zero", ctypes.c_uint32),
        ("symbol_id", ctypes.c_char_p),
        ("contract_id", ctypes.c_char_p),
        ("linkage_name", ctypes.c_char_p),
        ("entry", ctypes.c_void_p),
    ]


class LibraryDescriptorV2(ctypes.Structure):
    _fields_ = [
        ("magic", ctypes.c_uint32),
        ("abi_version", ctypes.c_uint32),
        ("struct_size", ctypes.c_uint32),
        ("reserved_zero", ctypes.c_uint32),
        ("package_id", ctypes.c_char_p),
        ("package_version", ctypes.c_char_p),
        ("target_abi", ctypes.c_char_p),
        ("compiler_identity", ctypes.c_char_p),
        ("export_count", ctypes.c_uint64),
        ("exports", ctypes.POINTER(ExportDescriptorV2)),
        ("export_descriptor_digest", ctypes.c_uint8 * 32),
    ]


def text(value: bytes) -> str:
    if value is None:
        raise ValueError("null Native descriptor string")
    return value.decode("utf-8")


def digest_list(values):
    canonical = bytearray(struct.pack("<I", len(set(values))))
    for value in sorted(set(values)):
        encoded = value.encode("utf-8")
        canonical += struct.pack("<I", len(encoded)) + encoded
    return hashlib.sha256(canonical).digest()


def main() -> int:
    if len(sys.argv) not in (2, 6):
        return 2
    canonical_example = (
        "LUNA_NATIVE_EXPORT_V2\n1\n1\n1\n"
        "symbol:typed-answer\ncontract:typed-answer\ntyped_answer")
    if digest_list([canonical_example]).hex() != (
            "f38aa5dc10637aad46e28639e1a784cc"
            "8c112f6cf06501a0d421479012575703"):
        return 25
    if digest_list([]).hex() != (
            "df3f619804a92fdb4057192dc43dd748"
            "ea778adc52bc498ce80524c014b81119"):
        return 26
    if ctypes.sizeof(ctypes.c_void_p) == 8 and (
            ctypes.sizeof(ExportDescriptorV2) != 56 or
            ExportDescriptorV2.entry_abi.offset != 16 or
            ExportDescriptorV2.symbol_id.offset != 24 or
            ExportDescriptorV2.entry.offset != 48 or
            ctypes.sizeof(LibraryDescriptorV2) != 96 or
            LibraryDescriptorV2.export_count.offset != 48 or
            LibraryDescriptorV2.exports.offset != 56 or
            LibraryDescriptorV2.export_descriptor_digest.offset != 64):
        return 27
    artifact = pathlib.Path(sys.argv[1]).resolve()
    library = ctypes.CDLL(str(artifact))
    query = library.luna_native_library_descriptor_v1
    query.argtypes = []
    query.restype = ctypes.POINTER(LibraryDescriptor)
    descriptor = query().contents
    if (descriptor.magic, descriptor.abi_version, descriptor.struct_size,
            descriptor.reserved_zero) != (
                0x4C4E4431, 1, ctypes.sizeof(LibraryDescriptor), 0):
        return 3
    if text(descriptor.package_id) != "org.luna.fixture.cffi_typed_export":
        return 4
    canonical_exports = []
    callable_entry = None
    callable_symbol = None
    callable_contract = None
    for index in range(descriptor.export_count):
        exported = descriptor.exports[index]
        if (exported.abi_version, exported.struct_size) != (
                1, ctypes.sizeof(ExportDescriptor)):
            return 5
        symbol = text(exported.symbol_id)
        contract = text(exported.contract_id)
        linkage = text(exported.linkage_name)
        canonical_exports.append(
            f"{exported.declaration_kind}\n{exported.flags}\n"
            f"{symbol}\n{contract}\n{linkage}")
        if linkage == "typed_answer" and exported.flags & 1:
            callable_entry = exported.entry
            callable_symbol = symbol
            callable_contract = contract
    binary = artifact.read_bytes()
    proof = binary.index(b"LUNANP1\0")
    if digest_list(canonical_exports) != binary[proof + 56 : proof + 88]:
        return 6
    if callable_entry is None:
        return 7
    typed_query = library.luna_native_library_descriptor_v2
    typed_query.argtypes = []
    typed_query.restype = ctypes.POINTER(LibraryDescriptorV2)
    typed = typed_query().contents
    if (typed.magic, typed.abi_version, typed.struct_size,
            typed.reserved_zero) != (
                0x4C4E4432, 2, ctypes.sizeof(LibraryDescriptorV2), 0):
        return 15
    if (text(typed.package_id), text(typed.package_version),
            text(typed.target_abi), text(typed.compiler_identity)) != (
                text(descriptor.package_id), text(descriptor.package_version),
                text(descriptor.target_abi), text(descriptor.compiler_identity)):
        return 16
    typed_rows = []
    found_answer = False
    for index in range(typed.export_count):
        row = typed.exports[index]
        if (row.abi_version, row.struct_size, row.declaration_kind,
                row.flags, row.entry_abi, row.reserved_zero) != (
                    2, ctypes.sizeof(ExportDescriptorV2), 1, 1, 1, 0):
            return 17
        symbol = text(row.symbol_id)
        contract = text(row.contract_id)
        linkage = text(row.linkage_name)
        typed_rows.append(
            f"LUNA_NATIVE_EXPORT_V2\n{row.declaration_kind}\n"
            f"{row.flags}\n{row.entry_abi}\n"
            f"{symbol}\n{contract}\n{linkage}")
        if (symbol, contract, linkage, row.entry) == (
                callable_symbol, callable_contract, "typed_answer",
                callable_entry):
            found_answer = True
    if not found_answer or digest_list(typed_rows) != bytes(
            typed.export_descriptor_digest):
        return 18
    answer = ctypes.CFUNCTYPE(ctypes.c_int32)(callable_entry)
    if answer() != 42:
        return 8
    if len(sys.argv) == 2:
        return 0

    trust = pathlib.Path(sys.argv[2]).resolve()
    verifier = pathlib.Path(sys.argv[3]).resolve()
    enemy = pathlib.Path(sys.argv[4]).resolve()
    enemy_trust = pathlib.Path(sys.argv[5]).resolve()
    command = [str(verifier), "--load-call", str(artifact), str(trust),
               callable_symbol, callable_contract]
    loaded = subprocess.run(command, capture_output=True, text=True, check=False)
    if loaded.returncode != 0 or loaded.stdout.strip() != "42":
        return 9
    typed_loaded = subprocess.run(
        [str(verifier), "--load-typed-call", str(artifact), str(trust),
         callable_symbol, callable_contract],
        capture_output=True, text=True, check=False)
    if typed_loaded.returncode != 0 or typed_loaded.stdout.strip() != "42":
        return 19
    digest_bytes = bytes(typed.export_descriptor_digest)
    if binary.count(digest_bytes) != 1:
        return 20
    corrupted = bytearray(binary)
    corrupted[corrupted.index(digest_bytes)] ^= 1
    hash_input = bytearray(corrupted)
    hash_input[proof:proof + 504] = bytes(504)
    corrupted[proof + 24:proof + 56] = hashlib.sha256(hash_input).digest()
    corrupt_artifact = artifact.with_name("v2-digest-tampered" + artifact.suffix)
    corrupt_trust = artifact.with_name("v2-digest-tampered.trust")
    trust_fields = trust.read_text(encoding="utf-8").rstrip("\n").split("\t")
    if len(trust_fields) != 7:
        return 21
    trust_fields[0] = bytes(corrupted[proof + 24:proof + 56]).hex()
    corrupt_artifact.write_bytes(corrupted)
    corrupt_trust.write_text("\t".join(trust_fields) + "\n", encoding="utf-8")
    try:
        malformed = subprocess.run(
            [str(verifier), "--load-only", str(corrupt_artifact),
             str(corrupt_trust)], capture_output=True, text=True,
            check=False)
        if malformed.returncode == 0 or (
                "v2 export rows do not match" not in malformed.stderr):
            return 22
    finally:
        corrupt_artifact.unlink(missing_ok=True)
        corrupt_trust.unlink(missing_ok=True)
    profile_row = struct.pack(
        "<IIIIII", 2, ctypes.sizeof(ExportDescriptorV2), 1, 1, 1, 0)
    if binary.count(profile_row) != 1:
        return 28
    unknown_profile = bytearray(binary)
    profile_offset = unknown_profile.index(profile_row) + 16
    unknown_profile[profile_offset:profile_offset + 4] = struct.pack("<I", 2)
    unknown_hash_input = bytearray(unknown_profile)
    unknown_hash_input[proof:proof + 504] = bytes(504)
    unknown_profile[proof + 24:proof + 56] = hashlib.sha256(
        unknown_hash_input).digest()
    unknown_artifact = artifact.with_name("v2-unknown-profile" + artifact.suffix)
    unknown_trust = artifact.with_name("v2-unknown-profile.trust")
    unknown_trust_fields = trust_fields.copy()
    unknown_trust_fields[0] = bytes(
        unknown_profile[proof + 24:proof + 56]).hex()
    unknown_artifact.write_bytes(unknown_profile)
    unknown_trust.write_text(
        "\t".join(unknown_trust_fields) + "\n", encoding="utf-8")
    try:
        rejected = subprocess.run(
            [str(verifier), "--load-only", str(unknown_artifact),
             str(unknown_trust)], capture_output=True, text=True,
            check=False)
        if rejected.returncode == 0 or (
                "v2 library descriptor contains an invalid export row"
                not in rejected.stderr):
            return 29
    finally:
        unknown_artifact.unlink(missing_ok=True)
        unknown_trust.unlink(missing_ok=True)
    # The independently linked and sealed C fixture in native_artifact.cmake
    # exercises the v1-only query path. Byte-patching a linked dylib to hide
    # its v2 query is not a portable way to construct that fixture.
    generation = subprocess.run(
        [str(verifier), "--generation-switch", str(artifact), str(trust),
         str(enemy), str(enemy_trust), callable_symbol, callable_contract],
        capture_output=True, text=True, check=False)
    if generation.returncode != 0 or generation.stdout.strip() != "42 13 42":
        return 14

    attack = artifact.with_name("atomic-source" + artifact.suffix)
    enemy_copy = artifact.with_name("atomic-enemy" + artifact.suffix)
    ready = artifact.with_name("atomic-load.ready")
    release = artifact.with_name("atomic-load.release")
    for path in (attack, enemy_copy, ready, release):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    shutil.copyfile(artifact, attack)
    shutil.copyfile(enemy, enemy_copy)
    paused = subprocess.Popen(
        command[:2] + [str(attack), str(trust), callable_symbol,
                       callable_contract, str(ready), str(release)],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    deadline = time.monotonic() + 15
    while not ready.exists() and paused.poll() is None:
        if time.monotonic() >= deadline:
            paused.kill()
            paused.communicate()
            return 10
        time.sleep(0.01)
    if not ready.exists():
        paused.communicate()
        return 11
    os.replace(enemy_copy, attack)
    release.touch()
    try:
        stdout, _ = paused.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        paused.kill()
        paused.communicate()
        return 12
    for path in (attack, ready, release):
        try:
            path.unlink()
        except FileNotFoundError:
            pass
    return 0 if paused.returncode == 0 and stdout.strip() == "42" else 13


if __name__ == "__main__":
    raise SystemExit(main())
