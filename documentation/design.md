# Design

**Status:** Stories 1–13 are implemented, measured, and accepted under the
documented feature scope. The final independent documentation review passed.
Story 11's pinned SQLite, zlib, and Lua corpus passed 12/12
ordinary cases and 18/18 relocation cases on EVO-X2 and native Pi arm64,
including the exact 12/18-case corpus selection guard. Story 13 has completed
cross-architecture gates, the corrected runtime-jit and Lang-Wasm sanitizer
gates, Tang torture, and source-matched 30-sample native Pi and quiet-host
EVO-X2 measurements. A final native Pi documentation-snapshot `make test`
passed 273/273 with zero skips.
Production Tang loader integration belongs to Tang Story 14, and the
cross-heap handle table belongs to Tang's host-extensibility Story 13.

`lang-wasm` decodes a WebAssembly 3.0 binary, validates it, then translates
validated functions for execution on runtime-core.

## Two passes

`glwasm_module_decode` reads the binary once into an opaque `GLWASM_Module`,
then `glwasm_module_validate` walks that module. The module handed to the
caller is the resolved form. The caller frees it with `glwasm_module_destroy`.

The split is what makes a deferred feature `GLWASM_ERR_UNSUPPORTED` rather
than `GLWASM_ERR_CORRUPT`. The decoder accepts every type the 3.0 grammar can
express, including a `v128` and a reference that names a type index, and
records that a deferred feature was used. If the rest of the module is
well-formed, decode returns `ERR_UNSUPPORTED` and destroys the module. If some
byte is not Wasm, `ERR_CORRUPT` wins even when a deferred feature was seen
earlier. Validation runs only when nothing was deferred.

A value type is a small descriptor (`GLWASM_ValType`): a kind, a nullability
bit, a heap type, and, for a concrete heap type, a type index. A `v128` is one
kind of that descriptor. Nothing in the decoder or the validator assumes a
value is at most 64 bits.

The validator is a stack machine with an explicit control stack. Block nesting
does not recurse in C. An unreachable path is polymorphic: a pop below the
frame's height yields bottom, and bottom matches any expected type.

`out_module` is written only on success. On failure it is left untouched and
everything the call allocated is freed. `GLWASM_Failure`, when the caller
passes one, is written on every return, including failure. Its `limit` is a
static field name when the result is `ERR_LIMIT`, and NULL otherwise.

## Errors

| Result | Meaning |
| --- | --- |
| `GLWASM_ERR_CORRUPT` | The bytes are not a Wasm binary: bad magic or version, a truncated integer, an integer longer than ceil(N/7) bytes for its N bits, or one whose unused final bits are not zero (unsigned) or a sign extension (signed), a bad section size or order, non-UTF-8, a structural mismatch |
| `GLWASM_ERR_UNSUPPORTED` | The grammar accepts it and this library does not implement it: a non-null or type-index reference, `v128`, a GC composite, the legacy `try` / `catch` / `rethrow` encodings, SIMD, atomics, the GC opcode prefix |
| `GLWASM_ERR_VALIDATE` | The module is well-formed and breaks a validation rule: a type mismatch, an index out of range, `min` greater than `max`, an alignment past the natural one |
| `GLWASM_ERR_LIMIT` | A `GLWASM_Limits` field. Checked before the allocation that would have satisfied it. `GLWASM_Failure.limit` names the field. `glwasm_result_string` stays "Limit exceeded" |
| `GLWASM_ERR_INVALID` | A bad caller argument: a NULL module pointer, or a NULL buffer with a non-zero length. An empty buffer is `ERR_CORRUPT` |

`GLWASM_ERR_VALIDATE` is appended immediately before `GLWASM_RESULT_COUNT`, so
the nine shared result values keep their numbers. That is the section 13
departure: the shared vocabulary has no code for "this is Wasm, and it fails a
rule." `ERR_INVALID` stays a bad caller argument. `ERR_FORMAT` stays a
different format.

Version must be 1. A different version is `ERR_CORRUPT`.

## Limits

`NULL` limits mean `glwasm_limits_default`. A field of zero is a cap of zero,
not a request for the default. The four numbered values are the untrusted row
of the host-policy note. The rest are the defaults this library uses until a
later story adds named presets. There is no feature switch.

| Field | Default | What it counts |
| --- | ---: | --- |
| `module_bytes` | 4 MiB | Bytes of the binary, checked before any allocation |
| `types` | 10,000 | Type-section entries |
| `imports` | 1,000 | Import-section entries |
| `exports` | 1,000 | Export-section entries |
| `functions` | 10,000 | Imported and defined functions |
| `globals` | 1,000 | Imported and defined globals |
| `tables` | 100 | Imported and defined tables |
| `memories` | 100 | Imported and defined memories |
| `tags` | 1,000 | Imported and defined tags |
| `data_segments` | 10,000 | Data segments |
| `element_segments` | 10,000 | Element segments |
| `locals` | 1,000 | Locals of one function, parameters included, and the parameter and result lists of one function type |
| `instructions` | 100,000 | Instructions of one function, `end` included, and the value and block-type pools that hold them |
| `nesting` | 128 | Control frames, the function frame included |
| `br_table` | 10,000 | `br_table` labels, the default included, and `try_table` catches |
| `name_length` | 1,024 | Bytes of one name |
| `table_size` | 1,000,000 | Elements of a table's minimum or effective maximum |
| `memory_maximum` | 64 MiB | Bytes of a memory's effective maximum |
| `memory_initial` | 64 MiB | Bytes of a memory's declared minimum |

