#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Check a corpus module against its exact, typed host-import manifest."""
from __future__ import annotations

import pathlib
import sys


class InvalidWasm(ValueError):
    pass


VALTYPE = {
    0x7F: "i32",
    0x7E: "i64",
    0x7D: "f32",
    0x7C: "f64",
    0x7B: "v128",
    0x70: "funcref",
    0x6F: "externref",
    0x6E: "anyref",
    0x6D: "eqref",
    0x6C: "i31ref",
    0x6B: "structref",
    0x6A: "arrayref",
    0x69: "exnref",
}


def uleb(data: bytes, pos: int) -> tuple[int, int]:
    value = shift = 0
    while pos < len(data) and shift < 35:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        if not byte & 0x80:
            return value, pos
        shift += 7
    raise InvalidWasm("invalid or truncated unsigned LEB128")


def name(data: bytes, pos: int) -> tuple[str, int]:
    length, pos = uleb(data, pos)
    end = pos + length
    if end > len(data):
        raise InvalidWasm("truncated import name")
    try:
        return data[pos:end].decode("utf-8"), end
    except UnicodeDecodeError as error:
        raise InvalidWasm("import name is not UTF-8") from error


def value_types(data: bytes, pos: int, end: int) -> tuple[list[str], int]:
    count, pos = uleb(data, pos)
    values: list[str] = []
    for _ in range(count):
        if pos >= end or data[pos] not in VALTYPE:
            raise InvalidWasm("unsupported or truncated value type")
        values.append(VALTYPE[data[pos]])
        pos += 1
    return values, pos


def module_types(data: bytes) -> list[tuple[list[str], list[str]]]:
    types: list[tuple[list[str], list[str]]] = []
    pos = 8
    while pos < len(data):
        section = data[pos]
        size, content = uleb(data, pos + 1)
        end = content + size
        if end > len(data):
            raise InvalidWasm("section extends beyond module")
        if section == 1:
            count, cursor = uleb(data, content)
            for _ in range(count):
                if cursor >= end or data[cursor] != 0x60:
                    raise InvalidWasm("imported non-function type is unsupported")
                cursor += 1
                params, cursor = value_types(data, cursor, end)
                results, cursor = value_types(data, cursor, end)
                types.append((params, results))
            if cursor != end:
                raise InvalidWasm("type section has trailing bytes")
        pos = end
    return types


def imports(data: bytes) -> list[tuple[str, str, str]]:
    if len(data) < 8 or data[:8] != b"\0asm\x01\0\0\0":
        raise InvalidWasm("not a WebAssembly 1.0 binary")
    types = module_types(data)
    pos = 8
    found: list[tuple[str, str, str]] = []
    while pos < len(data):
        section = data[pos]
        size, content = uleb(data, pos + 1)
        end = content + size
        if end > len(data):
            raise InvalidWasm("section extends beyond module")
        if section == 2:
            count, cursor = uleb(data, content)
            for _ in range(count):
                module, cursor = name(data, cursor)
                field, cursor = name(data, cursor)
                if cursor >= end:
                    raise InvalidWasm("truncated import kind")
                kind = data[cursor]
                cursor += 1
                if kind != 0:
                    raise InvalidWasm("only function imports are permitted")
                type_index, cursor = uleb(data, cursor)
                if type_index >= len(types):
                    raise InvalidWasm("function import has invalid type index")
                params, results = types[type_index]
                signature = f"({','.join(params)})->({','.join(results)})"
                found.append((module, field, signature))
            if cursor != end:
                raise InvalidWasm("import section has trailing bytes")
        pos = end
    return found


def expected_imports(manifest: pathlib.Path, workload: str) -> list[tuple[str, str, str]]:
    expected: list[tuple[str, str, str]] = []
    for line_number, line in enumerate(manifest.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 4 or any(not field for field in fields):
            raise InvalidWasm(f"{manifest}:{line_number}: malformed import row")
        name_workload, module, field, signature = fields
        if name_workload == workload:
            expected.append((module, field, signature))
    if len(set(expected)) != len(expected):
        raise InvalidWasm(f"{manifest}: duplicate import rows for {workload}")
    return expected


def main(args: list[str]) -> int:
    if len(args) != 4:
        raise SystemExit("usage: check_imports.py MANIFEST.tsv WORKLOAD MODULE.wasm")
    manifest, workload, module_path = pathlib.Path(args[1]), args[2], pathlib.Path(args[3])
    try:
        actual = imports(module_path.read_bytes())
        expected = expected_imports(manifest, workload)
    except (OSError, InvalidWasm) as error:
        raise SystemExit(f"{module_path}: {error}")
    if actual != expected:
        raise SystemExit(
            f"{module_path}: import mismatch; expected {expected!r}, got {actual!r}"
        )
    print(f"{module_path}: exact typed imports verified ({len(actual)})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
