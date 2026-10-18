#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
"""Validate and summarize raw lang-wasm benchmark samples.

Input is a CSV file with one row per timed sample. Raw rows are the evidence;
this tool never drops failed rows or invents missing samples. Percentiles use
the nearest-rank definition (rank = ceil(p * n)).
"""

import argparse
import csv
import hashlib
import importlib.util
import json
import math
import pathlib
import re
import statistics
import sys
from collections import defaultdict


FIELDS = (
    "run_id", "source_commit", "source_manifest_sha256",
    "environment_manifest_path", "environment_manifest",
    "environment_manifest_sha256", "image_id",
    "compiler", "compiler_version", "flags", "machine", "architecture",
    "fixture_converter", "fixture_converter_source_sha256",
    "workload", "workload_sha256", "input_manifest", "input_sha256", "policy", "fuel",
    "work_unit", "tier", "debugger", "phase", "segment", "warmup_work_units",
    "sample_index", "work_units", "compiled_entries", "compiled_returns",
    "jit_side_exits", "elapsed_ns", "result_sha256",
    "exit_code", "error_message", "command",
)
IDENTITY = (
    "source_commit", "source_manifest_sha256", "environment_manifest_path",
    "environment_manifest", "environment_manifest_sha256", "image_id", "compiler",
    "compiler_version", "flags", "machine", "architecture",
    "fixture_converter", "fixture_converter_source_sha256",
    "workload_sha256", "input_manifest", "input_sha256", "policy", "fuel", "work_unit",
    "warmup_work_units", "work_units",
)
RUN_IDENTITY = (
    "source_commit", "source_manifest_sha256", "environment_manifest_path",
    "environment_manifest", "environment_manifest_sha256", "image_id", "compiler",
    "compiler_version", "flags", "machine", "architecture",
    "fixture_converter", "fixture_converter_source_sha256",
    "command", "policy", "fuel",
)
GROUP = ("run_id", "workload", "tier", "debugger", "phase", "segment")
ALLOWED = {
    "tier": {"interpreter", "jit", "hybrid"},
    "debugger": {"none", "attached-idle", "step-diagnostic"},
    "phase": {"steady-state", "first-call", "poll-event"},
}
DEFAULT_WORKLOADS = (
    "loop-numeric", "calls", "linear-memory", "table-bulk", "mixed-boundary",
)
KNOWN_WORKLOADS = set(DEFAULT_WORKLOADS)
STEADY_WORK_UNITS = {
    "loop-numeric": ("guest-iteration", 1),
    "calls": ("call", 2),
    "linear-memory": ("byte", 8),
    "table-bulk": ("operation", 4),
    "mixed-boundary": ("boundary-call", 1),
}
MIN_STEADY_SAMPLES = 30
POLL_WORKLOAD = "loop-numeric"

_RECORDER_SPEC = importlib.util.spec_from_file_location(
    "_glwasm_benchmark_record_build", pathlib.Path(__file__).with_name("record_build.py"))
if _RECORDER_SPEC is None or _RECORDER_SPEC.loader is None:
    raise RuntimeError("cannot load benchmark build-transcript parser")
_RECORDER = importlib.util.module_from_spec(_RECORDER_SPEC)
_RECORDER_SPEC.loader.exec_module(_RECORDER)


def fail(message):
    raise ValueError(message)


def nearest_rank(values, fraction):
    ordered = sorted(values)
    return ordered[max(0, math.ceil(fraction * len(ordered)) - 1)]


def parse_input_manifest(text, where):
    values = {}
    for item in text.split(";"):
        if item.count("=") != 1:
            fail(f"{where}: malformed input_manifest")
        key, value = item.split("=", 1)
        if not key or not value or key in values:
            fail(f"{where}: malformed or duplicate input_manifest field")
        values[key] = value
    if set(values) != {"iterations", "warmup", "samples", "policy", "fuel"}:
        fail(f"{where}: input_manifest must contain iterations, warmup, samples, policy, and fuel")
    try:
        iterations = int(values["iterations"], 10)
        warmup = int(values["warmup"], 10)
        samples = int(values["samples"], 10)
        fuel = int(values["fuel"], 10)
    except ValueError:
        fail(f"{where}: input_manifest counts must be decimal integers")
    if (iterations < 1 or iterations > 100000 or warmup not in {0, 2} or
            samples < 30 or samples > 2**32 - 1 or samples % 30 != 0 or
            fuel < 1 or fuel > 2**64 - 1):
        fail(f"{where}: input_manifest values are outside the benchmark contract")
    if values["policy"] not in {"untrusted", "standard", "trusted"}:
        fail(f"{where}: invalid input policy")
    return iterations, warmup, samples, values["policy"], fuel


