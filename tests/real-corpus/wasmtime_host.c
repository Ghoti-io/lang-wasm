/* SPDX-License-Identifier: LGPL-3.0-only */
/* Run the production corpus module, without adding an oracle wrapper to it. */
#include <wasmtime.h>

#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RESULT_CAPACITY 8192u
#define INVOCATIONS 8u
#define MAX_MODULE_BYTES (64u * 1024u * 1024u)

typedef struct {
  const char *name;
  const wasm_valkind_t *params;
  size_t nparams;
  const wasm_valkind_t *results;
  size_t nresults;
} allowed_import;

static const wasm_valkind_t i32[] = {WASM_I32};
static const wasm_valkind_t i32_i32[] = {WASM_I32, WASM_I32};
static const wasm_valkind_t i32_i32_i32[] = {WASM_I32, WASM_I32, WASM_I32};
static const wasm_valkind_t i32_i32_i32_i32[] = {WASM_I32, WASM_I32, WASM_I32, WASM_I32};
static const wasm_valkind_t i32_i64_i32[] = {WASM_I32, WASM_I64, WASM_I32};
static const wasm_valkind_t i32_i64_i32_i32[] = {WASM_I32, WASM_I64, WASM_I32, WASM_I32};
static const allowed_import lua_imports[] = {
  {"clock_time_get", i32_i64_i32, 3, i32, 1},
  {"fd_close", i32, 1, i32, 1},
  {"fd_prestat_get", i32_i32, 2, i32, 1},
  {"fd_prestat_dir_name", i32_i32_i32, 3, i32, 1},
  {"fd_seek", i32_i64_i32_i32, 4, i32, 1},
  {"fd_write", i32_i32_i32_i32, 4, i32, 1},
  {"proc_exit", i32, 1, NULL, 0},
};

static bool name_is(const wasm_name_t *name, const char *expected) {
  return name && name->size == strlen(expected) &&
         memcmp(name->data, expected, name->size) == 0;
}

static bool signature_is(const wasm_functype_t *type,
                         const allowed_import *expected) {
  const wasm_valtype_vec_t *params = wasm_functype_params(type);
  const wasm_valtype_vec_t *results = wasm_functype_results(type);
  if (!params || !results || params->size != expected->nparams ||
      results->size != expected->nresults) return false;
  for (size_t i = 0; i < params->size; ++i)
    if (wasm_valtype_kind(params->data[i]) != expected->params[i]) return false;
  for (size_t i = 0; i < results->size; ++i)
    if (wasm_valtype_kind(results->data[i]) != expected->results[i]) return false;
  return true;
}

static wasm_trap_t *deny_import(void *env, wasmtime_caller_t *caller,
                               const wasmtime_val_t *args, size_t nargs,
                               wasmtime_val_t *results, size_t nresults) {
  size_t *calls = env;
  (void)caller; (void)args; (void)nargs; (void)results; (void)nresults;
  ++*calls;
  static const char message[] = "unexpected corpus host import call";
  return wasmtime_trap_new(message, sizeof(message) - 1u);
}

static void report_error(const char *stage, wasmtime_error_t *error,
                         wasm_trap_t *trap) {
  wasm_byte_vec_t message = {0};
  if (error) {
    wasmtime_error_message(error, &message);
    fprintf(stderr, "%s: %.*s\n", stage, (int)message.size, message.data);
    wasm_byte_vec_delete(&message);
    wasmtime_error_delete(error);
  }
  if (trap) {
    wasm_trap_message(trap, &message);
    fprintf(stderr, "%s: trap: %.*s\n", stage, (int)message.size, message.data);
    wasm_byte_vec_delete(&message);
    wasm_trap_delete(trap);
  }
}

static uint8_t *read_module(const char *path, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (!file) { perror(path); return NULL; }
  if (fseek(file, 0, SEEK_END) != 0) { perror(path); fclose(file); return NULL; }
  long size = ftell(file);
  if (size <= 0 || (unsigned long)size > MAX_MODULE_BYTES ||
      fseek(file, 0, SEEK_SET) != 0) {
    fprintf(stderr, "%s: empty or oversized module\n", path);
    fclose(file);
    return NULL;
  }
  uint8_t *bytes = malloc((size_t)size);
  if (!bytes) { perror("malloc"); fclose(file); return NULL; }
  size_t got = fread(bytes, 1, (size_t)size, file);
  int closed = fclose(file);
  if (got != (size_t)size || closed != 0) {
    fprintf(stderr, "%s: failed to read complete module\n", path);
    free(bytes);
    return NULL;
  }
  *length = (size_t)size;
  return bytes;
}