A missing memory or table maximum is the address type's own maximum, and that
effective maximum is what the cap sees. An i32 memory's own maximum is 65,536
pages (4 GiB). An i64 memory's is 2^48 pages. An i32 table's is 2^32−1 elements, so a limit of 2^32 is `ERR_VALIDATE`.
An i64 table's is 2^64−1 elements. So `(memory 1)` with no maximum fails
`memory_maximum` under the default. The cap is checked first (`ERR_LIMIT`);
a minimum above the maximum, or a maximum past the address type's own
maximum, is `ERR_VALIDATE`. A memory size is pages times 65,536 bytes.

## Section order

Sections follow the 3.0 grammar, not the numeric order of their ids. Custom
sections (id 0) may appear anywhere. Each other id appears at most once, and
each must come after every section the grammar places before it:

type, import, function, table, memory, tag, global, export, start, element,
data count, code, data.

The data count section is before the code section and before the data section.
When it is present, the data section's count must equal it, and an absent data
section is an empty list, so the count must be zero. The code section's count
must equal the number of defined functions.

## The type lattice

Numeric types match only themselves. References use the 3.0 heap lattice:

- `none` is the bottom of `eq`, `i31`, `struct` and `array`.
- `nofunc` is the bottom of `func` and of a concrete function type.
- `noextern` is the bottom of `extern`. `noexn` is the bottom of `exn`.
- `eq` is a subtype of `any`. `func`, `extern` and `exn` are the tops of
  lattices of their own, and none of them is a subtype of `any`.
- A concrete type index is a subtype of the supers its definition declares.
  Struct and array fields are compared there too: a mutable field is
  invariant, an immutable field is covariant.
- A non-null reference may flow to the nullable reference of a supertype. A
  nullable reference may not flow to a non-null one.

The bottom heap types of the in-scope lattices, `nofunc`, `noextern` and
`noexn`, are in scope as nullable references (`ref.null nofunc`, a table of
`(ref null nofunc)`, a block result), and are checked as subtypes of `func`,
`extern` and `exn`. They are part of the `funcref`, `externref` and `exnref`
lattices that reference types and exception handling use, not of GC. The
`any` hierarchy's heap types (`any`, `eq`, `i31`, `struct`, `array`, `none`)
stay `ERR_UNSUPPORTED`.

`ref.func` is in scope and produces a non-null `(ref func)`, which is a
subtype of `funcref`. Declaring that non-null type, or any other non-null or
type-index reference, in the type section is deferred and returns
`ERR_UNSUPPORTED`. Nullable `funcref`, `externref` and `exnref` are in scope.

`call_indirect` accepts a `funcref` table because `(ref null ft)` is a subtype
of `(ref null func)`.

## Integers

Every LEB128 integer may be padded up to ceil(N/7) bytes for an N-bit value
(5 for a `u32`, `s32` or `s33`, 10 for a 64-bit value), so `0x80 0x80 0x00` is
a legal zero. An encoding longer than that is `ERR_CORRUPT`, and so is one
whose unused bits in the final byte are not zero (unsigned) or not a sign
extension of the value (signed). This holds at every read site. Tools that
patch in a size or an index commonly pad it.

## Constant expressions and declared functions

`ref.func x` in a function body needs `x` to be declared: it must occur in the
module outside function bodies and the start function (the specification's
`C.refs`). That is every function index in an element segment (either form,
its offset too), a global or table initializer, a data offset, and a function
export. The start function does not declare anything.

`global.get` in a constant expression reads an immutable global. A global's
initializer sees the imports and the globals defined before it. An element or
data offset sees every global. A table initializer sees the imports only,
because the table section precedes the global section, as V8 also requires.
A mutable global, a later global, or the global itself is `ERR_VALIDATE`.

## Mixed address types

`memory.copy` and `table.copy` may name an i32 and an i64 memory or table.
The destination and source operands have their own address types and the
length has the narrower one (i32 when either is i32). `table.copy` needs only
that the source element type be a subtype of the destination's. V8 12.4
rejects the mixed forms; the specification allows them and so does
lang-wasm. `tests/unit/test_types.cpp` pins it.
# Text syntax parser

`parse.h` exposes a reusable syntax tree for source text. Each node owns its
bytes and children through the caller's allocator (or the default allocator),
and carries a byte offset plus one-based line and column. Nested `(; ;)`
comments, line comments, lists, atoms and quoted byte strings are recognized,
including byte and Unicode escapes. `script.h` classifies top-level WAST
commands into typed command kinds and retains the original syntax node for
the spec-suite runner. Source must be valid UTF-8, byte escapes retain arbitrary
string bytes, and nesting deeper than 256 levels is rejected to bound stack
use. Command envelopes validate actions, constant arguments and assertion
values while leaving execution to the runtime adapter. `glwasm_wat_module_parse`
lowers a module form to canonical binary sections, then reuses the decoder and
validator, so text and binary modules meet one set of checks. The lowering
covers the in-scope grammar the pinned corpus uses (the spec-suite gate lowers
every in-scope module of it) and rejects unsupported fields and instructions
explicitly. Generated-binary validation errors point to the module form;
syntax and lowering errors point to their source form.
Story 5 adds interpreter dispatch for `throw`, `throw_ref`, and `try_table`,
including tag identity, payload/ref transfer, nested propagation, and limit
bypass. The spec-suite gate below runs those assertions in both tiers.

