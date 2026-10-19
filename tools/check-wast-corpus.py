#!/usr/bin/env python3
"""Verify the pinned WebAssembly test corpus and its exhaustive classification."""
import argparse
from functools import lru_cache
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
        r"(?<![$A-Za-z0-9_.$-])(?:call_ref|return_call_ref|ref\.as_non_null|"
        r"br_on_(?:non_)?null)(?![A-Za-z0-9_.$-])|"
        r"\bref\s+(?:null\s+)?(?:\$[A-Za-z_][A-Za-z0-9_.$-]*|[0-9]+\b)|"
        r"\bref\s+(?:func|extern|exn)\b"),
    "gc": re.compile(
        r"\brec\s*\(|\banyref\b|ref\.null\s+any\b|ref\s+null\s+any\b|"
        r"struct\.|array\.|i31\.|ref\.(?:test|cast)|br_on_cast|"
        r"any\.convert_extern|extern\.convert_any"),
    "simd": re.compile(r"v128|i(?:8x16|16x8|32x4|64x2)\.|f(?:32x4|64x2)\."),
    "relaxed-simd": re.compile(r"relaxed[_\.]"),
    "threads": re.compile(r"atomic\.|memory\.atomic|shared"),
    "legacy-exceptions": re.compile(r"\b(?:try|catch|rethrow|delegate)\b"),
    "non-core": re.compile(r"@(?:custom|metadata|name|code)"),
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
    """Return the assertion form beginning on a one-based source line."""
    lines = source.splitlines(keepends=True)
    if line_number < 1 or line_number > len(lines):
        return ""
    line_start = sum(len(line) for line in lines[:line_number - 1])
    line_end = line_start + len(lines[line_number - 1])
    forms = [(start, form) for start, form in _cached_top_level_forms(source)
             if line_start <= start < line_end]
    for _, form in forms:
        if re.match(r"\(assert_[A-Za-z0-9_-]+\b", form):
            return form
    return forms[0][1] if forms else ""


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
    return reason == "typed-func-refs" and contains_binary_typed_reference(source)


def contains_binary_typed_reference(source: str) -> bool:
    """Recognize typed references in table types, never arbitrary binary payloads."""
    visible = mask_comments_and_strings(source)
    string = re.compile(r'"((?:\\.|[^"\\])*)"')
    for match in re.finditer(r'\(module(?:\s+\$[A-Za-z0-9_.$-]+)?\s+binary\b', visible):
        end = _balanced_form_end(source, match.start())
        if end is None:
            continue
        module_form = source[match.start():end]
        data = bytearray()
        for fragment in string.findall(module_form):
            i = 0
            while i < len(fragment):
                if fragment[i] == "\\" and i + 2 < len(fragment):
                    escaped = fragment[i + 1:i + 3]
                    if re.fullmatch(r"[0-9a-fA-F]{2}", escaped):
                        data.append(int(escaped, 16))
                        i += 3
                        continue
                    # Preserve an escaped character as ordinary string data;
                    # it cannot form a Wasm byte escape marker.
                    i += 2
                    continue
                data.extend(fragment[i].encode("utf-8"))
                i += 1
        if not data.startswith(b"\0asm\1\0\0\0"):
            continue
        # Parse section envelopes and table declarations. This avoids treating
        # code, custom-section data, or other arbitrary bytes as evidence.
        offset = 8
        while offset < len(data):
            section_id = data[offset]
            offset += 1
            section_size, offset = _read_u32_leb(data, offset, len(data))
            if section_size is None or offset + section_size > len(data):
                break
            section_end = offset + section_size
            if section_id == 1 and _type_section_has_typed_reference(data, offset, section_end):
                return True
            if section_id == 2 and _import_section_has_typed_table(data, offset, section_end):
                return True
            if section_id == 4 and _table_section_has_typed_table(data, offset, section_end):
                return True
            offset = section_end
    return False


