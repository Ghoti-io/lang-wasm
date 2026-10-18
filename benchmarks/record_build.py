#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Force and record the exact build used by a Story 13 benchmark run."""

import hashlib
import argparse
import json
import os
import pathlib
import platform
import re
import shlex
import shutil
import subprocess
import sys


ROOT = pathlib.Path(__file__).resolve().parents[1]
COMPILER_WRAPPERS = {"ccache", "sccache", "distcc", "icecc", "env"}
COMPILER_NAME = re.compile(
    r"(?:^|-)(cc|gcc|g\+\+|c\+\+|clang|clang\+\+|icx|icpx|icc|icpc)(?:-\d+(?:\.\d+)*)?$"
)
ENV_ASSIGNMENT = re.compile(r"[A-Za-z_][A-Za-z0-9_]*=.*")


def split_env_string(value):
    # GNU env -S expands ${NAME}, escape sequences, and comments using rules
    # different from shell tokenization. Reject those forms rather than record
    # a compiler selected by our parser instead of the one env executed.
    if any(character in value for character in ("$", "\\", "#")):
        raise ValueError("env -S expansion, escape, or comment syntax cannot be fingerprinted")
    try:
        return shlex.split(value)
    except ValueError as error:
        raise ValueError("malformed env -S compiler command") from error


def compiler_word_index(words, expanded=None):
    words = list(words)
    index = 0
    while index < len(words):
        while index < len(words) and ENV_ASSIGNMENT.fullmatch(words[index]):
            index += 1
        if index >= len(words):
            return None
        wrapper = pathlib.Path(words[index]).name
        if wrapper in COMPILER_WRAPPERS:
            index += 1
            if wrapper != "env":
                continue

            # GNU env accepts both separate and attached short-option
            # arguments (-u NAME and -uNAME), as well as --option=VALUE.
            # Parse these before looking through cache/distribution wrappers
            # so the digest is taken from the compiler that env execs.
            while index < len(words):
                option = words[index]
                if option == "--":
                    index += 1
                    break
                if option in {"-i", "--ignore-environment", "-0", "--null",
                              "-v", "--debug"}:
                    index += 1
                elif option in {"-u", "--unset", "-C", "--chdir", "--argv0"}:
                    if index + 1 >= len(words):
                        return None
                    index += 2
                elif option in {"-S", "--split-string"}:
                    if index + 1 >= len(words):
                        return None
                    split_text = words[index + 1]
                    split_command = split_env_string(split_text)
                    if not split_command:
                        return None
                    words[index:index + 2] = split_command
                    break
                elif option.startswith(("--unset=", "--chdir=", "--argv0=")):
                    if not option.partition("=")[2]:
                        return None
                    index += 1
                elif option.startswith("--split-string="):
                    split_text = option.partition("=")[2]
                    if not split_text:
                        return None
                    split_command = split_env_string(split_text)
                    if not split_command:
                        return None
                    words[index:index + 1] = split_command
                    break
                elif option.startswith("-u") and len(option) > 2:
                    index += 1
                elif option.startswith("-C") and len(option) > 2:
                    index += 1
                elif option.startswith("-S") and len(option) > 2:
                    split_command = split_env_string(option[2:])
                    if not split_command:
                        return None
                    words[index:index + 1] = split_command
                    break
                elif ENV_ASSIGNMENT.fullmatch(option):
                    index += 1
                else:
                    break
            continue

        if COMPILER_NAME.search(pathlib.Path(words[index]).name):
            if expanded is not None:
                expanded[:] = words
            return index
        return None
    return None