def load_pinned_module_hashes():
    path = pathlib.Path(__file__).resolve().parent / "workloads" / "module-hashes.tsv"
    pins = {}
    with path.open(encoding="utf-8") as stream:
        for line_number, line in enumerate(stream, start=1):
            fields = line.split()
            if not fields or fields[0].startswith("#"):
                continue
            if len(fields) != 2 or not re.fullmatch(r"[0-9a-fA-F]{64}", fields[1]):
                fail(f"{path}:{line_number}: invalid exact Wasm binary hash pin")
            if fields[0] in pins:
                fail(f"{path}:{line_number}: duplicate workload hash pin")
            pins[fields[0]] = fields[1].lower()
    if set(pins) != KNOWN_WORKLOADS:
        fail(f"{path}: must pin exactly the configured Story 13 workloads")
    return pins


def load(path):
    groups = defaultdict(list)
    csv_path = pathlib.Path(path).resolve()
    pinned_hashes = load_pinned_module_hashes()
    # One run repeats the same source manifest in every sample. Verify each
    # retained path/digest once; row-to-environment comparisons remain per row.
    verified_source_manifests = {}
    with open(path, newline="", encoding="utf-8") as stream:
        reader = csv.DictReader(stream)
        if tuple(reader.fieldnames or ()) != FIELDS:
            fail("CSV header must exactly match: " + ",".join(FIELDS))
        for line, row in enumerate(reader, start=2):
            where = f"line {line}"
            required = [field for field in FIELDS if field not in {"error_message"}]
            if None in row or any(row.get(field, "").strip() == "" for field in required):
                fail(f"{where}: missing or extra CSV field")
            for field, allowed in ALLOWED.items():
                if row[field] not in allowed:
                    fail(f"{where}: invalid {field} {row[field]!r}")
            if (row["debugger"] == "step-diagnostic") != (row["phase"] == "poll-event"):
                fail(f"{where}: step-diagnostic rows must use poll-event phase, and only those rows may")
            expected_segment = {"steady-state": "whole", "first-call": "first-call"}.get(row["phase"])
            if expected_segment is not None and row["segment"] != expected_segment:
                fail(f"{where}: {row['phase']} segment must be {expected_segment!r}")
            if row["phase"] == "poll-event" and row["segment"] not in {"stop", "resume"}:
                fail(f"{where}: poll-event segment must be 'stop' or 'resume'")
            for field in ("warmup_work_units", "sample_index", "work_units", "fuel", "compiled_entries",
                          "compiled_returns", "jit_side_exits", "elapsed_ns", "exit_code"):
                try:
                    row[field] = int(row[field], 10)
                except ValueError:
                    fail(f"{where}: {field} must be an integer")
            if row["sample_index"] < 0 or row["work_units"] <= 0 or row["elapsed_ns"] < 0:
                fail(f"{where}: sample index/work must be positive and timing nonnegative")
            if row["exit_code"] < 0 or row["exit_code"] > 255:
                fail(f"{where}: exit_code must be in the process status range 0..255")
            if row["warmup_work_units"] < 0:
                fail(f"{where}: warmup_work_units must be nonnegative")
            if row["phase"] == "steady-state" and row["warmup_work_units"] == 0:
                fail(f"{where}: steady-state samples require a nonzero warmup")
            if row["phase"] in {"first-call", "poll-event"} and row["work_units"] != 1:
                fail(f"{where}: first-call and poll-event rows measure exactly one event")
            if row["elapsed_ns"] == 0 and row["exit_code"] == 0:
                fail(f"{where}: successful sample must have positive elapsed time")
            if row["exit_code"] != 0:
                if not row["error_message"].strip():
                    fail(f"{where}: failed sample has no error_message")
                fail(f"{where}: failed sample (exit_code={row['exit_code']}): {row['error_message']}")
            if row["error_message"].strip():
                fail(f"{where}: successful sample has an error_message")
            if row["tier"] in {"jit", "hybrid"} and row["compiled_entries"] < 1:
                fail(f"{where}: compiled or hybrid row has no observed compiled entry")
            if row["tier"] == "interpreter" and row["compiled_entries"] != 0:
                fail(f"{where}: interpreter row unexpectedly entered compiled code")
            if row["tier"] in {"jit", "hybrid"} and row["compiled_returns"] < 1:
                fail(f"{where}: compiled or hybrid row has no confirmed compiled guest return")
            if row["tier"] in {"jit", "hybrid"} and row["phase"] == "steady-state" and row["compiled_returns"] < 3:
                fail(f"{where}: compiled steady-state row must confirm two warmup and one timed compiled return")
            if row["jit_side_exits"] != 0:
                fail(f"{where}: benchmark row observed a JIT side exit")
            if row["tier"] == "interpreter" and row["compiled_returns"] != 0:
                fail(f"{where}: interpreter row unexpectedly reported compiled returns")
            if row["phase"] == "first-call" and row["work_unit"] != "first-call":
                fail(f"{where}: first-call work_unit must be 'first-call'")
            expected_compiled_tier = "hybrid" if row["workload"] == "mixed-boundary" else "jit"
            if row["phase"] == "first-call" and (row["tier"], row["debugger"]) != (expected_compiled_tier, "none"):
                fail(f"{where}: first-call samples use the workload's compiled tier with no debugger")
            if row["phase"] == "poll-event" and row["work_unit"] != "event":
                fail(f"{where}: poll-event work_unit must be 'event'")
            if row["phase"] == "poll-event" and (row["workload"], row["tier"], row["debugger"]) != (
                    POLL_WORKLOAD, "interpreter", "step-diagnostic"):
                fail(f"{where}: poll-event samples are loop-numeric interpreter step diagnostics")
            if any(len(row[field]) != 64 for field in
                   ("source_manifest_sha256", "environment_manifest_sha256", "fixture_converter_source_sha256", "workload_sha256", "input_sha256", "result_sha256")):
                fail(f"{where}: source, environment, converter, workload, input, and result hashes must be SHA-256 hex digests")
            try:
                for field in ("source_manifest_sha256", "environment_manifest_sha256", "fixture_converter_source_sha256", "workload_sha256", "input_sha256", "result_sha256"):
                    int(row[field], 16)
            except ValueError:
                fail(f"{where}: hash contains non-hexadecimal characters")
            if len(row["source_commit"]) < 7 or len(row["source_commit"]) > 64 or not re.fullmatch(r"[0-9a-fA-F]+", row["source_commit"]):
                fail(f"{where}: source_commit must be a hexadecimal Git object id")
            if hashlib.sha256(row["environment_manifest"].encode("utf-8")).hexdigest() != row["environment_manifest_sha256"]:
                fail(f"{where}: environment_manifest_sha256 does not hash the recorded environment_manifest")
            try:
                environment = json.loads(row["environment_manifest"])
            except json.JSONDecodeError as error:
                fail(f"{where}: environment manifest is not JSON: {error}")
            required_environment = {
                "compiler", "compiler_version", "compiler_sha256", "cxx",
                "cxx_version", "cxx_sha256", "flags", "build_commands", "image_id",
                "source_commit", "source_manifest_sha256", "source_manifest_path", "machine",
                "architecture", "fixture_converter",
                "fixture_converter_source_sha256", "build_log_path", "build_log_sha256",
            }
            if not isinstance(environment, dict) or required_environment - environment.keys():
                missing = sorted(required_environment - environment.keys()) if isinstance(environment, dict) else sorted(required_environment)
                fail(f"{where}: environment manifest is missing required provenance keys: {', '.join(missing)}")
            environment_path = pathlib.Path(row["environment_manifest_path"])
            if environment_path.is_absolute() or ".." in environment_path.parts:
                fail(f"{where}: environment_manifest_path must stay beside the result files")
            try:
                retained_environment = (csv_path.parent / environment_path).read_bytes()
            except OSError as error:
                fail(f"{where}: cannot read retained environment manifest {environment_path}: {error}")
            if retained_environment != row["environment_manifest"].encode("utf-8"):
                fail(f"{where}: retained environment manifest differs from the recorded environment_manifest")
            if any(not isinstance(environment[key], str) or not environment[key].strip()
                   for key in required_environment - {"build_commands"}):
                fail(f"{where}: environment manifest provenance values must be nonempty strings")
            commands = environment["build_commands"]
            if (not isinstance(commands, dict) or
                    set(commands) != {"c_compile", "cxx_compile", "link"} or
                    any(not isinstance(value, str) or not value.strip()
                        for value in commands.values())):
                fail(f"{where}: environment manifest build_commands must record C, C++, and link command forms")
            if not re.fullmatch(r"[0-9a-fA-F]{64}", environment["build_log_sha256"]):
                fail(f"{where}: environment manifest build_log_sha256 must be a SHA-256 digest")
            build_log_path = pathlib.Path(environment["build_log_path"])
            if build_log_path.is_absolute() or ".." in build_log_path.parts:
                fail(f"{where}: build_log_path must stay beside the result files")
            try:
                build_log = (csv_path.parent / build_log_path).read_bytes()
            except OSError as error:
                fail(f"{where}: cannot read retained build log {build_log_path}: {error}")
            if hashlib.sha256(build_log).hexdigest() != environment["build_log_sha256"].lower():
                fail(f"{where}: retained build log does not match its recorded SHA-256")
            build_log_text = build_log.decode("utf-8", errors="strict")
            build_log_lines = {line.strip() for line in build_log_text.splitlines()}
            for command_kind, command_text in commands.items():
                if any(line.strip() not in build_log_lines
                       for line in command_text.splitlines()):
                    fail(f"{where}: {command_kind} contains a command absent from the retained build log")
            if commands != _RECORDER.compiler_commands(build_log_text):
                fail(f"{where}: build command manifest omits or changes compiler commands from the retained log")
            for key in ("compiler_sha256", "cxx_sha256", "source_manifest_sha256",
                        "fixture_converter_source_sha256"):
                if not re.fullmatch(r"[0-9a-fA-F]{64}", environment[key]):
                    fail(f"{where}: environment manifest {key} must be a SHA-256 digest")
            source_manifest_path = pathlib.Path(environment["source_manifest_path"])
            if source_manifest_path.is_absolute() or ".." in source_manifest_path.parts:
                fail(f"{where}: source_manifest_path must stay beside the result files")
            source_manifest_file = csv_path.parent / source_manifest_path
            try:
                source_key = (source_manifest_file.resolve(),
                              environment["source_manifest_sha256"].lower())
            except OSError as error:
                fail(f"{where}: cannot read source manifest {source_manifest_file}: {error}")
            if source_key not in verified_source_manifests:
                try:
                    source_manifest_bytes = source_manifest_file.read_bytes()
                    source_manifest = json.loads(source_manifest_bytes)
                except (OSError, json.JSONDecodeError) as error:
                    fail(f"{where}: cannot read source manifest {source_manifest_file}: {error}")
                if hashlib.sha256(source_manifest_bytes).hexdigest() != source_key[1]:
                    fail(f"{where}: source manifest file does not match its recorded SHA-256")
                verified_source_manifests[source_key] = source_manifest
            source_manifest = verified_source_manifests[source_key]
            if (not isinstance(source_manifest, dict) or
                    source_manifest.get("source_commit") != environment["source_commit"]):
                fail(f"{where}: source manifest commit differs from environment provenance")
            row_environment_pairs = (
                ("compiler", "compiler"), ("compiler_version", "compiler_version"),
                ("flags", "flags"), ("image_id", "image_id"),
                ("source_commit", "source_commit"),
                ("source_manifest_sha256", "source_manifest_sha256"),
                ("machine", "machine"), ("architecture", "architecture"),
                ("fixture_converter", "fixture_converter"),
                ("fixture_converter_source_sha256", "fixture_converter_source_sha256"),
            )
            for row_key, environment_key in row_environment_pairs:
                if row[row_key] != environment[environment_key]:
                    fail(f"{where}: row {row_key} differs from the recorded environment manifest")
            if row["fixture_converter"] != "lang-wasm-wat-lower":
                fail(f"{where}: unknown fixture converter")
            if hashlib.sha256(row["input_manifest"].encode("utf-8")).hexdigest() != row["input_sha256"]:
                fail(f"{where}: input_sha256 does not hash the recorded input_manifest")
            iterations, warmup_count, samples, policy, fuel = parse_input_manifest(row["input_manifest"], where)
            expected_command = (f"make benchmark BENCH_ITERATIONS={iterations} "
                                f"BENCH_SAMPLES={samples}")
            if row["command"] != expected_command:
                fail(f"{where}: command must exactly reproduce the input manifest")
            if row["policy"] != policy or row["fuel"] != fuel:
                fail(f"{where}: row policy/fuel differs from input_manifest")
            expected_warmup = 2 if row["phase"] == "steady-state" else 0
            if warmup_count != expected_warmup:
                fail(f"{where}: input_manifest warmup does not match measured phase")
            if row["workload"] not in KNOWN_WORKLOADS:
                fail(f"{where}: unknown workload {row['workload']!r}")
            if row["workload_sha256"].lower() != pinned_hashes[row["workload"]]:
                fail(f"{where}: workload_sha256 differs from the checked-in exact Wasm binary pin")
            if row["phase"] == "steady-state":
                expected_unit, multiplier = STEADY_WORK_UNITS[row["workload"]]
                expected_units = iterations * multiplier
                if row["work_unit"] != expected_unit or row["work_units"] != expected_units:
                    fail(f"{where}: work unit/count does not match workload and input_manifest")
                if row["warmup_work_units"] != warmup_count * expected_units:
                    fail(f"{where}: warmup work count does not match input_manifest")
            elif row["phase"] in {"first-call", "poll-event"}:
                if row["work_units"] != 1 or row["warmup_work_units"] != 0:
                    fail(f"{where}: compile and poll-event rows require one event and zero warmup")
            if row["phase"] == "first-call" and row["workload"] not in KNOWN_WORKLOADS:
                fail(f"{where}: first-call workload is not configured")
            groups[tuple(row[key] for key in GROUP)].append(row)
    if not groups:
        fail("no benchmark samples")
    # Recheck the retained files after reading all rows, so a manifest replaced
    # during summarization cannot be accepted merely because its first read
    # was valid. This keeps I/O bounded to two reads per distinct manifest.
    for source_path, expected_sha256 in verified_source_manifests:
        try:
            final_bytes = source_path.read_bytes()
        except OSError as error:
            fail(f"retained source manifest changed during summarization: {error}")
        if hashlib.sha256(final_bytes).hexdigest() != expected_sha256:
            fail("retained source manifest changed during summarization")
    return groups


