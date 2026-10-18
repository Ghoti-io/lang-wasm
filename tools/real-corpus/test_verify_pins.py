#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Regression checks for executable substitution in corpus preflight."""
from __future__ import annotations

import hashlib
import pathlib
import tempfile
import unittest

from verify_pins import verified_binary


class BinaryIntegrityTests(unittest.TestCase):
    def test_changed_executable_bytes_are_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            executable = root / "pinned-compiler"
            marker = root / "pinned-compiler.sha256"
            executable.write_bytes(b"original executable bytes")
            executable.chmod(0o755)
            marker.write_text(hashlib.sha256(executable.read_bytes()).hexdigest() + "\n")
            verified_binary(executable, marker, "test compiler")
            executable.write_bytes(b"substituted executable bytes")
            with self.assertRaisesRegex(ValueError, "executable bytes differ"):
                verified_binary(executable, marker, "test compiler")


if __name__ == "__main__":
    unittest.main()