def _balanced_form_end(source: str, start: int) -> int | None:
    """Return the exclusive end of a balanced form, respecting WAST strings/comments."""
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
                    return index + 1
            index += 1
    return None


def _skip_trivia(source: str, offset: int) -> int:
    while offset < len(source):
        if source[offset].isspace():
            offset += 1
        elif source.startswith(";;", offset):
            end = source.find("\n", offset + 2)
            offset = len(source) if end < 0 else end + 1
        elif source.startswith("(;", offset):
            depth = 1
            offset += 2
            while offset < len(source) and depth:
                if source.startswith("(;", offset):
                    depth += 1
                    offset += 2
                elif source.startswith(";)", offset):
                    depth -= 1
                    offset += 2
                else:
                    offset += 1
        else:
            break
    return offset


def _top_level_forms(source: str):
    """Yield balanced top-level forms with their byte offsets."""
    offset = 0
    while True:
        offset = _skip_trivia(source, offset)
        if offset >= len(source):
            return
        if source[offset] != "(":
            offset += 1
            continue
        end = _balanced_form_end(source, offset)
        if end is None:
            return
        yield offset, source[offset:end]
        offset = end


@lru_cache(maxsize=32)
def _cached_top_level_forms(source: str) -> tuple[tuple[int, str], ...]:
    return tuple(_top_level_forms(source))


def _direct_child_forms(form: str):
    """Yield direct parenthesized fields from an S-expression form."""
    offset = 1
    while offset < len(form) - 1:
        offset = _skip_trivia(form, offset)
        if offset >= len(form) - 1:
            return
        if form[offset] != "(":
            offset += 1
            continue
        end = _balanced_form_end(form, offset)
        if end is None:
            return
        yield form[offset:end]
        offset = end


def _read_u32_leb(data: bytearray, offset: int,
                  end: int | None = None) -> tuple[int | None, int]:
    limit = len(data) if end is None else end
    value = shift = 0
    while offset < limit and shift < 35:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return (value if value <= 0xffffffff else None), offset
        shift += 7
    return None, offset


def _read_s33_leb(data: bytearray, offset: int,
                  end: int) -> tuple[int | None, int]:
    value = shift = 0
    while offset < end and shift < 35:
        byte = data[offset]
        offset += 1
        if shift == 28 and byte < 0x80:
            sign = byte & 0x10
            if (byte & 0x60) != (0x60 if sign else 0):
                return None, offset
        value |= (byte & 0x7f) << shift
        shift += 7
        if byte < 0x80:
            if byte & 0x40:
                value |= -(1 << shift)
            return value, offset
    return None, offset


def _read_u64_leb(data: bytearray, offset: int,
                  end: int) -> tuple[int | None, int]:
    value = shift = 0
    while offset < end and shift < 70:
        byte = data[offset]
        offset += 1
        value |= (byte & 0x7f) << shift
        if byte < 0x80:
            return (value if value <= 0xffffffffffffffff else None), offset
        shift += 7
    return None, offset


def _read_value_type(data: bytearray, offset: int,
                     end: int) -> tuple[bool | None, int | None]:
    if offset >= end:
        return None, None
    prefix = data[offset]
    offset += 1
    if prefix in (0x63, 0x64):
        heap, offset = _read_s33_leb(data, offset, end)
        if heap is None:
            return None, None
        return heap >= 0 or (prefix == 0x64 and heap in (-16, -17, -23)), offset
    if prefix in (0x7c, 0x7d, 0x7e, 0x7f, 0x7b, 0x70, 0x6f, 0x6e,
                  0x6d, 0x6c, 0x6b, 0x6a, 0x69, 0x68):
        return False, offset
    return None, None


def _read_field_type(data: bytearray, offset: int,
                     end: int) -> tuple[bool | None, int | None]:
    typed, offset = _read_value_type(data, offset, end)
    if typed is None or offset is None or offset >= end:
        return None, None
    return typed, offset + 1  # mutability