def validate(groups, workloads):
    paired = defaultdict(dict)
    run_metadata = {}
    for key, rows in groups.items():
        indexes = [row["sample_index"] for row in rows]
        if len(indexes) != len(set(indexes)):
            fail(f"{key}: duplicate sample_index")
        _, _, expected_samples, _, _ = parse_input_manifest(rows[0]["input_manifest"], str(key))
        if len(rows) != expected_samples:
            fail(f"{key}: row count differs from input_manifest samples={expected_samples}")
        if sorted(indexes) != list(range(len(rows))):
            fail(f"{key}: sample_index must be contiguous from 0; missing samples are not inferred")
        baseline = tuple(rows[0][field] for field in IDENTITY)
        for row in rows[1:]:
            if tuple(row[field] for field in IDENTITY) != baseline:
                fail(f"{key}: run metadata/workload differs between samples")
            if row["command"] != rows[0]["command"]:
                fail(f"{key}: command differs between samples")
        metadata = tuple(rows[0][field] for field in RUN_IDENTITY)
        if key[0] in run_metadata and run_metadata[key[0]] != metadata:
            fail(f"{key[0]}: source/build/host/policy/fuel metadata differs within one run_id")
        run_metadata[key[0]] = metadata
        digest = rows[0]["result_sha256"]
        if any(row["result_sha256"] != digest for row in rows):
            fail(f"{key}: output digest is not stable across samples")
        if key[4] == "steady-state" and len(rows) < MIN_STEADY_SAMPLES:
            fail(f"{key}: {len(rows)} steady-state samples; at least {MIN_STEADY_SAMPLES} required")
        if key[4] in {"first-call", "poll-event"} and len(rows) < MIN_STEADY_SAMPLES:
            fail(f"{key}: {len(rows)} samples; at least {MIN_STEADY_SAMPLES} required for a reported distribution")
        expected_compiled_tier = "hybrid" if key[1] == "mixed-boundary" else "jit"
        if key[4] == "first-call" and (key[2], key[3]) != (expected_compiled_tier, "none"):
            fail(f"{key}: first-call is defined for the workload's compiled tier with no debugger")
        if key[4] == "poll-event" and (key[2], key[3]) != ("interpreter", "step-diagnostic"):
            fail(f"{key}: poll-event is defined only for interpreter step diagnostics")
        if key[4] == "steady-state":
            pair_key = (key[0], key[1], key[3], key[4], key[5])
            paired[pair_key][key[2]] = rows
    for key, tiers in paired.items():
        compiled_tier = "hybrid" if key[1] == "mixed-boundary" else "jit"
        if key[2] == "none" and key[3] == "steady-state":
            if set(tiers) != {"interpreter", compiled_tier}:
                fail(f"{key}: steady-state no-debugger result requires both tiers")
        if "interpreter" in tiers and compiled_tier in tiers:
            left, right = tiers["interpreter"], tiers[compiled_tier]
            for field in IDENTITY:
                if left[0][field] != right[0][field]:
                    fail(f"{key}: interpreter/JIT pair differs in {field}")
            if len(left) != len(right):
                fail(f"{key}: interpreter/JIT sample counts differ")
            if sorted(row["sample_index"] for row in left) != sorted(
                    row["sample_index"] for row in right):
                fail(f"{key}: interpreter/JIT sample indexes differ")
            if left[0]["result_sha256"] != right[0]["result_sha256"]:
                fail(f"{key}: interpreter/JIT validated output digests differ")
            if any(row["result_sha256"] != left[0]["result_sha256"] for row in left + right):
                fail(f"{key}: output digest changed between repeated samples")

    poll_states = defaultdict(dict)
    step_events = defaultdict(dict)
    for key, rows in groups.items():
        run_id, workload, tier, debugger, phase, segment = key
        if tier == "interpreter" and phase == "steady-state" and debugger in {"none", "attached-idle"}:
            poll_states[(run_id, workload)][debugger] = rows
        if phase == "poll-event":
            step_events[(run_id, workload)][segment] = rows
    for key, states in poll_states.items():
        if set(states) != {"none", "attached-idle"}:
            fail(f"{key}: instruction-poll comparison requires no-debugger and attached-idle interpreter samples")
        left, right = states["none"], states["attached-idle"]
        for field in IDENTITY:
            if left[0][field] != right[0][field]:
                fail(f"{key}: no-debugger/attached-idle comparison differs in {field}")
        if len(left) != len(right):
            fail(f"{key}: no-debugger/attached-idle sample counts differ")
        if sorted(row["sample_index"] for row in left) != sorted(
                row["sample_index"] for row in right):
            fail(f"{key}: no-debugger/attached-idle sample indexes differ")
        if any(row["result_sha256"] != left[0]["result_sha256"] for row in left + right):
            fail(f"{key}: no-debugger/attached-idle outputs differ")

    for key, segments in step_events.items():
        if set(segments) != {"stop", "resume"}:
            fail(f"{key}: step diagnostic requires both stop and resume segments")
        stop, resume = segments["stop"], segments["resume"]
        if len(stop) != len(resume):
            fail(f"{key}: stop and resume sample counts differ")
        if sorted(row["sample_index"] for row in stop) != sorted(
                row["sample_index"] for row in resume):
            fail(f"{key}: stop and resume sample indexes differ")
        for field in IDENTITY:
            if stop[0][field] != resume[0][field]:
                fail(f"{key}: stop and resume differ in {field}")
        if stop[0]["result_sha256"] != resume[0]["result_sha256"]:
            fail(f"{key}: stop and resume output digests differ")

    # Require the complete configured matrix, so a partial run cannot look
    # complete merely because the rows it did produce are internally sound.
    run_ids = {key[0] for key in groups}
    for run_id in run_ids:
        for workload in workloads:
            compiled_tier = "hybrid" if workload == "mixed-boundary" else "jit"
            for tier in ("interpreter", compiled_tier):
                key = (run_id, workload, tier, "none", "steady-state", "whole")
                if key not in groups:
                    fail(f"{key}: required steady-state matrix row is missing")
            for debugger in ("none", "attached-idle"):
                key = (run_id, workload, "interpreter", debugger, "steady-state", "whole")
                if key not in groups:
                    fail(f"{key}: required interpreter polling row is missing")
            cold = (run_id, workload, compiled_tier, "none", "first-call", "first-call")
            if cold not in groups:
                fail(f"{cold}: required first-call row is missing")
            steady_jit = (run_id, workload, compiled_tier, "none", "steady-state", "whole")
            if groups[cold][0]["result_sha256"] != groups[steady_jit][0]["result_sha256"]:
                fail(f"{cold}: first-call result differs from the steady-state compiled result")
        if POLL_WORKLOAD in workloads:
            for segment in ("stop", "resume"):
                poll = (run_id, POLL_WORKLOAD, "interpreter", "step-diagnostic", "poll-event", segment)
                if poll not in groups:
                    fail(f"{poll}: required stepping/breakpoint diagnostic row is missing")

    expected_workloads = set(workloads)
    unexpected = {key[1] for key in groups} - expected_workloads
    if unexpected:
        fail("unconfigured workload(s): " + ", ".join(sorted(unexpected)))


