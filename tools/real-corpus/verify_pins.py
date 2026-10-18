#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Validate corpus manifests and installed toolchain provenance before use."""
from __future__ import annotations

import hashlib
import pathlib
import re
import os
import shutil
import subprocess
import sys


WORKLOADS = ("sqlite", "zlib", "lua")
ARCHITECTURES = ("x86_64", "aarch64")
HEX64 = re.compile(r"[0-9a-f]{64}\Z")


def rows(path: pathlib.Path, columns: int) -> list[list[str]]:
    result = []
    for number, line in enumerate(path.read_text().splitlines(), 1):
        if not line or line.startswith("#"):
            continue
        fields = line.split("\t")
        if len(fields) != columns:
            raise ValueError(f"{path.name}:{number}: expected {columns} tab-separated columns")
        result.append(fields)
    return result


def verify_checksum_manifest(path: pathlib.Path, expected: set[str], base: pathlib.Path) -> None:
    declared: dict[str, str] = {}
    for number, line in enumerate(path.read_text().splitlines(), 1):
        checksum, separator, filename = line.partition("  ")
        if not separator or not HEX64.fullmatch(checksum) or filename in declared:
            raise ValueError(f"{path.name}:{number}: malformed or duplicate checksum row")
        declared[filename] = checksum
    if set(declared) != expected:
        raise ValueError(f"{path.name}: checksum set mismatch (expected {sorted(expected)}, got {sorted(declared)})")
    for filename, checksum in declared.items():
        actual = hashlib.sha256((base / filename).read_bytes()).hexdigest()
        if actual != checksum:
            raise ValueError(f"{path.name}: checksum mismatch for {filename}")


def tree_digest(root: pathlib.Path) -> str:
    """Recheck every path and file byte in a pinned SDK link-input tree."""
    if not root.is_dir():
        raise ValueError(f"required SDK link-input directory is missing: {root}")
    root_real = root.resolve(strict=True)
    digest = hashlib.sha256()
    for path in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        relative = path.relative_to(root).as_posix()
        if path.is_symlink():
            try:
                target = path.resolve(strict=True)
                target_relative = target.relative_to(root_real).as_posix()
            except (OSError, RuntimeError, ValueError) as error:
                raise ValueError(
                    f"SDK link-input symlink is dangling, cyclic, or escapes its measured tree: {path}") from error
            record = f"L\0{relative}\0{path.readlink()}\0{target_relative}\n".encode()
        elif path.is_dir():
            record = f"D\0{relative}\n".encode()
        elif path.is_file():
            content_hash = hashlib.sha256(path.read_bytes()).hexdigest()
            record = f"F\0{relative}\0{content_hash}\n".encode()
        else:
            raise ValueError(f"unsupported SDK link-input path: {path}")
        digest.update(record)
    return digest.hexdigest()


