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

#include <ghoti.io/lang-wasm/macros.h>

#include "cache_internal.h"
#include "../module_internal.h"

#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/cutil/safemath.h>

#include <stdbool.h>
#include <stdatomic.h>

enum { JIT_WINDOW_SIZE = 64, JIT_EXIT_THRESHOLD = 8 };
enum { JIT_OPCODE_COUNTERS = 256 + 18 };
enum { JIT_EMPTY = 0, JIT_COMPILING, JIT_COMPILED, JIT_REFUSED };

typedef struct GLWASM_JitRecord {
  GRCORE_Code *code;
  GLWASM_JitRefusal refusal;
  GLWASM_JitCacheStats stats;
  uint64_t recent_exits;
  uint8_t state;
  uint8_t next_entry;
  uint8_t window_filled;
  uint8_t reserved;
} GLWASM_JitRecord;

struct GLWASM_JitCache {
  GLWASM_Allocator allocator;
  GRCORE_CodePages *module_pages;
  atomic_bool locked;
  GLWASM_JitCacheKey key;
  uint32_t function_count;
  uint64_t opcode_refusal_counts[JIT_OPCODE_COUNTERS];
  uint64_t opcode_exit_counts[JIT_OPCODE_COUNTERS];
  GLWASM_JitRecord records[];
};

GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_module_init)(
    struct GLWASM_Module *module) {
  if (!module || module->jit_pages || module->jit_cache)
    return GLWASM_ERR_INVALID;
  GLWASM_Policy policy;
  if (module->has_policy) policy = module->policy;
  else if (glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy) != GLWASM_OK)
    return GLWASM_ERR_INTERNAL;
  GRCORE_Result core = grcore_code_pages_create(NULL, policy.max_jit_code_bytes,
      &module->jit_pages);
  if (core != GRCORE_OK) return core == GRCORE_ERR_OOM
      ? GLWASM_ERR_OOM : core == GRCORE_ERR_LIMIT
      ? GLWASM_ERR_LIMIT : GLWASM_ERR_INTERNAL;
  if (module->nfuncs == 0) return GLWASM_OK;
  GLWASM_JitCacheKey key = {1u, policy.jit_disabled_cpu_features, 1u};
  GLWASM_Result result = GHOTIIO_LANG_WASM(glwasm_jit_cache_create)(
      &module->alloc, key,
      module->nfuncs, module->jit_pages, &module->jit_cache);
  if (result != GLWASM_OK) {
    grcore_code_pages_release(module->jit_pages);
    module->jit_pages = NULL;
  }
  return result;
}

void GHOTIIO_LANG_WASM(glwasm_jit_module_destroy)(
    struct GLWASM_Module *module) {
  if (!module) return;
  GHOTIIO_LANG_WASM(glwasm_jit_cache_destroy)(module->jit_cache);
  module->jit_cache = NULL;
  grcore_code_pages_release(module->jit_pages);
  module->jit_pages = NULL;
}

static void cache_lock(GLWASM_JitCache *cache) {
  bool expected;
  do {
    expected = false;
  } while (!atomic_compare_exchange_weak_explicit(&cache->locked, &expected,
      true, memory_order_acquire, memory_order_relaxed));
}

static void cache_unlock(GLWASM_JitCache *cache) {
  atomic_store_explicit(&cache->locked, false, memory_order_release);
}

static GLWASM_JitRecord *record_for(GLWASM_JitCache *cache,
    uint32_t function) {
  return cache && function < cache->function_count
      ? &cache->records[function] : NULL;
}

static size_t opcode_counter_index(uint16_t opcode) {
  if (opcode <= UINT8_MAX) return opcode;
  if ((opcode & UINT16_C(0xff00)) == UINT16_C(0xfc00) &&
      (opcode & UINT16_C(0x00ff)) <= 17u)
    return 256u + (opcode & UINT16_C(0x00ff));
  return SIZE_MAX;
}

GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_create)(
    const GLWASM_Allocator *allocator, GLWASM_JitCacheKey key,
    uint32_t function_count, GRCORE_CodePages *module_pages,
    GLWASM_JitCache **out_cache) {
  if (!out_cache || !function_count || !module_pages ||
      !key.compiler_ir_version ||
      key.disabled_cpu_features & ~GRJIT_FEATURES_ALL)
    return GLWASM_ERR_INVALID;
  *out_cache = NULL;
  const GLWASM_Allocator *chosen = allocator
      ? allocator : glwasm_allocator_default();
  if (!chosen || !chosen->malloc_fn || !chosen->calloc_fn ||
      !chosen->realloc_fn || !chosen->free_fn) return GLWASM_ERR_INVALID;
  size_t records_bytes = 0;
  size_t bytes = 0;
  if (!gcu_safe_mul_size((size_t)function_count,
          sizeof(GLWASM_JitRecord), &records_bytes) ||
      !gcu_safe_add_size(sizeof(GLWASM_JitCache), records_bytes, &bytes))
    return GLWASM_ERR_LIMIT;
  GLWASM_JitCache *cache = chosen->calloc_fn(chosen->ctx, 1, bytes);
  if (!cache) return GLWASM_ERR_OOM;
  cache->allocator = *chosen;
  cache->module_pages = grcore_code_pages_retain(
      module_pages);
  cache->key = key;
  cache->function_count = function_count;
  atomic_init(&cache->locked, false);
  for (uint32_t i = 0; i < function_count; ++i)
    cache->records[i].stats.last_refused_opcode = UINT16_MAX;
  *out_cache = cache;
  return GLWASM_OK;
}

void GHOTIIO_LANG_WASM(glwasm_jit_cache_destroy)(GLWASM_JitCache *cache) {
  if (!cache) return;
  /* The caller has quiesced users and removed context registrations. */
  for (uint32_t i = 0; i < cache->function_count; ++i)
    grcore_code_release(cache->records[i].code);
  GLWASM_Allocator allocator = cache->allocator;
  grcore_code_pages_release(cache->module_pages);
  allocator.free_fn(allocator.ctx, cache);
}

GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_key)(
    const GLWASM_JitCache *cache, GLWASM_JitCacheKey *out_key) {
  if (!cache || !out_key) return GLWASM_ERR_INVALID;
  *out_key = cache->key;
  return GLWASM_OK;
}

GLWASM_JitCompileDecision GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitRefusal *reason) {
  if (reason) *reason = GLWASM_JIT_REFUSAL_NONE;
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record) return GLWASM_JIT_COMPILE_ERROR;
  cache_lock(cache);
  GLWASM_JitCompileDecision decision;
  if (record->stats.interpreter_only) {
    if (reason) *reason = GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE;
    decision = GLWASM_JIT_COMPILE_INTERPRET;
  } else if (record->state == JIT_COMPILED) {
    decision = GLWASM_JIT_COMPILE_READY;
  } else if (record->state == JIT_REFUSED) {
    if (reason) *reason = record->refusal;
    decision = GLWASM_JIT_COMPILE_INTERPRET;
  } else if (record->state == JIT_COMPILING) {
    decision = GLWASM_JIT_COMPILE_BUSY;
  } else {
    record->state = JIT_COMPILING;
    ++record->stats.compile_attempts;
    decision = GLWASM_JIT_COMPILE_OWNER;
  }
  cache_unlock(cache);
  return decision;
}

GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_publish)(
    GLWASM_JitCache *cache, uint32_t function, GRCORE_Code *code) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record || !code) return GLWASM_ERR_INVALID;
  cache_lock(cache);
  if (record->state != JIT_COMPILING || record->code) {
    cache_unlock(cache);
    return GLWASM_ERR_INVALID;
  }
  record->code = code; /* transfers the caller's reference */
  record->state = JIT_COMPILED;
  ++record->stats.compile_successes;
  cache_unlock(cache);
  return GLWASM_OK;
}

GLWASM_Result GHOTIIO_LANG_WASM(glwasm_jit_cache_refuse)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitRefusal reason) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record || reason <= GLWASM_JIT_REFUSAL_NONE ||
      reason >= GLWASM_JIT_REFUSAL_COUNT) return GLWASM_ERR_INVALID;
  cache_lock(cache);
  if (record->state != JIT_COMPILING) {
    cache_unlock(cache);
    return GLWASM_ERR_INVALID;
  }
  record->refusal = reason;
  record->state = JIT_REFUSED;
  ++record->stats.refusal_counts[reason];
  cache_unlock(cache);
  return GLWASM_OK;
}

GRCORE_Code * GHOTIIO_LANG_WASM(glwasm_jit_cache_retain_code)(
    GLWASM_JitCache *cache, uint32_t function) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record) return NULL;
  cache_lock(cache);
  GRCORE_Code *code = record->state == JIT_COMPILED &&
      !record->stats.interpreter_only
      ? grcore_code_retain(record->code) : NULL;
  cache_unlock(cache);
  return code;
}

const GRCORE_PageProvider * GHOTIIO_LANG_WASM(glwasm_jit_cache_pages)(
    const GLWASM_JitCache *cache) {
  return cache == NULL ? NULL : GHOTIIO_RUNTIME_CORE(
      grcore_code_pages_provider)(cache->module_pages);
}

