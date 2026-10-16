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
 * The decode matrix: a minimal module, corrupt bytes, a deferred feature,
 * and one past each limit under a counting allocator.
 */

#include <ghoti.io/lang-wasm/lang-wasm.h>

#include "module_internal.h"
#include "runtime/runtime.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace {

std::vector<uint8_t> hex(const char * text) {
  std::vector<uint8_t> out;
  int hi = -1;

  for (const char * p = text; *p; p++) {
    char c = *p;
    int v;

    if (c == ' ' || c == '\n' || c == '\t') {
      continue;
    }
    if (c >= '0' && c <= '9') {
      v = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      v = c - 'a' + 10;
    } else if (c >= 'A' && c <= 'F') {
      v = c - 'A' + 10;
    } else {
      continue;
    }
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

const char k_empty[] = "00 61 73 6d 01 00 00 00";

const char k_add[] =
    "00 61 73 6d 01 00 00 00 "
    "01 07 01 60 02 7f 7f 01 7f "
    "03 02 01 00 "
    "0a 09 01 07 00 20 00 20 01 6a 0b";

GLWASM_Limits loose();

TEST(Policy, PresetsAndLimitOverridesReachTheDecoder) {
  GLWASM_Policy policy;
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy), GLWASM_OK);
  EXPECT_EQ(policy.limits.module_bytes, 4u * 1024u * 1024u);
  EXPECT_FALSE(policy.features & GLWASM_FEATURE_MEMORY64);
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  EXPECT_EQ(policy.limits.module_bytes, 64u * 1024u * 1024u);
  EXPECT_TRUE(policy.features & GLWASM_FEATURE_MEMORY64);

  policy.features = 0;
  policy.limits = loose();
  std::vector<uint8_t> bytes = hex(k_add);
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(bytes.data(), bytes.size(),
      &policy, nullptr, nullptr, &module), GLWASM_OK);
  glwasm_module_destroy(module);
  module = nullptr;

  policy.limits.module_bytes = 1;
  EXPECT_EQ(glwasm_module_decode_with_policy(bytes.data(), bytes.size(),
      &policy, nullptr, nullptr, &module), GLWASM_ERR_LIMIT);
  EXPECT_EQ(module, nullptr);

  const std::vector<uint8_t> memory64 = hex(
      "00 61 73 6d 01 00 00 00 05 04 01 05 01 02");
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy), GLWASM_OK);
  EXPECT_EQ(glwasm_module_decode_with_policy(memory64.data(), memory64.size(),
      &policy, nullptr, nullptr, &module), GLWASM_ERR_UNSUPPORTED);
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  EXPECT_EQ(glwasm_module_decode_with_policy(memory64.data(), memory64.size(),
      &policy, nullptr, nullptr, &module), GLWASM_OK);
  if (module) glwasm_module_destroy(module);
}

GLWASM_Limits loose() {
  GLWASM_Limits limits;

  glwasm_limits_default(&limits);
  limits.module_bytes = UINT64_MAX;
  limits.types = 100000u;
  limits.imports = 100000u;
  limits.exports = 100000u;
  limits.functions = 100000u;
  limits.globals = 100000u;
  limits.tables = 100000u;
  limits.memories = 100000u;
  limits.tags = 100000u;
  limits.data_segments = 100000u;
  limits.element_segments = 100000u;
  limits.locals = 100000u;
  limits.instructions = 100000u;
  limits.nesting = 100000u;
  limits.br_table = 100000u;
  limits.name_length = 100000u;
  limits.table_size = UINT64_MAX;
  limits.memory_maximum = UINT64_MAX;
  limits.memory_initial = UINT64_MAX;
  return limits;
}

struct Count {
  uint64_t max_request;
  uint64_t beside;
  int violated;
  int calls;
  int live;
  uint32_t cap;
  size_t elem;
  int any;
};

struct Hdr {
  size_t size;
};

/* One instance of a different module object. A vector of the watched
 * element is never this size unless it is a single element, which is
 * within every cap these tests plant. */
int single_object(size_t bytes) {
  const size_t known[] = {
    sizeof(GLWASM_Module), sizeof(GLWASM_TypeDef), sizeof(GLWASM_Func),
    sizeof(GLWASM_Table), sizeof(GLWASM_Memory), sizeof(GLWASM_Global),
    sizeof(GLWASM_Tag), sizeof(GLWASM_Import), sizeof(GLWASM_Export),
    sizeof(GLWASM_Elem), sizeof(GLWASM_Data), sizeof(GLWASM_Instr),
    sizeof(GLWASM_BlockType), sizeof(GLWASM_Catch),
  };
  size_t i;

  for (i = 0; i < sizeof(known) / sizeof(known[0]); i++) {
    if (bytes == known[i]) {
      return 1;
    }
  }
  return 0;
}

