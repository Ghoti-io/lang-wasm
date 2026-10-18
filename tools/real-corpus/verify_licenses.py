#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Verify preserved corpus license text against the pinned upstream trees."""
from __future__ import annotations

import hashlib
import pathlib
import sys


def exactly_one(root: pathlib.Path, name: str) -> pathlib.Path:
    matches = list(root.rglob(name))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {name} under {root}, found {len(matches)}")
    return matches[0]


def extracted_licenses(sources: pathlib.Path) -> dict[str, bytes]:
    sqlite = exactly_one(sources / "sqlite", "sqlite3.c").read_bytes()
    begin = sqlite.find(b"** The author disclaims copyright to this source code.")
    if begin < 0:
        raise ValueError("SQLite dedication not found")
    end = sqlite.find(b"*************************************************************************\n", begin)
    if end < 0:
        raise ValueError("SQLite dedication terminator not found")
    end += len(b"*************************************************************************\n")

    lua = exactly_one(sources / "lua", "readme.html").read_bytes()
    begin_lua = lua.find(b'<H2><A NAME="license">License</A></H2>')
    end_marker = b"</BLOCKQUOTE>\n"
    end_lua = lua.find(end_marker, begin_lua)
    if begin_lua < 0 or end_lua < 0:
        raise ValueError("Lua license section not found")
    end_lua += len(end_marker)

    return {
        "licenses/SQLite-3.54.1-notice.txt": sqlite[begin:end],
        "licenses/zlib-1.3.2-LICENSE.txt": exactly_one(sources / "zlib", "LICENSE").read_bytes(),
        "licenses/Lua-5.5.1-license.html": lua[begin_lua:end_lua],
    }


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: verify_licenses.py SOURCE_ROOT CORPUS_ROOT")
    sources, corpus = map(pathlib.Path, sys.argv[1:])
    expected = extracted_licenses(sources)
    pins: dict[str, str] = {}
    for number, line in enumerate((corpus / "licenses.tsv").read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 3:
            raise SystemExit(f"licenses.tsv:{number}: expected three tab-separated fields")
        path, checksum, _source = fields
        if path in pins:
            raise SystemExit(f"licenses.tsv:{number}: duplicate license path {path}")
        pins[path] = checksum
    if set(pins) != set(expected):
        raise SystemExit(f"license pin set differs (expected {sorted(expected)}, got {sorted(pins)})")
    for name, source_bytes in expected.items():
        copied = (corpus / name).read_bytes()
        expected_hash = pins[name]
        source_hash = hashlib.sha256(source_bytes).hexdigest()
        copied_hash = hashlib.sha256(copied).hexdigest()
        if source_hash != expected_hash or copied_hash != expected_hash or copied != source_bytes:
            raise SystemExit(f"{name}: preserved license differs from pinned upstream text")
        print(f"{name}: verified upstream notice ({expected_hash})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
