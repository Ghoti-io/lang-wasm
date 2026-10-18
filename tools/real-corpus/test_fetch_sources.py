#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Regression checks for source-tree integrity before corpus compilation."""
from __future__ import annotations

import pathlib
import tempfile
import unittest

from fetch_sources import tree_digest


class SourceTreeIntegrityTests(unittest.TestCase):
    def test_equal_bytes_external_symlink_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source"
            source.mkdir()
            file = source / "driver.c"
            external = root / "external.c"
            file.write_bytes(b"identical verified source bytes\n")
            external.write_bytes(file.read_bytes())
            original_digest = tree_digest(source)
            self.assertEqual(len(original_digest), 64)
            file.unlink()
            file.symlink_to(external)
            with self.assertRaisesRegex(ValueError, "contains a symlink"):
                tree_digest(source)

    def test_directory_symlink_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            source = root / "source"
            source.mkdir()
            external = root / "external"
            external.mkdir()
            (source / "nested").symlink_to(external, target_is_directory=True)
            with self.assertRaisesRegex(ValueError, "contains a symlink"):
                tree_digest(source)


if __name__ == "__main__":
    unittest.main()