static bool get_function(wasmtime_context_t *context,
                         const wasmtime_instance_t *instance, const char *name,
                         wasmtime_func_t *out) {
  wasmtime_extern_t item;
  if (!wasmtime_instance_export_get(context, instance, name, strlen(name), &item)) {
    fprintf(stderr, "missing export %s\n", name);
    return false;
  }
  bool valid = item.kind == WASMTIME_EXTERN_FUNC;
  if (valid) *out = item.of.func;
  wasmtime_extern_delete(&item);
  if (!valid) fprintf(stderr, "export %s is not a function\n", name);
  return valid;
}

static bool call_i64(wasmtime_context_t *context, const wasmtime_func_t *function,
                     const char *name, bool has_argument, uint32_t argument,
                     uint64_t *out) {
  wasmtime_val_t input = {.kind = WASMTIME_I32};
  input.of.i32 = (int32_t)argument;
  wasmtime_val_t result = {0};
  wasm_trap_t *trap = NULL;
  wasmtime_error_t *error = wasmtime_func_call(context, function,
      has_argument ? &input : NULL, has_argument ? 1 : 0, &result, 1, &trap);
  if (error || trap) { report_error(name, error, trap); return false; }
  if (result.kind != WASMTIME_I64) {
    fprintf(stderr, "%s returned a non-i64 value\n", name);
    wasmtime_val_unroot(&result);
    return false;
  }
  *out = (uint64_t)result.of.i64;
  wasmtime_val_unroot(&result);
  return true;
}

static bool call_i32(wasmtime_context_t *context, const wasmtime_func_t *function,
                     const char *name, uint32_t *out) {
  wasmtime_val_t result = {0};
  wasm_trap_t *trap = NULL;
  wasmtime_error_t *error = wasmtime_func_call(context, function,
                                               NULL, 0, &result, 1, &trap);
  if (error || trap) { report_error(name, error, trap); return false; }
  if (result.kind != WASMTIME_I32) {
    fprintf(stderr, "%s returned a non-i32 value\n", name);
    wasmtime_val_unroot(&result);
    return false;
  }
  *out = (uint32_t)result.of.i32;
  wasmtime_val_unroot(&result);
  return true;
}

