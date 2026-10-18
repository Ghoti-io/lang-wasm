#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
set -eu
usage() {
  cat <<'USAGE'
usage: build-and-run.sh --check-pins | --propose-pins | --all-tiers
--check-pins    verify local source trees, tools, and pin manifests before build
--propose-pins  build and report candidate module hashes/results for review
--all-tiers     compare native and independent Wasmtime results to goldens
USAGE
}
mode=${1:-}
case "$mode" in --check-pins|--propose-pins|--all-tiers) ;; *) usage >&2; exit 2 ;; esac
here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../.." && pwd)
data="$repo/tests/real-corpus"
output="$repo/build/real-corpus"
output=${GLWASM_REAL_CORPUS_BUILD_DIR:-$output}
source_root="$output/upstream"
module_root="$output/modules"
native_root="$output/native"
module_pins="$data/module-hashes.tsv"
vectors="$data/vectors.tsv"
import_manifest="$data/imports.tsv"
architecture=$(uname -m)
case "$architecture" in
  x86_64|aarch64) ;;
  *) echo "unsupported corpus build architecture: $architecture" >&2; exit 1 ;;
esac

: "${WASI_SDK_PATH:=/opt/wasi-sdk-34.0}"
export WASI_SDK_PATH
WASI_CC="$WASI_SDK_PATH/bin/clang"
WASMTIME=/usr/local/bin/wasmtime
WASMTIME_C_API=/opt/wasmtime-c-api-49.0.3
CC=gcc
(cd "$repo" && sha256sum -c "$data/tools.sha256")
python3 "$here/test_fetch_sources.py"
python3 "$here/test_verify_pins.py"
python3 "$here/fetch_sources.py" --pins "$data/pins.tsv" \
  --destination "$source_root" --check-only
python3 "$here/verify_licenses.py" "$source_root" "$data"
(cd "$data" && sha256sum -c vectors.sha256)
(cd "$data" && sha256sum -c build-flags.sha256)
(cd "$data" && sha256sum -c drivers.sha256)
python3 "$here/verify_pins.py" "$data" "$architecture" "$mode"
python3 "$here/test_check_imports.py"
[ -x "$WASI_CC" ] || { echo "pinned wasi-sdk 34 is required at $WASI_SDK_PATH" >&2; exit 1; }
"$WASMTIME" --version 2>&1 | grep -Eq 'wasmtime 49\.0\.3([[:space:]]|$)' || {
  echo 'the independent engine must be Wasmtime 49.0.3' >&2; exit 1;
}
native_version=$($CC -dumpfullversion)
[ "$native_version" = 16.2.0 ] || {
  echo "native oracle requires GCC 16.2.0, found $native_version ($CC)" >&2; exit 1;
}
"$CC" -v 2>&1 | grep -q 'gcc version 16\.2\.0' || {
  echo "native compiler is not the pinned GCC 16.2.0: $CC" >&2; exit 1;
}
if [ "$mode" = --check-pins ]; then
  printf 'source, compiler, engine and pin manifests verified; module bytes require --all-tiers\n'
  exit 0
fi