## Pinned WAST corpus gate

The spec-suite input is pinned in the adjacent `runtime-jit` repository at
the immutable commit in `tools/wasm/WASM_SPEC_COMMIT`. Its fetch script reads
`WAST_FILES`, checks each fetched file against `SHA256SUMS`, and retains the
existing ten scalar numeric inputs used by runtime-jit's execution harness.
`tests/data-wast-classification.tsv` assigns every `.wast` file either `run`
or `skip(reason)`, with skip reasons limited to the feature-scope vocabulary.
The corpus checker requires each file-level skip to have a matching deferred
feature marker or suite path. Mixed files stay `run`; individual deferred
module commands or assertions are listed in
`tests/data-wast-assertion-skips.tsv` and are counted by reason.
`make test` checks the exact fetched file set, all digests and the exhaustive
classification before it parses the corpus through the public WAST command
parser. `testSpec_suite` then runs every in-scope command of every `run` file
in both tiers, each script in one store with `spectest` registered first, and
fails on any command that does not pass, so nothing in scope is pending.

The JIT tier is audited per instance as it runs. Every compile refusal and
every opcode exit must match a row of `tests/data-wast-jit-rules.tsv`; a JIT
tier that silently interpreted would otherwise pass the same commands. The
rules, and why each remains:

- **Calls that leave the instance** (`call`, `call_indirect`, `return_call`).
  Compiled code serves every instance of its module, so a callee in another
  instance, an imported callee of `return_call`, or a call at the native
  stack budget is made by the interpreter, which owns cross-instance frames
  and continues a deep recursion on its own stack.
- **Externref and typed references** (`ref.null` of those types, and a
  whole-function refusal, opcode `ffff`, for a signature or local of those
  types or over 16 parameters or results). Externref tokens may be host-heap
  handles, so compiling them needs a reference representation in the
  compiled frame state; that is deferred work, not a decision.
- **Retirement by exit rate.** A function whose recent entries keep exiting
  is retired to the interpreter. It is allowed only because each exit behind
  it is checked against its own row.

## Translation and source locations

The decoder records the absolute module byte offset before consuming every
instruction opcode. This is the canonical lowered binary's coordinate space
for text modules, so binary and WAT-built modules share one convention. After
validation, translation copies each function's instructions into module-owned
storage, pairs every structured opener with its matching `end` and optional
`else`, and resolves each branch depth to an instruction boundary. Loop
branches target the first instruction in the loop body; block and function
branches target the first instruction after the matching `end`. `br_table`
destinations are stored in a per-function side array. A parallel offset array
maps every translated position, plus its synthetic terminal boundary, to a
nondecreasing in-range opcode byte offset.

## Debugger identity and inspection (Story 7)

Wasm has no source-line table, so its runtime-core location has the module name
and line zero. The frame retains the full poll identity: the function word
packs a process-unique module id and Wasm function index, and the offset word
is the absolute module byte offset. The identity also carries runtime-core's
context-local Wasm engine id, preventing a Tang program/function pair with the
same numeric words from sharing breakpoints or verification. `runtime-debug`
instruction breakpoints and steps compare the engine, both 64-bit words, and
do not narrow an offset into the line field. DAP references include engine,
function, and offset and are validated only by the named engine; terminal
resume boundaries are not executable instructions.

The decoder keeps bounded module, function, and local names from the optional
`name` custom section. Malformed optional name metadata is ignored without
invalidating an otherwise valid module; complete earlier subsections remain
available, entries from the malformed subsection are rolled back and that
subsection plus the remainder are discarded, and the first duplicate name
wins. The
runtime copies module/function display names and translated offset maps into a
context-owned registry so debugger snapshots never borrow module storage.
External contexts count bound runtimes and remove the registry entry when its
last runtime is released; the runtime's own context destroys its registry with
the context. Local names remain module-owned, and missing names have stable
numeric fallbacks. The engine descriptor's scoped location, name, and
instruction-validation callbacks check the context-local engine id, so copied
descriptors cannot claim another engine's function identity.
An adopted context must outlive its binding; callers unbind on the context's
owner thread while it is parked before destroying either object. Shared debug
metadata remains registered until the final runtime unbinds or is destroyed.

The embedder may borrow the runtime-core context from a runtime for debugger
attachment. The borrow ends when the runtime is destroyed and can be read only
on its owner thread while runtime-core says the context is readable. The
descriptor exposes locals (including parameters), operand-stack slots, all
globals, and every memory through read-only callbacks. Memory reads acquire a
short provider lease, check the current range, copy, and release it before
returning. Debugger snapshots and views remain valid only for the current stop.
Every interpreted instruction boundary is a poll site so instruction
breakpoints and stepping can stop precisely. A resumed poll is skipped once;
its opcode fuel was already charged before the pause and is not charged twice.
The DAP stack trace publishes the exact function/offset reference; instruction
breakpoints accept that reference, and a per-stop `Linear memories` scope
provides references for the bounded `readMemory` request. Those references
expire with the stop.
Story 10 supplies the compiled tier and verifies that it reports the same
module/function/offset identity and frame data.

