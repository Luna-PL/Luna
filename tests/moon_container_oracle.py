#!/usr/bin/env python3
"""Independent Moon Container 0.3 conformance oracle.

This parser deliberately does not import, invoke, or bind Luna's container
reader. It consumes every field in all eight required sections from a real
CLI product and rejects trailing bytes, unresolved generic types, and generic
function recipes.
"""

from __future__ import annotations

import argparse
import hashlib
import pathlib
import struct
import subprocess
import sys


MAGIC = b"\x89MOON\r\n\x1a"
HEADER_SIZE = 80
DIRECTORY_ENTRY_SIZE = 32
TYPE_PARAM = 23
INFERENCE_VAR = 38
UNKNOWN_TYPE = 39


class FormatError(Exception):
    pass


class Cursor:
    def __init__(self, payload: bytes, section: str):
        self.payload = payload
        self.section = section
        self.offset = 0

    def require(self, size: int) -> None:
        if size < 0 or self.offset + size > len(self.payload):
            raise FormatError(f"{self.section}: truncated at byte {self.offset}")

    def u32(self, maximum=None) -> int:
        self.require(4)
        value = struct.unpack_from("<I", self.payload, self.offset)[0]
        self.offset += 4
        if maximum is not None and value > maximum:
            raise FormatError(
                f"{self.section}: scalar {value} exceeds {maximum}")
        return value

    def u64(self) -> int:
        self.require(8)
        value = struct.unpack_from("<Q", self.payload, self.offset)[0]
        self.offset += 8
        return value

    def i64(self) -> int:
        self.require(8)
        value = struct.unpack_from("<q", self.payload, self.offset)[0]
        self.offset += 8
        return value

    def boolean(self) -> bool:
        return bool(self.u32(1))

    def string(self) -> str:
        size = self.u32()
        self.require(size)
        raw = self.payload[self.offset : self.offset + size]
        self.offset += size
        try:
            return raw.decode("utf-8")
        except UnicodeDecodeError as error:
            raise FormatError(f"{self.section}: invalid UTF-8") from error

    def count(self) -> int:
        count = self.u32()
        if count > (len(self.payload) - self.offset) // 4:
            raise FormatError(f"{self.section}: impossible row count {count}")
        return count

    def finish(self) -> None:
        if self.offset != len(self.payload):
            raise FormatError(
                f"{self.section}: {len(self.payload) - self.offset} trailing bytes")


def reference(cursor: Cursor) -> tuple[str, str]:
    return cursor.string(), cursor.string()


def location(cursor: Cursor) -> None:
    cursor.string()
    cursor.i64()
    cursor.i64()


def table_refs(cursor: Cursor) -> list[int]:
    return [cursor.u32() for _ in range(cursor.count())]


def strings(cursor: Cursor) -> list[str]:
    return [cursor.string() for _ in range(cursor.count())]


def contract(cursor: Cursor) -> None:
    cursor.u32(2)
    cursor.u32(2)


def facts(cursor: Cursor) -> None:
    cursor.u32()
    cursor.u32()
    for _ in range(5):
        cursor.string()
    cursor.u32(3)
    cursor.u32(2)
    cursor.u32(2)
    cursor.u32(2)
    cursor.boolean()
    cursor.boolean()
    for _ in range(cursor.count()):
        contract(cursor)
    contract(cursor)
    cursor.u32(1)
    cursor.u32(5)
    cursor.u32(3)
    cursor.u32(2)
    cursor.u32(2)
    cursor.u32(7)
    for _ in range(12):
        cursor.boolean()
    cursor.string()


def constant(cursor: Cursor) -> None:
    tag = cursor.u32(3)
    if tag in (0, 1):
        cursor.u64()
    elif tag == 2:
        cursor.boolean()
    else:
        cursor.string()


def metadata(cursor: Cursor) -> None:
    cursor.string()
    for _ in range(cursor.count()):
        constant(cursor)
    cursor.u32(2)
    location(cursor)


def parse_manifest(payload: bytes) -> dict[str, object]:
    cursor = Cursor(payload, "manifest")
    package_id = cursor.string()
    package_version = cursor.string()
    package_kind = cursor.u32(2)
    target_triple = cursor.string()
    data_layout = cursor.string()
    entry = reference(cursor)
    features = cursor.u32(0x3F)
    cursor.finish()
    if not package_id or not package_version or package_kind not in (1, 2):
        raise FormatError("manifest: invalid package identity")
    if not target_triple or not data_layout:
        raise FormatError("manifest: missing target identity")
    if package_kind == 1 and (not entry[0] or not entry[1]):
        raise FormatError("manifest: application has no entrypoint")
    if package_kind == 2 and (entry[0] or entry[1]):
        raise FormatError("manifest: library has an entrypoint")
    return {"package_id": package_id, "entry": entry, "features": features}


