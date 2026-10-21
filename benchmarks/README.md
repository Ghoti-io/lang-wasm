# lang-wasm benchmark evidence

`runner.cpp` is an opt-in benchmark executable. It lowers each checked-in WAT
fixture to validated canonical Wasm bytes, hashes those exact bytes, then
decodes and executes those same bytes for every sample. The in-tree converter
source revision is covered by the source commit and source manifest; the
runner also refuses to execute a fixture whose binary hash differs from
`workloads/module-hashes.tsv`. The five pins were generated with
`make benchmark-fixture-hashes` on a native x86-64 host and a native arm64 host, and the
outputs and fixture/source hashes matched. Re-run that target when a workload
or the lowering implementation changes; review cross-architecture agreement
before collecting new measurements.

The runner requires runtime-debug and security development files; it hashes
fixture bytes in process with security's SHA-256 API. It takes
optional `iterations` and `samples` arguments (20,000 and 30 by default), and
rejects malformed, overflowing, zero, below-minimum, or unbalanced sample
counts. Sample counts must be a multiple of 30 so all five workload positions
and six tier orders balance exactly. Each steady-state
sample is preceded by two checked warmups. First-call and poll-event samples
have no warmup work. Interpreter, JIT, and attached-idle conditions are
interleaved using six balanced sample-order permutations to reduce clock and
thermal drift bias. The workload order also rotates across samples, so each
workload is measured at different positions in the run rather than in one fixed
block.

Debugger handles held by the runner are borrowed from their runtime context.
`grcore_context_destroy`, called by `glwasm_runtime_destroy`, runs the
runtime-debug context key's destructor and releases the debugger; the runner
must not try to destroy that handle separately.

## Reproducibility metadata

Required input variables are `GLWASM_BENCH_RUN_ID`, `GLWASM_BENCH_SOURCE_COMMIT`,
`GLWASM_BENCH_SOURCE_MANIFEST_SHA256`, `GLWASM_BENCH_SOURCE_MANIFEST_FILE`,
`GLWASM_BENCH_ENV_MANIFEST`,
`GLWASM_BENCH_IMAGE_ID`, `GLWASM_BENCH_POLICY`,
`GLWASM_BENCH_FUEL`, and `GLWASM_BENCH_OUTPUT`. Set the environment-manifest
path beside the raw CSV. Run `make benchmark-metadata` to exercise the forced
build and manifest capture without taking measurements; `make benchmark`
repeats that process before sampling. The target fingerprints the sources, then
forces the benchmark binary and its in-tree library dependencies to rebuild
with make's verbose command output captured in `<raw.csv>.build.log`. It
extracts the executed C compile, C++ compile, and link commands, fingerprints
the compiler binaries actually invoked, identifies the host and fixture
converter, derives effective flags from the captured commands, and generates
the JSON environment manifest. Supply the current build image as the full
`sha256:<64 hex digits>` digest. The command transcript
and manifest are retained next to the raw CSV. The benchmark exits before
sampling if the forced build fails or a required input/source identity is
inconsistent.
Compiler lookup honors explicit `env PATH=...`, `env -S` with literal arguments,
and `env -C` working directories. GNU `env -S` expansion, escape, or comment
syntax is rejected explicitly because its argument transformation differs from
shell tokenization; it cannot be recorded as a different compiler by accident.
Every C, C++, and link command is checked against the recorded compiler
binary's version and SHA-256, including later commands in the transcript.
Aliases that resolve to the same binary are accepted; a second compiler
identity that the manifest cannot represent is rejected. An unrecognized
wrapper around a compiler-shaped command also fails closed instead of being
omitted from both the recorder and the summarizer.

The runner opens `GLWASM_BENCH_ENV_MANIFEST` at the supplied path, then records
only its filename in raw rows. The recorder requires it in the same directory
as `GLWASM_BENCH_OUTPUT`; the summary verifies that sibling file in the fetched
result bundle. This permits an absolute results path on the build host without embedding a
machine-specific path in each sample.

The generated manifest includes compiler names, versions and binary hashes;
flags extracted from the captured commands; image ID;
source commit, full source-manifest hash and relative path; machine and
architecture; converter identity and source hash; exact executed C, C++, and
link command lines; and build-log relative path and hash. The build command
fields contain newline-separated actual invocations. The summarizer verifies
that the retained transcript matches its digest, the manifest contains every
compiler command in the transcript, and the retained environment manifest
matches the rows. A make
invocation and `CFLAGS`/`CXXFLAGS` alone are not accepted as build evidence.
The target generates the canonical reproduction command from the effective
iteration and sample arguments; the runner checks it against argv and the
summarizer verifies it against the input manifest.

The policy is one of `untrusted`, `standard`, or `trusted`. The runner loads
that preset, overrides its default fuel with `GLWASM_BENCH_FUEL`, and decodes
the exact fixture binary with that policy. Each row records the policy and
fuel. One run ID uses the same policy and fuel across every workload;
the summarizer rejects a run that mixes either setting even when each
workload's own rows are internally consistent. The input manifest records
iteration count, warmup count, requested sample
count, policy, and fuel; `input_sha256` must hash that exact manifest.