void GHOTIIO_LANG_WASM(glwasm_jit_cache_finish_retirement)(
    GLWASM_JitCache *cache, uint32_t function) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record) return;
  cache_lock(cache);
  GRCORE_Code *code = NULL;
  if (record->stats.interpreter_only) {
    code = record->code;
    record->code = NULL;
    record->state = JIT_REFUSED;
  }
  cache_unlock(cache);
  grcore_code_release(code);
}

int GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitExit exit) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record || exit < GLWASM_JIT_EXIT_RETURN ||
      exit >= GLWASM_JIT_EXIT_COUNT) return 0;
  cache_lock(cache);
  if (record->state != JIT_COMPILED) {
    cache_unlock(cache);
    return 0;
  }
  ++record->stats.exit_counts[exit];
  ++record->stats.compiled_entries;
  int guest_visible_exit = exit == GLWASM_JIT_EXIT_GUARD_DEOPT ||
      exit == GLWASM_JIT_EXIT_NAMED_UNSUPPORTED ||
      exit == GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE;
  /* TRAP_RECONSTRUCTION and actual Wasm traps describe guest semantics, not
   * an unsupported compiled path, so neither contributes to retirement. */
  unsigned slot = record->next_entry;
  if (record->window_filled == JIT_WINDOW_SIZE)
    record->stats.window_exits -=
        (uint32_t)((record->recent_exits >> slot) & UINT64_C(1));
  else
    ++record->window_filled;
  if (guest_visible_exit) {
    record->recent_exits |= UINT64_C(1) << slot;
    ++record->stats.window_exits;
    ++record->stats.guest_visible_exits;
  } else {
    record->recent_exits &= ~(UINT64_C(1) << slot);
    ++record->stats.completed_without_deopt;
  }
  record->next_entry = (uint8_t)((slot + 1u) % JIT_WINDOW_SIZE);
  record->stats.window_entries = record->window_filled;
  if (!record->stats.interpreter_only &&
      record->window_filled == JIT_WINDOW_SIZE &&
      record->stats.window_exits >= JIT_EXIT_THRESHOLD) {
    record->stats.interpreter_only = 1;
    record->refusal = GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE;
    ++record->stats.refusal_counts[GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE];
    cache_unlock(cache);
    return 1;
  }
  cache_unlock(cache);
  return 0;
}

int GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(
    GLWASM_JitCache *cache, uint32_t function, GLWASM_JitCacheStats *out) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record || !out) return 0;
  cache_lock(cache);
  *out = record->stats;
  cache_unlock(cache);
  return 1;
}

void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_unsupported_opcode)(
    GLWASM_JitCache *cache, uint32_t function, uint16_t opcode) {
  GLWASM_JitRecord *record = record_for(cache, function);
  size_t index = opcode_counter_index(opcode);
  if (!record || index == SIZE_MAX) return;
  cache_lock(cache);
  ++cache->opcode_refusal_counts[index];
  record->stats.last_refused_opcode = opcode;
  cache_unlock(cache);
}

void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_unsupported_signature)(
    GLWASM_JitCache *cache, uint32_t function) {
  GLWASM_JitRecord *record = record_for(cache, function);
  if (!record) return;
  cache_lock(cache);
  ++record->stats.signature_refusal_count;
  cache_unlock(cache);
}

void GHOTIIO_LANG_WASM(glwasm_jit_cache_note_opcode_exit)(
    GLWASM_JitCache *cache, uint32_t function, uint16_t opcode) {
  GLWASM_JitRecord *record = record_for(cache, function);
  size_t index = opcode_counter_index(opcode);
  if (!record || index == SIZE_MAX) return;
  cache_lock(cache);
  ++cache->opcode_exit_counts[index];
  cache_unlock(cache);
}

uint64_t GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_exit_count)(
    GLWASM_JitCache *cache, uint16_t opcode) {
  size_t index = opcode_counter_index(opcode);
  if (!cache || index == SIZE_MAX) return 0;
  cache_lock(cache);
  uint64_t count = cache->opcode_exit_counts[index];
  cache_unlock(cache);
  return count;
}

uint64_t GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_refusal_count)(
    GLWASM_JitCache *cache, uint16_t opcode) {
  size_t index = opcode_counter_index(opcode);
  if (!cache || index == SIZE_MAX) return 0;
  cache_lock(cache);
  uint64_t count = cache->opcode_refusal_counts[index];
  cache_unlock(cache);
  return count;
}
