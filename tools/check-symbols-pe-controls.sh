#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-only
# Prove the PE export gate accepts a named export and catches three defects.
set -eu

token=${1:?usage: check-symbols-pe-controls.sh TOKEN ROOT}
root=${2:?usage: check-symbols-pe-controls.sh TOKEN ROOT}
case "$token" in
  *[!A-Za-z0-9_]*|'') echo 'invalid symbol token' >&2; exit 2 ;;
esac
temp=$(mktemp -d "${TMPDIR:-/tmp}/glwasm-pe-symbols.XXXXXXXX")
trap 'rm -rf "$temp"' EXIT HUP INT TERM

printf '__declspec(dllexport) int %s_control(void) { return 1; }\n' \
  "$token" > "$temp/named.c"
cc -shared -o "$temp/named.dll" "$temp/named.c"
"$root/tools/check-symbols.sh" "$temp/named.dll" "$token" "$root" \
  > "$temp/named.out" 2>&1 || {
    cat "$temp/named.out" >&2
    echo 'PE named-export control failed' >&2
    exit 1
  }

printf '__declspec(dllexport) int unversioned_probe(void) { return 1; }\n' \
  > "$temp/leak.c"
cc -shared -o "$temp/leak.dll" "$temp/leak.c"

printf 'int %s_control(void) { return 1; }\n' "$token" \
  > "$temp/ordinal.c"
printf 'int hidden_ordinal(void) { return 2; }\n' >> "$temp/ordinal.c"
printf 'LIBRARY ordinal\nEXPORTS\n%s_control\nhidden_ordinal @2 NONAME\n' \
  "$token" > "$temp/ordinal.def"
cc -shared -o "$temp/ordinal.dll" "$temp/ordinal.c" "$temp/ordinal.def"

printf '__declspec(dllexport) int %s_missing(void) { return 3; }\n' \
  "$token" > "$temp/provider.c"
cc -shared -o "$temp/provider.dll" "$temp/provider.c" \
  -Wl,--out-implib,"$temp/provider.dll.a"
printf '__declspec(dllimport) int %s_missing(void);\n' "$token" \
  > "$temp/consumer.c"
printf '__declspec(dllexport) int %s_control(void) { return %s_missing(); }\n' \
  "$token" "$token" >> "$temp/consumer.c"
cc -shared -o "$temp/consumer.dll" "$temp/consumer.c" \
  "$temp/provider.dll.a"

reject() {
  label=$1
  needle=$2
  if "$root/tools/check-symbols.sh" "$temp/$label.dll" "$token" "$root" \
      > "$temp/$label.out" 2>&1; then
    echo "PE $label defect passed the symbol gate" >&2
    exit 1
  fi
  if ! grep -Fq "$needle" "$temp/$label.out"; then
    cat "$temp/$label.out" >&2
    echo "PE $label defect failed for the wrong reason" >&2
    exit 1
  fi
}
reject leak 'Exported symbols missing'
reject ordinal 'unnamed PE exports'
reject consumer 'Renamed but undefined'
echo 'check-symbols-pe-controls: named, leaked, ordinal-only and split-import cases behaved'