## Runtime and fuel

The following paragraph records the original Story 3 interpreter baseline; its
limited feature inventory is historical and is superseded by the Story 4/6
surface below.

The internal Story 3 interpreter runs as one runtime-core engine activation
with guest calls represented by persistent guest-stack frames. The dispatch
loop is non-recursive in C, reacquires frame slots after operations that may
move the stack, and replaces the active frame for tail calls. It currently
executes scalar numeric operations, locals, globals, structured control flow,
direct calls, indirect calls through initialized function tables, and tail
calls. Active element segments support both function-index and reference
expressions for `ref.func` and `ref.null`.

The engine polls at every instruction boundary and charges one fuel unit per
executed opcode from `glwasm_opcode_cost`. Guest traps return
`GLWASM_ERR_GUEST`; the runtime retains distinct kinds for unreachable,
integer divide-by-zero/overflow, table bounds, uninitialized elements, and
indirect-call type mismatch. Pause/resume, fuel accounting, guest-depth
limits, and always-move stack behavior have focused regression tests.
The current test-only fuel/depth override installs a fresh runtime-core
context after instantiation, so start-function fuel is not charged to a later
test invocation's budget. The corpus preflight also conservatively follows
every active target in the table selected by `call_indirect`; an unsupported
target can defer an assertion even when its test input cannot select that
table slot. Eligibility scans each reachable function's entire instruction
body, including statically unreachable instructions, so an unsupported opcode
in dead code can also defer that assertion. Active element expressions other
than one `ref.func` or `ref.null` are rejected as unsupported during runtime
creation and counted as deferred by the corpus harness. Runtime instances and
snapshots retain their resolved module; releasing the caller's handle does not
invalidate them.

The following WAST paragraph records the Story 3 baseline and is superseded by
the Story 4/6 runtime and public embedding implementation below. It remains as
history for the original corpus triage.

At the original Story 3 baseline, the WAST executor covered i32 `eqz` and comparisons plus selected integer
arithmetic/bitwise operators, selected i64 arithmetic/bitwise operators,
f32/f64 arithmetic, locals/globals, structured branches including `br_table`,
direct and indirect calls, tail calls, module start functions, and active
table initialization with one `ref.func` or `ref.null` per element. Table
growth and general table operations are not implemented. Runtime traps cover
`unreachable`, i32 division by zero or overflow, table bounds,
uninitialized elements, and indirect-call type mismatch. Other MVP
numeric operators and conversions and unsupported multi-result/value forms
were pending at the Story 3 baseline; the current interpreter covers validated
scalar numerics and multi-value result propagation as described below.
The harness executes supported `assert_return` and `assert_trap` commands. Integer values are type-checked, finite f32/f64 results compare bit-exactly, and canonical, arithmetic, and payload NaN forms use their corresponding WAST matching rules. Executed assertions fail on mismatches or unexpected traps. It is a test-only internal adapter,
not the public embedder/import API. Its limited memory, table, and import
coverage described the Story 3 baseline only; Stories 4, 6, and 12 below record
the current implementation status.

## Stories 4, 6, and 12: instance and host boundaries

Runtime state now belongs to each instance: memory32/64, table32/64, globals,
active data/element initialization, and dropped-segment state. Linear memory
and table operations use explicit overflow and bounds checks; bulk operations
retain progress across polls. Runtime allocations use the context allocator,
and memory growth follows the context budget and allocator refusal path.

The public host API links exact module/field grants for functions and typed
resources. Policies are copied presets and can disable features or set context
budget defaults before decode or execution. Host memory/table providers grant
temporary leases which end before polls and growth. Callback pauses preserve an
explicit resume entrypoint, so earlier callback effects are not repeated.
Snapshots preserve the paused guest execution plus globals, memories, tables,
and segment-drop state; imported resources, reference-bearing modules, and
pending host continuations are rejected explicitly until their owners provide
snapshot hooks.

The mixed-engine boundary API stores engine identity, an opaque instance token,
and foreign exception origin/identity in runtime-core activation state. It does
not store or interpret heap addresses. A test-only provider fixture exercises
token checking, temporary rooting, promotion, and relocation. The production
cross-heap handle table belongs to Story 13 of `spec-tang-host-extensibility`;
Lang-Wasm Story 13 covers benchmarks and documentation.

Uncaught Wasm exceptions crossing an adopted-context boundary receive a
process-unique opaque identity token. Runtime-core records the Wasm engine and
token on the live boundary activation; `glwasm_runtime_last_foreign_exception`
returns the same pair to the caller after the nested call reports
`GLWASM_ERR_GUEST`. The originating runtime retains the exception until it is
destroyed, so the identity can be passed back as an `exnref` to that same
runtime and rethrown without exposing a heap address. Runtime limits remain
separate: a limit unwind does not publish a foreign exception.
Exception records use the context's counting allocator and stay owned by the
runtime until destruction so every `exnref` and exported foreign identity
remains valid. The context memory budget therefore also bounds this retained
state; exhaustion produces an uncatchable `GLWASM_ERR_LIMIT`, even for a loop
that throws and catches without keeping an exception reference.

