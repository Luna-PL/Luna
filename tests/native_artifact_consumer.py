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
