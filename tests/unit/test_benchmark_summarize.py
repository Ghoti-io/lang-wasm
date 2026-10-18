#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Focused raw-sample contract tests; no runtime or benchmark build required."""

import contextlib
import csv
import hashlib
import importlib.util
import io
import json
import pathlib
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location(
    "wasm_bench_summarize", ROOT / "benchmarks" / "summarize.py")
summarize = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(summarize)
RECORD_SPEC = importlib.util.spec_from_file_location(
    "wasm_bench_record_build", ROOT / "benchmarks" / "record_build.py")
record_build = importlib.util.module_from_spec(RECORD_SPEC)
RECORD_SPEC.loader.exec_module(record_build)


def digest(text):
    return hashlib.sha256(text.encode()).hexdigest()


def source_manifest():
    return (json.dumps({"schema": 1, "source_commit": "a" * 40},
                       sort_keys=True, separators=(",", ":")) + "\n")


def fixture_rows(workload="loop-numeric"):
    rows = []
    build_log = "cc -O2 -c source.c -o source.o\ng++ -O2 -c source.cpp -o source.o\ng++ -o benchmark objects.o -lruntime\n"
    env_manifest = json.dumps({
        "compiler": "gcc", "compiler_version": "16.2.0",
        "compiler_sha256": digest("gcc"), "cxx": "g++",
        "cxx_version": "16.2.0", "cxx_sha256": digest("g++"),
        "flags": "-O2", "build_commands": {
            "c_compile": "cc -O2 -c source.c -o source.o", "cxx_compile": "g++ -O2 -c source.cpp -o source.o",
            "link": "g++ -o benchmark objects.o -lruntime",
        }, "image_id": "sha256:" + "b" * 64,
        "source_commit": "a" * 40,
        "source_manifest_sha256": digest(source_manifest()),
        "source_manifest_path": "source-manifest.json",
        "machine": "test-host", "architecture": "x86_64",
        "fixture_converter": "lang-wasm-wat-lower",
        "fixture_converter_source_sha256": digest("lowerer source"),
        "build_log_path": "build.log", "build_log_sha256": digest(build_log),
    }, sort_keys=True)
    input_manifest = "iterations=10;warmup=2;samples=30;policy=untrusted;fuel=10000000"
    base = {
        "run_id": "review-run", "source_commit": "a" * 40,
        "source_manifest_sha256": digest(source_manifest()),
        "environment_manifest_path": "environment.json",
        "environment_manifest": env_manifest,
        "environment_manifest_sha256": digest(env_manifest),
        "image_id": "sha256:" + "b" * 64, "compiler": "gcc",
        "compiler_version": "16.2.0", "flags": "-O2",
        "machine": "test-host", "architecture": "x86_64",
        "fixture_converter": "lang-wasm-wat-lower",
        "fixture_converter_source_sha256": digest("lowerer source"),
        "workload": workload, "workload_sha256": digest("wasm bytes"),
        "input_manifest": input_manifest, "input_sha256": digest(input_manifest),
        "policy": "untrusted", "fuel": "10000000",
        "work_unit": "guest-iteration", "tier": "interpreter",
        "debugger": "none", "phase": "steady-state", "segment": "whole",
        "warmup_work_units": "20", "sample_index": "0", "work_units": "10",
        "compiled_entries": "0", "compiled_returns": "0", "jit_side_exits": "0",
        "elapsed_ns": "100", "result_sha256": digest("45"), "exit_code": "0",
        "error_message": "", "command": "make benchmark BENCH_ITERATIONS=10 BENCH_SAMPLES=30",
    }
    groups = [
        ("interpreter", "none", "steady-state", "whole"),
        ("jit", "none", "steady-state", "whole"),
        ("interpreter", "attached-idle", "steady-state", "whole"),
        ("jit", "none", "first-call", "first-call"),
        ("interpreter", "step-diagnostic", "poll-event", "stop"),
        ("interpreter", "step-diagnostic", "poll-event", "resume"),
    ]
    if workload == "mixed-boundary":
        groups = [("hybrid" if tier == "jit" else tier, debugger, phase, segment)
                  for tier, debugger, phase, segment in groups]
    if workload != "loop-numeric":
        groups = [group for group in groups if group[1] != "step-diagnostic"]
    for tier, debugger, phase, segment in groups:
        for index in range(30):
            row = dict(base, tier=tier, debugger=debugger, phase=phase,
                       segment=segment, sample_index=str(index))
            if phase == "first-call":
                row.update(work_unit="first-call", warmup_work_units="0", work_units="1",
                           compiled_entries="1", compiled_returns="1")
            elif phase == "poll-event":
                row.update(work_unit="event", warmup_work_units="0", work_units="1")
            elif tier == "jit":
                row.update(compiled_entries="3", compiled_returns="3")
            if workload == "mixed-boundary" and phase == "steady-state":
                row.update(work_unit="boundary-call", work_units="10",
                           warmup_work_units="20")
                if tier == "hybrid":
                    row.update(compiled_entries="3", compiled_returns="3")
            if workload == "mixed-boundary" and phase == "first-call":
                row.update(tier="hybrid", work_unit="first-call", work_units="1",
                           warmup_work_units="0", compiled_entries="1",
                           compiled_returns="1")
            if workload == "table-bulk" and phase == "steady-state":
                row.update(work_unit="operation", work_units="40",
                           warmup_work_units="80")
            if phase in {"first-call", "poll-event"}:
                phase_manifest = input_manifest.replace("warmup=2", "warmup=0")
                row.update(input_manifest=phase_manifest,
                           input_sha256=digest(phase_manifest))
            rows.append(row)
    return rows