Resumable pause eligibility is scoped to the live boundary. Runtime-core drops
the enclosing nested native activation's resumable contribution when that
boundary activation is removed, whether by normal leave or unwind, so later
polls in the still-running native callback cannot pause without a continuation
to resume. Runtime-core boundary tests cover both removal paths; the Story 12
tests cover pause/resume while the boundary remains live.
An already-paused context is rejected before a nested call changes its
boundary or guest frames; re-entry from a host callback parked inside an
active guest call remains supported. Runtime-core defines `GRCORE_Outcome`
only when its run returns `GRCORE_OK`. Lang-Wasm initializes outcome storage
and reports a pause only when core execution and the guest run state are
successful. A limit unwind therefore remains `GLWASM_ERR_LIMIT` on every
target, including Win64, even if the prior call had paused.

## Story 10: baseline JIT rejected alternatives

The baseline compiler consumes the validated module-owned translated
instruction form, including its precomputed structured-control destinations
and source-byte offsets. Lowering raw decoded instructions independently was
considered, but rejected because it would duplicate control-flow resolution
and could make interpreter, debugger, and compiled execution disagree about
branch targets or guest locations. Rewriting the decoded instruction array
in place was also rejected: that array is the canonical decoded view, and
mutating it would make validation/debug inspection depend on whether a JIT
pass had already run. The separate translated form is immutable for the
module's lifetime and shared by all instances.

The module policy fixes the JIT CPU feature mask before the module cache is
created. Presets disable every optional feature, producing portable baseline
code; a caller may set `jit_disabled_cpu_features` to
`GRJIT_FEATURES_NONE` to use the host's detected features. Unknown mask bits
are rejected during policy decoding. The cache key and compiler read the same
immutable module value, and runtime-jit intersects the requested set with
host detection, so a policy cannot enable an absent CPU feature. A decoded
module cannot change masks after code is cached; a different mask requires a
new decoded module.

Compiled memory loads and stores call the runtime's checked memory helper,
which validates address arithmetic and the current memory/provider range for
each access. Emitting native pointer loads or stores based only on module
validation was rejected because guest addresses and current memory length are
dynamic, `memory.grow` can change the valid range, and external providers may
relocate their backing storage. A native out-of-range access would turn a
defined Wasm trap into process memory corruption or a host fault. Any future
inline fast path must preserve overflow-safe range checks and the provider's
lease/lifetime rules before bypassing this helper.

Direct and table-selected calls use the runtime-core registered entry slot,
not a raw JIT code pointer. The call push/pop hooks create and release a Wasm
guest frame and lease a spill area at the current activation depth. This keeps
recursive calls and host re-entry from overwriting a caller's locals or operand
stack. A compile-on-call refusal deopts at the call's pre-effect state. If a
callee reaches an operation that is not lowered, its named exit rebuilds the
whole compiled chain; the interpreter resumes in the callee and retains the
caller's post-argument, post-call continuation. Exceptions and traps therefore
retain their guest identity across reconstructed frames. Supported
`try_table` paths use checked compiled catch edges, while unmatched guest
exceptions propagate to the next guest handler or host boundary. Poll and
limit exits preserve the resumable frame state; a runtime limit unwind is
never delivered to a Wasm catch as a guest exception.

Numeric host imports run through a runtime-core GC-point helper. It retains
the pre-call frame map while the callback runs and carries only numeric Wasm
arguments in activation-owned spill storage. A successful callback continues
compiled execution. A pausing callback records its continuation and rebuilds
the pre-call frame so resume invokes the continuation once. Other callback
errors rebuild the post-call frame with the error recorded, preventing replay
of a completed host effect. Stable function-reference tokens can also cross a
direct host call. Externref and typed-reference signatures keep the exact
pre-effect interpreter exit.

The baseline lowerer also treats nullable `funcref` values as stable
store-address tokens. `ref.null`, `ref.func`, and `ref.is_null` therefore
stay compiled without exposing moving heap objects to generated code; externref
and typed references still exit before their instruction. Table and memory
operations that touch no reference held in compiled code run through
GC-point helpers that the interpreter's own primitives back: `table.get`,
`table.set`, `table.grow`, `table.size`, `memory.size`, `data.drop`,
`elem.drop`, `memory.init`/`memory.copy`/`memory.fill` (one 4096-byte chunk
per call, with a poll between chunks) and `table.init`/`table.copy`/
`table.fill` (one call of at most 1024 elements). Helpers validate every
range and reference before mutation and report a trap without an effect;
the interpreter then raises it from the pre-effect state. Imported storage,
whose begin/end callbacks belong to the embedder, a table operation over 1024
elements, and a reference the helper cannot vouch for take an opcode-counted
pre-effect exit instead.

Trapping truncations (`i32.trunc_f32_s` and the other seven) compile as a
range check and the saturating conversion, which agree on every input inside
the range. Each range is open or half-open on bounds exact in both float
widths, and ordered comparisons reject NaN, so NaN and the first input past
either bound take the trap exit. `unreachable` takes it unconditionally.
Trap exits are not opcode exits: they describe guest semantics, so they
count toward neither the spec-suite rules nor retirement.

Numeric and nullable-funcref global reads and writes also remain compiled.
Generated code calls helpers with the active runtime instance and a global
index; the shared artifact never captures an instance pointer. Imported
mutable globals are refreshed from their host slot on every read and written
back after validation, so compiled and interpreted accesses observe the same
value. Reference globals whose validity requires a host token callback remain
on the stable compile-refusal path until the JIT can represent and root those
references safely.