def summarize(groups, workloads):
    print("# elapsed units are nanoseconds; p95 uses nearest-rank sorted[ceil(0.95*n)-1].")
    print("# configured_workloads=" + ",".join(workloads) + "; completeness=" +
          ("full-story13" if set(workloads) == KNOWN_WORKLOADS else "partial"))
    print("# steady-state unit=ns/work-unit; first-call unit=ns/first-call; poll-event unit=ns/event.")
    print("# first-call and poll-event are distinct latency distributions, never included in steady-state throughput.")
    writer = csv.writer(sys.stdout, lineterminator="\n")
    writer.writerow(("run_id", "workload", "tier", "debugger", "phase", "segment", "unit",
                     "sample_count", "min_elapsed_ns", "median_elapsed_ns",
                     "p95_elapsed_ns", "max_elapsed_ns", "work_unit",
                     "median_ns_per_work_unit"))
    for key in sorted(groups):
        rows = groups[key]
        values = [row["elapsed_ns"] for row in rows]
        median = statistics.median(values)
        work_units = rows[0]["work_units"]
        unit = f"ns/{rows[0]['work_unit']}" if key[4] == "steady-state" else {
            "first-call": "ns/first-call", "poll-event": "ns/event"}[key[4]]
        per_unit = median / work_units if key[4] == "steady-state" else ""
        writer.writerow((*key, unit, len(values), min(values), median,
                         nearest_rank(values, 0.95), max(values), rows[0]["work_unit"], per_unit))

    print("# polling_overhead: attached_idle / none, measured in the interpreter on identical guest work")
    writer.writerow(("run_id", "workload", "sample_count", "work_unit",
                     "none_median_ns_per_work_unit", "attached_idle_median_ns_per_work_unit",
                     "ratio", "delta_ns_per_work_unit"))
    for run_id, workload in sorted({(key[0], key[1]) for key in groups}):
        baseline = groups[(run_id, workload, "interpreter", "none", "steady-state", "whole")]
        attached = groups[(run_id, workload, "interpreter", "attached-idle", "steady-state", "whole")]
        work_units = baseline[0]["work_units"]
        base_ns = statistics.median(row["elapsed_ns"] for row in baseline) / work_units
        attached_ns = statistics.median(row["elapsed_ns"] for row in attached) / work_units
        writer.writerow((run_id, workload, len(baseline), baseline[0]["work_unit"],
                         base_ns, attached_ns, attached_ns / base_ns,
                         attached_ns - base_ns))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("samples_csv", help="raw per-sample CSV; preserve this file")
    parser.add_argument("--workload", action="append", dest="workloads",
                        choices=sorted(KNOWN_WORKLOADS),
                        help="configure one required workload; repeat as needed (default: complete Story 13 set)")
    args = parser.parse_args()
    try:
        groups = load(args.samples_csv)
        workloads = tuple(args.workloads or DEFAULT_WORKLOADS)
        if not workloads or len(workloads) != len(set(workloads)):
            fail("configured workloads must be nonempty and unique")
        unknown = set(workloads) - KNOWN_WORKLOADS
        if unknown:
            fail("unknown configured workload(s): " + ", ".join(sorted(unknown)))
        validate(groups, workloads)
        summarize(groups, workloads)
    except (OSError, ValueError) as error:
        print(f"summarize.py: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
