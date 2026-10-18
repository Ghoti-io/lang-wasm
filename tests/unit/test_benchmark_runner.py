#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Exercise runner-level failed-sample CSV emission after sample setup."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile


runner = Path(sys.argv[1]).resolve()
repo = Path(__file__).resolve().parents[2]


def runner_path(path: Path) -> str:
    # This Python process runs on the Linux build host even for a Win64 runner.
    # Wine maps the host root to Z:, so pass paths the runner itself can open.
    if runner.suffix.lower() == ".exe":
        return "Z:" + str(path).replace("/", "\\")
    return str(path)


with tempfile.TemporaryDirectory(prefix="glwasm-benchmark-runner-") as temp:
    root = Path(temp)
    output = root / "failed.csv"
    environment_manifest = root / "environment.json"
    environment_manifest.write_text('{"test":"runner-failure"}\n', encoding="utf-8")
    env = os.environ.copy()
    env.update({
        "GLWASM_BENCH_RUN_ID": "runner-failure-regression",
        "GLWASM_BENCH_SOURCE_COMMIT": "a" * 40,
        "GLWASM_BENCH_SOURCE_MANIFEST_SHA256": "b" * 64,
        "GLWASM_BENCH_IMAGE_ID": "sha256:" + "c" * 64,
        "GLWASM_BENCH_COMPILER": "test-compiler",
        "GLWASM_BENCH_COMPILER_VERSION": "test-version",
        "GLWASM_BENCH_FLAGS": "test-flags",
        "GLWASM_BENCH_MACHINE": "test-machine",
        "GLWASM_BENCH_ARCHITECTURE": "test-architecture",
        "GLWASM_BENCH_COMMAND": "make benchmark BENCH_ITERATIONS=1 BENCH_SAMPLES=30",
        "GLWASM_BENCH_ENV_MANIFEST": runner_path(environment_manifest),
        "GLWASM_BENCH_POLICY": "untrusted",
        "GLWASM_BENCH_FUEL": "10000000",
        "GLWASM_BENCH_OUTPUT": runner_path(output),
        "GLWASM_BENCH_TEST_FAIL_AFTER_SETUP": "1",
    })
    result = subprocess.run([str(runner), "1", "30"], cwd=repo, env=env,
                            capture_output=True, text=True, check=False)
    assert result.returncode != 0, result
    assert output.exists() and output.stat().st_size > 0, result
    with output.open(newline="", encoding="utf-8") as stream:
        rows = list(csv.DictReader(stream))
    assert len(rows) == 1, rows
    row = rows[0]
    assert row["run_id"] == "runner-failure-regression", row
    assert row["workload"] == "loop-numeric", row
    assert len(row["workload_sha256"]) == 64, row
    assert row["input_manifest"] and len(row["input_sha256"]) == 64, row
    assert row["phase"] == "steady-state" and row["tier"] == "interpreter", row
    assert row["source_commit"] == "a" * 40, row
    assert row["source_manifest_sha256"] == "b" * 64, row
    assert row["image_id"] == "sha256:" + "c" * 64, row
    # The runner opens the absolute host/Wine path but emits a portable sibling
    # filename so summarize.py can verify the fetched result bundle.
    assert row["environment_manifest_path"] == environment_manifest.name, row
    assert row["compiler"] == "test-compiler" and row["architecture"] == "test-architecture", row
    assert row["exit_code"] != "0" and row["error_message"], row
    assert "injected failure" in row["error_message"], row