def required(name):
    value = os.environ.get(name, "").strip()
    if not value:
        raise ValueError(f"{name} is required")
    return value


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def compiler_info(command):
    words = shlex.split(command)
    expanded = []
    index = compiler_word_index(words, expanded)
    if index is None:
        raise ValueError(f"cannot identify compiler driver in command: {command}")
    executable = expanded[index]
    environment = os.environ.copy()
    working_directory = ROOT
    position = 0
    while position < index:
        word = expanded[position]
        wrapper = pathlib.Path(word).name
        if ENV_ASSIGNMENT.fullmatch(word):
            name, value = word.split("=", 1)
            environment[name] = value
        elif wrapper in COMPILER_WRAPPERS:
            pass
        elif word in {"-i", "--ignore-environment"}:
            environment.clear()
        elif word in {"-u", "--unset", "-C", "--chdir", "--argv0"}:
            if position + 1 >= index:
                raise ValueError(f"incomplete env option in compiler command: {command}")
            value = expanded[position + 1]
            if word in {"-u", "--unset"}:
                environment.pop(value, None)
            elif word in {"-C", "--chdir"}:
                working_directory = (working_directory / value).resolve()
            position += 1
        elif word.startswith(("--unset=", "--chdir=")):
            option, _, value = word.partition("=")
            if option == "--unset":
                environment.pop(value, None)
            else:
                working_directory = (working_directory / value).resolve()
        elif word.startswith("-u") and len(word) > 2:
            environment.pop(word[2:], None)
        elif word.startswith("-C") and len(word) > 2:
            working_directory = (working_directory / word[2:]).resolve()
        elif word in {"-0", "--null", "-v", "--debug", "--"} or word.startswith("--argv0="):
            pass
        else:
            raise ValueError(f"unrecognized compiler command prefix: {word}")
        position += 1
    search_path = os.pathsep.join(str((working_directory / entry).resolve())
        if not pathlib.Path(entry).is_absolute() else entry
        for entry in os.get_exec_path(environment))
    if "/" in executable:
        candidate = pathlib.Path(executable)
        resolved = str(candidate if candidate.is_absolute() else working_directory / candidate)
        if not pathlib.Path(resolved).is_file() or not os.access(resolved, os.X_OK):
            resolved = None
    else:
        resolved = shutil.which(executable, path=search_path)
    if not resolved:
        raise ValueError(f"compiler executable is not on PATH: {executable}")
    real = pathlib.Path(resolved).resolve(strict=True)
    version = subprocess.run([resolved, "--version"], cwd=working_directory,
                              env=environment, check=True,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True).stdout.splitlines()[0].strip()
    # Record a portable compiler family label but hash the binary actually run.
    invoked_as = pathlib.Path(executable).name
    label = "gcc" if invoked_as in {"cc", "gcc"} else "g++" if invoked_as in {"c++", "g++"} else real.name
    if label.endswith("-" + platform.machine() + "-linux-gnu"):
        label = label[:-(len("-" + platform.machine() + "-linux-gnu"))]
    return label, version, sha256(real.read_bytes())


def model_name_from_cpuinfo(contents, fallback):
    values = {}
    for line in contents.splitlines():
        key, separator, value = line.partition(":")
        if separator:
            values[key.strip().lower()] = value.strip()
    # On x86, /proc/cpuinfo's numeric `model` field appears before `model name`.
    # Prefer a human-readable identity, and use ARM's Hardware field where
    # model name is not provided.
    for key in ("model name", "hardware", "model"):
        value = values.get(key)
        if value:
            return value
    return fallback


def model_name():
    try:
        return model_name_from_cpuinfo(
            pathlib.Path("/proc/cpuinfo").read_text(), platform.node())
    except OSError:
        pass
    return platform.node()


