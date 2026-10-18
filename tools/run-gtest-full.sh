#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-3.0-only
#
# Run one complete Google Test binary for a release gate. Caller-supplied
# selection/sharding variables must not silently reduce the suite.

set -u

# Sanitizer libraries belong on the test process, not this Bash wrapper.
# Preloading libasan into Bash makes LeakSanitizer report Bash allocations.
if [[ ${GTEST_FULL_TEST_LD_PRELOAD+x} && -n ${LD_PRELOAD:-} ]]; then
    printf 'run-gtest-full: wrapper must start without LD_PRELOAD\n' >&2
    exit 2
fi

if [[ ${1:-} == --selftest ]]; then
    positive=$(GTEST_FILTER=-* GTEST_TOTAL_SHARDS=2 GTEST_SHARD_INDEX=1 \
        GTEST_SHARD_STATUS_FILE=/nonexistent/shard-status \
        GTEST_REPEAT=7 GTEST_ALSO_RUN_DISABLED_TESTS=1 "$0" bash -c '
        [[ -z ${GTEST_FILTER+x} && -z ${GTEST_TOTAL_SHARDS+x} &&
           -z ${GTEST_SHARD_INDEX+x} && -z ${GTEST_SHARD_STATUS_FILE+x} &&
           -z ${GTEST_REPEAT+x} &&
           -z ${GTEST_ALSO_RUN_DISABLED_TESTS+x} &&
           "$2" == "--gtest_filter=*" ]] || exit 1
        printf "[==========] Running 1 test from 1 test suite.\n[ RUN      ] Probe.Passes\n[       OK ] Probe.Passes (0 ms)\n[==========] 1 test from 1 test suite ran. (0 ms total)\n[  PASSED  ] 1 test.\n"
    ' 2>&1)
    if [[ $? -ne 0 || "$positive" != *"1 test from 1 test suite ran."* ]]; then
        printf 'run-gtest-full: inherited selection was not cleared\n' >&2
        exit 1
    fi
    if [[ $(uname -s) == Linux ]]; then
        preload=$(LD_PRELOAD= GTEST_FULL_TEST_LD_PRELOAD=libc.so.6 "$0" bash -c '
            [[ -z ${GTEST_FULL_TEST_LD_PRELOAD+x} &&
               ${LD_PRELOAD:-} == libc.so.6 ]] || exit 1
            printf "[==========] Running 1 test from 1 test suite.\n[ RUN      ] Probe.Preload\n[       OK ] Probe.Preload (0 ms)\n[==========] 1 test from 1 test suite ran. (0 ms total)\n[  PASSED  ] 1 test.\n"
        ' 2>&1)
        if [[ $? -ne 0 || "$preload" != *"completed 1, passed 1, skipped 0"* ]]; then
            printf 'run-gtest-full: child-only preload was not preserved\n' >&2
            exit 1
        fi
        printf 'run-gtest-full: child-only preload control passed\n'
    else
        printf 'run-gtest-full: preload DSO control applies on Linux only\n'
    fi
    zero=$("$0" bash -c '
        printf "[==========] 0 tests from 0 test suites ran. (0 ms total)\\n"
    ' 2>&1)
    if [[ $? -ne 2 || "$zero" != *"incomplete or zero-passing Google Test run"* ]]; then
        printf 'run-gtest-full: planted zero-selection was not rejected\n' >&2
        exit 1
    fi
    skipped=$("$0" bash -c '
        printf "[==========] Running 1 test from 1 test suite.\n[ RUN      ] Probe.Skips\n[  SKIPPED ] Probe.Skips (0 ms)\n[==========] 1 test from 1 test suite ran. (0 ms total)\n[  PASSED  ] 0 tests.\n[  SKIPPED ] 1 test, listed below:\n[  SKIPPED ] Probe.Skips\n"
    ' 2>&1)
    if [[ $? -ne 2 || "$skipped" != *"incomplete or zero-passing Google Test run"* ]]; then
        printf 'run-gtest-full: planted all-skipped suite was not rejected\n' >&2
        exit 1
    fi
    named=$("$0" bash -c '
        printf "[==========] Running 2 tests from 1 test suite.\n[ RUN      ] Probe.Passes\n[       OK ] Probe.Passes (0 ms)\n[ RUN      ] Probe.Skips\n[  SKIPPED ] Probe.Skips (0 ms)\n[==========] 2 tests from 1 test suite ran. (0 ms total)\n[  PASSED  ] 1 test.\n[  SKIPPED ] 1 test, listed below:\n[  SKIPPED ] Probe.Skips\n"
    ' 2>&1)
    if [[ $? -ne 0 || "$named" != *"completed 2, passed 1, skipped 1"* ]]; then
        printf 'run-gtest-full: named-skip inventory was not counted\n' >&2
        exit 1
    fi
    printf 'run-gtest-full: selection, zero-case, all-skipped, and named-skip controls passed\n'
    exit 0
fi

if [[ $# -eq 0 ]]; then
    printf 'usage: run-gtest-full.sh COMMAND [ARG ...]\n' >&2
    exit 2
fi

output=$(mktemp "${TMPDIR:-/tmp}/gtest-full.XXXXXXXX") || exit 2
trap 'rm -f "$output"' EXIT

child_preload=()
if [[ ${GTEST_FULL_TEST_LD_PRELOAD+x} ]]; then
    child_preload=(LD_PRELOAD="$GTEST_FULL_TEST_LD_PRELOAD")
fi

env -u GTEST_FULL_TEST_LD_PRELOAD \
    -u GTEST_FILTER -u GTEST_TOTAL_SHARDS -u GTEST_SHARD_INDEX \
    -u GTEST_SHARD_STATUS_FILE \
    -u GTEST_REPEAT -u GTEST_ALSO_RUN_DISABLED_TESTS \
    "${child_preload[@]}" \
    "$@" --gtest_brief=0 --gtest_color=no --gtest_filter='*' 2>&1 | tee "$output"
statuses=("${PIPESTATUS[@]}")
if (( statuses[0] != 0 )); then
    exit "${statuses[0]}"
fi
if (( statuses[1] != 0 )); then
    printf 'run-gtest-full: output capture failed\n' >&2
    exit 2
fi
if ! awk '
    /^\[=+\] Running [0-9]+ tests? from/ {
        started = $3
        saw_start = 1
    }
    /^\[ *RUN *\] / {
        line = $0
        sub(/^.*\] +/, "", line)
        if (seen_run[line]++) duplicate_run = 1
        runs++
    }
    /^\[=+\] [0-9]+ tests? from [0-9]+ test suites? ran\./ {
        total = $2
        complete = 1
    }
    /^\[ *PASSED *\] [0-9]+ tests?\./ {
        line = $0
        sub(/^.*\] /, "", line)
        split(line, words, " ")
        passed = words[1]
    }
    /^\[ *SKIPPED *\] [0-9]+ tests?([.,]|$)/ {
        line = $0
        sub(/^.*\] /, "", line)
        split(line, words, " ")
        skipped = words[1]
        skip_list_started = 1
    }
    skip_list_started && /^\[ *SKIPPED *\] [^0-9 ]/ {
        line = $0
        sub(/^.*\] +/, "", line)
        if (!(line in seen_run)) unknown_skip = 1
        if (seen_skip[line]++) duplicate_skip = 1
        named_skips++
    }
    END {
        if (!saw_start || !complete || total <= 0 || passed <= 0 ||
            started != total || runs != total || duplicate_run ||
            skipped != named_skips || unknown_skip || duplicate_skip ||
            total != passed + skipped)
            exit 1
        printf "run-gtest-full: completed %d, passed %d, skipped %d\n", total, passed, skipped
    }
' "$output"; then
    printf 'run-gtest-full: incomplete or zero-passing Google Test run\n' >&2
    exit 2
fi
