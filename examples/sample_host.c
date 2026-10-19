/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/lang-wasm.h>

#include <stdio.h>
#include <stdlib.h>

static int read_file(const char *path, unsigned char **out, size_t *length) {
  FILE *file = fopen(path, "rb");
  if (!file) return 0;
  if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
  long end = ftell(file);
  if (end < 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 0; }
  size_t size = (size_t)end;
  unsigned char *bytes = size ? (unsigned char *)malloc(size) : NULL;
  if (size && (!bytes || fread(bytes, 1, size, file) != size)) {
    free(bytes); fclose(file); return 0;
  }
  fclose(file); *out = bytes; *length = size; return 1;
}

int main(int argc, char **argv) {
  const char *path = argc > 1 ? argv[1] : "libs/lang-wasm/examples/runaway.wasm";
  unsigned char *bytes = NULL;
  size_t length = 0;
  if (!read_file(path, &bytes, &length)) {
    fprintf(stderr, "could not read plugin: %s\n", path);
    return 1;
  }

  GLWASM_Policy policy;
  GLWASM_Module *module = NULL;
  GLWASM_Failure failure = {0};
  GLWASM_Result result = glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy);
  if (result == GLWASM_OK) {
    policy.default_fuel = 2000;
    policy.default_memory_bytes = 2u * 1024u * 1024u;
    result = glwasm_module_decode_with_policy(bytes, length, &policy, NULL,
        &failure, &module);
  }
  free(bytes);
  if (result != GLWASM_OK) {
    fprintf(stderr, "plugin rejected: %s\n", glwasm_result_string(result));
    return 1;
  }

  /* The store's budgets are the policy's: a runaway plugin runs out of
   * fuel and comes back paused, never unbounded. */
  GLWASM_Store *store = NULL;
  result = glwasm_store_create(&policy, &store);
  GLWASM_Runtime *runtime = NULL;
  GLWASM_InstantiateError error = {0};
  if (result == GLWASM_OK)
    result = glwasm_store_instantiate(store, module, NULL, 0, 1, &error,
        &runtime);
  glwasm_module_destroy(module);
  if (result != GLWASM_OK) {
    fprintf(stderr, "plugin instance failed: %s\n", glwasm_result_string(result));
    glwasm_store_destroy(store);
    return 1;
  }

  uint64_t ignored = 0;
  result = glwasm_runtime_invoke_export(runtime, "run", NULL, 0, &ignored);
  if (result == GLWASM_ERR_PAUSED) {
    uint32_t function = 0;
    uint64_t offset = 0;
    if (glwasm_runtime_pause_position(runtime, &function, &offset) == GLWASM_OK)
      printf("plugin paused at function %u, byte offset %llu\n", function,
          (unsigned long long)offset);
    else
      puts("plugin paused");
  } else {
    fprintf(stderr, "plugin returned: %s\n", glwasm_result_string(result));
    glwasm_store_destroy(store);
    return 1;
  }
  glwasm_store_destroy(store);
  return 0;
}