def fields(cursor: Cursor) -> None:
    for _ in range(cursor.count()):
        cursor.string()
        cursor.string()


def parse_types(payload: bytes) -> dict[str, object]:
    cursor = Cursor(payload, "type")
    ids: list[str] = []
    kinds: list[int] = []
    for _ in range(cursor.count()):
        type_id = cursor.string()
        cursor.string()
        cursor.string()
        cursor.u32(4)
        cursor.u32(6)
        kind = cursor.u32(UNKNOWN_TYPE)
        facts(cursor)
        for _ in range(4):
            cursor.string()
        strings(cursor)
        strings(cursor)
        cursor.string()
        cursor.u64()
        cursor.boolean()
        strings(cursor)
        cursor.string()
        for _ in range(cursor.count()):
            contract(cursor)
        contract(cursor)
        cursor.boolean()
        cursor.u32(1)
        cursor.u32(4)
        fields(cursor)
        fields(cursor)
        for _ in range(cursor.count()):
            cursor.string()
            strings(cursor)
        cursor.i64()
        cursor.string()
        cursor.string()
        cursor.string()
        cursor.u32()
        cursor.u64()
        cursor.u64()
        cursor.string()
        reference(cursor)
        strings(cursor)
        ids.append(type_id)
        kinds.append(kind)
    cursor.finish()
    if ids != sorted(ids) or len(ids) != len(set(ids)) or any(not value for value in ids):
        raise FormatError("type: TypeIds are not strictly ordered")
    unresolved = {TYPE_PARAM, INFERENCE_VAR, UNKNOWN_TYPE}.intersection(kinds)
    if unresolved:
        raise FormatError(f"type: concrete container has unresolved kinds {unresolved}")
    return {"ids": ids, "kinds": kinds}


def parse_symbols(payload: bytes) -> list[str]:
    cursor = Cursor(payload, "symbol")
    symbols: list[str] = []
    for _ in range(cursor.count()):
        symbols.append(cursor.string())
        for _ in range(4):
            cursor.string()
        cursor.u32(6)
        cursor.u32(2)
        cursor.string()
        location(cursor)
    cursor.finish()
    if symbols != sorted(symbols) or len(symbols) != len(set(symbols)):
        raise FormatError("symbol: SymbolIds are not strictly ordered")
    return symbols


def parse_contracts(payload: bytes) -> list[str]:
    cursor = Cursor(payload, "contract")
    symbols: list[str] = []
    for _ in range(cursor.count()):
        symbols.append(cursor.string())
        cursor.string()
        facts(cursor)
        reference(cursor)
        cursor.string()
    cursor.finish()
    if symbols != sorted(symbols) or len(symbols) != len(set(symbols)):
        raise FormatError("contract: SymbolIds are not strictly ordered")
    return symbols


def parse_sysmeta(payload: bytes) -> list[str]:
    cursor = Cursor(payload, "sysmeta")
    schema_ids: list[str] = []
    for _ in range(cursor.count()):
        schema_ids.append(cursor.string())
        cursor.string()
        fields(cursor)
        location(cursor)
    if schema_ids != sorted(schema_ids) or len(schema_ids) != len(set(schema_ids)):
        raise FormatError("sysmeta: schema IDs are not strictly ordered")
    symbols: list[str] = []
    for _ in range(cursor.count()):
        symbols.append(cursor.string())
        for _ in range(cursor.count()):
            metadata(cursor)
    cursor.finish()
    if symbols != sorted(symbols) or len(symbols) != len(set(symbols)):
        raise FormatError("sysmeta: SymbolIds are not strictly ordered")
    return symbols


def parse_imports(payload: bytes) -> int:
    cursor = Cursor(payload, "imports")
    count = cursor.count()
    for _ in range(count):
        cursor.u32(1)
        for _ in range(7):
            cursor.string()
        reference(cursor)
        cursor.string()
        location(cursor)
    cursor.finish()
    return count