void note_bytes(Count * c, size_t bytes) {
  if (bytes > c->max_request) {
    c->max_request = bytes;
  }
  if (bytes != sizeof(GLWASM_Module) && bytes > c->beside) {
    c->beside = bytes;
  }
  c->calls++;
  if (c->any) {
    c->violated = 1;
    return;
  }
  if (single_object(bytes)) {
    return;
  }
  if (c->elem == 1 && bytes > c->cap) {
    c->violated = 1;
    return;
  }
  if (c->elem > 1 && bytes > static_cast<uint64_t>(c->cap) * c->elem &&
      bytes % c->elem == 0 && bytes / c->elem > c->cap) {
    c->violated = 1;
  }
}

void note_items(Count * c, size_t nitems, size_t size) {
  uint64_t bytes = static_cast<uint64_t>(nitems) * size;

  if (bytes > c->max_request) {
    c->max_request = bytes;
  }
  if (bytes != sizeof(GLWASM_Module) && bytes > c->beside) {
    c->beside = bytes;
  }
  c->calls++;
  if (c->any) {
    c->violated = 1;
    return;
  }
  if (c->elem != 0 && size == c->elem && nitems > c->cap) {
    c->violated = 1;
  }
}

void * count_malloc(void * ctx, size_t size) {
  Count * c = static_cast<Count *>(ctx);
  Hdr * h;

  note_bytes(c, size == 0 ? 1 : size);
  h = static_cast<Hdr *>(std::malloc(sizeof(Hdr) + (size == 0 ? 1 : size)));
  if (!h) {
    return NULL;
  }
  h->size = size;
  c->live++;
  return h + 1;
}

void * count_calloc(void * ctx, size_t nitems, size_t size) {
  Count * c = static_cast<Count *>(ctx);
  size_t bytes;
  Hdr * h;

  if (nitems != 0 && size > SIZE_MAX / nitems) {
    return NULL;
  }
  note_items(c, nitems, size);
  bytes = nitems * size;
  h = static_cast<Hdr *>(std::malloc(sizeof(Hdr) + (bytes == 0 ? 1 : bytes)));
  if (!h) {
    return NULL;
  }
  std::memset(h + 1, 0, bytes == 0 ? 1 : bytes);
  h->size = bytes;
  c->live++;
  return h + 1;
}

void * count_realloc(void * ctx, void * ptr, size_t size) {
  Count * c = static_cast<Count *>(ctx);
  Hdr * h;
  Hdr * n;

  if (!ptr) {
    return count_malloc(ctx, size);
  }
  note_bytes(c, size == 0 ? 1 : size);
  h = static_cast<Hdr *>(ptr) - 1;
  n = static_cast<Hdr *>(std::realloc(h, sizeof(Hdr) + (size == 0 ? 1 : size)));
  if (!n) {
    return NULL;
  }
  n->size = size;
  return n + 1;
}

void count_free(void * ctx, void * ptr) {
  Count * c = static_cast<Count *>(ctx);

  if (!ptr) {
    return;
  }
  c->live--;
  std::free(static_cast<Hdr *>(ptr) - 1);
}

GLWASM_Allocator counter_alloc(Count * c) {
  GLWASM_Allocator a;

  a.ctx = c;
  a.malloc_fn = count_malloc;
  a.calloc_fn = count_calloc;
  a.realloc_fn = count_realloc;
  a.free_fn = count_free;
  return a;
}

