#!/bin/sh
#
# Summarize gcov data for a build that was compiled with --coverage.
#
# Run from the project root, with the object directory as the only argument.
# Prints per-file line coverage, the project total, and - separately - the
# growth and resize lines that never executed.
#
# That last list is the point of the report. A dynamic structure whose
# reallocation path is never taken by any test is not covered by "all tests
# pass"; a defect of exactly that shape (a hash table that corrupted itself
# when it grew) survived a full green suite in this codebase.

set -eu

OBJ_DIR="${1:?usage: coverage.sh <object-dir>}"

if ! command -v gcov >/dev/null 2>&1; then
  echo "coverage: gcov not found (install gcc's gcov)" >&2
  exit 1
fi

GCDA=$(find "$OBJ_DIR" -name '*.gcda' 2>/dev/null || true)
if [ -z "$GCDA" ]; then
  echo "coverage: no profile data under $OBJ_DIR" >&2
  echo "coverage: the tests must run after an instrumented build" >&2
  exit 1
fi

rm -f ./*.gcov
# gcov resolves the Source: paths relative to the directory it runs in, so it
# is invoked from the project root with the object directory passed per file.
for g in $GCDA; do
  gcov -p -r -o "$(dirname "$g")" "$g" >/dev/null 2>&1 || true
done

awk '
  FNR == 1 { src = "" }
  # The Source: header names the file this .gcov describes.
  src == "" && /Source:/ {
    n = index($0, "Source:")
    src = substr($0, n + 7)
    sub(/^[ \t]+/, "", src)
    # Only report on the library itself, not tests or system headers.
    if (src !~ /^src\//) { src = "SKIP" }
    next
  }
  src == "" || src == "SKIP" { next }

  {
    # Each line is "<count>:<lineno>:<text>", where the count is a number, a
    # dash for a non-executable line, or ##### for one never executed.
    c1 = index($0, ":")
    if (c1 == 0) next
    count = substr($0, 1, c1 - 1)
    rest  = substr($0, c1 + 1)
    c2 = index(rest, ":")
    if (c2 == 0) next
    lineno = substr(rest, 1, c2 - 1) + 0
    text   = substr(rest, c2 + 1)
    gsub(/^[ \t]+|[ \t]+$/, "", count)
    if (count == "-" || lineno == 0) next

    # A header included more than once per object appears repeatedly; a line
    # counts as executed if any instantiation reached it.
    key = src ":" lineno
    seen[key] = 1
    if (count != "#####" && count != "$$$$$") {
      hit[key] = 1
    } else if (!(key in hit)) {
      body[key] = text
    }
    file_of[key] = src
  }

  END {
    for (key in seen) {
      f = file_of[key]
      total[f]++
      grand_total++
      if (key in hit) {
        covered[f]++
        grand_covered++
      } else {
        t = body[key]
        if (t ~ /realloc|capacity|[Gg]row|GROW|rehash|resize|reserve/) {
          split(key, parts, ":")
          gaps[f] = gaps[f] " " parts[2]
          gap_count++
        }
      }
    }

    printf "\n%-52s %8s %s\n", "FILE", "LINES", "COVERED"
    n = 0
    for (f in total) { files[++n] = f }
    # Simple insertion sort: least covered first, so the gaps lead.
    for (i = 2; i <= n; i++) {
      v = files[i]; rv = covered[v] / total[v]
      j = i - 1
      while (j >= 1 && (covered[files[j]] / total[files[j]]) > rv) {
        files[j + 1] = files[j]; j--
      }
      files[j + 1] = v
    }
    for (i = 1; i <= n; i++) {
      f = files[i]
      printf "%-52s %8d %6.1f%%\n", f, total[f], covered[f] * 100 / total[f]
    }
    printf "%-52s %8d %6.1f%%\n", "TOTAL", grand_total,
      (grand_total ? grand_covered * 100 / grand_total : 0)

    if (gap_count > 0) {
      printf "\n%d growth/capacity lines never executed:\n", gap_count
      for (i = 1; i <= n; i++) {
        f = files[i]
        if (f in gaps) printf "  %s:%s\n", f, gaps[f]
      }
      printf "\nA reallocation path no test reaches is untested, not working.\n"
    } else {
      printf "\nEvery growth/capacity line was executed.\n"
    }
  }
' ./*.gcov

rm -f ./*.gcov