class SummarizerContractTests(unittest.TestCase):
    def load_rows(self, rows, manifest_text=None, environment_file_text=None,
                  build_log_text=None, source_reads=None,
                  environment_files=None, mutate_source_after_first_read=False):
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "samples.csv"
            (path.parent / "source-manifest.json").write_text(
                source_manifest() if manifest_text is None else manifest_text,
                encoding="utf-8")
            environment_path = pathlib.Path(rows[0]["environment_manifest_path"])
            (path.parent / environment_path).write_text(
                rows[0]["environment_manifest"] if environment_file_text is None
                else environment_file_text, encoding="utf-8")
            for name, text in (environment_files or {}).items():
                (path.parent / name).write_text(text, encoding="utf-8")
            (path.parent / "build.log").write_text(
                "cc -O2 -c source.c -o source.o\ng++ -O2 -c source.cpp -o source.o\ng++ -o benchmark objects.o -lruntime\n"
                if build_log_text is None else build_log_text,
                encoding="utf-8")
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=summarize.FIELDS)
                writer.writeheader()
                writer.writerows(rows)
            original_read_bytes = pathlib.Path.read_bytes

            def counted_read_bytes(path):
                data = original_read_bytes(path)
                if source_reads is not None and path.name == "source-manifest.json":
                    source_reads.append(path)
                    if mutate_source_after_first_read and len(source_reads) == 1:
                        path.write_text("changed during summarization", encoding="utf-8")
                return data

            with mock.patch.object(pathlib.Path, "read_bytes", counted_read_bytes), \
                 mock.patch.object(summarize, "load_pinned_module_hashes",
                                   return_value={name: digest("wasm bytes")
                                                 for name in summarize.KNOWN_WORKLOADS}):
                return summarize.load(path)

    def test_complete_paired_matrix_is_accepted(self):
        groups = self.load_rows(fixture_rows())
        summarize.validate(groups, ("loop-numeric",))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            summarize.summarize(groups, ("loop-numeric",))
        self.assertIn("nearest-rank", output.getvalue())
        self.assertIn("ns/guest-iteration", output.getvalue())

    def test_one_run_cannot_mix_policy_or_fuel_across_workloads(self):
        for policy, fuel in (("trusted", "10000000"),
                             ("untrusted", "5000000")):
            with self.subTest(policy=policy, fuel=fuel):
                calls = fixture_rows("calls")
                for row in calls:
                    if row["phase"] == "steady-state":
                        row.update(work_unit="call", work_units="20",
                                   warmup_work_units="40")
                    manifest = row["input_manifest"].replace(
                        "policy=untrusted", f"policy={policy}").replace(
                        "fuel=10000000", f"fuel={fuel}")
                    row.update(policy=policy, fuel=fuel,
                               input_manifest=manifest,
                               input_sha256=digest(manifest))
                groups = self.load_rows(fixture_rows() + calls)
                with self.assertRaisesRegex(ValueError, "policy/fuel metadata differs"):
                    summarize.validate(groups, ("loop-numeric", "calls"))

    def test_retained_environment_manifest_must_match_raw_rows(self):
        with self.assertRaisesRegex(ValueError, "retained environment manifest differs"):
            self.load_rows(fixture_rows(), environment_file_text="{}")

    def test_unstable_group_digest_is_rejected(self):
        rows = fixture_rows()
        rows[1]["result_sha256"] = digest("different")
        groups = self.load_rows(rows)
        with self.assertRaisesRegex(ValueError, "not stable"):
            summarize.validate(groups, ("loop-numeric",))

    def test_interpreter_jit_digest_mismatch_is_rejected(self):
        rows = fixture_rows()
        for row in rows:
            if row["tier"] == "jit" and row["phase"] == "steady-state":
                row["result_sha256"] = digest("different")
        groups = self.load_rows(rows)
        with self.assertRaisesRegex(ValueError,
                                    "interpreter/JIT validated output digests differ"):
            summarize.validate(groups, ("loop-numeric",))

    def test_work_unit_count_and_input_hash_are_checked(self):
        rows = fixture_rows()
        rows[0]["work_units"] = "11"
        with self.assertRaisesRegex(ValueError, "work unit/count"):
            self.load_rows(rows)
        rows = fixture_rows()
        rows[0]["input_manifest"] = "iterations=10;warmup=2;samples=30;policy=untrusted;fuel=1"
        with self.assertRaisesRegex(ValueError, "does not hash"):
            self.load_rows(rows)

    def test_missing_required_matrix_group_is_rejected(self):
        rows = [row for row in fixture_rows() if not (
            row["tier"] == "interpreter" and row["debugger"] == "attached-idle")]
        groups = self.load_rows(rows)
        with self.assertRaisesRegex(ValueError, "instruction-poll comparison requires"):
            summarize.validate(groups, ("loop-numeric",))

    def test_command_must_match_recorded_effective_arguments(self):
        rows = fixture_rows()
        rows[0]["command"] = "make benchmark BENCH_ITERATIONS=11 BENCH_SAMPLES=30"
        with self.assertRaisesRegex(ValueError, "command must exactly reproduce"):
            self.load_rows(rows)

    def test_sample_count_must_match_input_manifest(self):
        rows = fixture_rows()
        rows[0]["input_manifest"] = rows[0]["input_manifest"].replace(
            "samples=30", "samples=60")
        rows[0]["input_sha256"] = digest(rows[0]["input_manifest"])
        rows[0]["command"] = "make benchmark BENCH_ITERATIONS=10 BENCH_SAMPLES=60"
        groups = self.load_rows(rows)
        with self.assertRaisesRegex(ValueError, "row count differs from input_manifest"):
            summarize.validate(groups, ("loop-numeric",))

    def test_sample_count_must_preserve_balanced_workload_and_tier_orders(self):
        with self.assertRaisesRegex(ValueError, "outside the benchmark contract"):
            summarize.parse_input_manifest(
                "iterations=10;warmup=2;samples=31;policy=untrusted;fuel=10000000",
                "test")

    def test_failure_row_is_never_summarized_as_success(self):
        rows = fixture_rows()
        rows[0].update(exit_code="1", error_message="planted failure", elapsed_ns="0")
        with self.assertRaisesRegex(ValueError, "failed sample"):
            self.load_rows(rows)

    def test_summary_identifies_partial_workload_selection(self):
        groups = self.load_rows(fixture_rows())
        summarize.validate(groups, ("loop-numeric",))
        output = io.StringIO()
        with contextlib.redirect_stdout(output):
            summarize.summarize(groups, ("loop-numeric",))
        self.assertIn("configured_workloads=loop-numeric; completeness=partial",
                      output.getvalue())

    def test_mixed_boundary_uses_hybrid_tier(self):
        groups = self.load_rows(fixture_rows("mixed-boundary"))
        summarize.validate(groups, ("mixed-boundary",))

    def test_paired_samples_require_identical_sample_indexes(self):
        rows = fixture_rows()
        row = next(row for row in rows if (
            row["tier"] == "jit" and row["debugger"] == "none" and
            row["phase"] == "steady-state" and row["segment"] == "whole" and
            row["sample_index"] == "29"))
        row["sample_index"] = "30"
        groups = self.load_rows(rows)
        with self.assertRaisesRegex(ValueError, "sample_index must be contiguous"):
            summarize.validate(groups, ("loop-numeric",))

    def test_table_bulk_counts_four_observed_operations(self):
        groups = self.load_rows(fixture_rows("table-bulk"))
        summarize.validate(groups, ("table-bulk",))
        rows = fixture_rows("table-bulk")
        rows[0]["work_units"] = "10"
        with self.assertRaisesRegex(ValueError, "work unit/count"):
            self.load_rows(rows)

    def test_nonsteady_manifest_must_not_claim_warmup(self):
        rows = fixture_rows()
        row = next(row for row in rows if row["phase"] == "first-call")
        row["input_manifest"] = row["input_manifest"].replace(
            "warmup=0", "warmup=2")
        row["input_sha256"] = digest(row["input_manifest"])
        with self.assertRaisesRegex(ValueError, "warmup does not match measured phase"):
            groups = self.load_rows(rows)
            summarize.validate(groups, ("loop-numeric",))

    def test_modified_retained_source_manifest_is_rejected(self):
        changed = json.dumps({"schema": 1, "source_commit": "b" * 40},
                             sort_keys=True, separators=(",", ":")) + "\n"
        with self.assertRaisesRegex(ValueError, "source manifest file does not match"):
            self.load_rows(fixture_rows(), manifest_text=changed)

    def test_repeated_rows_bound_source_manifest_reads(self):
        source_reads = []
        self.load_rows(fixture_rows(), source_reads=source_reads)
        self.assertEqual(len(source_reads), 2)  # initial validation and final recheck

    def test_later_row_with_changed_source_hash_is_rejected(self):
        rows = fixture_rows()
        later = rows[1]
        later["source_manifest_sha256"] = "b" * 64
        later["environment_manifest_path"] = "environment-alt.json"
        environment = json.loads(later["environment_manifest"])
        environment["source_manifest_sha256"] = later["source_manifest_sha256"]
        later["environment_manifest"] = json.dumps(environment, sort_keys=True)
        later["environment_manifest_sha256"] = digest(later["environment_manifest"])
        with self.assertRaisesRegex(ValueError, "source manifest file does not match"):
            self.load_rows(rows, environment_files={
                later["environment_manifest_path"]: later["environment_manifest"]})

    def test_source_manifest_change_during_rows_is_rejected(self):
        with self.assertRaisesRegex(ValueError,
                                    "retained source manifest changed during summarization"):
            self.load_rows(fixture_rows(), source_reads=[],
                           mutate_source_after_first_read=True)

    def test_source_manifest_commit_must_match_environment(self):
        rows = fixture_rows()
        changed = json.dumps({"schema": 1, "source_commit": "b" * 40},
                             sort_keys=True, separators=(",", ":")) + "\n"
        for row in rows:
            environment = json.loads(row["environment_manifest"])
            environment["source_manifest_sha256"] = digest(changed)
            row["source_manifest_sha256"] = digest(changed)
            row["environment_manifest"] = json.dumps(environment, sort_keys=True)
            row["environment_manifest_sha256"] = digest(row["environment_manifest"])
        with self.assertRaisesRegex(ValueError, "source manifest commit differs"):
            self.load_rows(rows, manifest_text=changed)

    def test_modified_build_transcript_is_rejected(self):
        rows = fixture_rows()
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "samples.csv"
            (path.parent / "source-manifest.json").write_text(source_manifest(), encoding="utf-8")
            (path.parent / "environment.json").write_text(
                rows[0]["environment_manifest"], encoding="utf-8")
            (path.parent / "build.log").write_text("different command\n", encoding="utf-8")
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=summarize.FIELDS)
                writer.writeheader()
                writer.writerows(rows)
            with mock.patch.object(summarize, "load_pinned_module_hashes",
                                   return_value={name: digest("wasm bytes")
                                                 for name in summarize.KNOWN_WORKLOADS}):
                with self.assertRaisesRegex(ValueError, "retained build log does not match"):
                    summarize.load(path)

    def test_manifest_command_must_appear_in_build_transcript(self):
        rows = fixture_rows()
        for row in rows:
            environment = json.loads(row["environment_manifest"])
            environment["build_commands"]["c_compile"] = "cc -O2 -c invented.c -o invented.o"
            row["environment_manifest"] = json.dumps(environment, sort_keys=True)
            row["environment_manifest_sha256"] = digest(row["environment_manifest"])
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "samples.csv"
            (path.parent / "source-manifest.json").write_text(source_manifest(), encoding="utf-8")
            (path.parent / "environment.json").write_text(
                rows[0]["environment_manifest"], encoding="utf-8")
            log = "cc -O2 -c source.c -o source.o\ng++ -O2 -c source.cpp -o source.o\ng++ -o benchmark objects.o -lruntime\n"
            (path.parent / "build.log").write_text(log, encoding="utf-8")
            with path.open("w", newline="", encoding="utf-8") as stream:
                writer = csv.DictWriter(stream, fieldnames=summarize.FIELDS)
                writer.writeheader()
                writer.writerows(rows)
            with mock.patch.object(summarize, "load_pinned_module_hashes",
                                   return_value={name: digest("wasm bytes")
                                                 for name in summarize.KNOWN_WORKLOADS}):
                with self.assertRaisesRegex(ValueError, "command absent from the retained build log"):
                    summarize.load(path)

    def test_manifest_must_record_every_compiler_command_in_build_transcript(self):
        rows = fixture_rows()
        log = ("cc -O2 -c source.c -o source.o\n"
               "cc -O3 -c second.c -o second.o\n"
               "g++ -O2 -c source.cpp -o source.o\n"
               "g++ -o benchmark objects.o -lruntime\n")
        for row in rows:
            environment = json.loads(row["environment_manifest"])
            environment["build_log_sha256"] = digest(log)
            row["environment_manifest"] = json.dumps(environment, sort_keys=True)
            row["environment_manifest_sha256"] = digest(row["environment_manifest"])
        with self.assertRaisesRegex(ValueError, "omits or changes compiler commands"):
            self.load_rows(rows, build_log_text=log)

    def test_manifest_command_must_match_a_complete_build_log_line(self):
        rows = fixture_rows()
        for row in rows:
            environment = json.loads(row["environment_manifest"])
            environment["build_commands"]["c_compile"] = "cc -O2 -c source.c"
            row["environment_manifest"] = json.dumps(environment, sort_keys=True)
            row["environment_manifest_sha256"] = digest(row["environment_manifest"])
        with self.assertRaisesRegex(ValueError, "command absent from the retained build log"):
            self.load_rows(rows)

    def test_build_recorder_captures_c_cpp_and_link_commands(self):
        transcript = (
            "cc -O2 -c src/a.c -o build/a.o\n"
            "g++ -O1 -std=c++20 -c tests/runner.cpp -o build/runner.o\n"
            "g++ -shared -o build/lib.a build/a.o\n"
            "g++ -o build/runner build/runner.o build/lib.a\n")
        commands = record_build.compiler_commands(transcript)
        self.assertIn("cc -O2 -c src/a.c", commands["c_compile"])
        self.assertIn("g++ -O1 -std=c++20 -c tests/runner.cpp", commands["cxx_compile"])
        self.assertIn("g++ -o build/runner", commands["link"])

    def test_build_recorder_captures_versioned_cross_and_wrapped_compilers(self):
        transcript = (
            "clang-19 -O2 -c src/a.c -o build/a.o\n"
            "CCACHE_DISABLE=1 clang-19 -O2 -c src/b.c -o build/b.o\n"
            "ccache aarch64-linux-gnu-g++-16 -O1 -c tests/runner.cpp -o build/runner.o\n"
            "env -S 'CCACHE_DISABLE=1 clang++-19 -O1 -c tests/other.cpp -o build/other.o'\n"
            "clang-19 -o build/runner build/runner.o build/a.o\n")
        commands = record_build.compiler_commands(transcript)
        self.assertIn("clang-19 -O2", commands["c_compile"])
        self.assertIn("CCACHE_DISABLE=1 clang-19 -O2", commands["c_compile"])
        self.assertIn("ccache aarch64-linux-gnu-g++-16", commands["cxx_compile"])
        self.assertIn("env -S 'CCACHE_DISABLE=1 clang++-19", commands["cxx_compile"])
        self.assertIn("clang-19 -o build/runner", commands["link"])
        self.assertEqual(record_build.compiler_word_index(
            ["ccache", "/usr/bin/clang-19", "-O2", "-c", "src/a.c"]), 1)
        self.assertEqual(record_build.compiler_word_index(
            ["env", "CCACHE_DISABLE=1", "aarch64-linux-gnu-g++-16", "-c", "a.cpp"]), 2)
        self.assertEqual(record_build.compiler_word_index(
            ["env", "-u", "CCACHE_DISABLE", "clang-19", "-c", "src/a.c"]), 3)
        self.assertEqual(record_build.compiler_word_index(
            ["env", "--unset=CCACHE_DISABLE", "clang-19", "-c", "src/a.c"]), 2)
        self.assertEqual(record_build.compiler_word_index(
            ["env", "-S", "CCACHE_DISABLE=1 clang-19 -c src/a.c"]), 2)

    def test_build_recorder_rejects_later_unrecorded_compiler_binary(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            for name in ("gcc-16", "gcc-17", "g++-16", "g++-17"):
                driver = root / name
                driver.write_bytes(name.encode("ascii"))
                driver.chmod(0o755)
            (root / "cc").symlink_to(root / "gcc-16")
            commands = {
                "c_compile": f"{root / 'gcc-16'} -c a.c -o a.o",
                "cxx_compile": f"{root / 'g++-16'} -c a.cpp -o a-cxx.o",
                "link": f"{root / 'g++-16'} -o app a.o a-cxx.o",
            }
            with mock.patch.object(record_build.subprocess, "run",
                                   return_value=mock.Mock(stdout="compiler version 16\n")):
                c_info, cxx_info = record_build.compiler_identities(commands)
                self.assertEqual(c_info[2], digest("gcc-16"))
                self.assertEqual(cxx_info[2], digest("g++-16"))

                alias = dict(commands)
                alias["c_compile"] += f"\n{root / 'cc'} -c b.c -o b.o"
                self.assertEqual(record_build.compiler_identities(alias)[0][2],
                                 digest("gcc-16"))

                for role, command in (
                    ("c_compile", f"{root / 'gcc-17'} -c b.c -o b.o"),
                    ("cxx_compile", f"{root / 'g++-17'} -c b.cpp -o b.o"),
                    ("link", f"{root / 'g++-17'} -o other a.o a-cxx.o"),
                ):
                    changed = dict(commands)
                    changed[role] += "\n" + command
                    with self.subTest(role=role):
                        with self.assertRaisesRegex(ValueError, role):
                            record_build.compiler_identities(changed)

    def test_build_recorder_rejects_unknown_wrapper_around_compiler(self):
        baseline = (
            "gcc -c src/a.c -o build/a.o\n"
            "g++ -c tests/runner.cpp -o build/runner.o\n"
            "g++ -o build/runner build/runner.o build/a.o\n"
        )
        harmless = record_build.compiler_commands(
            baseline + "python3 tools/generate.py --output generated.h\n")
        self.assertEqual(harmless["c_compile"].count("\n"), 0)
        self.assertEqual(harmless["link"].count("\n"), 0)
        for hidden in (
            "custom-wrapper clang-19 -c src/b.c -o build/b.o\n",
            "custom-wrapper clang++-19 -c tests/other.cpp -o build/other.o\n",
            "custom-wrapper clang++-19 -o build/other build/runner.o\n",
        ):
            with self.subTest(hidden=hidden):
                with self.assertRaisesRegex(ValueError, "unrecognized driver or wrapper"):
                    record_build.compiler_commands(baseline + hidden)

    def test_build_recorder_hashes_the_driver_selected_by_env_split_string(self):
        with tempfile.TemporaryDirectory() as directory:
            driver = pathlib.Path(directory) / "clang++-19"
            driver.write_bytes(b"pinned compiler executable")
            with mock.patch.object(record_build.shutil, "which",
                                   return_value=str(driver)), mock.patch.object(
                                       record_build.subprocess, "run",
                                       return_value=mock.Mock(stdout="clang version 19\n")):
                label, version, binary_hash = record_build.compiler_info(
                    "env -S 'CCACHE_DISABLE=1 clang++-19 -O1 -c tests/other.cpp'")
            self.assertEqual(label, "clang++-19")
            self.assertEqual(version, "clang version 19")
            self.assertEqual(binary_hash, digest("pinned compiler executable"))

    def test_build_recorder_uses_command_path_instead_of_ambient_path(self):
        with tempfile.TemporaryDirectory() as directory:
            root = pathlib.Path(directory)
            ambient = root / "ambient"
            selected = root / "selected"
            ambient.mkdir()
            selected.mkdir()
            for path, contents in ((ambient / "clang-19", b"wrong compiler"),
                                   (selected / "clang-19", b"selected compiler")):
                path.write_bytes(contents)
                path.chmod(0o755)
            with mock.patch.dict(record_build.os.environ, {"PATH": str(ambient)}):
                with mock.patch.object(record_build.subprocess, "run",
                                       return_value=mock.Mock(stdout="clang version 19\n")) as run:
                    for command in (
                        f"env PATH={selected} clang-19 -c source.c",
                        f"env -S 'PATH={selected} clang-19 -c source.c'",
                    ):
                        label, version, binary_hash = record_build.compiler_info(command)
                        self.assertEqual(label, "clang-19")
                        self.assertEqual(version, "clang version 19")
                        self.assertEqual(binary_hash, digest("selected compiler"))
                        self.assertEqual(run.call_args.args[0][0],
                                         str(selected / "clang-19"))
                        self.assertEqual(run.call_args.kwargs["env"]["PATH"], str(selected))

    def test_build_recorder_rejects_env_split_expansion_that_it_cannot_model(self):
        for split in (
            "PATH=${TOOL_DIR}/bin clang-19 -c source.c",
            r"PATH=/toolchain/bin\:extra clang-19 -c source.c",
            "PATH=/toolchain/bin clang-19 -c source.c # comment",
        ):
            command = f"env -S {split!r}"
            with self.subTest(command=command):
                with self.assertRaisesRegex(ValueError, "cannot be fingerprinted"):
                    record_build.compiler_commands(command + "\n")

    def test_build_recorder_parses_valid_env_option_forms(self):
        cases = (
            (["env", "-uNAME", "-Cdir", "gcc-16", "-c", "a.c"], 3),
            (["env", "-u", "NAME", "-C", "dir", "gcc-16"], 5),
            (["env", "--unset=NAME", "--chdir=dir", "gcc-16"], 3),
            (["env", "-i", "-0", "--debug", "CCACHE_DISABLE=1", "--",
              "ccache", "clang-19"], 7),
            (["env", "--ignore-environment", "--null", "-v", "gcc-16"], 4),
            (["env", "--argv0", "shown-as", "aarch64-linux-gnu-gcc-16"], 3),
            (["env", "--argv0=shown-as", "clang-19"], 2),
            (["env", "-SCCACHE_DISABLE=1 clang++-19 -c a.cpp"], 2),
            (["env", "--split-string=CCACHE_DISABLE=1 clang++-19 -c a.cpp"], 2),
            (["env", "-S", "ccache aarch64-linux-gnu-g++-16 -c a.cpp"], 2),
        )
        for words, expected in cases:
            with self.subTest(words=words):
                self.assertEqual(record_build.compiler_word_index(words), expected)
        attached_split = record_build.shlex.split(
            "env -S'CCACHE_DISABLE=1 clang-19 -c src/a.c'")
        self.assertEqual(record_build.compiler_word_index(attached_split), 2)

    def test_build_recorder_rejects_incomplete_env_arguments(self):
        for words in (["env", "-u"], ["env", "-C"], ["env", "-S"],
                      ["env", "--argv0"], ["env", "--split-string="]):
            with self.subTest(words=words):
                self.assertIsNone(record_build.compiler_word_index(words))

    def test_build_recorder_prefers_human_readable_cpu_model(self):
        x86 = "processor : 0\nmodel : 112\nmodel name : Example Xeon\n"
        arm = "processor : 0\nmodel : 4\nHardware : Raspberry Pi 4 Model B\n"
        self.assertEqual(record_build.model_name_from_cpuinfo(x86, "fallback"),
                         "Example Xeon")
        self.assertEqual(record_build.model_name_from_cpuinfo(arm, "fallback"),
                         "Raspberry Pi 4 Model B")


if __name__ == "__main__":
    unittest.main()