def parse_exports(payload: bytes) -> int:
    cursor = Cursor(payload, "exports")
    count = cursor.count()
    for _ in range(count):
        cursor.string()
        reference(cursor)
        cursor.string()
        cursor.u32(6)
        cursor.string()
        location(cursor)
    cursor.finish()
    return count


def optional_expr(cursor: Cursor, depth: int) -> None:
    if cursor.boolean():
        expression(cursor, depth)


def expression_vector(cursor: Cursor, depth: int) -> None:
    for _ in range(cursor.count()):
        optional_expr(cursor, depth)


def parameter(cursor: Cursor) -> None:
    cursor.string()
    cursor.boolean()
    cursor.u32(2)
    cursor.u32(2)
    cursor.string()


def expression(cursor: Cursor, depth: int) -> None:
    if depth >= 256:
        raise FormatError("code: expression nesting exceeds 256")
    opcode = cursor.u32(28)
    if opcode == 0:
        raise FormatError("code: expression opcode zero")
    cursor.string()
    location(cursor)
    nested = depth + 1
    if opcode in (1, 2):
        cursor.u64()
    elif opcode == 3:
        cursor.string()
    elif opcode == 4:
        cursor.boolean()
    elif opcode == 5:
        pass
    elif opcode == 6:
        cursor.string()
        cursor.u32()
        reference(cursor)
    elif opcode == 7:
        optional_expr(cursor, nested)
        cursor.u32(34)
        optional_expr(cursor, nested)
    elif opcode == 8:
        cursor.u32(34)
        optional_expr(cursor, nested)
    elif opcode == 9:
        optional_expr(cursor, nested)
        expression_vector(cursor, nested)
        strings(cursor)
        reference(cursor)
        cursor.boolean()
        cursor.u32(2)
        cursor.string()
        cursor.string()
        cursor.string()
        cursor.u32(10)
        for _ in range(4):
            cursor.string()
        reference(cursor)
        reference(cursor)
        reference(cursor)
        if cursor.boolean():
            constant(cursor)
    elif opcode == 10:
        cursor.string()
        reference(cursor)
        cursor.string()
        expression_vector(cursor, nested)
        for _ in range(cursor.count()):
            reference(cursor)
            for _ in range(cursor.count()):
                constant(cursor)
    elif opcode == 11:
        cursor.string()
        reference(cursor)
        optional_expr(cursor, nested)
        expression_vector(cursor, nested)
        for _ in range(cursor.count()):
            cursor.string()
            cursor.boolean()
    elif opcode == 12:
        cursor.string()
        cursor.string()
        expression_vector(cursor, nested)
        cursor.string()
    elif opcode == 13:
        cursor.boolean()
        optional_expr(cursor, nested)
    elif opcode == 14:
        optional_expr(cursor, nested)
        cursor.string()
    elif opcode == 15:
        optional_expr(cursor, nested)
        optional_expr(cursor, nested)
    elif opcode == 16:
        optional_expr(cursor, nested)
    elif opcode == 17:
        expression_vector(cursor, nested)
        cursor.string()
    elif opcode == 18:
        for _ in range(cursor.count()):
            cursor.string()
            optional_expr(cursor, nested)
    elif opcode == 19:
        optional_expr(cursor, nested)
        cursor.string()
        cursor.u32(0)
    elif opcode == 20:
        cursor.u32()
        cursor.string()
        cursor.u32(0)
        for _ in range(cursor.count()):
            cursor.u32()
            optional_expr(cursor, nested)
    elif opcode == 21:
        optional_expr(cursor, nested)
        cursor.u32()
    elif opcode in (22, 24):
        cursor.boolean()
        optional_expr(cursor, nested)
    elif opcode == 23:
        optional_expr(cursor, nested)
    elif opcode == 25:
        for _ in range(cursor.count()):
            parameter(cursor)
        cursor.string()
        if cursor.boolean():
            graph(cursor, nested)
        cursor.string()
        strings(cursor)
        cursor.string()
        cursor.string()
    elif opcode == 26:
        optional_expr(cursor, nested)
        expression_vector(cursor, nested)
    elif opcode == 27:
        cursor.u32()
        cursor.u64()
    elif opcode == 28:
        cursor.u32(34)
        optional_expr(cursor, nested)
        optional_expr(cursor, nested)