def _read_composite_type(data: bytearray, offset: int,
                         end: int) -> tuple[bool | None, int | None]:
    if offset >= end:
        return None, None
    form = data[offset]
    offset += 1
    if form == 0x60:  # function: parameter vector followed by result vector
        for _ in range(2):
            count, offset = _read_u32_leb(data, offset, end)
            if count is None:
                return None, None
            for _ in range(count):
                typed, offset = _read_value_type(data, offset, end)
                if typed is None:
                    return None, None
                if typed:
                    return True, offset
        return False, offset
    if form in (0x5f, 0x5e):  # struct fields or one array field
        count = 1
        if form == 0x5f:
            count, offset = _read_u32_leb(data, offset, end)
            if count is None:
                return None, None
        for _ in range(count):
            typed, offset = _read_field_type(data, offset, end)
            if typed is None:
                return None, None
            if typed:
                return True, offset
        return False, offset
    return None, None


def _read_sub_type(data: bytearray, offset: int,
                   end: int) -> tuple[bool | None, int | None]:
    if offset >= end:
        return None, None
    if data[offset] in (0x4f, 0x50):
        offset += 1
        count, offset = _read_u32_leb(data, offset, end)
        if count is None:
            return None, None
        for _ in range(count):
            supertype, offset = _read_u32_leb(data, offset, end)
            if supertype is None:
                return None, None
        return _read_composite_type(data, offset, end)
    return _read_composite_type(data, offset, end)


def _read_type(data: bytearray, offset: int,
               end: int) -> tuple[bool | None, int | None]:
    if offset >= end:
        return None, None
    if data[offset] == 0x4e:  # recursive type group
        offset += 1
        count, offset = _read_u32_leb(data, offset, end)
        if count is None:
            return None, None
        typed = False
        for _ in range(count):
            member, offset = _read_sub_type(data, offset, end)
            if member is None:
                return None, None
            typed |= member
        return typed, offset
    return _read_sub_type(data, offset, end)


def _type_section_has_typed_reference(data: bytearray, start: int,
                                      end: int) -> bool:
    count, offset = _read_u32_leb(data, start, end)
    if count is None:
        return False
    for _ in range(count):
        typed, offset = _read_type(data, offset, end)
        if typed is None:
            return False
        if typed:
            return True
    return False


def _skip_limits(data: bytearray, offset: int, end: int) -> int | None:
    flags, offset = _read_u32_leb(data, offset, end)
    if flags is None:
        return None
    read_limit = _read_u64_leb if flags & 4 else _read_u32_leb
    minimum, offset = read_limit(data, offset, end)
    if minimum is None:
        return None
    if flags & 1:
        maximum, offset = read_limit(data, offset, end)
        if maximum is None:
            return None
    return offset


def _read_table_type(data: bytearray, offset: int, end: int,
                     allow_initializer: bool) -> tuple[bool | None, int | None]:
    has_initializer = offset < end and data[offset] == 0x40
    if has_initializer:
        if not allow_initializer or offset + 1 >= end or data[offset + 1] != 0:
            return None, None
        offset += 2
    if offset >= end:
        return None, None
    prefix = data[offset]
    if prefix in (0x63, 0x64):
        heap, offset = _read_s33_leb(data, offset + 1, end)
        if heap is None:
            return None, None
        # Nullable concrete references and non-nullable function/extern/exception
        # references are deferred; nullable abstract funcref/externref are core.
        typed = heap >= 0 or (prefix == 0x64 and heap in (-16, -17, -23))
    elif 0x68 <= prefix <= 0x73:
        offset += 1
        typed = False
    else:
        return None, None
    offset = _skip_limits(data, offset, end)
    if offset is None:
        return None, None
    if has_initializer:
        offset = _skip_const_expr(data, offset, end)
        if offset is None:
            return None, None
    return typed, offset