void set_cap(GLWASM_Limits * limits, const char * field, uint64_t cap) {
  if (std::strcmp(field, "module_bytes") == 0) {
    limits->module_bytes = cap;
  } else if (std::strcmp(field, "types") == 0) {
    limits->types = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "imports") == 0) {
    limits->imports = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "exports") == 0) {
    limits->exports = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "functions") == 0) {
    limits->functions = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "globals") == 0) {
    limits->globals = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "tables") == 0) {
    limits->tables = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "memories") == 0) {
    limits->memories = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "tags") == 0) {
    limits->tags = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "data_segments") == 0) {
    limits->data_segments = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "element_segments") == 0) {
    limits->element_segments = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "locals") == 0) {
    limits->locals = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "instructions") == 0) {
    limits->instructions = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "nesting") == 0) {
    limits->nesting = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "br_table") == 0) {
    limits->br_table = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "name_length") == 0) {
    limits->name_length = static_cast<uint32_t>(cap);
  } else if (std::strcmp(field, "table_size") == 0) {
    limits->table_size = cap;
  } else if (std::strcmp(field, "memory_maximum") == 0) {
    limits->memory_maximum = cap;
  } else if (std::strcmp(field, "memory_initial") == 0) {
    limits->memory_initial = cap;
  }
}

struct LimitCase {
  const char * field;
  uint64_t cap;
  size_t elem;
  int any;
  const char * module;
};

}  // namespace

TEST(Decode, MinimalModule) {
  std::vector<uint8_t> bytes = hex(k_add);
  GLWASM_Module * module = NULL;
  GLWASM_Failure failure;
  GLWASM_Result result;

  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL,
      &failure, &module);
  EXPECT_EQ(result, GLWASM_OK);
  EXPECT_EQ(failure.result, GLWASM_OK);
  EXPECT_EQ(failure.limit, nullptr);
  ASSERT_NE(module, nullptr);
  EXPECT_EQ(glwasm_module_dump(module, stdout), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Decode, TranslationOffsetsAreTotalMonotonicAndInRange) {
  std::vector<uint8_t> bytes = hex(k_add);
  GLWASM_Module * module = NULL;
  ASSERT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_OK);
  ASSERT_NE(module, nullptr);
  ASSERT_EQ(module->nfuncs, 1u);
  const GLWASM_TranslatedFunc * translated = module->funcs[0].translated;
  ASSERT_NE(translated, nullptr);
  ASSERT_EQ(translated->offset_count, translated->nins + 1u);
  ASSERT_EQ(translated->nins, module->funcs[0].nins);
  for (uint32_t i = 0; i < translated->offset_count; ++i) {
    EXPECT_LT(translated->offsets[i], bytes.size());
    if (i != 0) {
      EXPECT_LE(translated->offsets[i - 1], translated->offsets[i]);
    }
  }
  for (uint32_t i = 0; i < translated->nins; ++i) {
    EXPECT_EQ(translated->ins[i].source.source_offset,
        translated->offsets[i]);
  }
  glwasm_module_destroy(module);
}

TEST(Decode, TranslationResolvesBlockBranchAfterEnd) {
  static const char wat[] =
      "(module (func (block (br 0))))";
  GLWASM_Module * module = NULL;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(wat), sizeof(wat) - 1u, NULL, NULL,
      &error, &module), GLWASM_OK) << (error.message ? error.message : "");
  ASSERT_NE(module, nullptr);
  const GLWASM_TranslatedFunc * translated = module->funcs[0].translated;
  ASSERT_NE(translated, nullptr);
  uint32_t branch = translated->nins;
  uint32_t block_end = translated->nins;
  for (uint32_t i = 0; i < translated->nins; ++i) {
    if (translated->ins[i].source.op == 0x0c) branch = i;
    if (translated->ins[i].source.op == 0x0b && i != translated->nins - 1u)
      block_end = i;
  }
  ASSERT_LT(branch, translated->nins);
  ASSERT_LT(block_end, translated->nins);
  EXPECT_EQ(translated->ins[branch].branch_target, block_end + 1u);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeExecutesNumericFunctionOnGuestStack) {
  static const char wat[] =
      "(module (func (export \"run\") (result i32) "
      "(i32.add (i32.const 19) (i32.const 23))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeRunsStartAndUsesPortableArithmeticShift) {
  static const char wat[] =
      "(module (global $state (mut i32) (i32.const 0)) "
      "(func $init (global.set $state (i32.const 42))) (start $init) "
      "(func (export \"run\") (result i32) "
      "(i32.shr_s (i32.const -8) (i32.const 1))) "
      "(func (export \"state\") (result i32) (global.get $state)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, UINT32_C(0xfffffffc));
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 2, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeRejectsMultiResultCalleeAndClearsOutputOnFailure) {
  static const char wat[] =
      "(module (func (result i32 i32) (i32.const 1) (i32.const 2)) "
      "(func (export \"run\") (result i32) (drop (call 0))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &value),
      GLWASM_ERR_UNSUPPORTED);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);

  runtime = reinterpret_cast<GLWASM_Runtime *>(1);
  EXPECT_EQ(glwasm_runtime_create(nullptr, &runtime), GLWASM_ERR_INVALID);
  EXPECT_EQ(runtime, nullptr);
}

