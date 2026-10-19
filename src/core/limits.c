/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 *
 * Ghoti.io Lang-wasm is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-wasm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The untrusted starting caps. The four numbered values are host-policy.md;
 * the rest are the defaults documentation/design.md records.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/lang-wasm/limits.h>
#include <ghoti.io/lang-wasm/policy.h>
#include "../module_internal.h"
#include <limits.h>
#include <string.h>

GLWASM_Result glwasm_limits_default(GLWASM_Limits * limits) {
  if (!limits) {
    return GLWASM_ERR_INVALID;
  }
  limits->module_bytes = 4ull * 1024ull * 1024ull;
  limits->types = 10000u;
  limits->imports = 1000u;
  limits->exports = 1000u;
  limits->functions = 10000u;
  limits->globals = 1000u;
  limits->tables = 100u;
  limits->memories = 100u;
  limits->tags = 1000u;
  limits->data_segments = 10000u;
  limits->element_segments = 10000u;
  limits->locals = 1000u;
  limits->instructions = 100000u;
  limits->nesting = 128u;
  limits->br_table = 10000u;
  limits->name_length = 1024u;
  limits->table_size = 1000000ull;
  limits->memory_maximum = 64ull * 1024ull * 1024ull;
  limits->memory_initial = 64ull * 1024ull * 1024ull;
  return GLWASM_OK;
}

GLWASM_Result glwasm_policy_preset(GLWASM_PolicyPreset preset,
    GLWASM_Policy *out) {
  if (!out || preset < GLWASM_POLICY_UNTRUSTED ||
      preset > GLWASM_POLICY_TRUSTED) return GLWASM_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (glwasm_limits_default(&out->limits) != GLWASM_OK)
    return GLWASM_ERR_INTERNAL;
  out->features = GLWASM_FEATURE_BULK_MEMORY |
      GLWASM_FEATURE_REFERENCE_TYPES | GLWASM_FEATURE_TAIL_CALLS |
      GLWASM_FEATURE_EXCEPTIONS;
  out->default_fuel = UINT64_C(10000000);
  out->default_guest_depth = 256;
  out->default_native_depth = 64;
  out->default_native_stack_bytes = UINT64_C(4) << 20;
  out->default_memory_bytes = out->limits.memory_maximum;
  out->max_jit_code_bytes = UINT64_C(16) * 1024 * 1024;
  out->jit_disabled_cpu_features = GRJIT_FEATURES_ALL;
  if (preset >= GLWASM_POLICY_STANDARD) {
    out->features |= GLWASM_FEATURE_MEMORY64 | GLWASM_FEATURE_TABLE64 |
        GLWASM_FEATURE_MULTI_MEMORY | GLWASM_FEATURE_EXTENDED_CONST;
    out->limits.module_bytes = UINT64_C(64) * 1024 * 1024;
    out->limits.functions = 100000;
    out->limits.memory_maximum = UINT64_C(1) * 1024 * 1024 * 1024;
    out->limits.memory_initial = out->limits.memory_maximum;
    out->limits.table_size = UINT64_C(10000000);
    out->default_memory_bytes = out->limits.memory_maximum;
    out->max_jit_code_bytes = UINT64_C(64) * 1024 * 1024;
  }
  if (preset == GLWASM_POLICY_TRUSTED) {
    out->limits.module_bytes = SIZE_MAX;
    out->limits.types = out->limits.imports = out->limits.exports = UINT32_MAX;
    out->limits.functions = out->limits.globals = out->limits.tables = UINT32_MAX;
    out->limits.memories = out->limits.tags = UINT32_MAX;
    out->limits.data_segments = out->limits.element_segments = UINT32_MAX;
    out->limits.locals = out->limits.instructions = UINT32_MAX;
    out->limits.nesting = out->limits.br_table = out->limits.name_length = UINT32_MAX;
    out->limits.table_size = UINT64_MAX;
    out->limits.memory_maximum = UINT64_MAX;
    out->limits.memory_initial = UINT64_MAX;
    out->default_memory_bytes = UINT64_MAX;
    out->max_jit_code_bytes = UINT64_C(256) * 1024 * 1024;
  }
  return GLWASM_OK;
}

