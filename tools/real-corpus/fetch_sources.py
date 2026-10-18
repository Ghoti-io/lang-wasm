#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Fetch and verify pinned upstream corpora; never used by the test gate."""
from __future__ import annotations

import argparse
import hashlib
import io
import pathlib
import tarfile
import urllib.request
import zipfile


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def tree_digest(root: pathlib.Path) -> str:
    if root.is_symlink() or not root.is_dir():
        raise ValueError(f"source tree root is missing or is a symlink: {root}")
    result = hashlib.sha256()
    for path in sorted(root.rglob("*")):
        if path.is_symlink():
            raise ValueError(f"verified source tree contains a symlink: {path}")
        if path.is_dir():
            continue
        if not path.is_file():
            raise ValueError(f"verified source tree contains a non-file: {path}")
        if path == root / ".source-pin":
            continue
        relative = path.relative_to(root).as_posix().encode()
        content = path.read_bytes()
        result.update(len(relative).to_bytes(8, "little"))
        result.update(relative)
        result.update(len(content).to_bytes(8, "little"))
        result.update(content)
    return result.hexdigest()


def safe_extract(data: bytes, destination: pathlib.Path, name: str) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    if data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as archive:
            for member in archive.infolist():
                path = pathlib.PurePosixPath(member.filename)
                if path.is_absolute() or ".." in path.parts:
                    raise ValueError(f"{name}: unsafe zip member {member.filename!r}")
            archive.extractall(destination)
        return
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:*") as archive:
        for member in archive.getmembers():
            path = pathlib.PurePosixPath(member.name)
            if path.is_absolute() or ".." in path.parts:
                raise ValueError(f"{name}: unsafe archive member {member.name!r}")
            if not (member.isfile() or member.isdir()):
                raise ValueError(f"{name}: unsupported archive member {member.name!r}")
        archive.extractall(destination, filter="data")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pins", required=True, type=pathlib.Path)
    parser.add_argument("--destination", required=True, type=pathlib.Path)
    parser.add_argument("--check-only", action="store_true")
    args = parser.parse_args()
    for line_number, line in enumerate(args.pins.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != 6:
            raise SystemExit(f"{args.pins}:{line_number}: expected six tab-separated fields")
        name, version, identity, expected_archive, expected_tree, url = fields
        for label, digest_value in (("archive", expected_archive),
                                    ("tree", expected_tree)):
            if len(digest_value) != 64 or any(
                ch not in "0123456789abcdef" for ch in digest_value
            ):
                raise SystemExit(
                    f"{args.pins}:{line_number}: malformed {label} SHA-256")
        target = args.destination / name
        marker = target / ".source-pin"
        if marker.is_symlink():
            raise SystemExit(f"{name}: source pin marker is a symlink")
        if marker.exists():
            marker_fields = marker.read_text().split()
            actual_tree = tree_digest(target)
            if (marker_fields == [identity, expected_archive, expected_tree] and
                    actual_tree == expected_tree):
                print(f"{name}: verified source tree {actual_tree}")
                continue
            raise SystemExit(f"{name}: source tree changed after verified extraction")
        if args.check_only:
            raise SystemExit(f"{name}: no verified local source; run fetch-sources.sh explicitly")
        request = urllib.request.Request(url, headers={"User-Agent": "ghoti-lang-wasm-corpus/1"})
        with urllib.request.urlopen(request, timeout=120) as response:
            archive = response.read()
        actual = digest(archive)
        if actual != expected_archive:
            raise SystemExit(f"{name}: archive SHA-256 mismatch: expected {expected_archive}, got {actual}")
        if target.exists():
            import shutil
            shutil.rmtree(target)
        target.mkdir(parents=True)
        safe_extract(archive, target, name)
        if name == "sqlite":
            amalgamations = list(target.rglob("sqlite3.c"))
            if len(amalgamations) != 1:
                raise SystemExit("sqlite: expected exactly one sqlite3.c amalgamation")
            amalgamation = amalgamations[0].read_bytes()
            if identity.encode() not in amalgamation[:65536]:
                raise SystemExit("sqlite: amalgamation Fossil source ID mismatch")
            actual_source = hashlib.sha3_256(amalgamation).hexdigest()
            source_pin = "e2b869d3003b33c942b7dd7063604fbc53553e1b66609570df39b9b7b558ee81"
            if actual_source != source_pin:
                raise SystemExit(f"sqlite: amalgamation SHA3-256 mismatch: {actual_source}")
        elif name == "zlib":
            headers = list(target.rglob("zlib.h"))
            if len(headers) != 1 or f'#define ZLIB_VERSION "{version}"' not in headers[0].read_text():
                raise SystemExit(f"zlib: archive does not contain pinned version {version}")
        elif name == "lua":
            headers = list(target.rglob("lua.h"))
            header_text = headers[0].read_text() if len(headers) == 1 else ""
            version_parts = version.split(".")
            expected_parts = ("MAJOR_N", "MINOR_N", "RELEASE_N")
            if len(version_parts) != 3 or any(
                f"#define LUA_VERSION_{part}\t{number}" not in header_text
                for part, number in zip(expected_parts, version_parts)
            ):
                raise SystemExit(f"lua: archive does not contain pinned version {version}")
        actual_tree = tree_digest(target)
        if actual_tree != expected_tree:
            raise SystemExit(
                f"{name}: extracted source tree SHA-256 mismatch: "
                f"expected {expected_tree}, got {actual_tree}")
        marker.write_text(f"{identity} {expected_archive} {actual_tree}\n")
        print(f"{name}: fetched and verified source tree {actual_tree}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