The baseline lowerer emits f32/f64 arithmetic, min/max/copysign, unary
absolute/negate/sqrt/rounding operations, all six comparison families,
integer-to-float conversion, promote/demote, reinterpret, and saturating
float-to-integer conversion. Float inequality retains Wasm's unordered NaN
result. Trapping float-to-integer conversions use named pre-instruction exits
so the interpreter produces the exact invalid-conversion or overflow trap.
Signed and unsigned integer division/remainder execute in compiled code after
the IR verifier's required guards. Zero divisors and signed `MIN / -1` branch
to the named pre-instruction exit, where the interpreter produces the
corresponding Wasm trap.

The interpreter implements all scalar numeric instruction families accepted
by the validator, including trapping and saturating float-to-integer
conversions. Public `*_results` invocation APIs carry result vectors through
direct and tail calls, host imports, pause/resume, nested boundaries, and
snapshots. Existing scalar entrypoints remain available and return
`GLWASM_ERR_UNSUPPORTED` for signatures with more than one result. Multi-value
host imports use `function_results` and `resume_results`; `GLWASM_HostCall`
provides the result count and types.

The compiler and callable entry path return multiple typed results for
straight-line functions and direct/indirect calls within runtime-jit's
16-argument/16-result callable limit. Direct and indirect tail calls use the
registered code entry and replace the active guest frame through the tail hook.
Calls above the IR value limit take a named pre-effect exit; the interpreter
then completes the call and returns the full result slice. Unsupported
structured multi-value joins take a named pre-effect exit. Supported
`try_table` landing edges execute in compiled code, including matched catches
and propagation of unmatched exceptions; a limit unwind crosses those frames
without becoming a catchable Wasm exception. AUTO mode may continue in the
interpreter after a named exit, while REQUIRE mode reports it through the test
counters.

Executable code and its cache are owned by the decoded module, with per-
instance runtime entry slots and registrations referring to that shared
artifact. Per-instance code
ownership was considered, but rejected because it would compile duplicate
copies for instances of the same module and defeat the module cache. Context-
owned code was also rejected: two contexts can share a module, so either
context could destroy code still installed in the other. Module ownership
gives the artifact one clear lifetime. Each runtime unregisters its own entry
mapping and registration before releasing its binding. When two instances share a context and
the same code range, runtime-core counts their exact duplicate registrations;
the final unregister retires the range. Active runtime-core references keep
in-flight frames alive while retirement completes.

Each decoded module starts with one caller reference. Runtime creation retains
the module and runtime destruction releases that reference after it has
finished clearing context slots and instance state. Snapshots retain it until
snapshot release as well. `glwasm_module_destroy` releases the caller
reference; the module cache and executable-page group are physically destroyed
only after the caller, all runtimes, and all snapshots release theirs. Callers
must synchronize a release against operations that are beginning to use the
module pointer.

Compiled helper callbacks use static-lifetime function pointers and resolve
the runtime and mutable instance state from the active context/environment on
each invocation. The code token identifies the decoded module and function;
it is not a runtime pointer. Capturing a `GLWASM_Runtime *` or instance
address in cached executable code was rejected because the artifact is shared
across instances and contexts, and could dispatch through a destroyed or
unrelated instance. Runtime-core owns activation and code references for the
duration of a call; an instance's context-local slot is removed during
unbinding, while the module-owned artifact is retired only after no active
frame can call it. Callback state that must survive a pause belongs in the
runtime's resumable activation state, not in a transient C callback stack.

## Story 11: real-program corpus

The real-program gate is an explicit `make test-real-corpus` target so the
engine's ordinary test suite stays offline and does not require third-party
build tools. Before the gate, `tools/real-corpus/fetch-sources.sh` fetches
immutable upstream archives and verifies each archive checksum, then records
and rechecks an extracted-tree digest. The gate itself never fetches. Changes
to a source tree after extraction fail closed. Both scripts use
`GLWASM_REAL_CORPUS_BUILD_DIR` when set, defaulting to `build/real-corpus`.
On EVO, point it to writable scratch, fetch there before the offline gate, and
keep that path for the candidate and full runs:

```sh
export GLWASM_REAL_CORPUS_BUILD_DIR=/scratch/lang-wasm-real-corpus
tools/real-corpus/fetch-sources.sh
tools/real-corpus/build-and-run.sh --propose-pins
```

Once reviewed candidate pins are recorded, `make test-real-corpus` uses the
same writable directory for the full corpus gate. Its test executable,
objects, and generated headers are also placed below
`$GLWASM_REAL_CORPUS_BUILD_DIR/lang-wasm-build/` for read-only checkouts.

The three drivers exercise SQLite 3.54.1's `:memory:` database, zlib 1.3.2's
in-memory compression and round trip, and an embedded Lua 5.5.1 script. Each
reports status and exposes its complete canonical output as bounded byte
chunks. The vectors encode every selected SQLite row and column, all 4096 zlib
round-trip bytes, and both Lua return values in wrapped hex files referenced by
`vectors.tsv`. EVO-X2 and Pi must confirm them. `vectors.sha256` pins the TSV
and all output files. `licenses.tsv` pins the preserved upstream notices, and
the test verifies those bytes against the fetched source trees. The upstream
amalgamation and libraries remain unmodified; Ghoti-owned drivers sit beside
the license copies.