TEST(Decode, RuntimeRejectsUnboundImportedGlobals) {
  static const char wat[] =
      "(module (import \"other\" \"global_i32\" (global i32)) "
      "(func (export \"run\") (result i32) (global.get 0)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  EXPECT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(runtime, nullptr);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeUsesConfiguredGuestDepthAndMovingStack) {
  static const char wat[] =
      "(module (func (export \"run\") (call 0)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(runtime, UINT64_MAX, 3, 1), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &value), GLWASM_ERR_LIMIT);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeReportsMappedWasmTrapKind) {
  static const char wat[] =
      "(module (func (export \"run\") (result i32) "
      "(i32.div_s (i32.const 1) (i32.const 0))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &value), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_DIVIDE_BY_ZERO);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimePausesAndResumesAtSavedInstruction) {
  static const char wat[] =
      "(module (func (export \"run\") (result i32) "
      "(i32.add (i32.const 19) (i32.const 23))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_request_test_pause(runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &value), GLWASM_ERR_PAUSED);
  int paused = 0;
  EXPECT_EQ(glwasm_runtime_resume(runtime, &value, &paused), GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(value, 42u);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime), module->funcs[0].nins);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeExecutesLocalsAndDirectGuestCalls) {
  static const char wat[] =
      "(module (func (result i32) (i32.const 21)) "
      "(func (export \"run\") (result i32) (local i32) "
      "(local.set 0 (call 0)) (i32.add (local.get 0) (local.get 0))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeExecutesGlobalAndIfElse) {
  static const char wat[] =
      "(module (global i32 (i32.const 40)) "
      "(func (export \"run\") (result i32) "
      "(if (result i32) (i32.const 1) "
      "(then (i32.add (global.get 0) (i32.const 2))) "
      "(else (i32.const 0)))))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeExecutesCallIndirect) {
  static const char wat[] =
      "(module (type (func (result i32))) (table 1 funcref) "
      "(func (result i32) (i32.const 42)) "
      "(func (export \"run\") (result i32) "
      "(call_indirect (type 0) (i32.const 0))) "
      "(elem (i32.const 0) func 0))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, RuntimeMatchesStructuralIndirectTypesAndRecoversAfterTrap) {
  static const char wat[] =
      "(module (type $T (func (result i32))) "
      "(type $U (func (result i32))) "
      "(type $I64 (func (result i64))) (table 1 funcref) "
      "(func (type $I64) (i64.const 9)) "
      "(func (export \"bad\") (type $T) "
      "(call_indirect (type $T) (i32.const 0))) "
      "(func (export \"good\") (type $U) (i32.const 42)) "
      "(elem (i32.const 0) func 0))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &value),
            GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),
      GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 2, nullptr, 0, &value), GLWASM_OK);
  EXPECT_EQ(value, 42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, WatTypeUseOffsetsLocalsPastParameters) {
  static const char wat[] =
      "(module (type $sig (func (param i32) (result i32))) "
      "(func (export \"run\") (type $sig) (local $zero i32) "
      "(local.get $zero)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(wat),
      sizeof(wat)-1, nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  EXPECT_EQ(module->types[module->funcs[0].type_index].nparams, 1u);
  EXPECT_EQ(module->funcs[0].type_index, 0u);
  ASSERT_GE(module->ntypes, 1u);
  EXPECT_EQ(module->funcs[0].ins[0].a, 1u);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  const uint64_t arg=42;uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime,0,&arg,1,&value),GLWASM_OK);
  EXPECT_EQ(value,0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(Decode, TranslationResolvesIfElseAndLoopDestinations) {
  static const char wat[] =
      "(module (func (param i32) (result i32) "
      "(block (loop (if (local.get 0) (then (br 1)) (else (br 0))))) "
      "(i32.const 7)))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(wat), sizeof(wat) - 1u, nullptr,
      nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  const auto * translated = module->funcs[0].translated;
  ASSERT_NE(translated, nullptr);
  uint32_t if_index = UINT32_MAX;
  uint32_t else_index = UINT32_MAX;
  uint32_t loop_index = UINT32_MAX;
  uint32_t outer_block_index = UINT32_MAX;
  uint32_t branch_to_block = UINT32_MAX;
  uint32_t branch_to_loop = UINT32_MAX;
  uint32_t outer_block_end = UINT32_MAX;
  for (uint32_t i = 0; i < translated->nins; ++i) {
    switch (translated->ins[i].source.op) {
      case 0x02: outer_block_index = i; break;
      case 0x03: loop_index = i; break;
      case 0x04: if_index = i; break;
      case 0x05: else_index = i; break;
      case 0x0c:
        if (branch_to_block == UINT32_MAX) branch_to_block = i;
        else branch_to_loop = i;
        break;
      default: break;
    }
  }
  ASSERT_NE(if_index, UINT32_MAX);
  ASSERT_NE(else_index, UINT32_MAX);
  ASSERT_NE(loop_index, UINT32_MAX);
  ASSERT_NE(outer_block_index, UINT32_MAX);
  ASSERT_NE(branch_to_block, UINT32_MAX);
  ASSERT_NE(branch_to_loop, UINT32_MAX);
  uint32_t block_depth = 0;
  for (uint32_t i = outer_block_index; i < translated->nins; ++i) {
    const uint16_t op = translated->ins[i].source.op;
    if (op == 0x02 || op == 0x03 || op == 0x04) ++block_depth;
    else if (op == 0x0b && --block_depth == 0) {
      outer_block_end = i;
      break;
    }
  }
  ASSERT_NE(outer_block_end, UINT32_MAX);
  EXPECT_EQ(translated->ins[if_index].branch_target, else_index + 1u);
  EXPECT_EQ(translated->ins[else_index].branch_target, UINT32_MAX);
  EXPECT_EQ(translated->ins[branch_to_block].branch_target, loop_index + 1u);
  EXPECT_GT(translated->ins[branch_to_loop].branch_target, else_index);
  EXPECT_LT(translated->ins[branch_to_loop].branch_target,
      translated->nins);
  glwasm_module_destroy(module);
}

TEST(Decode, TranslationResolvesDirectCallAndBranchTableTargets) {
  static const char wat[] =
      "(module (func (result i32) (i32.const 9)) "
      "(func (param i32) (result i32) "
      "(block (block (local.get 0) (br_table 0 1))) (i32.const 0)) "
      "(func (result i32) (call 0)))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(wat), sizeof(wat) - 1u, nullptr,
      nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  const auto * table_fn = module->funcs[1].translated;
  const auto * call_fn = module->funcs[2].translated;
  ASSERT_NE(table_fn, nullptr);
  ASSERT_NE(call_fn, nullptr);
  uint32_t table_op = UINT32_MAX;
  uint32_t call_op = UINT32_MAX;
  for (uint32_t i = 0; i < table_fn->nins; ++i)
    if (table_fn->ins[i].source.op == 0x0e) table_op = i;
  for (uint32_t i = 0; i < call_fn->nins; ++i)
    if (call_fn->ins[i].source.op == 0x10) call_op = i;
  ASSERT_NE(table_op, UINT32_MAX);
  ASSERT_NE(call_op, UINT32_MAX);
  EXPECT_EQ(table_fn->branch_table_count, 2u);
  const auto & branch = table_fn->ins[table_op];
  ASSERT_LT(branch.alternate_target + 1u, table_fn->branch_table_count + 1u);
  EXPECT_LT(table_fn->branch_table_targets[branch.alternate_target],
      table_fn->branch_table_targets[branch.alternate_target + 1u]);
  EXPECT_EQ(branch.branch_target,
      table_fn->branch_table_targets[branch.alternate_target + 1u]);
  EXPECT_EQ(call_fn->ins[call_op].call_target, 0u);
  glwasm_module_destroy(module);
}

TEST(Decode, EmptyModule) {
  std::vector<uint8_t> bytes = hex(k_empty);
  GLWASM_Module * module = NULL;

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Decode, BadMagicIsCorrupt) {
  std::vector<uint8_t> bytes = hex("00 00 00 00 01 00 00 00");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Failure failure;

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL,
      &failure, &module), GLWASM_ERR_CORRUPT);
  EXPECT_EQ(failure.result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, TruncatedIsCorrupt) {
  std::vector<uint8_t> bytes = hex("00 61 73 6d");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, NullBufferIsInvalid) {
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Failure failure;

  EXPECT_EQ(glwasm_module_decode(NULL, 1, NULL, NULL, &failure, &module),
      GLWASM_ERR_INVALID);
  EXPECT_EQ(failure.result, GLWASM_ERR_INVALID);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
  EXPECT_EQ(glwasm_module_decode(NULL, 0, NULL, NULL, NULL, NULL),
      GLWASM_ERR_INVALID);
}

