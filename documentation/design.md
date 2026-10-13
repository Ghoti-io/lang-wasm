# Design

`lang-wasm` decodes a WebAssembly 3.0 binary and then validates it. It does
not instantiate a module, resolve an import, or run an instruction.

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
| `GLWASM_ERR_CORRUPT` | The bytes are not a Wasm binary: bad magic or version, a truncated or over-long integer, a bad section size or order, non-UTF-8, a structural mismatch |
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
pages (4 GiB). An i64 memory's is 2^48 pages. An i32 table's is 2^32 elements.
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
- `eq`, `func`, `extern` and `exn` are subtypes of `any`.
- A concrete type index is a subtype of the supers its definition declares.
  Struct and array fields are compared there too: a mutable field is
  invariant, an immutable field is covariant.
- A non-null reference may flow to the nullable reference of a supertype. A
  nullable reference may not flow to a non-null one.

`ref.func` is in scope and produces a non-null `(ref func)`, which is a
subtype of `funcref`. Declaring that non-null type, or any other non-null or
type-index reference, in the type section is deferred and returns
`ERR_UNSUPPORTED`. Nullable `funcref`, `externref` and `exnref` are in scope.

`call_indirect` accepts a `funcref` table because `(ref null ft)` is a subtype
of `(ref null func)`.