static int module_feature_enabled(const GLWASM_Module *m, uint64_t features) {
  if (!(features & GLWASM_FEATURE_REFERENCE_TYPES)) {
    for (uint32_t i = 0; i < m->ntypes; ++i) {
      const GLWASM_TypeDef *t = &m->types[i];
      for (uint32_t j = 0; j < t->nparams; ++j)
        if (t->params[j].kind == GLWASM_VK_REF) return 0;
      for (uint32_t j = 0; j < t->nresults; ++j)
        if (t->results[j].kind == GLWASM_VK_REF) return 0;
    }
    for (uint32_t i = 0; i < m->nglobals; ++i)
      if (m->globals[i].type.kind == GLWASM_VK_REF) return 0;
  }
  if (m->ntables > 1 && !(features & GLWASM_FEATURE_REFERENCE_TYPES)) return 0;
  if (m->nmems > 1 && !(features & GLWASM_FEATURE_MULTI_MEMORY)) return 0;
  for (uint32_t i = 0; i < m->nmems; ++i)
    if (m->mems[i].addr64 && !(features & GLWASM_FEATURE_MEMORY64)) return 0;
  for (uint32_t i = 0; i < m->ntables; ++i) {
    if (m->tables[i].addr64 && !(features & GLWASM_FEATURE_TABLE64)) return 0;
    if (m->tables[i].elem.kind == GLWASM_VK_REF &&
        !(features & GLWASM_FEATURE_REFERENCE_TYPES)) return 0;
  }
  if (m->ntags && !(features & GLWASM_FEATURE_EXCEPTIONS)) return 0;
  if (!(features & GLWASM_FEATURE_EXTENDED_CONST)) {
    for (uint32_t i = 0; i < m->nglobals; ++i)
      for (uint32_t j = 0; j < m->globals[i].init.nins; ++j) {
        uint16_t op = m->globals[i].init.ins[j].op;
        if (op != 0x0b && op != 0x41 && op != 0x42 && op != 0x43 &&
            op != 0x44 && op != 0x23 && op != 0xd0 && op != 0xd2) return 0;
      }
    for (uint32_t i = 0; i < m->nelems; ++i)
      for (uint32_t j = 0; j < m->elems[i].offset.nins; ++j) {
        uint16_t op = m->elems[i].offset.ins[j].op;
        if (op != 0x0b && op != 0x41 && op != 0x42 && op != 0x43 &&
            op != 0x44 && op != 0x23) return 0;
      }
    for (uint32_t i = 0; i < m->ndatas; ++i)
      for (uint32_t j = 0; j < m->datas[i].offset.nins; ++j) {
        uint16_t op = m->datas[i].offset.ins[j].op;
        if (op != 0x0b && op != 0x41 && op != 0x42 && op != 0x43 &&
            op != 0x44 && op != 0x23) return 0;
      }
  }
  for (uint32_t i = 0; i < m->nfuncs; ++i) {
    const GLWASM_Func *f = &m->funcs[i];
    if (!(features & GLWASM_FEATURE_REFERENCE_TYPES)) {
      for (uint32_t j = 0; j < f->nlocals; ++j)
        if (f->locals[j].kind == GLWASM_VK_REF) return 0;
      for (uint32_t j = 0; j < f->nv; ++j)
        if (f->vpool[j].kind == GLWASM_VK_REF) return 0;
    }
    if (f->ncatches && !(features & GLWASM_FEATURE_EXCEPTIONS)) return 0;
    for (uint32_t j = 0; j < f->nins; ++j) {
      uint16_t op = f->ins[j].op;
      if ((op == 0x12 || op == 0x13) && !(features & GLWASM_FEATURE_TAIL_CALLS)) return 0;
      if (op >= 0xd0 && op <= 0xd6 && !(features & GLWASM_FEATURE_REFERENCE_TYPES)) return 0;
      if (op >= 0xfc08 && op <= 0xfc11 && !(features & GLWASM_FEATURE_BULK_MEMORY)) return 0;
      if (op == 0x1f && !(features & GLWASM_FEATURE_EXCEPTIONS)) return 0;
      if (!(features & GLWASM_FEATURE_EXTENDED_CONST) && f->is_const &&
          op != 0x0b && op != 0x41 && op != 0x42 && op != 0x43 &&
          op != 0x44 && op != 0x23 && op != 0xd0 && op != 0xd2) return 0;
    }
  }
  for (uint32_t i = 0; i < m->nelems; ++i)
    if ((m->elems[i].use_exprs || m->elems[i].reftype.kind == GLWASM_VK_REF) &&
        !(features & GLWASM_FEATURE_REFERENCE_TYPES)) return 0;
  return 1;
}

GLWASM_Result glwasm_module_decode_with_policy(const uint8_t *bytes,
    size_t length, const GLWASM_Policy *policy,
    const GLWASM_Allocator *allocator, GLWASM_Failure *failure,
    GLWASM_Module **out_module) {
  if (!policy || !out_module || policy->allow_tang_v1_imports > 1u ||
      (policy->jit_disabled_cpu_features & ~GRJIT_FEATURES_ALL))
    return GLWASM_ERR_INVALID;
  GLWASM_Module *module = NULL;
  GLWASM_Result result = glwasm_module_decode(bytes, length, &policy->limits,
      allocator, failure, &module);
  if (result != GLWASM_OK) return result;
  if (!module_feature_enabled(module, policy->features)) {
    glwasm_module_destroy(module);
    if (failure) { failure->result = GLWASM_ERR_UNSUPPORTED; failure->limit = NULL; }
    return GLWASM_ERR_UNSUPPORTED;
  }
  /* The binary decoder initializes the default policy's module resources.
   * Replace those with the caller's immutable cap before exposing the module. */
  GHOTIIO_LANG_WASM(glwasm_jit_module_destroy)(module);
  module->policy = *policy;
  module->has_policy = 1;
  result = GHOTIIO_LANG_WASM(glwasm_jit_module_init)(module);
  if (result != GLWASM_OK) {
    glwasm_module_destroy(module);
    if (failure) { failure->result = result; failure->limit = NULL; }
    return result;
  }
  *out_module = module;
  return GLWASM_OK;
}
