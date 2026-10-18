# Real-program corpus inputs

This directory contains only Ghoti-owned drivers, golden vectors, and the
source pin manifest. Upstream trees are fetched into the ignored build tree by
`tools/real-corpus/fetch-sources.sh`; the fetch verifies the immutable source
identity before extracting anything. The test gate never fetches sources. Both
commands use `GLWASM_REAL_CORPUS_BUILD_DIR` when set, defaulting to
`build/real-corpus`; set it to writable scratch when the checkout is read-only.
For EVO, export the scratch path, fetch the sources once, then generate
candidate pins:

```sh
export GLWASM_REAL_CORPUS_BUILD_DIR=/scratch/lang-wasm-real-corpus
tools/real-corpus/fetch-sources.sh
tools/real-corpus/build-and-run.sh --propose-pins
```

After reviewing and recording the candidate vectors and architecture-specific
module hashes, run `make test-real-corpus` with that same environment variable.
The Makefile also places its compiled test executable, objects, and generated
headers under `$GLWASM_REAL_CORPUS_BUILD_DIR/lang-wasm-build/` so the complete
target can run with a read-only checkout.

The suite image installer validates each wasi-sdk archive checksum, compares
the extracted `bin/clang` bytes with that exact archive member, and stores the
derived binary hash, exact first-line version identity, and default target in
the SDK. It also derives markers for `wasm-ld`, the full WASI sysroot, and
Clang's compiler-runtime resource tree from that verified archive. Every gate
and candidate-generation run rechecks those linker inputs and requires
`wasm32-unknown-wasip1`; the `wasi-clang` rows in `toolchains.tsv` bind the checks to
the same archive hash as the `wasi-sdk` rows. Measured-tree symlinks must
resolve internally without cycles; their resolved target paths and contents
are included, and dangling or escaping links fail closed. The pinned upstream
source trees reject all symlinks, including a regular file replaced with an
external symlink to identical bytes; the corpus preflight exercises this
substitution before accepting any source tree. The image also
verifies the Wasmtime 49.0.3 CLI and C API archives against their official
release digests. It records the CLI and C API shared-library binary hashes and
the C API header-tree hash from the verified archives, then rechecks the
installed bytes at every gate. The pinned GCC base-image digest is checked
against `toolchains.tsv`; the image records resolved paths and hashes of the
actual `gcc` driver, `cc1`, assembler, `collect2`, and linker executables.
The gate rechecks their paths and bytes before compiling. A preflight
regression substitutes executable bytes without changing the marker and
requires rejection.

`make test-real-corpus` runs interpreter and JIT tests with an explicitly
configured runtime-heap in GC-torture mode and runtime-core's always-move
stack mode. A keyed poll request allocates in a substantive inner function
while each `corpus_run` call is active; every invocation requires a fresh
collection, preservation of a rooted probe, and a stack move.
`make test-real-corpus-relocate` repeats both tiers with relocation enabled
and requires the linked runtime-heap to have been built with `RELOCATE=yes`;
it roots a live probe and fails unless that object moves during every
`corpus_run` and its marker remains valid after each invocation. Build
that runtime-heap into a private prefix and put its pkg-config directory first
on `PKG_CONFIG_PATH` before invoking the target. Neither gate adds a production
lang-wasm dependency on runtime-heap.
Both targets first run a planted `GTEST_FILTER=-*` control and require it to
fail with a zero-selection diagnostic. The executable then requires all 12
ordinary or all 18 relocation cases to be selected and passed with no skips;
an inherited filter cannot turn an empty or partial run into a passing gate.

`pins.tsv` carries separate archive and extracted-tree hashes. The tree hash
is checked against the manifest as well as the local extraction marker, so a
rewritten marker cannot bless modified upstream source. The Wasmtime C API
host loads the exact `modules/{sqlite,zlib,lua}.wasm` bytes used by lang-wasm,
instantiates each module once, and calls its `corpus_run`,
`corpus_output_size`, and `corpus_output_chunk` exports eight times on that
same instance. Every status, length, and output byte must match the golden
vector. In proposal mode, the native result supplies the candidate expected
bytes, so native and Wasmtime must agree before a vector can be proposed.
The host rejects every unlisted or mistyped import and supplies only trap
callbacks for the exact Lua imports; any host callback call fails the run.
SQLite and zlib remain import-free. Lua's reactor has
exactly the seven typed `wasi_snapshot_preview1` libc imports in `imports.tsv`;
the manifest rejects every extra, missing, mistyped, or non-function import.
The lang-wasm corpus host resolves these names only to callbacks that fail if
invoked, so the deterministic workload receives no filesystem, clock,
process, or other host service. The manifest hash pins this boundary.

The seven Lua imports are linker references from wasi-sdk libc objects:
`clock_time_get` from time APIs, prestat lookups from `preopens`, `proc_exit`
from `_Exit`, `fd_close` from fd renumbering, `fd_seek` from `lseek`, and
`fd_write` from `writev`. They remain an exact, fail-closed test-host boundary;
the Lua corpus does not invoke those services. The C API host reads every
chunk from the same instance in one process. The retired CLI oracle used a
separately compiled wrapper module, which could not prove that Wasmtime
executed the production module bytes.

The three workloads are SQLite's in-memory database API, zlib's in-memory
deflate/inflate API, and Lua's embedded interpreter core with its string and
table libraries. Lua is built without its unused hosted I/O, OS, dynamic-load,
debug, and other standard-library modules. Its remaining imports come from
wasi-sdk's C library and reactor/SJLJ support, not from those omitted Lua
libraries. The driver supplies an explicit allocator and fixed seed, and
registers only the two libraries the script uses. The drivers are kept
separate from upstream files. They expose a `corpus_run` status function and
bounded byte-output functions. SQLite encodes every selected row and column,
zlib exposes all 4096 bytes after verifying the inflate round trip, and Lua
encodes both returned values. Native, Wasmtime, interpreter, and JIT executions
compare every output byte and status; `vectors.tsv` maps workloads to wrapped,
human-reviewable hex files with the complete records.

The SQLite golden includes all three rows selected by `id >= 2`, in descending
ID order. It was corrected after native and Wasmtime independently produced the
same 74-byte result; the previous 49-byte golden had omitted the first row.

`vectors.tsv` maps complete canonical output records, and `vectors.sha256` pins
the manifest and each hex file. The expected values are derived from the fixed
inputs and encoding specified by the drivers; EVO-X2 and Pi native and Wasmtime runs must
confirm them before acceptance. Candidate files are written under the
configured scratch directory. Do not replace vectors to make a failing engine
pass; an oracle change requires reviewing the workload and source/toolchain
pins.