def verify_sdk_link_inputs(wasi_root: pathlib.Path) -> None:
    """Revalidate linker binary and all sysroot/compiler-runtime files."""
    sdk = wasi_root.resolve(strict=True)
    expected_linker_path = (wasi_root / ".ghoti-wasm-ld-path").read_text().strip()
    linker = wasi_root / "bin/wasm-ld"
    resolved_linker = linker.resolve(strict=True)
    try:
        actual_linker_path = resolved_linker.relative_to(sdk).as_posix()
    except ValueError as error:
        raise ValueError("wasi-sdk wasm-ld resolves outside its SDK") from error
    if actual_linker_path != expected_linker_path:
        raise ValueError("wasi-sdk wasm-ld resolved path differs from the verified archive")
    expected_linker_hash = (wasi_root / ".ghoti-wasm-ld-binary-sha256").read_text().strip()
    if not HEX64.fullmatch(expected_linker_hash) or hashlib.sha256(
            resolved_linker.read_bytes()).hexdigest() != expected_linker_hash:
        raise ValueError("wasi-sdk wasm-ld binary differs from the verified archive")
    expected_linker_version = (wasi_root / ".ghoti-wasm-ld-version").read_text().rstrip("\n")
    actual_linker_version = subprocess.run([str(linker), "--version"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.splitlines()
    if (not expected_linker_version.startswith("LLD ") or not actual_linker_version or
            actual_linker_version[0] != expected_linker_version):
        raise ValueError("wasi-sdk wasm-ld version differs from the verified archive")

    sysroot_relative = (wasi_root / ".ghoti-wasi-sysroot-path").read_text().strip()
    resource_relative = (wasi_root / ".ghoti-clang-resource-path").read_text().strip()
    if sysroot_relative != "share/wasi-sysroot" or not re.fullmatch(
            r"lib/clang/[^/]+", resource_relative):
        raise ValueError("wasi-sdk sysroot/compiler-resource path marker is malformed")
    for relative, marker_name, label in (
        (sysroot_relative, ".ghoti-wasi-sysroot-sha256", "WASI sysroot"),
        (resource_relative, ".ghoti-clang-resource-sha256", "clang resource directory"),
    ):
        root = (wasi_root / relative).resolve(strict=True)
        try:
            root.relative_to(sdk)
        except ValueError as error:
            raise ValueError(f"wasi-sdk {label} resolves outside its SDK") from error
        expected = (wasi_root / marker_name).read_text().strip()
        if not HEX64.fullmatch(expected) or tree_digest(root) != expected:
            raise ValueError(f"wasi-sdk {label} differs from the checksum-verified archive")


def verified_binary(path: pathlib.Path, marker: pathlib.Path, label: str) -> None:
    expected = marker.read_text().strip()
    if not HEX64.fullmatch(expected):
        raise ValueError(f"{label} has a malformed archive/image-derived binary hash")
    if hashlib.sha256(path.resolve(strict=True).read_bytes()).hexdigest() != expected:
        raise ValueError(f"{label} executable bytes differ from the verified archive/image")


def native_gcc_tool(driver: str, name: str) -> pathlib.Path:
    selected = driver if name == "driver" else subprocess.run(
        [driver, f"-print-prog-name={name}"], check=True, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    if not selected:
        raise ValueError(f"GCC returned no {name} executable")
    located = selected if "/" in selected else shutil.which(selected)
    if not located:
        raise ValueError(f"GCC {name} executable is missing: {selected}")
    resolved = pathlib.Path(located).resolve(strict=True)
    if not resolved.is_file() or not os.access(resolved, os.X_OK):
        raise ValueError(f"GCC {name} executable is missing or not executable: {resolved}")
    return resolved


def verify_native_compiler(marker_root: pathlib.Path) -> None:
    driver = shutil.which("gcc")
    if not driver:
        raise ValueError("native GCC driver is missing")
    for name in ("driver", "cc1", "as", "collect2", "ld"):
        executable = native_gcc_tool(driver, name)
        expected_path = (marker_root / f"native-gcc-{name}-path").read_text().strip()
        if not expected_path or str(executable) != expected_path:
            raise ValueError(f"native GCC {name} executable path differs from pinned image")
        verified_binary(executable, marker_root / f"native-gcc-{name}.sha256",
                        f"native GCC {name}")


def verify_wasmtime_binaries(marker_root: pathlib.Path) -> None:
    verified_binary(pathlib.Path("/usr/local/bin/wasmtime"),
                    marker_root / "wasmtime-binary.sha256", "Wasmtime CLI")
    api_root = pathlib.Path("/opt/wasmtime-c-api-49.0.3")
    verified_binary(api_root / "lib/libwasmtime.so",
                    api_root / ".ghoti-library-sha256", "Wasmtime C API library")
    expected_headers = (api_root / ".ghoti-headers-sha256").read_text().strip()
    if not HEX64.fullmatch(expected_headers) or tree_digest(api_root / "include") != expected_headers:
        raise ValueError("Wasmtime C API headers differ from the verified archive")


def main() -> int:
    if len(sys.argv) != 4:
        raise SystemExit("usage: verify_pins.py CORPUS_DIR ARCH MODE")
    data, architecture, mode = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
    if architecture not in ARCHITECTURES or mode not in (
        "--check-pins", "--propose-pins", "--all-tiers"
    ):
        raise SystemExit("unsupported architecture or mode")

    pins = rows(data / "pins.tsv", 6)
    expected_sources = {"sqlite": "3.54.1", "zlib": "1.3.2", "lua": "5.5.1"}
    if len(pins) != len(expected_sources) or {row[0]: row[1] for row in pins} != expected_sources:
        raise SystemExit("pins.tsv must contain exactly the three specified upstream releases")
    for name, version, _identity, archive_hash, tree_hash, url in pins:
        if (not HEX64.fullmatch(archive_hash) or not HEX64.fullmatch(tree_hash) or
                not url.startswith("https://")):
            raise SystemExit(f"pins.tsv contains an invalid hash or URL for {name} {version}")
    for name, _version, identity, _archive_hash, _tree_hash, _url in pins:
        expected_identity = r"[0-9a-f]{40}" if name == "zlib" else r"[0-9a-f]{64}"
        if not re.fullmatch(expected_identity, identity):
            raise SystemExit(f"pins.tsv contains malformed source identity for {name}")

    verify_checksum_manifest(data / "vectors.sha256",
        {"vectors.tsv", *(f"vectors/{name}.hex" for name in WORKLOADS)}, data)
    verify_checksum_manifest(data / "build-flags.sha256", {"build-flags.sh"}, data)
    verify_checksum_manifest(data / "drivers.sha256",
        {"corpus_result.h", "sqlite_driver.c", "zlib_driver.c", "lua_driver.c",
         "native_main.c", "wasmtime_host.c"}, data)
    project_root = data.parent.parent
    tool_files = {
        "Makefile",
        "tests/real-corpus/real_corpus_test.cpp",
        "tests/real-corpus/test_selection_guard.py",
        "tests/real-corpus/imports.tsv",
        "tools/real-corpus/build-and-run.sh",
        "tools/real-corpus/check_imports.py",
        "tools/real-corpus/fetch-sources.sh",
        "tools/real-corpus/fetch_sources.py",
        "tools/real-corpus/test_fetch_sources.py",
        "tools/real-corpus/test_check_imports.py",
        "tools/real-corpus/test_verify_pins.py",
        "tools/real-corpus/verify_licenses.py",
        "tools/real-corpus/verify_pins.py",
        "../../suite/Containerfile.build",
        "../../suite/Containerfile.build.arm64",
        "../../suite/tools/install-lang-wasm-corpus-tools.py",
    }
    verify_checksum_manifest(data / "tools.sha256", tool_files, project_root)

    toolchain_rows = rows(data / "toolchains.tsv", 4)
    required = {
        ("native-gcc", "16.2.0", "linux/amd64"),
        ("native-gcc", "16.2.0", "linux/arm64"),
        ("wasi-sdk", "34.0", "linux/x86_64"),
        ("wasi-sdk", "34.0", "linux/aarch64"),
        ("wasi-clang", "34.0", "linux/x86_64"),
        ("wasi-clang", "34.0", "linux/aarch64"),
        ("wasmtime", "49.0.3", "linux/x86_64"),
        ("wasmtime", "49.0.3", "linux/aarch64"),
        ("wasmtime-c-api", "49.0.3", "linux/x86_64"),
        ("wasmtime-c-api", "49.0.3", "linux/aarch64"),
    }
    actual_keys = {(row[0], row[1], row[2]) for row in toolchain_rows}
    if len(toolchain_rows) != len(required) or actual_keys != required:
        raise SystemExit("toolchains.tsv does not contain the complete pinned platform matrix")
    if len({(row[0], row[2]) for row in toolchain_rows}) != len(toolchain_rows):
        raise SystemExit("toolchains.tsv contains duplicate platform pins")
    for name, _version, platform, checksum in toolchain_rows:
        expected_form = r"sha256:[0-9a-f]{64}" if name == "native-gcc" else r"[0-9a-f]{64}"
        if not re.fullmatch(expected_form, checksum):
            raise SystemExit(f"malformed toolchain checksum for {name} {platform}")

    platform_arch = "amd64" if architecture == "x86_64" else "arm64"
    sdk_platform = "linux/x86_64" if architecture == "x86_64" else "linux/aarch64"
    tool_pins = {(row[0], row[2]): row[3] for row in toolchain_rows}
    marker_root = pathlib.Path("/usr/local/share/ghoti-lang-wasm-corpus")
    markers = {
        "native-gcc": (marker_root / "native-image.sha256").read_text().strip(),
        "wasi-sdk": (pathlib.Path(os.environ.get("WASI_SDK_PATH", "/opt/wasi-sdk-34.0")) /
                      ".ghoti-archive-sha256").read_text().strip(),
        "wasi-clang": (pathlib.Path(os.environ.get("WASI_SDK_PATH", "/opt/wasi-sdk-34.0")) /
                       ".ghoti-clang-archive-sha256").read_text().strip(),
        "wasmtime": (marker_root / "wasmtime-archive.sha256").read_text().strip(),
        "wasmtime-c-api": (pathlib.Path("/opt/wasmtime-c-api-49.0.3") /
                           ".ghoti-archive-sha256").read_text().strip(),
    }
    expected_markers = {
        "native-gcc": tool_pins[("native-gcc", f"linux/{platform_arch}")],
        "wasi-sdk": tool_pins[("wasi-sdk", sdk_platform)],
        "wasi-clang": tool_pins[("wasi-clang", sdk_platform)],
        "wasmtime": tool_pins[("wasmtime", sdk_platform)],
        "wasmtime-c-api": tool_pins[("wasmtime-c-api", sdk_platform)],
    }
    if expected_markers["wasi-clang"] != expected_markers["wasi-sdk"]:
        raise SystemExit("wasi-clang pin must identify the same verified archive as wasi-sdk")
    if markers != expected_markers:
        raise SystemExit(f"installed toolchain provenance differs from toolchains.tsv: {markers}")
    verify_native_compiler(marker_root)
    verify_wasmtime_binaries(marker_root)

    wasi_root = pathlib.Path(os.environ.get("WASI_SDK_PATH", "/opt/wasi-sdk-34.0"))
    clang = wasi_root / "bin/clang"
    try:
        resolved_clang = clang.resolve(strict=True)
        resolved_clang.relative_to(wasi_root.resolve(strict=True))
    except (OSError, ValueError) as error:
        raise SystemExit(f"installed wasi-sdk clang is missing or resolves outside its SDK: {error}")
    clang_hash = (wasi_root / ".ghoti-clang-binary-sha256").read_text().strip()
    if not HEX64.fullmatch(clang_hash):
        raise SystemExit("installed wasi-sdk clang has a malformed archive-derived binary hash")
    actual_clang_hash = hashlib.sha256(resolved_clang.read_bytes()).hexdigest()
    if actual_clang_hash != clang_hash:
        raise SystemExit("installed wasi-sdk clang binary differs from its verified archive member")
    expected_clang_version = (wasi_root / ".ghoti-clang-version").read_text().rstrip("\n")
    expected_clang_target = (wasi_root / ".ghoti-clang-target").read_text().strip()
    if (not expected_clang_version.startswith("clang version ") or
            expected_clang_target != "wasm32-unknown-wasip1"):
        raise SystemExit("installed wasi-sdk clang identity marker is malformed")
    version_output = subprocess.run([str(clang), "--version"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.splitlines()
    if not version_output:
        raise SystemExit("installed wasi-sdk clang returned an empty version identity")
    actual_clang_version = version_output[0]
    actual_clang_target = subprocess.run([str(clang), "-dumpmachine"], check=True,
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT).stdout.strip()
    if (actual_clang_version != expected_clang_version or
        actual_clang_target != expected_clang_target):
        raise SystemExit(
            "installed wasi-sdk clang version/target differs from the verified archive: "
            f"expected {expected_clang_version!r}/{expected_clang_target!r}, "
            f"got {actual_clang_version!r}/{actual_clang_target!r}")
    verify_sdk_link_inputs(wasi_root)

    vectors = rows(data / "vectors.tsv", 5)
    seen = set()
    for workload, test_case, status, length_text, filename in vectors:
        if workload not in WORKLOADS or test_case != "default" or workload in seen:
            raise SystemExit("vectors.tsv has an unknown, duplicate, or non-default case")
        seen.add(workload)
        if status != "1" or not re.fullmatch(r"0|[1-9][0-9]*", length_text):
            raise SystemExit(f"vectors.tsv has malformed result record for {workload}")
        size = int(length_text)
        if size > 8192 or filename != f"vectors/{workload}.hex":
            raise SystemExit(f"vectors.tsv has invalid output path/length for {workload}")
        lines = (data / filename).read_text().splitlines()
        output_hex = "".join(lines)
        if (len(output_hex) != size * 2 or not re.fullmatch(r"[0-9a-f]*", output_hex) or
            any(len(line) != 128 for line in lines[:-1]) or
            (lines and not 1 <= len(lines[-1]) <= 128)):
            raise SystemExit(f"vectors.tsv has invalid complete output for {workload}")
    if seen != set(WORKLOADS):
        raise SystemExit("vectors.tsv must contain exactly one vector per workload")

    module_path = data / "module-hashes.tsv"
    if mode != "--propose-pins":
        module_rows = rows(module_path, 3)
        expected_keys = {(workload, arch) for workload in WORKLOADS for arch in ARCHITECTURES}
        actual = {(row[0], row[1]) for row in module_rows}
        if len(module_rows) != len(expected_keys) or actual != expected_keys:
            raise SystemExit("module-hashes.tsv must pin all three modules for both architectures")
        for workload, arch, checksum in module_rows:
            if not HEX64.fullmatch(checksum):
                raise SystemExit(f"malformed module hash for {workload}/{arch}")

    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        raise SystemExit(f"corpus pin validation failed: {error}")
