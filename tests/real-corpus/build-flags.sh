# SPDX-License-Identifier: LGPL-3.0-only
# Shared C options are intentionally short and source-controlled; every output
# hash is tied to these exact values plus the separately pinned tool releases.
CORPUS_CFLAGS='-std=c17 -O2 -ffp-contract=off -ffunction-sections -fdata-sections'
CORPUS_SQLITE_DEFINES='-DSQLITE_OS_OTHER=1 -DSQLITE_TEMP_STORE=3 -DSQLITE_THREADSAFE=0 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 -DSQLITE_DEFAULT_MEMSTATUS=0'
CORPUS_ZLIB_SOURCES='adler32.c compress.c crc32.c deflate.c infback.c inffast.c inflate.c inftrees.c trees.c uncompr.c zutil.c'
CORPUS_NATIVE_LDFLAGS='-Wl,--gc-sections -lm'
CORPUS_LUA_NATIVE_LDFLAGS='-Wl,--gc-sections -lm -ldl'
CORPUS_WASI_LDFLAGS='-mexec-model=reactor -Wl,--export=corpus_run -Wl,--export=corpus_output_size -Wl,--export=corpus_output_chunk -Wl,--gc-sections -lm'
# Lua 5.5 includes signal declarations in its state headers, uses setjmp/
# longjmp for protected calls, and sizes its tmpname buffer from L_tmpnam.
# wasi-sdk deliberately provides neither hosted tmpname sizing nor POSIX
# signals and disables SJLJ by default. Use its signal shim, the documented
# standard-exception SJLJ lowering and runtime library, and an explicit
# compile-time tmpname buffer bound. The fixture links only Lua core, auxiliary,
# string and table sources; its explicit state allocator and seed avoid host
# initialization APIs, while the unused hosted standard libraries are omitted.
CORPUS_LUA_WASI_CFLAGS='-fvisibility=hidden -D_WASI_EMULATED_SIGNAL -DL_tmpnam=256 -mllvm -wasm-enable-sjlj -mllvm -wasm-use-legacy-eh=false'
CORPUS_LUA_WASI_LDFLAGS='-lwasi-emulated-signal -lsetjmp'