def _table_section_has_typed_table(data: bytearray, start: int, end: int) -> bool:
    count, offset = _read_u32_leb(data, start, end)
    if count is None:
        return False
    for _ in range(count):
        typed, offset = _read_table_type(data, offset, end, True)
        if typed is None:
            return False
        if typed:
            return True
    return False


def _skip_name(data: bytearray, offset: int, end: int) -> int | None:
    length, offset = _read_u32_leb(data, offset, end)
    if length is None or offset + length > end:
        return None
    return offset + length


def _import_section_has_typed_table(data: bytearray, start: int, end: int) -> bool:
    count, offset = _read_u32_leb(data, start, end)
    if count is None:
        return False
    for _ in range(count):
        offset = _skip_name(data, offset, end)
        if offset is None:
            return False
        offset = _skip_name(data, offset, end)
        if offset is None or offset >= end:
            return False
        kind = data[offset]
        offset += 1
        if kind == 0:
            _, offset = _read_u32_leb(data, offset, end)
        elif kind == 1:
            typed, offset = _read_table_type(data, offset, end, False)
            if typed:
                return True
            if offset is None:
                return False
        elif kind == 2:
            offset = _skip_limits(data, offset, end)
        elif kind == 3:
            if offset >= end:
                return False
            prefix = data[offset]
            offset += 1
            if prefix in (0x63, 0x64):
                _, offset = _read_s33_leb(data, offset, end)
            if offset is not None and offset < end:
                offset += 1  # mutability
            else:
                return False
        elif kind == 4:
            if offset >= end:
                return False
            offset += 1  # attribute
            _, offset = _read_u32_leb(data, offset, end)
        else:
            return False
        if offset is None or offset > end:
            return False
    return False


def _skip_const_expr(data: bytearray, offset: int, end: int) -> int | None:
    while offset < end:
        opcode = data[offset]
        offset += 1
        if opcode == 0x0b:
            return offset
        if opcode in (0x41, 0x42):
            _, offset = _read_u32_leb(data, offset, end)
        elif opcode in (0x23, 0xd2):
            _, offset = _read_u32_leb(data, offset, end)
        elif opcode == 0x43:
            offset += 4
        elif opcode == 0x44:
            offset += 8
        elif opcode == 0xd0:
            _, offset = _read_s33_leb(data, offset, end)
        else:
            return None
        if offset is None or offset > end:
            return None
    return None


