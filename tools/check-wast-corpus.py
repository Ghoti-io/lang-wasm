#!/usr/bin/env python3
"""Verify the pinned WebAssembly test corpus and its exhaustive classification."""
import argparse
import hashlib
import re
import sys
from pathlib import Path

REASONS = {
    "typed-func-refs", "gc", "simd", "relaxed-simd", "threads",
    "legacy-exceptions", "non-core",
}
FEATURE_MARKERS = {
    "typed-func-refs": re.compile(
        r"call_ref|return_call_ref|ref\.as_non_null|br_on_(?:non_)?null|"
        r"(?:ref|param|result|local)\s+(?:null\s+)?(?:\$[A-Za-z_]|[0-9]+\b|func\b|extern\b|exn\b)|"
        r"\\64\\(?:70|6f|69)"),
    "gc": re.compile(
        r"\brec\s*\(|\banyref\b|ref\.null\s+any\b|ref\s+null\s+any\b|"
        r"struct\.|array\.|i31\.|ref\.(?:test|cast)|br_on_cast|"
        r"any\.convert_extern|extern\.convert_any"),
    "simd": re.compile(r"v128|i(?:8x16|16x8|32x4|64x2)\.|f(?:32x4|64x2)\."),
    "relaxed-simd": re.compile(r"relaxed[_\.]"),
    "threads": re.compile(r"atomic\.|memory\.atomic|shared"),
    "legacy-exceptions": re.compile(r"\b(?:try|catch|rethrow|delegate)\b"),
    "non-core": re.compile(r"@(?:custom|metadata|name|code)|stack.guard.page"),
}


def fail(message: str) -> None:
    print(f"check-wast-corpus: {message}", file=sys.stderr)
    raise SystemExit(1)


def read_unique_lines(path: Path) -> list[str]:
    try:
        lines = [line.strip() for line in path.read_text(encoding="utf-8").splitlines()]
    except OSError as error:
        fail(f"cannot read {path}: {error}")
    lines = [line for line in lines if line and not line.startswith("#")]
    if len(lines) != len(set(lines)):
        fail(f"{path} contains duplicate entries")
    return lines


def source_form_at_line(source: str, line_number: int) -> str:
    """Return the balanced WAST form beginning on a one-based source line."""
    lines = source.splitlines(keepends=True)
    if line_number < 1 or line_number > len(lines):
        return ""
    start = sum(len(line) for line in lines[:line_number - 1])
    while start < len(source) and source[start].isspace():
        start += 1
    if start >= len(source) or source[start] != "(":
        return ""
    depth = 0
    in_string = escaped = False
    line_comment = False
    block_depth = 0
    index = start
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if line_comment:
            if char == "\n":
                line_comment = False
            index += 1
            continue
        if block_depth:
            if pair == "(;":
                block_depth += 1
                index += 2
            elif pair == ";)":
                block_depth -= 1
                index += 2
            else:
                index += 1
            continue
        if in_string:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue
        if pair == ";;":
            line_comment = True
            index += 2
        elif pair == "(;":
            block_depth = 1
            index += 2
        elif char == '"':
            in_string = True
            index += 1
        else:
            if char == "(":
                depth += 1
            elif char == ")":
                depth -= 1
                if depth == 0:
                    return source[start:index + 1]
            index += 1
    return ""


def mask_comments_and_strings(source: str) -> str:
    """Keep WAST tokens while preventing marker matches in prose or literals."""
    output = list(source)
    index = 0
    in_string = escaped = False
    line_comment = False
    block_depth = 0
    while index < len(source):
        char = source[index]
        pair = source[index:index + 2]
        if line_comment:
            if char == "\n":
                line_comment = False
            else:
                output[index] = " "
            index += 1
            continue
        if block_depth:
            if pair == "(;":
                output[index:index + 2] = [" ", " "]
                block_depth += 1
                index += 2
            elif pair == ";)":
                output[index:index + 2] = [" ", " "]
                block_depth -= 1
                index += 2
            else:
                if char != "\n":
                    output[index] = " "
                index += 1
            continue
        if in_string:
            if char != "\n":
                output[index] = " "
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                in_string = False
            index += 1
            continue
        if pair == ";;":
            output[index:index + 2] = [" ", " "]
            line_comment = True
            index += 2
        elif pair == "(;":
            output[index:index + 2] = [" ", " "]
            block_depth = 1
            index += 2
        elif char == '"':
            output[index] = " "
            in_string = True
            index += 1
        else:
            index += 1
    return "".join(output)


