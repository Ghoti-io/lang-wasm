#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Fingerprint the exact source trees used by a benchmark snapshot."""

import argparse
import hashlib
import json
import os
import pathlib
import stat
import sys


LIBRARIES = (
    "cutil", "unicode", "chron", "security", "regex", "text",
    "runtime-core", "runtime-heap", "runtime-debug", "runtime-jit",
    "lang-wasm",
)
EXCLUDED = {".git", "__pycache__", "build", "docs", "results"}


def source_manifest(source_commit):
    libraries_root = pathlib.Path(__file__).resolve().parents[2]
    libraries = []
    for name in LIBRARIES:
        root = libraries_root / name
        if not root.is_dir():
            raise ValueError(f"source library is missing: {root}")
        files = []
        for current, directories, filenames in os.walk(root, followlinks=False):
            retained_directories = []
            for directory in sorted(directories):
                if directory in EXCLUDED:
                    continue
                path = pathlib.Path(current) / directory
                if path.is_symlink():
                    data = os.readlink(path).encode("utf-8")
                    files.append({
                        "path": path.relative_to(root).as_posix(),
                        "kind": "symlink",
                        "mode": stat.S_IMODE(path.lstat().st_mode),
                        "size": len(data),
                        "sha256": hashlib.sha256(data).hexdigest(),
                    })
                else:
                    retained_directories.append(directory)
            directories[:] = retained_directories
            for filename in sorted(filenames):
                path = pathlib.Path(current) / filename
                relative = path.relative_to(root).as_posix()
                if path.is_symlink():
                    kind = "symlink"
                    data = os.readlink(path).encode("utf-8")
                    mode = stat.S_IMODE(path.lstat().st_mode)
                elif path.is_file():
                    kind = "file"
                    data = path.read_bytes()
                    mode = stat.S_IMODE(path.stat().st_mode)
                else:
                    continue
                files.append({
                    "path": relative,
                    "kind": kind,
                    "mode": mode,
                    "size": len(data),
                    "sha256": hashlib.sha256(data).hexdigest(),
                })
        libraries.append({"name": name, "files": files})
    return {
        "schema": 1,
        "source_commit": source_commit,
        "libraries": libraries,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--write", required=True, type=pathlib.Path)
    parser.add_argument("--expected-sha256")
    args = parser.parse_args()
    payload = (json.dumps(source_manifest(args.source_commit), sort_keys=True,
                          separators=(",", ":")) + "\n").encode("utf-8")
    actual = hashlib.sha256(payload).hexdigest()
    if args.expected_sha256 and actual != args.expected_sha256.lower():
        raise ValueError(
            "source tree digest differs from GLWASM_BENCH_SOURCE_MANIFEST_SHA256 "
            f"(expected {args.expected_sha256}, got {actual})")
    args.write.parent.mkdir(parents=True, exist_ok=True)
    args.write.write_bytes(payload)
    print(actual)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as exc:
        print(f"source_manifest: {exc}", file=sys.stderr)
        raise SystemExit(1)