Builds use GCC 16.2.0 for native oracles and wasi-sdk 34 for no-entry reactor
modules. Their architecture-specific archive checksums, plus Wasmtime 49.0.3,
are recorded in `tests/real-corpus/toolchains.tsv` and verified by the suite
image installer. The installer verifies the extracted `bin/clang` bytes
against their member in the checksum-verified SDK archive and records the
archive-derived hash and exact version/target identity. It also records the
`wasm-ld` binary and version, the complete WASI sysroot tree, and Clang's
compiler-runtime resource tree. Every gate and candidate-generation run
rechecks those link inputs before compilation, along with the installed Clang
binary hash, version string, and `wasm32-unknown-wasip1` target. Sysroot and compiler
resource digests include resolved internal symlink targets; dangling, cyclic,
or escaping links fail closed.
The Wasmtime CLI invokes the status and byte-output exports.
A typed binary preflight checks production modules against the exact reviewed
`imports.tsv` manifest and checks the independent oracle modules against a
separate `oracle-imports.tsv` manifest. SQLite and zlib production modules have
no imports. Lua has seven typed wasi-sdk libc imports; the test host resolves
them only to callbacks that fail if invoked. The oracle's SQLite and zlib
modules each use only the typed `fd_write` import for one-call bounded output.
Module SHA-256 values are recorded in `module-hashes.tsv` after independent
EVO-X2 and Pi builds have been reviewed, with separate rows for x86_64 and
aarch64. Both architectures must match the same semantic vectors; byte
identity across distinct wasi-sdk distributions is not required.

Run `tools/real-corpus/build-and-run.sh --check-pins` to verify local pins,
`--propose-pins` to write candidate module hashes and full native/Wasmtime
vectors under the configured scratch directory, or `--all-tiers` to compare
the independent results with pinned vectors. `make test-real-corpus` adds
Lang-Wasm interpreter and JIT execution
of each module. The final gate requires the pinned native image and toolchain
on EVO-X2 and arm64; no timing result is treated as a correctness oracle.

On 2026-10-17, normal `--all-tiers` gates passed on EVO-X2
(`xgates-lang-wasm-story11-final-x2-20261017-1402`) and the Pi
(`xgates-lang-wasm-story11-final-pi-20261017-1402`). Both used the
`ghoti-build:gcc16` image, GCC 16.2.0, wasi-sdk 34.0 and Wasmtime 49.0.3;
both verified the exact production import counts (0/0/7 for SQLite/zlib/Lua),
oracle import counts (1/1/7), and native/Wasmtime byte equality for all three
pinned vectors. The records were status 1 with lengths 74, 4096 and 182 bytes,
respectively. These runs do not replace the separate `make test-real-corpus`
interpreter/JIT, torture-heap, or relocation checks.

The C++ corpus gate also attaches a test-only runtime-heap to the Wasm
runtime-core context. Its GC-torture cases explicitly enable torture and
always-move stack mode, then require nonzero collection and stack-move
counters. `make test-real-corpus-relocate` runs both interpreter and JIT
workloads with the same torture settings plus runtime-heap relocation; it
fails unless the linked heap reports relocation support and a relocation
collection. Build runtime-heap with `RELOCATE=yes` into a private prefix and
put that prefix first on `PKG_CONFIG_PATH` to run this gate. Runtime-heap is
test-only here; production `lang-wasm` depends on `runtime-jit` for its
module-shared compiled tier, in addition to `cutil` and `runtime-core`.
The corpus executable requires every registered case to be selected and
passed (12 ordinary, 18 relocation) with no skips. Both Makefile targets
first prove that a planted inherited `GTEST_FILTER=-*` returns failure, so
an empty or partial Google Test selection cannot satisfy the release gate.

## Story 13: benchmark and evidence decisions

The benchmark accepts WAT only as the reviewable fixture source. It lowers
each fixture once through the in-tree canonical lowerer, validates the result,
hashes the resulting Wasm bytes, and uses those exact bytes for every sample.
The alternative was to invoke an external WAT compiler such as WABT during
each run. That would add a separate release and architecture-specific toolchain
to an otherwise self-contained benchmark and could silently change binaries
between environments. The in-tree conversion is pinned by its source commit
and environment manifest; generated module hashes are checked in before a
measurement pass. A second alternative was to hash only WAT text. That was
rejected because text identity does not prove the executed binary identity.

Fuel and policy are explicit benchmark inputs. Each sample decodes the pinned
binary with the selected policy preset and the configured fuel value. A fixed
hard-coded fuel field was rejected because it could disagree with the runtime
limit and misstate the measured workload. The canonical input manifest is
stored in each CSV row and hashed so the summarizer can verify the policy,
fuel, iteration count, and expected operation count.

The cold-compile distribution uses a fresh decoded module for each sample
because compiled code is module-owned. Warmup and steady-state invocations
share one module and instance so the timed invocation observes a warmed cache.
Combining compile and steady-state timing was rejected because it mixes two
different costs. Reusing only the runtime for a cold sample was also rejected
because that would reuse the module cache and would not measure cold
compilation.

