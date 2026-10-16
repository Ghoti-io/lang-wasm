# Ghoti.io Lang-wasm

A WebAssembly 3.0 decoder, validator, translator and interpreter foundation
(`GLWASM`). Binary decoding and validation remain separate passes. It is part of the
[Ghoti.io suite](https://github.com/Ghoti-io/suite).

The design, including the limit defaults and the type lattice, is in
[documentation/design.md](documentation/design.md).

## Status

**Interpreter core in progress.** Decode and validation of the in-scope feature set are in
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
monotonic offset map including its terminal boundary. Executable coverage is
provided by an internal runtime-core-backed interpreter used by the Story 3
test adapter. Its current executable subset covers i32 `eqz`, `eq`, `ne`, signed and
unsigned `lt`, `gt`, `le`, and `ge`; i32 `add`, `sub`, `mul`, signed/unsigned
`div`, `and`, `or`, `xor`, and shifts; i64 `add`, `sub`, `mul`, `and`, `or`,
and `xor`; and f32/f64 `add`, `sub`, `mul`, `div`, `min`, `max`, and
`copysign`. It also covers locals/globals, structured control flow including
`br_table`, direct calls, `call_indirect`, tail calls, module start functions,
and these trap cases: `unreachable`, i32 division by zero or overflow, table
bounds failures, uninitialized elements, and indirect-call type mismatches.
Other MVP numeric operators and conversions,
unsupported multi-result/value forms, and later-story instructions remain
pending. Guest calls use persistent runtime-core frames rather than C
recursion; traps carry Wasm trap kinds, and polls, fuel, guest-depth limits,
pause/resume, and moving-stack behavior have focused tests. Fuel costs one
unit per Wasm opcode using the cost table shared with the later JIT. Runtime
table initialization supports active `ref.func` and `ref.null` elements;
table growth, imported tables, and other table operations remain pending. This
is not yet a public embedder or import API. Unsupported later-story instructions
remain pending, never counted as passes. This library depends on `cutil` and
`runtime-core` through pkg-config.

The WAST gate pins its supported execution count and deferred standalone
invocation count in `tests/data-wast-execution-baseline.tsv`, keyed to the
fetched spec commit. A changed count requires updating that baseline and the
Story 3 review record. Its summary reports executed assertions, assertions
pending later-story or unsupported behavior, and standalone invocations
deferred because runtime creation or execution is not supported for that case.
The baseline covers the executed and deferred-invocation counts; pending
assertions remain visible as a scope measure and are not counted as passes.
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
function must have an exact module-and-field entry in its allowlist. The
versioned `tang.v1` namespace is not granted implicitly. Destroy instances
before their borrowed module, and resume a paused call with
`glwasm_runtime_resume`.

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
Tang loader integration and cross-heap handle rooting remain owned by Tang
Story 14 and runtime-core Story 13, respectively.

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
