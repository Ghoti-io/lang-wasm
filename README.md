# Ghoti.io Lang-wasm

A WebAssembly 3.0 decoder, validator, translator, interpreter and baseline JIT
(`GLWASM`). Binary decoding and validation remain separate passes. It is part of the
[Ghoti.io suite](https://github.com/Ghoti-io/suite).

The design, including the limit defaults and the type lattice, is in
[documentation/design.md](documentation/design.md).

## Status

**Interpreter and baseline JIT implemented for the in-scope feature set.** Decode and validation are in
place: multiple memories, memory64, table64, exceptions, tail calls, bulk
memory and extended constant expressions. The public `parse.h` API parses
WAT/WAST parenthesized syntax into an owned tree, preserving source locations
and decoding byte-string and Unicode escapes. `script.h` recognizes typed
WAST command forms and retains their syntax trees. A deferred feature that
decodes (a typed function reference, `v128`, the legacy `try` / `catch` /
`rethrow` encodings) is `GLWASM_ERR_UNSUPPORTED`. `glwasm_wat_module_parse`
lowers a narrow function subset to canonical binary sections and feeds those
sections through the regular decoder/validator. It currently supports numeric
function parameters/results (`i32`, `i64`, `f32`, `f64`, including multi-value
results), explicit named/numeric function types and type uses, inline and module
function imports/exports, folded numeric expressions, linear integer/floating
constants, the standard scalar numeric operator family, local access, direct
and indirect calls, reference null/function instructions, table get/set, basic
folded/linear block, loop, if/else, branch and return forms, numeric branch
tables, defined memories/tables including memory64/table64, function/table/memory/global
imports, start functions, active,
passive and declarative function-index elements, expression-form elements,
active/passive data segments, selected bulk memory/table operations,
multi-memory scalar accesses, constant numeric globals, and function/table/
memory/global exports. Exception lowering currently covers tag imports,
definitions and exports, `throw`, and folded `try_table` catch clauses. Inline
block parameter groups, catch payload/ref-stack semantics, and the remaining
exception forms are not yet covered. Proposal grammars outside the listed
subset return an explicit unsupported or syntax error. This is an incremental
frontend and does not yet cover the in-scope WAT grammar.
Validated instructions retain absolute module byte offsets (for WAT, offsets
refer to the canonical lowered binary). Translation creates module-owned
instruction arrays with resolved structured branch destinations and a total,
monotonic offset map including its terminal boundary. The runtime-core-backed
interpreter and baseline JIT cover the supported numeric, control-flow, call,
memory, table, bulk-operation, reference, and exception paths exercised by the
pinned WAST and real-program corpora. Unsupported instructions and signatures
receive explicit refusals; the JIT's normal policy falls back before a guest
effect, while its required-tier test mode reports the refusal. Guest calls use
persistent runtime-core frames rather than C recursion; traps carry Wasm trap
kinds, and polls, fuel, guest-depth limits, pause/resume, and moving-stack
behavior have focused tests. Fuel costs one unit per Wasm opcode using a table
shared by the interpreter and JIT. Runtime tables support imported and defined
resources, initialization, growth, and bulk operations. The public embedder
and import API provides exact import grants. Deferred features remain visible
in the WAST classification and are never counted as passes. The library depends on
`cutil`, `runtime-core`, and `runtime-jit` through pkg-config. The `runtime-jit`
dependency supplies Story 10's module-shared compiled tier; `runtime-debug` is
an optional test-only dependency for the debugger contract tests.

## Debugging a paused context

Story 7 exposes `glwasm_runtime_context` as a borrowed context for attaching
`runtime-debug`. A Wasm frame reports its module name, full-width function
identity and absolute byte offset; its source-line field is zero. Optional
module/function/local names come from the bounded `name` custom section.
Paused frames expose locals, operand-stack slots, imported and defined globals,
and read-only, bounds-checked memory ranges. Instruction breakpoints match the
exact `(engine, module, function, byte offset)` identity. Instruction stepping advances one
poll; function stepping enters, steps over, or exits functions. Memory and
frame views are available only while the context is readable and remain valid
only for that stop. The compiled debugger path and cross-tier frame differential
are implemented and exercised in ordinary, GC-torture, and relocation gates.

The frame differential compares identity, locals, operand-stack slots, globals
and read-only memory at each instruction poll. It runs with a torture heap and
an always-moving runtime-core stack; `make test-debug-relocate` repeats that
comparison with a live rooted probe and requires the relocation collector to
move it and preserve its value. The relocation target requires a
`RELOCATE=yes` runtime-heap installed in the first `PKG_CONFIG_PATH` prefix.

The spec-suite gate, `testSpec_suite`, runs every in-scope command of the
pinned corpus in both the interpreter and the JIT tier and fails on any command
that does not pass; out-of-scope commands are named, with a reason, in
`tests/data-wast-assertion-skips.tsv`. In the JIT tier it also fails on any
compile refusal or exit to the interpreter that has no named rule in
`tests/data-wast-jit-rules.tsv` (`make test-spec-suite` runs it alone). The
rules are explained in `documentation/design.md`.

The same gate runs the corpus a second time as a frame differential: each
invocation runs natively on a fuel allowance drawn from its place in the script,
stops where that runs out, and its whole stack (every frame's identity, locals
and operands, the globals, and every memory) must match the interpreter's at
the same instruction. It runs on a torture heap collected at every stop with a
rooted probe, and a stack that moves at every push; `make test-spec-relocate`
repeats it with relocation required. The gate fails if too few invocations stop,
or too few stops are inside compiled code.
The corpus gate separately reports runtime-unsupported and runtime-init-trap
outcomes in its diagnostic summaries. Trap assertions match recognized Wasm trap names;
table bounds also accepts the spec-suite spelling `out of bounds table access`,
and uninitialized-element messages may include a numeric table index.

`make test` verifies the complete pinned `wg-3.0` corpus and parses its WAST
commands. Fetch the corpus with `../runtime-jit/tools/wasm/fetch.sh`. Updating
the pin requires changing `tools/wasm/WASM_SPEC_COMMIT`, regenerating
`tools/wasm/WAST_FILES` and `SHA256SUMS`, fetching the new corpus, and
reviewing `tests/data-wast-classification.tsv`. The corpus gate rejects
missing, changed, extra or unclassified files. The test adapter executes the supported assertion subset, checks argument and result types against function signatures, compares finite floating-point results bit-exactly, and handles canonical, arithmetic, and payload NaN assertions according to their WAST forms. It reports results separately from assertions pending later-story or unsupported instructions.
File and assertion skips must identify a deferred feature
present in the source; a skip cannot hide an otherwise in-scope test.

`glwasm_text_parse` returns an owned generic syntax tree with byte offsets and
one-based line/column locations. `glwasm_script_parse` recognizes module
(text, binary and quote), register, invoke, get, and the script assertion
forms; each `GLWASM_Command` borrows its syntax node from the owning script.
Parsing does not execute commands or claim assertion success.
`glwasm_wat_module_parse` returns an owned, decoded and validated
`GLWASM_Module`; its error location points to the source form that failed
parsing or lowering. A validation failure from the generated binary is
currently attributed to the module form as a whole.

## Example

```c
#include <ghoti.io/lang-wasm/lang-wasm.h>
#include <stdio.h>

int main(void) {
  static const unsigned char bytes[] = {
      0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};
  GLWASM_Module * module = NULL;
  GLWASM_Failure failure;
  GLWASM_Result result;

  result = glwasm_module_decode(bytes, sizeof bytes, NULL, NULL, &failure,
      &module);
  if (result != GLWASM_OK) {
    fprintf(stderr, "%s\n", glwasm_result_string(result));
    return 1;
  }
  glwasm_module_destroy(module);
  return 0;
}
```

`NULL` limits are the defaults from `glwasm_limits_default`. `NULL` allocator
is the default. The module pointer is written only on success.

## Host policy and instances

`glwasm_policy_preset` copies the untrusted, standard, or trusted policy into
caller-owned storage. Change fields on that copy to override limits, feature
bits, or runtime budget defaults. `glwasm_module_decode_with_policy` applies
the limits before decode allocations, rejects disabled in-scope features, and
stores the selected policy with the module. The policy is immutable after
decode. Instantiate with `glwasm_runtime_create_with_imports`; every imported
function must have an exact module-and-field entry in its allowlist. Imports
under `tang.*` also require `allow_tang_v1_imports` in the decoded module
policy; this capability is disabled by every preset and grants only the
`tang.v1` version. Destroy instances before their borrowed module, and resume a paused call with
`glwasm_runtime_resume`.

The module policy's `jit_disabled_cpu_features` selects an immutable JIT cache
key. Presets disable optional CPU features for portable baseline code. A
caller may set this mask to `GRJIT_FEATURES_NONE` before decode to use features
the host actually supports; unknown mask bits are rejected. Instances of one
module share compiled artifacts. When two instances bind to the same
runtime-core context, each keeps its own entry slot and registration ownership,
so unbinding or destroying one leaves the other's compiled code available.

The linker accepts exact module-and-field grants for functions, memories,
tables, globals, and tags. Resource imports use typed provider callbacks; memory
and table access holds a short lease that ends before polling or growth. A host
callback that pauses must provide an explicit resume callback, so resuming does
not repeat the callback's initial side effects.

Paused snapshots preserve the guest stack and instance state, including globals,
memories, tables, and segment drop state. Snapshot creation reports
`GLWASM_ERR_UNSUPPORTED` for imported modules/resources and modules using
externref or exnref, as well as a pending host callback continuation. Restore
requires the same still-live module and begins under the destination runtime's
budget.

Each host call exposes the active runtime-core context, the Wasm engine ID, and
an opaque per-instance token. A mixed-engine adapter can register its engine
in that context and open a runtime-core boundary activation for each call. The
boundary stores foreign exception origin and identity tokens; the owner remains
responsible for rooting and relocation. The test-only real-Wasm fixture covers
reentry, pause/resume, instance isolation, and identity transport. Production
Tang loader integration belongs to Story 14 of
`spec-tang-host-extensibility`. Cross-heap handle rooting is owned by Story 13
(CAP-14) of `spec-tang-host-extensibility`, with runtime-core providing the
implementation.

An uncaught Wasm exception crossing a nested boundary is returned as an
opaque `GLWASM_ForeignException` engine/identity pair through
`glwasm_runtime_last_foreign_exception`. Its identity is stable for the
originating runtime's lifetime and can be passed back as an `exnref` to that
runtime. The implementation never exposes the exception object's address;
limits remain uncatchable and do not set the foreign-exception result.
Exception records are allocated through the runtime-core context allocator and
remain owned by the originating runtime until it is destroyed. This keeps
`exnref` and foreign identity tokens valid across rethrows. Their allocations
count against the context memory budget; exhausting it is a terminal,
uncatchable `GLWASM_ERR_LIMIT`, including when a guest repeatedly throws and
catches exceptions without retaining an `exnref`.

The runnable sample host invokes `examples/runaway.wasm` under the untrusted
policy and prints the pause location:

```bash
make -C libs/lang-wasm sample PREFIX="$PWD/.local-lang-wasm"
```

## Building

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/lang-wasm test PREFIX="$PWD/.local-lang-wasm"
```

Pass `PREFIX` on every make. pkg-config is how the library finds `cutil` and
`runtime-core`.
Once installed, a program links with
`pkg-config --cflags --libs ghoti.io-lang-wasm-0`.

Verify the installed consumer path with:

```bash
make -C libs/lang-wasm check-install PREFIX="$PWD/.local"
```

This checks pkg-config metadata, installed include and library paths, and a C
and C++ consumer that links and runs through the installed umbrella header.

## Compiler, runtime, and measurement status

The release acceptance compiler is GCC 16.2.0 in the pinned
`ghoti-build:gcc16` image. Native results are required from EVO-X2 x86-64 and
Raspberry Pi 4 arm64. The architecture matrix also includes the documented
qemu arm64 and Wine Win64 gates, with baseline/no-optional-feature and
supported-feature CPU masks where those gates apply. A compiler or runtime
refusal must be recorded as a limitation; it is not a passing skip. Follow
the [xarch guide](../../suite/tools/xarch/README.md) and
[Wine guide](../../suite/tools/xwin/README.md) for the cross-platform gate
environments.

The explicit safety targets are `test`, `test-valgrind`, `test-asan`,
`test-tsan`, `test-gc-torture`, `fuzz-replay`, `test-boundary-gc`,
`test-boundary-relocate`, `test-debug-relocate`, `test-real-corpus`,
`test-real-corpus-relocate`, and `check-install`. The optional measurement
targets are `benchmark-fixture-hashes`, `benchmark-metadata`, and
`benchmark`; they are not dependencies of the ordinary build or test targets.
The benchmark requires runtime-debug development files and explicit provenance
inputs. See [the benchmark protocol](benchmarks/README.md) for setup, exact
commands, raw artifact requirements, workload definitions, and summaries.

As of 2026-10-18, the five canonical Wasm module hashes match between X2 and
Pi. Native GCC 16 ordinary tests, GC torture, Valgrind, fuzz replay, and the
installed-consumer check passed on the committed implementation snapshot.
Wine Win64 and qemu AArch64 gates passed on earlier implementation snapshots;
their only subsequent executable-source differences are the Windows Tang test
adjustment and the pinned corpus checksum correction, respectively. The
corrected source-matched runtime-jit and Lang-Wasm ASan/UBSan and TSan gates
passed. Tang's ordinary and GC-torture gates passed after its planted-control
filter was narrowed to the tests that run under torture.
The corrected pinned real-program corpus passed in ordinary and relocating
heap modes on X2 and native Pi 4; the relocation arm ran all 18 cases with
matching native and Wasmtime vectors. The Story 13 measurement matrix records
the exact gate snapshots, skips, and the earlier superseded checksum failure.
The final-source Pi benchmark has 30 paired samples per tier and workload;
the four fully compiled workload medians favor JIT by 2.47x–14.33x.
The mixed-boundary hybrid median favors its interpreter comparison by 2.20x.
The quiet-host X2 benchmark used the same source manifest and 30 paired
samples per tier and workload. Its median speedups were 16.19x for numeric
loops, 3.65x for calls, 12.60x for linear memory, 3.63x for table/bulk, and
2.47x for the hybrid mixed boundary. Attached-idle debugger medians were
within 0.2% of the unattached interpreter medians on these X2 workloads.
Interpret performance per workload and consult the retained raw data,
provenance, first-call latency, and debugger polling distributions before
drawing broader conclusions. The mixed-boundary
`hybrid` workload measures a
compiled outer loop that calls an interpreted nested instance; it is not a
whole-workload JIT comparison. First-call latency and debugger polling
diagnostics are separate from steady-state timing.