Before execution, `make benchmark` fingerprints the exact source files in all
shipped library trees (including dirty or untracked files, excluding generated
`build`, `docs`, `results`, and cache directories), writes the complete per-file
manifest including file modes,
and rejects it unless its SHA-256 matches the value in the environment
manifest. Preserve that file beside the raw CSV; the summarizer verifies its
digest and source commit. `GLWASM_BENCH_SOURCE_COMMIT` identifies the base
lang-wasm commit, while the source-manifest digest identifies the exact files
that were built.

Example environment setup (record the values and command in the manifest):

```sh
mkdir -p results
export GLWASM_BENCH_RUN_ID=wasm-x86-64-20261016
export GLWASM_BENCH_SOURCE_COMMIT="$(git rev-parse HEAD)"
export GLWASM_BENCH_SOURCE_MANIFEST_FILE=results/source-manifest.json
export GLWASM_BENCH_SOURCE_MANIFEST_SHA256="$(python3 benchmarks/source_manifest.py \
  --source-commit "$GLWASM_BENCH_SOURCE_COMMIT" \
  --write "$GLWASM_BENCH_SOURCE_MANIFEST_FILE")"
export GLWASM_BENCH_ENV_MANIFEST=results/environment.json
export GLWASM_BENCH_IMAGE_ID="$IMAGE_DIGEST"
export GLWASM_BENCH_POLICY=untrusted
export GLWASM_BENCH_FUEL=10000000
export GLWASM_BENCH_OUTPUT=results/raw.csv
make benchmark
# The target created raw.csv.build.log and environment.json. Keep both with
# source-manifest.json and raw.csv; the summarizer checks their hashes/content.
python3 benchmarks/summarize.py results/raw.csv > results/summary.csv
```

## Workloads and metrics

Each fixture has a deterministic expected result. The runner validates output
before and after timing and verifies paired tiers use the same pinned binary,
input, policy, and fuel.

| Workload | Guest operation | Primary metric |
| --- | --- | --- |
| `loop-numeric` | Long integer loop accumulating zero through `n-1` | ns/guest iteration |
| `calls` | One direct and one indirect call per iteration | ns/call |
| `linear-memory` | One 4-byte store and one 4-byte load per iteration | ns/byte transferred |
| `table-bulk` | One table copy, one table fill, and two indirect calls that observe both results | ns/operation |
| `mixed-boundary` | Each host import re-enters a second Wasm instance through the runtime-core boundary | ns/boundary call |

The mixed-boundary `hybrid` tier compiles the outer workload while the nested
instance executes in the interpreter. It measures the boundary plus compiled
outer-loop path; it is not a whole-workload JIT comparison. The `jit` label is
reserved for workloads whose measured Wasm execution is compiled.

Interpreter polling is measured with no debugger, an attached idle debugger,
and a separate step diagnostic. The idle pair uses identical guest work and
reports both ratio and absolute difference per work unit. The diagnostic times
stop discovery and resume to the next stop as separate event distributions;
it excludes debugger setup, transport, and work after resume. Collect at least
30 rows in each group.

## Compiled-path proof and failures

REQUIRE-JIT rejects tier refusal. The runner also requires the exported `run`
function to complete through compiled `RETURN` for both warmups and the timed
invocation, and rejects every other JIT exit across the module. Raw rows include
compiled entries, compiled returns from `run`, and JIT side-exit counts. The
summarizer independently checks these fields; wrapper entry alone cannot
qualify a measurement as compiled throughput.

If a sample fails after its workload row is configured, the runner writes a
raw row with nonzero `exit_code`, the error text, and any elapsed time captured
so far, flushes the CSV, and exits unsuccessfully. A failed row is evidence and
the summarizer rejects the dataset; it is never dropped. Configuration errors
before any sample begins are reported to stderr and do not create a fabricated
sample. For the two-part step diagnostic, the validated stop row is flushed
before requesting the step or starting resume. If the step/resume then fails,
the completed stop row remains and the runner emits a failed resume row.

## Summarizer contract

`summarize.py` validates the exact CSV schema, checked-in exact Wasm binary
hash pins, provenance hashes, recorded
environment manifest, command, input manifest/hash, policy/fuel consistency,
workload-specific units and work counts, contiguous sample indexes, stable
output digests, and compiled-path evidence. It requires the complete configured
matrix: interpreter/JIT steady state without a debugger, interpreter steady
state with attached-idle debugger, first-call latency, and (for `loop-numeric`)
both step stop and resume groups. Every group needs at least 30 samples;
paired groups must agree on input, binary, provenance, work units, sample
count, and output. The retained environment manifest and source manifest must
match the exact contents and hashes recorded in the CSV. Metadata fields cannot
be blank.

Elapsed measurements are nanoseconds. Summaries report min, median,
nearest-rank p95 (`sorted[ceil(0.95*n)-1]`), max, and median ns per work unit.
First-call and poll-event distributions are reported separately from
steady-state throughput. Raw CSV, stderr, and the exact environment-manifest
file must be retained. Never combine x86-64 and arm64 rows in one statistical group.