def operation(cursor: Cursor, depth: int) -> None:
    opcode = cursor.u32(5)
    if opcode == 0:
        raise FormatError("code: operation opcode zero")
    location(cursor)
    if opcode == 1:
        cursor.string()
        cursor.u32()
        cursor.boolean()
        cursor.boolean()
        cursor.u32(2)
        if cursor.boolean():
            cursor.u32(2)
        cursor.string()
        optional_expr(cursor, depth)
        cursor.boolean()
        cursor.boolean()
        cursor.string()
    elif opcode == 2:
        cursor.u32()
        cursor.string()
        cursor.u32(0)
    elif opcode == 3:
        optional_expr(cursor, depth)
    elif opcode == 4:
        optional_expr(cursor, depth)
        cursor.u32(7)
        cursor.boolean()
    elif opcode == 5:
        optional_expr(cursor, depth)


def edge(cursor: Cursor) -> None:
    cursor.u32()
    table_refs(cursor)


def terminator(cursor: Cursor, depth: int) -> None:
    cursor.u32(7)
    location(cursor)
    optional_expr(cursor, depth)
    cursor.string()
    edge(cursor)
    edge(cursor)
    for _ in range(cursor.count()):
        cursor.u32()
        edge(cursor)
        table_refs(cursor)
    table_refs(cursor)


def graph(cursor: Cursor, depth: int) -> None:
    if depth >= 256 or not cursor.boolean():
        raise FormatError("code: unsealed or too-deep CFG")
    cursor.u32()
    cursor.u32()
    cursor.u32()
    for index in range(cursor.count()):
        if cursor.u32() != index:
            raise FormatError("code: non-canonical block ID")
        cursor.u32()
        cursor.u32()
        location(cursor)
        for _ in range(cursor.count()):
            operation(cursor, depth)
        terminator(cursor, depth)
    for index in range(cursor.count()):
        if cursor.u32() != index:
            raise FormatError("code: non-canonical region ID")
        cursor.u32()
        cursor.u32(7)
        cursor.u32()
        cursor.u32()
        cursor.u32()
        table_refs(cursor)
        location(cursor)
        reference(cursor)
        table_refs(cursor)
    for index in range(cursor.count()):
        if cursor.u32() != index:
            raise FormatError("code: non-canonical scope ID")
        cursor.u32()
        cursor.u32()
        table_refs(cursor)
        table_refs(cursor)
        location(cursor)
    for index in range(cursor.count()):
        if cursor.u32() != index:
            raise FormatError("code: non-canonical local ID")
        cursor.u32()
        cursor.u32(4)
        cursor.string()
        cursor.string()
        cursor.u32(2)
        cursor.u32(2)
    for index in range(cursor.count()):
        if cursor.u32() != index:
            raise FormatError("code: non-canonical cleanup ID")
        cursor.u32()
        cursor.u32()
        for _ in range(cursor.count()):
            cursor.u32(3)
            cursor.u64()
            cursor.u32()
        cursor.string()
        cursor.u32(1)
        cursor.u32(7)
        if cursor.boolean():
            cursor.u32()
            cursor.u64()


def parse_code(payload: bytes) -> list[dict[str, object]]:
    cursor = Cursor(payload, "code")
    functions: list[dict[str, object]] = []
    symbols: list[str] = []
    for _ in range(cursor.count()):
        symbol, contract_id = reference(cursor)
        package_id = cursor.string()
        module_path = cursor.string()
        name = cursor.string()
        generated_name = cursor.string()
        for _ in range(5):
            cursor.boolean()
        cursor.string()
        cursor.string()
        type_parameters = strings(cursor)
        for _ in range(cursor.count()):
            parameter(cursor)
        cursor.string()
        cursor.boolean()
        cursor.u32(2)
        is_instance = cursor.boolean()
        concrete_arguments = strings(cursor)
        location(cursor)
        if cursor.boolean():
            graph(cursor, 0)
        if type_parameters and not is_instance:
            raise FormatError(f"code: retained generic recipe {name}")
        symbols.append(symbol)
        functions.append(
            {
                "symbol": symbol,
                "contract": contract_id,
                "package": package_id,
                "module": module_path,
                "name": name,
                "generated": generated_name,
                "is_instance": is_instance,
                "concrete_arguments": concrete_arguments,
            }
        )
    cursor.finish()
    if symbols != sorted(symbols) or len(symbols) != len(set(symbols)):
        raise FormatError("code: function SymbolIds are not strictly ordered")
    return functions