TEST(Decode, TypedFuncrefIsUnsupported) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 01 06 01 60 01 64 00 00");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Failure failure;
  GLWASM_Result result;

  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL,
      &failure, &module);
  EXPECT_EQ(result, GLWASM_ERR_UNSUPPORTED);
  EXPECT_NE(result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, V128IsUnsupported) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 01 05 01 60 01 7b 00");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Result result;

  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module);
  EXPECT_EQ(result, GLWASM_ERR_UNSUPPORTED);
  EXPECT_NE(result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, FuncrefIsInScope) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 01 05 01 60 01 70 00");
  GLWASM_Module * module = NULL;

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_OK);
  glwasm_module_destroy(module);
}

TEST(Decode, ExplicitNonNullAbstractHeapTypeIsDeferred) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00"
      "01 06 01 60 01 64 70 00"
      "03 02 01 00"
      "0a 07 01 05 00 20 00 1a 0b");
  GLWASM_Module * module = NULL;

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(module, nullptr);
}

TEST(Decode, DefaultsMatchTheNumberedCaps) {
  GLWASM_Limits limits;

  EXPECT_EQ(glwasm_limits_default(NULL), GLWASM_ERR_INVALID);
  ASSERT_EQ(glwasm_limits_default(&limits), GLWASM_OK);
  EXPECT_EQ(limits.module_bytes, 4ull * 1024ull * 1024ull);
  EXPECT_EQ(limits.types, 10000u);
  EXPECT_EQ(limits.imports, 1000u);
  EXPECT_EQ(limits.exports, 1000u);
  EXPECT_EQ(limits.functions, 10000u);
  EXPECT_EQ(limits.globals, 1000u);
  EXPECT_EQ(limits.tables, 100u);
  EXPECT_EQ(limits.memories, 100u);
  EXPECT_EQ(limits.tags, 1000u);
  EXPECT_EQ(limits.data_segments, 10000u);
  EXPECT_EQ(limits.element_segments, 10000u);
  EXPECT_EQ(limits.locals, 1000u);
  EXPECT_EQ(limits.instructions, 100000u);
  EXPECT_EQ(limits.nesting, 128u);
  EXPECT_EQ(limits.br_table, 10000u);
  EXPECT_EQ(limits.name_length, 1024u);
  EXPECT_EQ(limits.table_size, 1000000ull);
  EXPECT_EQ(limits.memory_maximum, 64ull * 1024ull * 1024ull);
  EXPECT_EQ(limits.memory_initial, 64ull * 1024ull * 1024ull);
}