def contains_feature_marker(source: str, reason: str) -> bool:
    if FEATURE_MARKERS[reason].search(mask_comments_and_strings(source)):
        return True
    if reason == "typed-func-refs" and re.search(r"\\64\\(?:70|6f|69)", source):
        return True
    # A command can target a function whose test name names the deferred op
    # (for example, call_ref-unreached) without repeating that op in its body.
    if reason == "typed-func-refs":
        return bool(re.search(r'["\'][^"\']*(?:call_ref|return_call_ref|'
                              r'ref_as_non_null|br_on_null|br_on_non_null)', source))
    return False


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--metadata", type=Path, required=True,
                        help="runtime-jit/tools/wasm directory")
    parser.add_argument("--corpus", type=Path, required=True,
                        help="runtime-jit/third_party/wasm-spec directory")
    parser.add_argument("--classification", type=Path, required=True)
    parser.add_argument("--assertion-skips", type=Path, required=True)
    args = parser.parse_args()

    commit_file = args.metadata / "WASM_SPEC_COMMIT"
    try:
        commit = commit_file.read_text(encoding="ascii").strip()
    except OSError as error:
        fail(f"cannot read {commit_file}: {error}")
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        fail(f"invalid pinned commit in {commit_file}: {commit!r}")
    corpus_root = args.corpus / commit / "test"
    if not corpus_root.is_dir():
        fail(f"missing fetched suite at {corpus_root}; run runtime-jit/tools/wasm/fetch.sh")

    wast_files = read_unique_lines(args.metadata / "WAST_FILES")
    for path in wast_files:
        if not path.startswith("test/") or not path.endswith(".wast") or ".." in Path(path).parts:
            fail(f"invalid suite path in WAST_FILES: {path}")
    expected_paths = set(wast_files) | {"test/LICENSE"}
    actual_checksums: dict[str, str] = {}
    try:
        for line in (args.metadata / "SHA256SUMS").read_text(encoding="ascii").splitlines():
            if not line.strip():
                continue
            digest, path = line.split(maxsplit=1)
            path = path.lstrip(" *")
            if path in actual_checksums:
                fail(f"duplicate checksum entry for {path}")
            if not re.fullmatch(r"[0-9a-f]{64}", digest):
                fail(f"malformed SHA-256 for {path}")
            actual_checksums[path] = digest
    except (OSError, ValueError) as error:
        fail(f"cannot read checksum manifest: {error}")
    if set(actual_checksums) != expected_paths:
        missing = sorted(expected_paths - set(actual_checksums))
        extra = sorted(set(actual_checksums) - expected_paths)
        fail(f"checksum manifest differs from WAST_FILES (missing={missing[:5]}, extra={extra[:5]})")

    classifications: dict[str, str] = {}
    try:
        for number, line in enumerate(args.classification.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 2:
                fail(f"{args.classification}:{number}: expected path<TAB>classification")
            path, classification = fields
            if path in classifications:
                fail(f"{args.classification}:{number}: duplicate classification for {path}")
            if classification != "run":
                match = re.fullmatch(r"skip\(([^)]+)\)", classification)
                if not match or match.group(1) not in REASONS:
                    fail(f"{args.classification}:{number}: invalid classification {classification!r}")
            classifications[path] = classification
    except OSError as error:
        fail(f"cannot read {args.classification}: {error}")
    if set(classifications) != set(wast_files):
        missing = sorted(set(wast_files) - set(classifications))
        extra = sorted(set(classifications) - set(wast_files))
        fail(f"classification differs from WAST_FILES (unclassified={missing[:8]}, unknown={extra[:8]})")

    # A skip must be justified by syntax that belongs to the named deferred
    # feature. This prevents a routine in-scope file from being hidden by
    # changing only its classification row.
    for path, classification in classifications.items():
        if classification == "run":
            continue
        reason = classification[5:-1]
        relative = path.lower()
        if reason == "gc" and "/gc/" in relative:
            continue
        if reason == "simd" and "/simd/" in relative and "/relaxed-simd/" not in relative:
            continue
        if reason == "relaxed-simd" and "/relaxed-simd/" in relative:
            continue
        if reason == "legacy-exceptions" and "/legacy/exceptions/" in relative:
            continue
        if reason == "non-core" and ("/custom/" in relative or relative.endswith("skip-stack-guard-page.wast")):
            continue
        try:
            source = (corpus_root.parent / path).read_text(encoding="utf-8")
        except OSError as error:
            fail(f"cannot read classified WAST file {path}: {error}")
        if not contains_feature_marker(source, reason):
            fail(f"{path} is classified {classification} but has no {reason} feature marker")

    assertion_skips: set[tuple[str, int]] = set()
    try:
        for number, line in enumerate(args.assertion_skips.read_text(encoding="utf-8").splitlines(), 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            fields = line.split("\t")
            if len(fields) != 3:
                fail(f"{args.assertion_skips}:{number}: expected path<TAB>line<TAB>skip(reason)")
            path, source_line, classification = fields
            if path not in classifications or classifications[path] != "run":
                fail(f"{args.assertion_skips}:{number}: assertion skip must belong to a run file: {path}")
            if not source_line.isdecimal() or int(source_line) < 1:
                fail(f"{args.assertion_skips}:{number}: invalid source line {source_line!r}")
            match = re.fullmatch(r"skip\(([^)]+)\)", classification)
            if not match or match.group(1) not in REASONS:
                fail(f"{args.assertion_skips}:{number}: invalid skip reason {classification!r}")
            key = (path, int(source_line))
            if key in assertion_skips:
                fail(f"{args.assertion_skips}:{number}: duplicate assertion skip {key}")
            source = (corpus_root.parent / path).read_text(encoding="utf-8")
            form = source_form_at_line(source, int(source_line))
            reason = match.group(1)
            if not form or not contains_feature_marker(form, reason):
                fail(f"{args.assertion_skips}:{number}: {key} does not contain a {reason} feature")
            assertion_skips.add(key)
    except OSError as error:
        fail(f"cannot read {args.assertion_skips}: {error}")

    actual_wast = {
        "test/" + path.relative_to(corpus_root).as_posix()
        for path in corpus_root.rglob("*.wast")
    }
    if actual_wast != set(wast_files):
        fail(f"fetched suite file set differs (missing={sorted(set(wast_files)-actual_wast)[:8]}, "
             f"unexpected={sorted(actual_wast-set(wast_files))[:8]})")
    for relative_path in sorted(expected_paths):
        path = args.corpus / commit / relative_path
        try:
            got = hashlib.sha256(path.read_bytes()).hexdigest()
        except OSError as error:
            fail(f"missing pinned file {path}: {error}")
        if got != actual_checksums[relative_path]:
            fail(f"{relative_path} has SHA-256 {got}; expected {actual_checksums[relative_path]}")

    print(f"wg-3.0 corpus verified: commit={commit}, files={len(wast_files)}, "
          f"checksum-verified={len(expected_paths)}")
    print(f"classification: run={sum(c == 'run' for c in classifications.values())}, "
          f"skip={sum(c != 'run' for c in classifications.values())}")
    for reason in sorted(REASONS):
        count = sum(c == f"skip({reason})" for c in classifications.values())
        if count:
            print(f"  skip({reason})={count}")
    print(f"assertion-level skips: {len(assertion_skips)}")


if __name__ == "__main__":
    main()