def parse_container(data: bytes) -> dict[str, object]:
    if len(data) < HEADER_SIZE or data[:8] != MAGIC:
        raise FormatError("container: invalid or truncated magic/header")
    major, minor, header_size, flags, count, reserved = struct.unpack_from(
        "<IIIIII", data, 8
    )
    directory_offset, encoded_size = struct.unpack_from("<QQ", data, 32)
    if (major, minor) != (0, 3) or header_size != HEADER_SIZE:
        raise FormatError("container: unsupported version/header size")
    if flags or reserved or directory_offset != HEADER_SIZE or encoded_size != len(data):
        raise FormatError("container: invalid header fields")
    if count != 8 or HEADER_SIZE + count * DIRECTORY_ENTRY_SIZE > len(data):
        raise FormatError("container: invalid section count")
    expected_digest = hashlib.sha256(data[:48] + bytes(32) + data[80:]).digest()
    if data[48:80] != expected_digest:
        raise FormatError("container: SHA-256 mismatch")
    sections: dict[int, bytes] = {}
    previous_end = (HEADER_SIZE + count * DIRECTORY_ENTRY_SIZE + 7) & ~7
    for index in range(count):
        base = HEADER_SIZE + index * DIRECTORY_ENTRY_SIZE
        section_id, section_flags, offset, length, decoded_length = struct.unpack_from(
            "<IIQQQ", data, base
        )
        if section_id != index + 1 or section_flags or length != decoded_length:
            raise FormatError("container: non-canonical directory entry")
        if offset % 8 or offset < previous_end or offset + length > len(data):
            raise FormatError("container: invalid section range")
        if any(data[previous_end:offset]):
            raise FormatError("container: non-zero padding")
        sections[section_id] = data[offset : offset + length]
        previous_end = offset + length
    if previous_end != len(data):
        raise FormatError("container: trailing bytes")

    manifest = parse_manifest(sections[1])
    types = parse_types(sections[2])
    symbols = parse_symbols(sections[3])
    contracts = parse_contracts(sections[4])
    functions = parse_code(sections[5])
    import_count = parse_imports(sections[6])
    export_count = parse_exports(sections[7])
    sysmeta_symbols = parse_sysmeta(sections[8])
    code_symbols = [str(function["symbol"]) for function in functions]
    if symbols != contracts or symbols != sysmeta_symbols or symbols != code_symbols:
        raise FormatError("container: normalized declaration/code key sets differ")
    if manifest["entry"] not in {
        (function["symbol"], function["contract"]) for function in functions
    }:
        raise FormatError("container: entrypoint is not executable")
    return {
        "manifest": manifest,
        "types": types,
        "functions": functions,
        "imports": import_count,
        "exports": export_count,
    }


def run() -> int:
    argument_parser = argparse.ArgumentParser()
    argument_parser.add_argument("--compiler", required=True)
    argument_parser.add_argument("--package", required=True)
    argument_parser.add_argument("--work-dir", required=True)
    arguments = argument_parser.parse_args()

    work_dir = pathlib.Path(arguments.work_dir)
    work_dir.mkdir(parents=True, exist_ok=True)
    outputs = [work_dir / "first.moon", work_dir / "second.moon"]
    for output in outputs:
        completed = subprocess.run(
            [arguments.compiler, "build", arguments.package, "-t", "moon", "-o", str(output)],
            check=False,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        if completed.returncode != 0:
            raise FormatError(
                f"compiler failed ({completed.returncode}): {completed.stderr.strip()}"
            )
    first = outputs[0].read_bytes()
    second = outputs[1].read_bytes()
    if first != second:
        raise FormatError("container: repeated CLI builds are not deterministic")
    report = parse_container(first)
    functions = report["functions"]
    if len(functions) != 4 or not any(
        function["is_instance"] and function["concrete_arguments"]
        for function in functions
    ):
        raise FormatError("projection: concrete generic instance was not retained")
    names = {function["name"] for function in functions}
    if "forward" not in names or "dead_concrete" in names:
        raise FormatError("projection: transitive callee retention/dead stripping failed")
    if len(report["types"]["ids"]) != 5:
        raise FormatError("projection: unexpected concrete type-table shape")
    print(
        "moon-container-oracle: ok "
        f"bytes={len(first)} types={len(report['types']['ids'])} "
        f"functions={len(functions)} sha256={hashlib.sha256(first).hexdigest()}"
    )
    return 0


if __name__ == "__main__":
    try:
        sys.exit(run())
    except (FormatError, OSError) as error:
        print(f"moon-container-oracle: {error}", file=sys.stderr)
        sys.exit(1)