. "$data/build-flags.sh"
mkdir -p "$module_root" "$native_root"
sqlite_dir=$(find "$source_root/sqlite" -name sqlite3.c -print -quit | xargs -r dirname)
zlib_dir=$(find "$source_root/zlib" -name zlib.h -print -quit | xargs -r dirname)
lua_src=$(find "$source_root/lua" -type d -name src -print -quit)
[ -n "$sqlite_dir" ] && [ -n "$zlib_dir" ] && [ -n "$lua_src" ] || {
  echo 'verified source tree is missing expected sources' >&2; exit 1;
}
lua_sources=''
for file in "$lua_src"/*.c; do
  case "$(basename "$file")" in
    lua.c|luac.c|onelua.c|lbaselib.c|lcorolib.c|ldblib.c|liolib.c|lmathlib.c|loadlib.c|loslib.c|lutf8lib.c|linit.c)
      # Keep this embedded workload independent of host services. The upstream
      # linit aggregator retains optional I/O, OS, dynamic-loading and clock
      # APIs that this driver neither registers nor exercises.
      ;;
    *) lua_sources="$lua_sources $file" ;;
  esac
done

# Native and WASI share source lists and optimization/floating-point flags.
# Modules have no entry point and export only the corpus status and byte-output
# functions; drivers do not access argv, filesystem, clock, or environment APIs.
# shellcheck disable=SC2086
"$CC" $CORPUS_CFLAGS $CORPUS_SQLITE_DEFINES -I"$sqlite_dir" "$sqlite_dir/sqlite3.c" \
  "$data/sqlite_driver.c" "$data/native_main.c" $CORPUS_NATIVE_LDFLAGS \
  -o "$native_root/sqlite"
# shellcheck disable=SC2086
"$CC" $CORPUS_CFLAGS -I"$zlib_dir" \
  $(for file in $CORPUS_ZLIB_SOURCES; do printf '%s ' "$zlib_dir/$file"; done) \
  "$data/zlib_driver.c" "$data/native_main.c" $CORPUS_NATIVE_LDFLAGS \
  -o "$native_root/zlib"
# shellcheck disable=SC2086
"$CC" $CORPUS_CFLAGS -I"$lua_src" $lua_sources "$data/lua_driver.c" \
  "$data/native_main.c" $CORPUS_LUA_NATIVE_LDFLAGS -o "$native_root/lua"
# shellcheck disable=SC2086
"$WASI_CC" $CORPUS_CFLAGS $CORPUS_SQLITE_DEFINES -I"$sqlite_dir" "$sqlite_dir/sqlite3.c" \
  "$data/sqlite_driver.c" $CORPUS_WASI_LDFLAGS \
  -o "$module_root/sqlite.wasm"
# shellcheck disable=SC2086
"$WASI_CC" $CORPUS_CFLAGS -I"$zlib_dir" \
  $(for file in $CORPUS_ZLIB_SOURCES; do printf '%s ' "$zlib_dir/$file"; done) \
  "$data/zlib_driver.c" $CORPUS_WASI_LDFLAGS \
  -o "$module_root/zlib.wasm"
# shellcheck disable=SC2086
"$WASI_CC" $CORPUS_CFLAGS $CORPUS_LUA_WASI_CFLAGS -I"$lua_src" $lua_sources "$data/lua_driver.c" \
  $CORPUS_WASI_LDFLAGS $CORPUS_LUA_WASI_LDFLAGS \
  -o "$module_root/lua.wasm"

for name in sqlite zlib lua; do
  python3 "$here/check_imports.py" "$import_manifest" "$name" "$module_root/$name.wasm"
done
# The independent host links only the checksum-pinned C API. It loads the
# production modules above, never a separately compiled oracle reactor.
"$CC" -std=c17 -O2 -Wall -Wextra -Werror \
  -I"$WASMTIME_C_API/include" "$data/wasmtime_host.c" \
  -L"$WASMTIME_C_API/lib" -Wl,-rpath,"$WASMTIME_C_API/lib" \
  -lwasmtime -o "$native_root/wasmtime-host"
sha256() { sha256sum "$1" | awk '{print $1}'; }
run_wasmtime() {
  name=$1
  expected_hex=$2
  "$native_root/wasmtime-host" "$name" "$module_root/$name.wasm" "$expected_hex"
}
run_native() {
  name=$1
  raw=$("$native_root/$name") || return
  python3 - "$name" "$raw" <<'PY'
import sys
name, raw = sys.argv[1:]
parts = raw.rstrip("\n").split("\t")
if len(parts) != 3 or not parts[1].isdigit() or len(parts[2]) != int(parts[1]) * 2:
    raise SystemExit(f"{name}: malformed native corpus output: {raw!r}")
try:
    bytes.fromhex(parts[2])
except ValueError:
    raise SystemExit(f"{name}: native corpus output contains invalid hex")
print(":".join(parts))
PY
}
if [ "$mode" = --propose-pins ]; then
  candidate_modules="$output/proposed-module-hashes.tsv"
  candidate_vectors="$output/proposed-vectors"
  mkdir -p "$candidate_vectors/vectors"
  printf '# workload<TAB>architecture<TAB>module SHA-256\n' > "$candidate_modules"
  printf '# workload<TAB>case<TAB>status<TAB>byte-length<TAB>output-hex-file\n' > "$candidate_vectors/vectors.tsv"
  for name in sqlite zlib lua; do
    native=$(run_native "$name")
    python3 - "$name" "$native" <<'PY'
import hashlib, sys
name, value = sys.argv[1:]
status, length, output_hex = value.split(":", 2)
print(f"{name} native output: status={status} length={length} sha256={hashlib.sha256(bytes.fromhex(output_hex)).hexdigest()}")
PY
    result_hex=${native##*:}
    independent=$(run_wasmtime "$name" "$result_hex")
    [ "$native" = "$independent" ] || { echo "$name: native and Wasmtime outputs differ" >&2; exit 1; }
    status=${native%%:*}
    result_tail=${native#*:}
    result_length=${result_tail%%:*}
    result_hex=${result_tail#*:}
    python3 -c 'import pathlib,sys; value=sys.argv[1]; path=pathlib.Path(sys.argv[2]); path.write_text("\n".join(value[i:i+128] for i in range(0,len(value),128))+"\n")' \
      "$result_hex" "$candidate_vectors/vectors/$name.hex"
    printf '%s\t%s\t%s\n' "$name" "$architecture" "$(sha256 "$module_root/$name.wasm")" >> "$candidate_modules"
    printf '%s\tdefault\t%s\t%s\tvectors/%s.hex\n' "$name" "$status" "$result_length" "$name" >> "$candidate_vectors/vectors.tsv"
  done
  (cd "$candidate_vectors" && sha256sum vectors.tsv vectors/*.hex > vectors.sha256)
  printf 'candidate module hashes: %s\ncandidate complete vectors: %s\n' \
    "$candidate_modules" "$candidate_vectors"
  exit 0
fi

[ -f "$module_pins" ] || { echo 'module-hashes.tsv is missing; reviewed pins are required' >&2; exit 1; }
for name in sqlite zlib lua; do
  expected_module=$(awk -F '\t' -v name="$name" -v arch="$architecture" '$1==name && $2==arch {print $3}' "$module_pins")
  actual_module=$(sha256 "$module_root/$name.wasm")
  if [ -z "$expected_module" ] || [ "$actual_module" != "$expected_module" ]; then
    printf '%s: module SHA-256 mismatch (expected %s, got %s)\n' "$name" "${expected_module:-<missing>}" "$actual_module" >&2
    exit 1
  fi
done
for name in sqlite zlib lua; do
  expected_status=$(awk -F '\t' -v name="$name" '$1==name && $2=="default" {print $3}' "$vectors")
  expected_length=$(awk -F '\t' -v name="$name" '$1==name && $2=="default" {print $4}' "$vectors")
  expected_file=$(awk -F '\t' -v name="$name" '$1==name && $2=="default" {print $5}' "$vectors")
  expected_hex=$(tr -d '\n' < "$data/$expected_file")
  expected="$expected_status:$expected_length:$expected_hex"
  native=$(run_native "$name")
  independent=$(run_wasmtime "$name" "$expected_hex")
  for engine in native wasmtime; do
    actual=$native
    [ "$engine" = wasmtime ] && actual=$independent
    if [ -z "$expected" ] || [ "$actual" != "$expected" ]; then
      python3 - "$name" "$engine" "$expected" "$actual" <<'PY' >&2
import sys
name, engine, expected, actual = sys.argv[1:]
def parts(value):
    status, length, data = value.split(":", 2)
    return status, int(length), bytes.fromhex(data)
try:
    expected_status, expected_length, expected_bytes = parts(expected)
    actual_status, actual_length, actual_bytes = parts(actual)
except (ValueError, TypeError):
    raise SystemExit(f"{name}/default: {engine} emitted malformed result record")
if actual_status != expected_status:
    detail = f"status expected {expected_status}, got {actual_status}"
elif actual_length != expected_length:
    detail = f"length expected {expected_length}, got {actual_length}"
else:
    offset = next((i for i, (want, got) in enumerate(zip(expected_bytes, actual_bytes))
                   if want != got), None)
    if offset is None:
        offset = min(len(expected_bytes), len(actual_bytes))
    want = f"0x{expected_bytes[offset]:02x}" if offset < len(expected_bytes) else "<end>"
    got = f"0x{actual_bytes[offset]:02x}" if offset < len(actual_bytes) else "<end>"
    detail = f"first byte mismatch at offset {offset}: expected {want}, got {got}"
raise SystemExit(f"{name}/default: {engine} result mismatch: {detail}")
PY
    fi
  done
done
printf 'native and Wasmtime match all pinned real-program vectors\n'