TEST(Decode, ResultStringsCoverTheEnum) {
  int i;

  for (i = 0; i < GLWASM_RESULT_COUNT; i++) {
    const char * s = glwasm_result_string(static_cast<GLWASM_Result>(i));

    ASSERT_NE(s, nullptr);
    EXPECT_STRNE(s, "Unknown error");
  }
  EXPECT_STREQ(glwasm_result_string(GLWASM_ERR_LIMIT), "Limit exceeded");
  EXPECT_STREQ(glwasm_result_string(GLWASM_ERR_VALIDATE), "Validation failed");
  EXPECT_STREQ(glwasm_result_string(GLWASM_ERR_PAUSED), "Guest paused");
  EXPECT_STREQ(glwasm_result_string(GLWASM_RESULT_COUNT), "Unknown error");
  EXPECT_NE(glwasm_version_string(), nullptr);
  EXPECT_EQ(glwasm_version_number(), GLWASM_VERSION_NUMBER);
}

TEST(Decode, OnePastEachLimit) {
  const LimitCase cases[] = {
    {"module_bytes", 7, 0, 1, k_empty},
    {"types", 1, sizeof(GLWASM_TypeDef), 0,
        "00 61 73 6d 01 00 00 00 01 07 02 60 00 00 60 00 00"},
    {"imports", 1, sizeof(GLWASM_Import), 0,
        "00 61 73 6d 01 00 00 00 "
        "01 04 01 60 00 00 "
        "02 0d 02 01 61 01 61 00 00 01 61 01 62 00 00"},
    {"exports", 1, sizeof(GLWASM_Export), 0,
        "00 61 73 6d 01 00 00 00 07 01 02"},
    {"functions", 1, sizeof(GLWASM_Func), 0,
        "00 61 73 6d 01 00 00 00 01 04 01 60 00 00 03 03 02 00 00"},
    {"globals", 1, sizeof(GLWASM_Global), 0,
        "00 61 73 6d 01 00 00 00 "
        "06 0b 02 7f 00 41 00 0b 7f 00 41 00 0b"},
    {"tables", 1, sizeof(GLWASM_Table), 0,
        "00 61 73 6d 01 00 00 00 04 07 02 70 00 00 70 00 00"},
    {"memories", 1, sizeof(GLWASM_Memory), 0,
        "00 61 73 6d 01 00 00 00 05 07 02 01 01 01 01 01 01"},
    {"tags", 1, sizeof(GLWASM_Tag), 0,
        "00 61 73 6d 01 00 00 00 01 04 01 60 00 00 0d 05 02 00 00 00 00"},
    {"data_segments", 1, sizeof(GLWASM_Data), 0,
        "00 61 73 6d 01 00 00 00 0b 05 02 01 00 01 00"},
    {"element_segments", 1, sizeof(GLWASM_Elem), 0,
        "00 61 73 6d 01 00 00 00 09 07 02 01 00 00 01 00 00"},
    {"locals", 1, sizeof(GLWASM_ValType), 0,
        "00 61 73 6d 01 00 00 00 "
        "01 04 01 60 00 00 03 02 01 00 "
        "0a 06 01 04 01 02 7f 0b"},
    {"instructions", 1, sizeof(GLWASM_Instr), 0,
        "00 61 73 6d 01 00 00 00 "
        "01 04 01 60 00 00 03 02 01 00 "
        "0a 05 01 03 00 01 0b"},
    {"nesting", 1, 1, 0,
        "00 61 73 6d 01 00 00 00 "
        "01 04 01 60 00 00 03 02 01 00 "
        "0a 07 01 05 00 02 40 0b 0b"},
    {"br_table", 1, sizeof(uint32_t), 0,
        "00 61 73 6d 01 00 00 00 "
        "01 04 01 60 00 00 03 02 01 00 "
        "0a 08 01 06 00 0e 01 00 00 0b"},
    {"name_length", 1, 1, 0,
        "00 61 73 6d 01 00 00 00 07 02 01 02"},
    {"table_size", 1, 0, 0,
        "00 61 73 6d 01 00 00 00 04 04 01 70 00 02"},
    {"memory_maximum", 65536, 0, 0,
        "00 61 73 6d 01 00 00 00 05 04 01 01 02 02"},
    {"memory_initial", 65536, 0, 0,
        "00 61 73 6d 01 00 00 00 05 04 01 01 02 02"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    const LimitCase & k = cases[i];
    std::vector<uint8_t> bytes = hex(k.module);
    GLWASM_Limits limits = loose();
    Count count;
    GLWASM_Allocator alloc;
    GLWASM_Failure failure;
    GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
    GLWASM_Result result;

    SCOPED_TRACE(k.field);
    set_cap(&limits, k.field, k.cap);
    std::memset(&count, 0, sizeof(count));
    count.cap = k.cap > 0xffffffffull ? 0xffffffffu
        : static_cast<uint32_t>(k.cap);
    count.elem = k.elem;
    count.any = k.any;
    alloc = counter_alloc(&count);
    result = glwasm_module_decode(bytes.data(), bytes.size(), &limits, &alloc,
        &failure, &module);
    EXPECT_EQ(result, GLWASM_ERR_LIMIT);
    EXPECT_EQ(failure.result, GLWASM_ERR_LIMIT);
    EXPECT_STREQ(failure.limit, k.field);
    EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
    EXPECT_EQ(count.violated, 0);
    EXPECT_EQ(count.live, 0);
    if (k.any) {
      EXPECT_EQ(count.max_request, 0ull);
      EXPECT_EQ(count.calls, 0);
    }
    if (std::strcmp(k.field, "table_size") == 0 ||
        std::strcmp(k.field, "memory_maximum") == 0 ||
        std::strcmp(k.field, "memory_initial") == 0) {
      uint64_t illegal = std::strcmp(k.field, "table_size") == 0
          ? 2ull : 2ull * 65536ull;
      uint64_t requested = count.max_request == sizeof(GLWASM_Module)
          ? count.beside : count.max_request;

      EXPECT_LT(requested, illegal);
    }
  }
}

TEST(Decode, OverlongSectionCountIsCorrupt) {
  /* Six bytes for a u32 count is longer than ceil(32/7) = 5 allows. A padded
   * count of five bytes or fewer is legal (test_leb.cpp). */
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 01 07 80 80 80 80 80 00");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, TableSharedFlagIsCorrupt) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 04 05 01 70 03 00 01");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Result result;

  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module);
  EXPECT_EQ(result, GLWASM_ERR_CORRUPT);
  EXPECT_NE(result, GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, EmptyBufferIsCorrupt) {
  unsigned char byte = 0;
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(&byte, 0, NULL, NULL, NULL, &module),
      GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, BadVersionIsCorrupt) {
  std::vector<uint8_t> bytes = hex("00 61 73 6d 02 00 00 00");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, LegacyTryIsUnsupported) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 "
      "01 04 01 60 00 00 "
      "03 02 01 00 "
      "0a 04 01 02 00 06");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Result result;

  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module);
  EXPECT_EQ(result, GLWASM_ERR_UNSUPPORTED);
  EXPECT_NE(result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, ZeroTypesRefusesOneType) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 01 04 01 60 00 00");
  GLWASM_Limits limits = loose();
  GLWASM_Failure failure;
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  limits.types = 0;
  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), &limits, NULL,
      &failure, &module), GLWASM_ERR_LIMIT);
  EXPECT_STREQ(failure.limit, "types");
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, UnboundedMemoryHitsTheMaximum) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 05 03 01 00 01");
  GLWASM_Failure failure;
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL,
      &failure, &module), GLWASM_ERR_LIMIT);
  EXPECT_STREQ(failure.limit, "memory_maximum");
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, LocalIndexSpaceOverflowIsMalformed) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 "
      "01 06 01 60 02 7f 7f 00 "
      "03 02 01 00 "
      "0a 0a 01 08 01 ff ff ff ff 0f 7f 0b");
  GLWASM_Limits limits = loose();
  GLWASM_Failure failure{};
  GLWASM_Module * module = nullptr;
  limits.locals = UINT32_MAX;
  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), &limits, nullptr,
                                 &failure, &module),
            GLWASM_ERR_CORRUPT);
  EXPECT_EQ(failure.result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, nullptr);
}