JIT evidence is taken from per-function entry and exit counters. Each warmed
sample requires two compiled returns during warmup and one during its timed
invocation, and has zero side exits. Counting a JIT wrapper entry alone was
rejected because it can side-exit into interpreter execution and falsely appear
to measure compiled guest work. Failed samples are retained with nonzero exit
status and an error message; aborting without a row was rejected because it
hides where a run stopped and can make a partial dataset appear complete.

Interpreter, JIT, and attached-idle debugger samples use six balanced
permutations. Running all samples for one tier before the next was rejected
because clock, thermal, or host-load drift could bias the comparison. The
workloads rotate order across samples for the same reason; measuring each
workload in one fixed block would confound workload identity with run position.
The debugger diagnostic uses runtime-core's actual stopped identity. It verifies
the first breakpoint site matches the decoded module, function, and engine,
then verifies the step stop advances within that function; assuming a stop
from the debugger reason alone was rejected because it does not prove the
requested instruction ran.

Raw rows embed the exact command and environment-manifest contents, plus the
manifest path and hash. Recording argv alone was rejected because it omits
environment values that affect compilation and execution. The summarizer
recomputes the input and environment-manifest hashes, validates each workload's
work-unit name and count, checks the exact required paired matrix, rejects
unstable outputs and failed samples, and reports nearest-rank distributions.
Manual inspection or a summary-only artifact was rejected because neither
preserves enough information to reproduce and audit individual measurements.

### Accepted runtime architecture and alternatives

The interpreter and JIT consume one validated, immutable translated form.
Translation owns its resolved branch destinations and source offsets for the
decoded module's lifetime. Explicit overflow-safe bounds checks run at every
compiled memory access against the current provider range; module validation
alone cannot establish a runtime address. Compiled bulk copy/fill validates
the full range before mutation and polls in the interpreter's 4096-byte
chunks, retaining continuation state across pauses. Supported `try_table`
handlers use checked compiled catch edges; unmatched guest exceptions
propagate through guest frames. Unsupported instructions leave compiled code
from a pre-effect frame state. Traps preserve guest semantics, and runtime
limit unwind bypasses Wasm catches.

Executable code and its cache belong to the decoded module and are shared by
its instances. Each instance/runtime owns its entry slot and code registration;
runtime-core counts exact duplicate context/code-range registrations, and the
last owner unregisters the range. Generated helper callbacks have static
lifetime and resolve instance state from the active context; cached code never
captures a runtime or instance address. Work that
must survive a callback pause lives in the resumable activation. The debugger
uses the runtime's actual instruction-boundary poll identity, and polling
diagnostics separately measure no debugger, attached-idle debugger, and
stepping/breakpoint work. These choices and the rejected alternatives are
described with their consequences in Story 10 above and in the benchmark
protocol; duplicating translation, eliding dynamic bounds checks, assigning
code to a context or instance, capturing transient callback state, and timing
debugger setup with steady-state execution are rejected.

### Compiler compatibility, limitations, and measured results

The required native compiler is GCC 16.2.0 in the pinned `ghoti-build:gcc16`
image. The release matrix calls for EVO-X2 x86-64 and Raspberry Pi 4 arm64,
plus qemu arm64 and Wine Win64 gates from the suite's documented images. CPU
feature masks must cover the baseline and supported feature sets where
applicable. A refusal or unsupported gate is recorded against the architecture
contract rather than skipped. Benchmark setup, exact metadata, raw artifacts,
and command lines are specified in `benchmarks/README.md`; the benchmark is
opt-in and does not make the ordinary build network-dependent.

As of 2026-10-18, the five exact Wasm fixture hashes agree between X2 and
Pi source snapshots. The documented implementation snapshots passed ordinary
suites on GCC 16 X2 and native Pi 4, the complete Wine Win64 and qemu AArch64
cross gates, Valgrind, GC torture, fuzz replay, and installed-consumer checks.
The matrix identifies each snapshot and its subsequent source differences.
The corrected pinned real-program corpus passed on both machines in ordinary
and relocating-heap modes, including all 18 relocation cases and matching
native and Wasmtime vectors. The Story 13 measurement matrix records the
source snapshots, image identities, named skips, gate ledgers, and retained
logs; an earlier aggregate corpus failure caused by a stale Makefile checksum
is recorded there and superseded by the corrected-source corpus jobs.

The final-source Pi run contains 30 paired samples per tier and workload.
Its median interpreter to compiled speedups were 14.33x for numeric loops,
2.47x for calls, 10.27x for linear memory, and 2.83x for table/bulk; the
mixed-boundary hybrid was 2.20x faster than its interpreter comparison. The
attached-idle debugger's median cost ranged by workload, including an
observed 8.9% increase for calls. The quiet-host X2 run used the same source
manifest and 30 paired samples per tier and workload. Its median interpreter
to compiled speedups were 16.19x for numeric loops, 3.65x for calls, 12.60x
for linear memory, and 3.63x for table/bulk; the mixed-boundary hybrid was
2.47x faster than its interpreter comparison. Attached-idle debugger medians
were within 0.2% of unattached interpreter medians on these X2 workloads.
Raw data, min/median/p95/max, first-call and poll-event measurements,
commands, and provenance are retained in the measurement bundles cited by
the matrix. The `mixed-boundary` tier is hybrid: the outer loop is compiled
and its nested instance is interpreted. These measurements cover the listed
fixtures and do not establish performance for every Wasm module or callback
pattern.
