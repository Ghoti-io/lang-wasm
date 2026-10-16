#!/bin/sh
set -eu

db="$(make -s -pn help 2>/dev/null)"
[ -n "$db" ] || { echo 'check-fp-contract: make returned no variables' >&2; exit 1; }

printf '%s\n' "$db" | awk '
  /^[A-Za-z_][A-Za-z0-9_]* *(:|::)?= / {
    key = $1
    value = $0
    sub(/^[^=]*= */, "", value)
    if (key ~ /(CFLAGS|CXXFLAGS)$/ && value ~ /-std=/) {
      count++
      last = ""
      n = split(value, flag, /[ \t]+/)
      for (i = 1; i <= n; ++i) {
        if (flag[i] ~ /^-ffp-contract=/) last = flag[i]
        if (flag[i] == "-ffast-math" || flag[i] == "-Ofast") bad = key
      }
      if (last != "-ffp-contract=off") bad = key
    }
      if (key == "FUZZ_SAN" || key == "FUZZ_LIB_FLAGS" || key == "FUZZ_BIN_FLAGS") {
        fuzz_count++
        fuzz_last = ""
        n = split(value, flag, /[ \t]+/)
        for (i = 1; i <= n; ++i) {
          if (flag[i] ~ /^-ffp-contract=/) fuzz_last = flag[i]
          if (flag[i] == "-ffast-math" || flag[i] == "-Ofast") bad = key
        }
        if (fuzz_last != "-ffp-contract=off") bad = key
      }
  }
  END {
    if (count == 0 || fuzz_count != 3 || bad != "") {
      printf "check-fp-contract: invalid guest-float flags%s\n", bad == "" ? " (none found)" : " in " bad > "/dev/stderr"
      exit 1
    }
    printf "check-fp-contract: %d compile flag sets and %d fuzz flag sets disable contraction\n", count, fuzz_count
  }'