static int digit(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int main(int argc, char **argv) {
  int result = 1;
  uint8_t *module_bytes = NULL;
  wasm_engine_t *engine = NULL;
  wasmtime_module_t *module = NULL;
  wasmtime_store_t *store = NULL;
  wasm_importtype_vec_t declared = {0};
  size_t import_calls = 0;
  if (argc != 4 || (strcmp(argv[1], "sqlite") && strcmp(argv[1], "zlib") &&
                    strcmp(argv[1], "lua"))) {
    fputs("usage: wasmtime-host {sqlite|zlib|lua} MODULE.wasm EXPECTED_HEX\n", stderr);
    return 2;
  }
  size_t expected_chars = strlen(argv[3]);
  if (expected_chars > RESULT_CAPACITY * 2u || expected_chars % 2u) {
    fputs("expected corpus hex is malformed or oversized\n", stderr);
    return 2;
  }
  for (size_t i = 0; i < expected_chars; ++i)
    if (digit(argv[3][i]) < 0) {
      fputs("expected corpus hex must be lowercase hexadecimal\n", stderr);
      return 2;
    }
  size_t module_length = 0;
  module_bytes = read_module(argv[2], &module_length);
  if (!module_bytes) goto done;
  engine = wasm_engine_new();
  if (!engine) { fputs("Wasmtime engine creation failed\n", stderr); goto done; }
  wasmtime_error_t *error = wasmtime_module_new(engine, module_bytes,
                                                module_length, &module);
  if (error) { report_error("module compilation", error, NULL); goto done; }
  store = wasmtime_store_new(engine, NULL, NULL);
  if (!store) { fputs("Wasmtime store creation failed\n", stderr); goto done; }
  wasmtime_context_t *context = wasmtime_store_context(store);
  wasmtime_store_limiter(store, 64 * 1024 * 1024, 1024, 1, 1, 1);
  wasmtime_module_imports(module, &declared);
  size_t expected_imports = strcmp(argv[1], "lua") == 0 ?
      sizeof(lua_imports) / sizeof(lua_imports[0]) : 0;
  if (declared.size != expected_imports) {
    fprintf(stderr, "%s: unexpected import count: %zu (expected %zu)\n",
            argv[1], declared.size, expected_imports);
    goto done;
  }
  wasmtime_extern_t imports[sizeof(lua_imports) / sizeof(lua_imports[0])] = {0};
  for (size_t i = 0; i < declared.size; ++i) {
    const wasm_importtype_t *item = declared.data[i];
    const wasm_externtype_t *external = wasm_importtype_type(item);
    const wasm_functype_t *function = wasm_externtype_as_functype_const(external);
    if (!name_is(wasm_importtype_module(item), "wasi_snapshot_preview1") ||
        !name_is(wasm_importtype_name(item), lua_imports[i].name) ||
        wasm_externtype_kind(external) != WASM_EXTERN_FUNC || !function ||
        !signature_is(function, &lua_imports[i])) {
      fprintf(stderr, "%s: unexpected host import at index %zu\n", argv[1], i);
      goto done;
    }
    imports[i].kind = WASMTIME_EXTERN_FUNC;
    wasmtime_func_new(context, function, deny_import, &import_calls, NULL,
                      &imports[i].of.func);
  }
  wasmtime_instance_t instance = {0};
  wasm_trap_t *trap = NULL;
  error = wasmtime_instance_new(context, module, imports, declared.size,
                                &instance, &trap);
  if (error || trap) { report_error("instantiation", error, trap); goto done; }
  wasmtime_func_t run, size, chunk;
  if (!get_function(context, &instance, "corpus_run", &run) ||
      !get_function(context, &instance, "corpus_output_size", &size) ||
      !get_function(context, &instance, "corpus_output_chunk", &chunk)) goto done;
  uint8_t bytes[RESULT_CAPACITY];
  const size_t expected_length = expected_chars / 2u;
  for (unsigned invocation = 1; invocation <= INVOCATIONS; ++invocation) {
    uint64_t status = 0;
    uint32_t length = 0;
    if (!call_i64(context, &run, "corpus_run", false, 0, &status) ||
        !call_i32(context, &size, "corpus_output_size", &length)) goto done;
    if (status != 1 || length != expected_length || length > RESULT_CAPACITY) {
      fprintf(stderr, "%s invocation %u: status=%" PRIu64 " length=%" PRIu32
                      " (expected status=1 length=%zu)\n",
              argv[1], invocation, status, length, expected_length);
      goto done;
    }
    for (uint32_t offset = 0; offset < length; offset += 7u) {
      uint64_t packed = 0;
      if (!call_i64(context, &chunk, "corpus_output_chunk", true,
                    offset / 7u, &packed)) goto done;
      if (packed >> 56 != 1u) {
        fprintf(stderr, "%s invocation %u: missing output chunk %u\n",
                argv[1], invocation, offset / 7u);
        goto done;
      }
      for (uint32_t i = 0; i < 7u && offset + i < length; ++i)
        bytes[offset + i] = (uint8_t)(packed >> (i * 8u));
    }
    for (size_t i = 0; i < length; ++i) {
      unsigned expected = (unsigned)((digit(argv[3][i * 2u]) << 4) |
                                     digit(argv[3][i * 2u + 1u]));
      if (bytes[i] != expected) {
        fprintf(stderr, "%s invocation %u: first byte mismatch at offset %zu: "
                        "expected 0x%02x, got 0x%02x\n",
                argv[1], invocation, i, expected, bytes[i]);
        goto done;
      }
    }
  }
  if (import_calls != 0) {
    fprintf(stderr, "%s: %zu unexpected host import calls\n", argv[1], import_calls);
    goto done;
  }
  static const char digits[] = "0123456789abcdef";
  printf("1:%zu:", expected_length);
  for (size_t i = 0; i < expected_length; ++i) {
    putchar(digits[bytes[i] >> 4]);
    putchar(digits[bytes[i] & 0x0fu]);
  }
  putchar('\n');
  if (fflush(stdout) != 0 || ferror(stdout)) { perror("stdout"); goto done; }
  result = 0;
done:
  wasm_importtype_vec_delete(&declared);
  if (store) wasmtime_store_delete(store);
  if (module) wasmtime_module_delete(module);
  if (engine) wasm_engine_delete(engine);
  free(module_bytes);
  return result;
}
