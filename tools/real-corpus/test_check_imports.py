#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Unit coverage for the exact typed-import corpus preflight."""
from __future__ import annotations

import pathlib
import sys
import tempfile
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import check_imports


MAGIC = b"\0asm\x01\0\0\0"


def section(identifier: int, payload: bytes) -> bytes:
    return bytes([identifier, len(payload)]) + payload


def wasm_with_import(module: str, field: str, params: bytes, results: bytes) -> bytes:
    def wasm_name(value: str) -> bytes:
        encoded = value.encode()
        return bytes([len(encoded)]) + encoded

    function_type = b"\x60" + params + results
    types = section(1, b"\x01" + function_type)
    imported_function = wasm_name(module) + wasm_name(field) + b"\x00\x00"
    imports = section(2, b"\x01" + imported_function)
    return MAGIC + types + imports


class EmptyImportContract(unittest.TestCase):
    def test_module_without_import_section(self) -> None:
        self.assertEqual(check_imports.imports(MAGIC), [])

    def test_empty_import_section(self) -> None:
        self.assertEqual(check_imports.imports(MAGIC + b"\x02\x01\x00"), [])

    def test_truncated_nonzero_import_section_fails_closed(self) -> None:
        with self.assertRaisesRegex(check_imports.InvalidWasm,
                                    "invalid or truncated unsigned LEB128"):
            check_imports.imports(MAGIC + b"\x02\x01\x01")

    def test_empty_import_section_with_trailing_data_fails(self) -> None:
        with self.assertRaisesRegex(check_imports.InvalidWasm, "trailing bytes"):
            check_imports.imports(MAGIC + b"\x02\x02\x00\x00")

    def test_typed_function_import_is_decoded_exactly(self) -> None:
        module = wasm_with_import("wasi_snapshot_preview1", "fd_close",
                                  b"\x01\x7f", b"\x01\x7f")
        self.assertEqual(check_imports.imports(module), [
            ("wasi_snapshot_preview1", "fd_close", "(i32)->(i32)")
        ])

    def test_non_function_import_fails_closed(self) -> None:
        # One import with a memory kind and minimal limits encoding.
        imported = b"\x01\x01m\x01x\x02\x00\x00"
        with self.assertRaisesRegex(check_imports.InvalidWasm, "only function imports"):
            check_imports.imports(MAGIC + section(2, imported))

    def test_manifest_is_exact_and_workload_scoped(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            manifest = pathlib.Path(directory) / "imports.tsv"
            manifest.write_text(
                "lua\twasi_snapshot_preview1\tfd_close\t(i32)->(i32)\n"
                "other\twasi_snapshot_preview1\tproc_exit\t(i32)->()\n")
            self.assertEqual(check_imports.expected_imports(manifest, "lua"), [
                ("wasi_snapshot_preview1", "fd_close", "(i32)->(i32)")
            ])
            self.assertEqual(check_imports.expected_imports(manifest, "sqlite"), [])

    def test_out_of_bounds_section_fails(self) -> None:
        with self.assertRaisesRegex(check_imports.InvalidWasm, "extends beyond"):
            check_imports.imports(MAGIC + b"\x02\x02\x00")


if __name__ == "__main__":
    unittest.main()
