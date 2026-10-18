/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 *
 * Ghoti.io Lang-wasm is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

#ifndef GHOTI_IO_GLWASM_SRC_JIT_CACHE_INTERNAL_H
#define GHOTI_IO_GLWASM_SRC_JIT_CACHE_INTERNAL_H

#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/core.h>
#include <ghoti.io/lang-wasm/allocator.h>
#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/code_pages.h>
#include <ghoti.io/runtime-jit/features.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Internal module cache contract. The owning module must stop new cache users
 * and clear every context registration before destroying this object. */
typedef struct GLWASM_JitCache GLWASM_JitCache;
struct GLWASM_Module;

typedef struct GLWASM_JitCacheKey {
  uint32_t target_abi;
  GRJIT_Features disabled_cpu_features;
  uint32_t compiler_ir_version;
} GLWASM_JitCacheKey;

typedef enum GLWASM_JitRefusal {
  GLWASM_JIT_REFUSAL_NONE = 0,
  GLWASM_JIT_REFUSAL_BACKEND_UNAVAILABLE,
  GLWASM_JIT_REFUSAL_UNSUPPORTED_IR,
  GLWASM_JIT_REFUSAL_RESOURCE_LIMIT,
  GLWASM_JIT_REFUSAL_PROFITABILITY,
  GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE,
  GLWASM_JIT_REFUSAL_COUNT
} GLWASM_JitRefusal;

typedef enum GLWASM_JitCompileDecision {
  GLWASM_JIT_COMPILE_OWNER = 0,
  GLWASM_JIT_COMPILE_READY,
  GLWASM_JIT_COMPILE_INTERPRET,
  GLWASM_JIT_COMPILE_BUSY, /* another context owns compilation; interpret */
  GLWASM_JIT_COMPILE_ERROR
} GLWASM_JitCompileDecision;

/* Stable identities for every way a compiled invocation can leave its body.
 * Only the two semantic deopt classes contribute to the retirement window. */
typedef enum GLWASM_JitExit {
  GLWASM_JIT_EXIT_RETURN = 0,
  /* The interpreter reconstructs a compiled frame so it can raise the exact
   * guest trap. This semantic handoff is observable in its own counter, but
   * is not a compiled-path deoptimization and never retires the artifact. */
  GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION,
  GLWASM_JIT_EXIT_WASM_TRAP,
  GLWASM_JIT_EXIT_LIMIT_UNWIND,
  GLWASM_JIT_EXIT_PAUSE,
  GLWASM_JIT_EXIT_POLL,
  GLWASM_JIT_EXIT_DEBUGGER,
  GLWASM_JIT_EXIT_HOST_CALL,
  /* A compiled guest exception propagated or was caught by an outer frame. */
  GLWASM_JIT_EXIT_WASM_EXCEPTION,
  GLWASM_JIT_EXIT_GUARD_DEOPT,
  GLWASM_JIT_EXIT_NAMED_UNSUPPORTED,
  GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE,
  GLWASM_JIT_EXIT_COUNT
} GLWASM_JitExit;

typedef struct GLWASM_JitCacheStats {
  uint64_t compile_attempts;
  uint64_t compile_successes;
  uint64_t refusal_counts[GLWASM_JIT_REFUSAL_COUNT];
  uint64_t exit_counts[GLWASM_JIT_EXIT_COUNT];
  uint64_t guest_visible_exits;
  uint64_t completed_without_deopt;
  uint64_t compiled_entries;
  uint64_t signature_refusal_count;
  uint16_t last_refused_opcode;
  uint32_t window_entries;
  uint32_t window_exits;
  uint8_t interpreter_only;
} GLWASM_JitCacheStats;

/* Called once after validation/translation has fixed function_count and policy. */
GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_module_init)(
    struct GLWASM_Module *module);
void GHOTIIO_LANG_WASM(glwasm_jit_module_destroy)(
    struct GLWASM_Module *module);

/* The function index is the key within a cache. Target ABI, IR/compiler
 * version and disabled CPU feature mask are immutable cache construction
 * inputs in the eventual module integration. */
GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_create)(
    const GLWASM_Allocator *allocator, GLWASM_JitCacheKey key,
    uint32_t function_count, GRCORE_CodePages *module_pages,
    GLWASM_JitCache **out_cache);
void GHOTIIO_LANG_WASM(glwasm_jit_cache_destroy)(GLWASM_JitCache *cache);
/* The immutable module cache identity, for tests and diagnostics. */
GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_key)(
    const GLWASM_JitCache *cache, GLWASM_JitCacheKey *out_key);
GLWASM_JitCompileDecision GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitRefusal *reason);
GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_publish)(
    GLWASM_JitCache *cache, uint32_t function, GRCORE_Code *code);
GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_refuse)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitRefusal reason);
GRCORE_Code * GHOTIIO_LANG_WASM(glwasm_jit_cache_retain_code)(
    GLWASM_JitCache *cache, uint32_t function);
const GRCORE_PageProvider * GHOTIIO_LANG_WASM(glwasm_jit_cache_pages)(
    const GLWASM_JitCache *cache);
/* Called only after each context has cleared its slot and unregistered the
 * artifact, and runtime-core's active-frame references protect live callers. */
void GHOTIIO_LANG_WASM(glwasm_jit_cache_finish_retirement)(
    GLWASM_JitCache *cache, uint32_t function);
/* Returns nonzero exactly once when the rate window retires this artifact. */
int GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitExit exit);
int GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitCacheStats *out);
void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_unsupported_opcode)(
    GLWASM_JitCache *cache, uint32_t function, uint16_t opcode);
void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_unsupported_signature)(
    GLWASM_JitCache *cache, uint32_t function);
void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_opcode_exit)(
    GLWASM_JitCache *cache, uint32_t function, uint16_t opcode);
uint64_t GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_exit_count)(
    GLWASM_JitCache *cache, uint16_t opcode);
uint64_t GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_refusal_count)(
    GLWASM_JitCache *cache, uint16_t opcode);

#ifdef __cplusplus
}
#endif

#endif