def compiler_commands(log):
    c_commands, cxx_commands, link_commands = [], [], []
    for raw in log.splitlines():
        line = raw.strip()
        if not line or line.startswith(("make[", "make:", "rm ", "mkdir ")):
            continue
        try:
            words = shlex.split(line)
        except ValueError:
            continue
        if not words:
            continue
        # Build recipes invoke compiler drivers directly or through common
        # compiler caches/distribution wrappers. Match the executable basename
        # so versioned and cross-prefixed drivers (for example clang-19 or
        # aarch64-linux-gnu-g++) are captured without treating arbitrary shell
        # commands containing a compiler name as compiler invocations.
        expanded = []
        if compiler_word_index(words, expanded) is None:
            # A force-build transcript must not silently omit a compiler
            # hidden behind an unrecognized wrapper. Otherwise both this
            # recorder and the summarizer would agree on the same incomplete
            # command set and accept a manifest with an unhashed toolchain.
            has_output = "-o" in words or any(
                word.startswith("-o") and not word.startswith("--") and
                len(word) > 2 for word in words)
            has_source = any(pathlib.Path(word).suffix in
                             {".c", ".cc", ".cpp", ".cxx"} for word in words)
            has_object = any(pathlib.Path(word).suffix in {".o", ".a"}
                             for word in words)
            if has_output and (("-c" in words and has_source) or has_object):
                raise ValueError(
                    f"compiler-like build command has an unrecognized driver "
                    f"or wrapper: {line}")
            continue
        if "-c" in expanded:
            source = next((word for word in expanded if pathlib.Path(word).suffix in {".c", ".cc", ".cpp", ".cxx"}), "")
            if source.endswith(".c"):
                c_commands.append(line)
            else:
                cxx_commands.append(line)
        elif any(pathlib.Path(word).suffix in {".o", ".a"} for word in expanded):
            link_commands.append(line)
    if not c_commands or not cxx_commands or not link_commands:
        raise ValueError("forced build transcript did not contain C compile, C++ compile, and link commands")
    return {
        "c_compile": "\n".join(c_commands),
        "cxx_compile": "\n".join(cxx_commands),
        "link": "\n".join(link_commands),
    }


def compiler_identities(commands):
    """Fingerprint every invoked driver; reject identities the CSV cannot name.

    The public row/manifest schema has one C and one C++ compiler identity.
    Different spellings of the same binary are allowed, but a later compile
    command or link using a third binary must fail before any timed sample.
    """
    by_prefix = {}
    by_role = {}
    for role in ("c_compile", "cxx_compile", "link"):
        identities = []
        for command in commands[role].splitlines():
            expanded = []
            index = compiler_word_index(shlex.split(command), expanded)
            if index is None:
                raise ValueError(f"cannot identify {role} compiler driver: {command}")
            prefix = tuple(expanded[:index + 1])
            if prefix not in by_prefix:
                by_prefix[prefix] = compiler_info(command)
            identities.append(by_prefix[prefix])
        if not identities:
            raise ValueError(f"forced build has no {role} compiler commands")
        by_role[role] = identities

    c_info = by_role["c_compile"][0]
    cxx_info = by_role["cxx_compile"][0]
    # Labels may differ for aliases such as cc and gcc. Version and exact
    # executable bytes establish whether a recorded identity is the same.
    def binary_identity(info):
        return info[1], info[2]

    for role, accepted in (("c_compile", {binary_identity(c_info)}),
                           ("cxx_compile", {binary_identity(cxx_info)}),
                           ("link", {binary_identity(c_info),
                                     binary_identity(cxx_info)})):
        for info in by_role[role]:
            if binary_identity(info) not in accepted:
                raise ValueError(
                    f"{role} uses a compiler binary not represented by the "
                    f"recorded C/C++ identities: {info[0]} {info[1]} {info[2]}")
    return c_info, cxx_info


def effective_flags(commands):
    flags = []
    separate = {"-I", "-L", "-l", "-D", "-U", "-include", "-isystem", "-iquote", "-MF", "-MT"}
    for command_text in commands.values():
        for line in command_text.splitlines():
            words = shlex.split(line)
            expanded = []
            compiler_index = compiler_word_index(words, expanded)
            if compiler_index is None:
                continue
            words = expanded
            index = compiler_index + 1
            while index < len(words):
                word = words[index]
                if word == "-o":
                    index += 2
                    continue
                if word == "-c":
                    index += 1
                    continue
                if word.startswith("-"):
                    flags.append(word)
                    if word in separate and index + 1 < len(words):
                        flags.append(words[index + 1])
                        index += 2
                        continue
                index += 1
    return " ".join(dict.fromkeys(flags))