TEST(Decode, DataInstructionsRequireDataCountSection) {
  const char * cases[] = {
      "00 61 73 6d 01 00 00 00 "
      "01 07 01 60 03 7f 7f 7f 00 "
      "03 02 01 00 "
      "0a 0e 01 0c 00 41 00 41 00 41 00 fc 08 00 00 0b",
      "00 61 73 6d 01 00 00 00 "
      "01 04 01 60 00 00 "
      "03 02 01 00 "
      "0a 07 01 05 00 fc 09 00 0b"};
  for (const char * encoded : cases) {
    std::vector<uint8_t> bytes = hex(encoded);
    GLWASM_Failure failure{};
    GLWASM_Module * module = nullptr;
    EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), nullptr, nullptr,
                                   &failure, &module),
              GLWASM_ERR_CORRUPT);
    EXPECT_EQ(failure.result, GLWASM_ERR_CORRUPT);
    EXPECT_EQ(module, nullptr);
  }
}

TEST(Decode, MemoryMinimumAboveMaximum) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 05 04 01 01 02 01");
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);

  EXPECT_EQ(glwasm_module_decode(bytes.data(), bytes.size(), NULL, NULL, NULL,
      &module), GLWASM_ERR_VALIDATE);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
}

TEST(Decode, CorruptTypeFreesPartial) {
  std::vector<uint8_t> bytes = hex(
      "00 61 73 6d 01 00 00 00 "
      "01 07 01 60 02 7f 7f 01 00 20 01 6a 0b");
  Count count;
  GLWASM_Allocator alloc;
  GLWASM_Failure failure;
  GLWASM_Module * module = reinterpret_cast<GLWASM_Module *>(1);
  GLWASM_Result result;

  std::memset(&count, 0, sizeof(count));
  alloc = counter_alloc(&count);
  result = glwasm_module_decode(bytes.data(), bytes.size(), NULL, &alloc,
      &failure, &module);
  EXPECT_EQ(result, GLWASM_ERR_CORRUPT);
  EXPECT_EQ(module, reinterpret_cast<GLWASM_Module *>(1));
  EXPECT_EQ(count.live, 0);
}

int main(int argc, char * * argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