def _module_typed_exports(module: str) -> tuple[str | None, dict[str, bool]]:
    """Map export names to whether the exported function uses deferred syntax."""
    named = re.match(r"\(module\s+(\$[A-Za-z0-9_.$-]+)", module)
    module_id = named.group(1) if named else None
    children = list(_direct_child_forms(module))
    type_names: dict[str, int] = {}
    type_features: dict[int, bool] = {}
    next_type = 0
    for child in children:
        if not re.match(r"\(type\b", child):
            continue
        name = re.match(r"\(type\s+(\$[A-Za-z0-9_.$-]+)", child)
        if name:
            type_names[name.group(1)] = next_type
        type_features[next_type] = bool(
            FEATURE_MARKERS["typed-func-refs"].search(mask_comments_and_strings(child)))
        next_type += 1

    def uses_typed_type(form: str) -> bool:
        for match in re.finditer(r"\(type\s+([^\s)]+)\s*\)", form):
            type_ref = match.group(1)
            if type_ref.startswith("$"):
                index = type_names.get(type_ref)
            else:
                try:
                    index = int(type_ref, 10)
                except ValueError:
                    index = None
            if index is not None and type_features.get(index, False):
                return True
        return False

    imported_names: dict[str, int] = {}
    function_features: dict[int, bool] = {}
    import_count = 0
    for child in children:
        if not re.match(r"\(import\b", child):
            continue
        for descriptor in _direct_child_forms(child):
            func = re.match(r"\(func(?:\s+(\$[A-Za-z0-9_.$-]+))?\b", descriptor)
            if func:
                function_features[import_count] = bool(
                    FEATURE_MARKERS["typed-func-refs"].search(
                        mask_comments_and_strings(descriptor))) or uses_typed_type(descriptor)
                if func.group(1):
                    imported_names[func.group(1)] = import_count
                import_count += 1
                break
    function_names: dict[str, int] = {}
    next_index = import_count
    inline_exports: dict[str, int] = {}
    for child in children:
        func = re.match(r"\(func(?:\s+(\$[A-Za-z0-9_.$-]+))?\b", child)
        if not func:
            continue
        index = next_index
        next_index += 1
        if func.group(1):
            function_names[func.group(1)] = index
        function_features[index] = bool(
            FEATURE_MARKERS["typed-func-refs"].search(mask_comments_and_strings(child))) \
            or uses_typed_type(child)
        for export in re.finditer(r'\(export\s+"((?:\\.|[^"\\])*)"\)', child):
            inline_exports[export.group(1)] = index
    exports = dict(inline_exports)
    for child in children:
        if not re.match(r"\(export\b", child):
            continue
        export = re.match(
            r'\(export\s+"((?:\\.|[^"\\])*)"\s+\(func\s+([^\s)]+)\)\s*\)', child)
        if not export:
            continue
        target = export.group(2)
        if target.startswith("$"):
            index = function_names.get(target, imported_names.get(target))
        else:
            try:
                index = int(target, 10)
            except ValueError:
                index = None
        if index is not None:
            exports[export.group(1)] = index
    return module_id, {name: function_features.get(index, False)
                       for name, index in exports.items()}


def _form_head(form: str) -> str | None:
    match = re.match(r"\(\s*([^\s()]+)", form)
    return match.group(1) if match else None


def assertion_contains_feature_marker(source: str, line_number: int,
                                      form: str, reason: str) -> bool:
    if contains_feature_marker(form, reason):
        return True
    if reason != "typed-func-refs":
        return False
    action = re.search(
        r'\(invoke\s+(?:(\$[A-Za-z0-9_.$-]+)\s+)?"((?:\\.|[^"\\])*)"', form)
    if not action:
        return False
    requested_module, target = action.groups()
    line_starts = _cached_line_starts(source)
    line_start = line_starts[min(line_number - 1, len(line_starts) - 1)]
    assertion_start = source.find(form, line_start)
    if assertion_start < 0:
        return False
    modules: dict[str, dict[str, bool]] = {}
    registrations: dict[str, dict[str, bool]] = {}
    current: dict[str, bool] | None = None
    for start, top_form in _cached_top_level_forms(source):
        if start >= assertion_start:
            break
        head = _form_head(top_form)
        if head == "module":
            module_id, exports = _module_typed_exports(top_form)
            current = exports
            if module_id:
                modules[module_id] = exports
        elif head == "register":
            register = re.match(
                r'\(register\s+"((?:\\.|[^"\\])*)"(?:\s+(\$[A-Za-z0-9_.$-]+))?',
                top_form)
            if register:
                name, module_id = register.groups()
                selected = modules.get(module_id) if module_id else current
                if selected is not None:
                    registrations[name] = selected
                    registrations["$" + name] = selected
    selected = registrations.get(requested_module) if requested_module else current
    if requested_module and selected is None:
        selected = modules.get(requested_module)
    return bool(selected and selected.get(target, False))


@lru_cache(maxsize=32)
def _cached_line_starts(source: str) -> tuple[int, ...]:
    return (0, *(index + 1 for index, char in enumerate(source) if char == "\n"))


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
        if reason == "non-core" and "/custom/" in relative:
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
            if not form or not assertion_contains_feature_marker(
                    (corpus_root.parent / path).read_text(encoding="utf-8"),
                    int(source_line), form, reason):
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
