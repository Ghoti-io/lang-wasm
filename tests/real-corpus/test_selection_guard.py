#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Prove that an inherited Google Test filter cannot make the corpus gate pass."""

import os
import subprocess
import sys


def main() -> int:
    if len(sys.argv) != 3:
        raise SystemExit("usage: test_selection_guard.py BINARY EXPECTED_TESTS")
    binary, expected = sys.argv[1:]
    expected_count = int(expected)
    env = os.environ.copy()
    env["GTEST_FILTER"] = "-*"
    result = subprocess.run(
        [binary, "--gtest_brief=1"],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    diagnostic = (
        f"real-corpus gate requires all {expected_count} cases: "
        f"registered={expected_count} selected=0 passed=0 skipped=0"
    )
    if result.returncode != 2 or diagnostic not in result.stderr:
        sys.stderr.write(
            "real-corpus zero-selection control failed: "
            f"exit={result.returncode}, expected=2\n"
        )
        sys.stderr.write(result.stdout)
        sys.stderr.write(result.stderr)
        return 1
    print(f"real-corpus zero-selection control passed ({expected_count} cases)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
