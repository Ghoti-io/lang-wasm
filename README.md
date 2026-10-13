# Ghoti.io Lang-wasm

A WebAssembly 3.0 binary decoder and validator (`GLWASM`). It reads a module
in one pass and checks it in a second. It does not instantiate, resolve an
import, or run code. It is part of the
[Ghoti.io suite](https://github.com/Ghoti-io/suite).

The design, including the limit defaults and the type lattice, is in
[documentation/design.md](documentation/design.md).

## Status

**A scaffold** (2026-10-13). Decode and validation of the in-scope feature
set are in place: multiple memories, memory64, table64, exceptions, tail
calls, bulk memory and extended constant expressions. A deferred feature that
decodes (a typed function reference, `v128`, the legacy `try` / `catch` /
`rethrow` encodings) is `GLWASM_ERR_UNSUPPORTED`. An interpreter, a text
format and a host API are later work. Depends on `cutil` only.

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

## Building

```bash
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/lang-wasm test PREFIX="$PWD/.local-lang-wasm"
```

Pass `PREFIX` on every make. pkg-config is how the library finds `cutil`.
Once installed, a program links with
`pkg-config --cflags --libs ghoti.io-lang-wasm-0`.