def source_manifest_values(csv_path):
    manifest_path = pathlib.Path(required("GLWASM_BENCH_SOURCE_MANIFEST_FILE"))
    if not manifest_path.is_file():
        raise ValueError(f"source manifest does not exist: {manifest_path}")
    raw = manifest_path.read_bytes()
    expected = required("GLWASM_BENCH_SOURCE_MANIFEST_SHA256").lower()
    if sha256(raw) != expected:
        raise ValueError("source manifest file does not match GLWASM_BENCH_SOURCE_MANIFEST_SHA256")
    commit = required("GLWASM_BENCH_SOURCE_COMMIT")
    doc = json.loads(raw)
    if doc.get("source_commit") != commit:
        raise ValueError("source manifest commit differs from GLWASM_BENCH_SOURCE_COMMIT")
    try:
        rel = manifest_path.resolve().relative_to(csv_path.parent.resolve())
    except ValueError as exc:
        raise ValueError("source manifest must be stored beside the raw CSV") from exc
    return expected, rel.as_posix()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--make", required=True)
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    make = args.make
    log_path = pathlib.Path(args.log).resolve()
    csv_path = pathlib.Path(required("GLWASM_BENCH_OUTPUT")).resolve()
    env_path = pathlib.Path(required("GLWASM_BENCH_ENV_MANIFEST")).resolve()
    if env_path.parent != csv_path.parent:
        raise ValueError("environment manifest must be stored beside the raw CSV")
    log_path.parent.mkdir(parents=True, exist_ok=True)

    # -B ensures the transcript describes commands that actually execute for
    # this benchmark binary and its in-tree library, not stale objects.
    command = shlex.split(make) + ["--always-make", "--no-print-directory", "benchmark-build"]
    proc = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True)
    log_path.write_text(proc.stdout, encoding="utf-8")
    if proc.returncode:
        sys.stdout.write(proc.stdout)
        raise ValueError(f"forced benchmark build failed with exit status {proc.returncode}; see {log_path}")

    commands = compiler_commands(proc.stdout)
    c_info, cxx_info = compiler_identities(commands)
    source_hash, source_path = source_manifest_values(csv_path)
    converter = (ROOT / "src/parse/wat_lower.c").read_bytes() + b"\0" + (ROOT / "src/parse/wat_lower_internal.h").read_bytes()
    image_id = required("GLWASM_BENCH_IMAGE_ID")
    if not re.fullmatch(r"sha256:[0-9a-fA-F]{64}", image_id):
        raise ValueError("GLWASM_BENCH_IMAGE_ID must be the current sha256 image digest")
    flags = effective_flags(commands)
    machine, architecture = model_name(), platform.machine()
    converter_hash = sha256(converter)
    manifest = {
        "schema": 1,
        "compiler": c_info[0], "compiler_version": c_info[1], "compiler_sha256": c_info[2],
        "cxx": cxx_info[0], "cxx_version": cxx_info[1], "cxx_sha256": cxx_info[2],
        "flags": flags, "build_commands": commands, "image_id": image_id,
        "source_commit": required("GLWASM_BENCH_SOURCE_COMMIT"),
        "source_manifest_sha256": source_hash, "source_manifest_path": source_path,
        "machine": machine, "architecture": architecture,
        "fixture_converter": "lang-wasm-wat-lower",
        "fixture_converter_source_sha256": converter_hash,
        "kernel": platform.release(), "cpu_model": machine,
        "build_log_path": log_path.name, "build_log_sha256": sha256(proc.stdout.encode("utf-8")),
    }
    env_path.write_text(json.dumps(manifest, sort_keys=True, separators=(",", ":")) + "\n", encoding="utf-8")
    # These values are generated from the actual compiler commands and then
    # exported into the runner's environment in the calling make recipe.
    exports = {
        "GLWASM_BENCH_COMPILER": c_info[0],
        "GLWASM_BENCH_COMPILER_VERSION": c_info[1],
        "GLWASM_BENCH_CXX": cxx_info[0],
        "GLWASM_BENCH_CXX_VERSION": cxx_info[1],
        "GLWASM_BENCH_MACHINE": machine,
        "GLWASM_BENCH_ARCHITECTURE": architecture,
        "GLWASM_BENCH_FLAGS": flags,
    }
    for name, value in exports.items():
        print(f"export {name}={shlex.quote(value)}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, json.JSONDecodeError, subprocess.SubprocessError) as exc:
        print(f"record_build: {exc}", file=sys.stderr)
        raise SystemExit(1)
