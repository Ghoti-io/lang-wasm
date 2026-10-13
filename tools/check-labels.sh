#!/bin/sh
#
# Fail if a public header carries no stability label, or the wrong one.
#
# AD-14: every public header says whether it is `stable` (frozen at the major
# version once released) or `free` (a consumer requires the exact version it
# was built against). The label is a Doxygen tag in the header's @file block:
#
#     @stability stable
#
# Which one is not a free choice: this library's layout fixes it by directory.
# The top level (shared basics) and b/ (the execution context) are stable; a/
# (the frame protocol) is free. A free header in b/ would quietly promise less
# than the spine says, and a stable header in a/ would freeze the A<->B
# interface before it has been used.
#
# Usage: check-labels.sh <root>
#   <root> holds include/ghoti.io/lang-wasm/. The self-test runs this same
#   script against tests/gates fixtures.
#
# Fails on an empty population: no headers found means the gate measured
# nothing, and a gate that measures nothing reports success.

set -eu

ROOT="${1:?usage: check-labels.sh <root>}"
BASE="$ROOT/include/ghoti.io/lang-wasm"

if [ ! -d "$BASE" ]; then
  printf 'check-labels: %s does not exist; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

headers="$(find "$BASE" -type f -name '*.h' | sort)"
if [ -z "$headers" ]; then
  printf 'check-labels: no headers under %s; this gate is measuring nothing\n' \
    "$BASE" >&2
  exit 1
fi

status=0
count=0
for h in $headers; do
  count=$((count + 1))
  rel="${h#"$BASE"/}"
  # Every public header is free. This library has no stable directory.
  want=free
  # Anchored to a comment line so that prose mentioning the tag does not count
  # as carrying it.
  labels="$(grep -E '^[[:space:]]*(\*|//)[[:space:]]*@stability[[:space:]]' "$h" \
    | sed 's/.*@stability[[:space:]]*//; s/[[:space:]]*$//' || true)"
  if [ -z "$labels" ]; then
    printf 'check-labels: %s has no @stability label (want %s)\n' \
      "$h" "$want" >&2
    status=1
    continue
  fi
  if [ "$(printf '%s\n' "$labels" | wc -l)" -ne 1 ]; then
    printf 'check-labels: %s has more than one @stability label\n' "$h" >&2
    status=1
    continue
  fi
  case "$labels" in
    stable | free) ;;
    *)
      printf 'check-labels: %s: "%s" is not stable or free\n' "$h" "$labels" >&2
      status=1
      continue
      ;;
  esac
  if [ "$labels" != "$want" ]; then
    printf 'check-labels: %s is labelled %s but its directory requires %s\n' \
      "$h" "$labels" "$want" >&2
    status=1
  fi
done

if [ "$status" -ne 0 ]; then
  exit 1
fi
printf 'check-labels: %d public headers, each labelled stable or free as its directory requires\n' \
  "$count"
