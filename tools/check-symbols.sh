#!/bin/sh
#
# Fail if the shared library's exported names or the headers break the
# versioned-namespace rules (CONVENTIONS.md section 4).
#
#   - every exported data or code symbol carries the version token;
#   - every public function declaration says GLWASM_API;
#   - no symbol is renamed by the preprocessor on the way in and left
#     undefined (a "split symbol": a call site that saw the macro and a
#     definition that did not);
#   - every header includes macros.h, uses this library's include-guard
#     prefix, and shares its guard with no other header.
#
# Usage: check-symbols.sh <shared-library> <version-token> <root>
#   <root> holds include/ and src/. The Makefile runs this against the real
#   tree, and check-gates.sh runs the same script against tests/gates/symbols
#   fixtures and small shared objects it builds, so that each check is seen
#   to fail on a planted defect.
#
# Fails on an empty population: a library exporting nothing means the gate
# measured nothing.

set -u

LIB="${1:?usage: check-symbols.sh <shared-library> <version-token> <root>}"
TOKEN="${2:?usage: check-symbols.sh <shared-library> <version-token> <root>}"
ROOT="${3:?usage: check-symbols.sh <shared-library> <version-token> <root>}"

if [ ! -f "$LIB" ]; then
  printf 'check-symbols: %s does not exist; this gate is measuring nothing\n' \
    "$LIB" >&2
  exit 1
fi

exported="$(nm -D --defined-only "$LIB" | awk '$2 ~ /^[TDBR]$/ {print $3}')"
if [ -z "$exported" ]; then
  printf 'check-symbols: %s exports no symbol at all; this gate is measuring nothing\n' \
    "$LIB" >&2
  exit 1
fi

cd "$ROOT" || exit 1

leaked="$(printf '%s\n' "$exported" | grep -v "^${TOKEN}_" | grep -v '^_' || true)"
if [ -n "$leaked" ]; then
  printf '### Exported symbols missing the %s_ namespace ###\n%s\n' "$TOKEN" "$leaked" >&2
  exit 1
fi

unexported="$(find include -name '*.h' -exec awk '/^#if DOXYGEN/{d=1} d==0 && !/GLWASM_API/ && /^[A-Za-z_][A-Za-z0-9_ ]*\**[[:space:]]*glwasm_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$0} /^#endif/{d=0}' {} + \
  | grep -vE 'typedef|static inline' || true)"
if [ -n "$unexported" ]; then
  printf '### Public declarations without GLWASM_API ###\n%s\n' "$unexported" >&2
  exit 1
fi

split="$(nm -D --undefined-only "$LIB" | awk '{print $2}' | grep "^${TOKEN}_" || true)"
if [ -n "$split" ]; then
  printf '### Renamed but undefined - a split symbol ###\n%s\n' "$split" >&2
  exit 1
fi

nomacros="$(find include src -name '*.h' \
  ! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
  -exec grep -L '#include <ghoti.io/lang-wasm/macros.h>' {} + || true)"
if [ -n "$nomacros" ]; then
  printf '### Headers that do not include macros.h ###\n%s\n' "$nomacros" >&2
  exit 1
fi

badguards="$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $2; d=1}' {} + \
  | awk '$1 !~ /^GHOTI_IO_GLWASM_/ {print $1}' || true)"
if [ -n "$badguards" ]; then
  printf '### Include guards with the wrong prefix ###\n%s\n' "$badguards" >&2
  exit 1
fi

dupguards="$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $2; d=1}' {} + \
  | sort | uniq -d || true)"
if [ -n "$dupguards" ]; then
  printf '### Headers sharing an include guard ###\n%s\n' "$dupguards" >&2
  exit 1
fi

printf 'Every exported symbol carries the %s_ namespace.\n' "$TOKEN"
