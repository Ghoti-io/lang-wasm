/*
 * SPDX-License-Identifier: LGPL-3.0-only
 */

#include "../../src/jit/cache_internal.h"
#include "../../src/jit/artifact_internal.h"
#include "../../src/module_internal.h"
#include "../../src/runtime/runtime.h"
#include "../../src/runtime/runtime_layout.h"

#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/code_pages.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/lang-wasm/host.h>

#include <gtest/gtest.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <thread>

namespace {

void release_payload(void *) {}

GLWASM_Result jit_test_host_call(const GLWASM_HostCall *call,
    uint64_t *result) {
  if (!call || !call->user || !result) return GLWASM_ERR_INTERNAL;
  auto *calls = static_cast<uint32_t *>(call->user);
  ++*calls;
  *result = 42;
  return GLWASM_OK;
}

GLWASM_Result jit_test_host_narrow_bits(const GLWASM_HostCall *call,
    uint64_t *result) {
  if (!call || !result || call->argument_count != 2u || !call->types ||
      call->types[0] != GLWASM_HOST_I32 ||
      call->types[1] != GLWASM_HOST_F32 || !call->arguments ||
      call->arguments[0] != 7u ||
      call->arguments[1] != UINT64_C(0x3f800000))
    return GLWASM_ERR_INTERNAL;
  *result = 42u;
  return GLWASM_OK;
}

struct JitHostPauseState { uint32_t initial = 0; uint32_t resumes = 0; };
GLWASM_Result jit_test_host_pause(const GLWASM_HostCall *call, uint64_t *) {
  if (!call || !call->user) return GLWASM_ERR_INTERNAL;
  ++static_cast<JitHostPauseState *>(call->user)->initial;
  return GLWASM_ERR_PAUSED;
}
GLWASM_Result jit_test_host_resume(const GLWASM_HostCall *call,
    uint64_t *result) {
  if (!call || !call->user || !result) return GLWASM_ERR_INTERNAL;
  ++static_cast<JitHostPauseState *>(call->user)->resumes;
  *result = 42;
  return GLWASM_OK;
}
GLWASM_Result jit_test_host_error(const GLWASM_HostCall *call, uint64_t *) {
  if (!call || !call->user) return GLWASM_ERR_INTERNAL;
  ++*static_cast<uint32_t *>(call->user);
  return GLWASM_ERR_LINK;
}
GLWASM_Result jit_test_host_multi(const GLWASM_HostCall *call,
    uint64_t *results) {
  if (!call || !call->user || !results || call->result_count != 2 ||
      !call->result_types || call->result_types[0] != GLWASM_HOST_I32 ||
      call->result_types[1] != GLWASM_HOST_I64)
    return GLWASM_ERR_INTERNAL;
  ++*static_cast<uint32_t *>(call->user);
  results[0] = 31;
  results[1] = UINT64_C(0x123456789abcdef0);
  return GLWASM_OK;
}
struct JitHostMultiPauseState { uint32_t initial = 0; uint32_t resumes = 0; };
GLWASM_Result jit_test_host_multi_pause(const GLWASM_HostCall *call,
    uint64_t *) {
  if (!call || !call->user || call->result_count != 2)
    return GLWASM_ERR_INTERNAL;
  ++static_cast<JitHostMultiPauseState *>(call->user)->initial;
  return GLWASM_ERR_PAUSED;
}
GLWASM_Result jit_test_host_multi_resume(const GLWASM_HostCall *call,
    uint64_t *results) {
  if (!call || !call->user || !results || call->result_count != 2)
    return GLWASM_ERR_INTERNAL;
  ++static_cast<JitHostMultiPauseState *>(call->user)->resumes;
  results[0] = 8; results[1] = 13;
  return GLWASM_OK;
}

static const GRCORE_EngineDescriptor external_caller =
    GRCORE_ENGINE_DESCRIPTOR_INIT("wasm-jit-require-test", nullptr, nullptr,
        nullptr, {nullptr, nullptr, nullptr}, {0, 0, 0}, nullptr, nullptr,
        nullptr, nullptr);

struct Fixture {
  GRCORE_CodePages *pages = nullptr;
  GLWASM_JitCache *cache = nullptr;

  bool init() {
    if (grcore_code_pages_create(nullptr, 1024u * 1024u, &pages) != GRCORE_OK)
      return false;
    GLWASM_JitCacheKey key{1u, GRJIT_FEATURES_ALL, 1u};
    return GHOTIIO_LANG_WASM(glwasm_jit_cache_create)(nullptr, key, 1,
        pages, &cache) == GLWASM_OK;
  }
  ~Fixture() {
    GHOTIIO_LANG_WASM(glwasm_jit_cache_destroy)(cache);
    grcore_code_pages_release(pages);
  }
};

TEST(JitCache, GrantsOneCompileClaimAndPublishesOneImmutableHandle) {
  Fixture fixture;
  ASSERT_TRUE(fixture.init());
  GLWASM_JitRefusal reason;
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_OWNER);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_BUSY);

  GRCORE_Code *created = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, nullptr, release_payload, &created),
      GRCORE_OK);
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_publish)(fixture.cache, 0,
      created), GLWASM_OK);
  GRCORE_Code *shared = GHOTIIO_LANG_WASM(glwasm_jit_cache_retain_code)(
      fixture.cache, 0);
  ASSERT_EQ(shared, created);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_READY);
  grcore_code_release(shared);
}

TEST(JitCache, RetiresAtEightGuestVisibleExitsInLatestSixtyFourEntries) {
  Fixture fixture;
  ASSERT_TRUE(fixture.init());
  GLWASM_JitRefusal reason;
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_OWNER);
  GRCORE_Code *created = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, nullptr, release_payload, &created),
      GRCORE_OK);
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_publish)(fixture.cache, 0,
      created), GLWASM_OK);

  for (unsigned i = 0; i < 56; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_RETURN), 0);
  for (unsigned i = 0; i < 7; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_GUARD_DEOPT), 0);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
      0, GLWASM_JIT_EXIT_NAMED_UNSUPPORTED), 1);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_retain_code)(fixture.cache, 0),
      nullptr);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_INTERPRET);
  EXPECT_EQ(reason, GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE);
  GLWASM_JitCacheStats stats{};
  ASSERT_TRUE(GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(fixture.cache, 0,
      &stats));
  EXPECT_EQ(stats.window_entries, 64u);
  EXPECT_EQ(stats.window_exits, 8u);
  EXPECT_EQ(stats.guest_visible_exits, 8u);
  EXPECT_EQ(stats.interpreter_only, 1u);
  GHOTIIO_LANG_WASM(glwasm_jit_cache_finish_retirement)(fixture.cache, 0);
}

TEST(JitCache, ExpiredExitsStopContributingToTheRollingWindow) {
  Fixture fixture;
  ASSERT_TRUE(fixture.init());
  GLWASM_JitRefusal reason;
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_OWNER);
  GRCORE_Code *created = nullptr;
  ASSERT_EQ(grcore_code_create(nullptr, nullptr, release_payload, &created),
      GRCORE_OK);
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_publish)(fixture.cache, 0,
      created), GLWASM_OK);

  for (unsigned i = 0; i < 7; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_GUARD_DEOPT), 0);
  for (unsigned i = 0; i < 57; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_RETURN), 0);

  /* Overwrite the seven exits with completed returns. They must age out. */
  for (unsigned i = 0; i < 7; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_RETURN), 0);
  GLWASM_JitCacheStats stats{};
  ASSERT_TRUE(GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(fixture.cache, 0,
      &stats));
  EXPECT_EQ(stats.window_entries, 64u);
  EXPECT_EQ(stats.window_exits, 0u);
  EXPECT_EQ(stats.interpreter_only, 0u);

  for (unsigned i = 0; i < 7; ++i)
    EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
        0, GLWASM_JIT_EXIT_NAMED_UNSUPPORTED), 0);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_record_entry)(fixture.cache,
      0, GLWASM_JIT_EXIT_GUARD_DEOPT), 1);
  ASSERT_TRUE(GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(fixture.cache, 0,
      &stats));
  EXPECT_EQ(stats.window_exits, 8u);
  EXPECT_EQ(stats.interpreter_only, 1u);
}

TEST(JitCache, CompileRefusalIsStableAndCounted) {
  Fixture fixture;
  ASSERT_TRUE(fixture.init());
  GLWASM_JitRefusal reason;
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_OWNER);
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_refuse)(fixture.cache, 0,
      GLWASM_JIT_REFUSAL_UNSUPPORTED_IR), GLWASM_OK);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_INTERPRET);
  EXPECT_EQ(reason, GLWASM_JIT_REFUSAL_UNSUPPORTED_IR);
  GLWASM_JitCacheStats stats{};
  ASSERT_TRUE(GHOTIIO_LANG_WASM(glwasm_jit_cache_stats)(fixture.cache, 0,
      &stats));
  EXPECT_EQ(stats.compile_attempts, 1u);
  EXPECT_EQ(stats.refusal_counts[GLWASM_JIT_REFUSAL_UNSUPPORTED_IR], 1u);
}

TEST(JitCache, PrefixedOpcodesKeepStableExitCounterIdentity) {
  Fixture fixture;
  ASSERT_TRUE(fixture.init());
  GLWASM_JitRefusal reason;
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_begin)(fixture.cache, 0,
      &reason), GLWASM_JIT_COMPILE_OWNER);
  GHOTIIO_LANG_WASM(glwasm_jit_cache_note_opcode_exit)(fixture.cache, 0,
      UINT16_C(0xfc0a));
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_exit_count)(
      fixture.cache, UINT16_C(0xfc0a)), 1u);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_opcode_exit_count)(
      fixture.cache, UINT16_C(0xfc0b)), 0u);
}

TEST(JitCache, ModuleProviderAndCacheOutliveEitherIndependentRuntime) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  policy.max_jit_code_bytes = grcore_page_provider_default()->page_size;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  ASSERT_NE(module->jit_pages, nullptr);
  ASSERT_NE(module->jit_cache, nullptr);
  GLWASM_JitCache *shared_cache = module->jit_cache;
  GRCORE_CodePages *shared_pages = module->jit_pages;

  GLWASM_Runtime *first = nullptr;
  GLWASM_Runtime *second = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &first), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &second), GLWASM_OK);
  ASSERT_NE(glwasm_runtime_context(first), glwasm_runtime_context(second));
  EXPECT_EQ(module->jit_cache, shared_cache);
  EXPECT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_pages)(shared_cache),
      grcore_code_pages_provider(shared_pages));

  const uint64_t args[] = {10, 32};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(first,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(first, 0, args, 2, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(second, 0, args, 2, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(first, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compile_successes, 1u);
  EXPECT_EQ(stats.compiled_entries, 2u);
  GRCORE_Code *artifact = GHOTIIO_LANG_WASM(
      glwasm_jit_cache_retain_code)(shared_cache, 0);
  ASSERT_NE(artifact, nullptr);
  GRCORE_CodePages *pages_hold = grcore_code_pages_retain(shared_pages);
  ASSERT_GT(grcore_code_pages_in_use(shared_pages), 0u);
  glwasm_runtime_destroy(first);
  EXPECT_EQ(module->jit_cache, shared_cache);
  ASSERT_EQ(glwasm_runtime_invoke(second, 0, args, 2, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  glwasm_runtime_destroy(second);
  glwasm_module_destroy(module);
  EXPECT_GT(grcore_code_pages_in_use(pages_hold), 0u);
  grcore_code_release(artifact);
  EXPECT_EQ(grcore_code_pages_in_use(pages_hold), 0u);
  grcore_code_pages_release(pages_hold);
}

TEST(JitCache, ConcurrentInstancesPublishOneArtifactAndKeepIndependentRuntimes) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  ASSERT_NE(module, nullptr);
  std::atomic<unsigned> ready{0};
  std::atomic<bool> start{false};
  GLWASM_Runtime *runtimes[2] = {nullptr, nullptr};
  GRCORE_Context *contexts[2] = {nullptr, nullptr};
  GLWASM_Result created[2] = {GLWASM_ERR_INTERNAL, GLWASM_ERR_INTERNAL};
  GLWASM_Result results[2] = {GLWASM_ERR_INTERNAL, GLWASM_ERR_INTERNAL};
  uint64_t outputs[2] = {0, 0};
  auto invoke = [&](unsigned index) {
    /* A core context belongs to its creating thread. Create, execute and
     * destroy each runtime on its owner thread while sharing the module. */
    created[index] = glwasm_runtime_create(module, &runtimes[index]);
    if (created[index] == GLWASM_OK)
      contexts[index] = glwasm_runtime_context(runtimes[index]);
    ready.fetch_add(1, std::memory_order_release);
    while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
    if (created[index] == GLWASM_OK) {
      const uint64_t args[] = {20u + index, 22u};
      results[index] = glwasm_runtime_invoke(runtimes[index], 0, args, 2,
          &outputs[index]);
      glwasm_runtime_destroy(runtimes[index]);
      runtimes[index] = nullptr;
    }
  };
  std::thread a(invoke, 0);
  std::thread b(invoke, 1);
  while (ready.load(std::memory_order_acquire) != 2) std::this_thread::yield();
  EXPECT_EQ(created[0], GLWASM_OK);
  EXPECT_EQ(created[1], GLWASM_OK);
  EXPECT_NE(contexts[0], contexts[1]);
  start.store(true, std::memory_order_release);
  a.join();
  b.join();
  EXPECT_EQ(created[0], GLWASM_OK);
  EXPECT_EQ(created[1], GLWASM_OK);
  EXPECT_EQ(results[0], GLWASM_OK);
  EXPECT_EQ(results[1], GLWASM_OK);
  EXPECT_EQ(outputs[0], 42u);
  EXPECT_EQ(outputs[1], 43u);
  GLWASM_JitCacheStats stats{};
  ASSERT_TRUE(glwasm_jit_cache_stats(module->jit_cache, 0, &stats));
  EXPECT_EQ(stats.compile_attempts, 1u);
  EXPECT_EQ(stats.compile_successes, 1u);
  EXPECT_GT(stats.compiled_entries, 0u);

  GRCORE_CodePages *pages = grcore_code_pages_retain(module->jit_pages);
  ASSERT_NE(pages, nullptr);
  EXPECT_GT(grcore_code_pages_in_use(pages), 0u);
  glwasm_module_destroy(module);
  EXPECT_EQ(grcore_code_pages_in_use(pages), 0u);
  grcore_code_pages_release(pages);
}

TEST(JitCache, CodeBudgetRefusalReleasesTemporaryPagesAndIsStable) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  policy.max_jit_code_bytes = 0;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  ASSERT_NE(module, nullptr);
  GRCORE_CodePages *pages = grcore_code_pages_retain(module->jit_pages);
  ASSERT_NE(pages, nullptr);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t args[] = {20, 22};
  uint64_t output = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, args, 2, &output),
      GLWASM_ERR_LIMIT);
  GLWASM_JitCacheStats stats{};
  ASSERT_TRUE(glwasm_jit_cache_stats(module->jit_cache, 0, &stats));
  EXPECT_EQ(stats.compile_attempts, 1u);
  EXPECT_EQ(stats.compile_successes, 0u);
  EXPECT_EQ(stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT], 1u);
  EXPECT_EQ(grcore_code_pages_in_use(pages), 0u);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, args, 2, &output),
      GLWASM_ERR_LIMIT);
  ASSERT_TRUE(glwasm_jit_cache_stats(module->jit_cache, 0, &stats));
  EXPECT_EQ(stats.compile_attempts, 1u);
  EXPECT_EQ(stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT], 1u);
  EXPECT_EQ(grcore_code_pages_in_use(pages), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
  EXPECT_EQ(grcore_code_pages_in_use(pages), 0u);
  grcore_code_pages_release(pages);
}

TEST(JitCache, RuntimePinsModuleAfterCallerReleasesModuleHandle) {
  static const uint8_t source[] =
      "(module (func (export \"run\") (result i32) (i32.const 42)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  /* The instance owns a module reference, including the shared cache and its
   * executable-page group. Dropping the caller's handle cannot invalidate it. */
  glwasm_module_destroy(module);
  module = nullptr;
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
}

TEST(JitCache, InterpreterImplementsValidatedIntegerAndFloatNumericFamilies) {
  static const uint8_t source[] = "(module"
      " (func (export \"rem\") (param i32 i32) (result i32)"
      " (local.get 0) (local.get 1) (i32.rem_s))"
      " (func (export \"rot\") (param i64 i64) (result i64)"
      " (local.get 0) (local.get 1) (i64.rotl))"
      " (func (export \"nan_ne\") (result i32)"
      " (f32.const nan) (f32.const 1) (f32.ne))"
      " (func (export \"nearest\") (result f64)"
      " (f64.const 2.5) (f64.nearest))"
      " (func (export \"sat\") (result i64)"
      " (f64.const nan) (i64.trunc_sat_f64_u))"
      " (func (export \"trunc_trap\") (result i32)"
      " (f64.const nan) (i32.trunc_f64_s))"
      " (func (export \"div_overflow\") (result i32)"
      " (i32.const -2147483648) (i32.const -1) (i32.div_s))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t args[] = {UINT32_C(0xfffffff8), 3};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "rem", args, 2, &result),
      GLWASM_OK);
  EXPECT_EQ(static_cast<uint32_t>(result), UINT32_C(0xfffffffe));
  args[0] = UINT64_C(0x8000000000000001); args[1] = 1;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "rot", args, 2, &result),
      GLWASM_OK);
  EXPECT_EQ(result, UINT64_C(0x0000000000000003));
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "nan_ne", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "nearest", nullptr, 0,
      &result), GLWASM_OK);
  double nearest = 0;
  std::memcpy(&nearest, &result, sizeof(nearest));
  EXPECT_EQ(nearest, 2.0);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "sat", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 0u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "trunc_trap", nullptr, 0,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_INTEGER_OVERFLOW);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "div_overflow", nullptr, 0,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_INTEGER_OVERFLOW);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, InterpreterCarriesMultiValueAcrossCallsTailCallsPauseAndSnapshot) {
  static const uint8_t source[] = "(module"
      " (func $pair (result i32 i64) (i32.const 7) (i64.const 99))"
      " (func (export \"call\") (result i32 i64) (call $pair))"
      " (func (export \"tail\") (result i32 i64) (return_call $pair))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t results[2] = {};
  uint32_t too_small = 1;
  EXPECT_EQ(glwasm_runtime_invoke_export_results(runtime, "call", nullptr, 0,
      results, &too_small), GLWASM_ERR_LIMIT);
  EXPECT_EQ(too_small, 2u);
  uint32_t count = 2;
  ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, "call", nullptr, 0,
      results, &count), GLWASM_OK);
  EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 7u); EXPECT_EQ(results[1], 99u);
  count = 2;
  GLWASM_Result tail_result = glwasm_runtime_invoke_export_results(runtime,
      "tail", nullptr, 0, results, &count);
  if (tail_result != GLWASM_OK) {
    GLWASM_TestRuntimeFailureInfo failure{};
    if (glwasm_runtime_test_failure_info(runtime, 2, &failure) == GLWASM_OK) {
      std::fprintf(stderr, "interpreter tail failure: result=%d run=%d "
          "core=%d outcome=%d function=%u pc=%u opcode=%u reason=%u "
          "call=%u sp=%llu base=%llu required=%llu\n",
          static_cast<int>(tail_result), static_cast<int>(failure.run_error),
          static_cast<int>(failure.core_result),
          static_cast<int>(failure.outcome), failure.last_function,
          failure.last_pc, failure.last_opcode, failure.diagnostic_reason,
          failure.call_target, static_cast<unsigned long long>(failure.call_sp),
          static_cast<unsigned long long>(failure.call_base),
          static_cast<unsigned long long>(failure.call_required));
    }
  }
  EXPECT_EQ(tail_result, GLWASM_OK);
  EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 7u); EXPECT_EQ(results[1], 99u);
  uint64_t scalar = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "call", nullptr, 0,
      &scalar), GLWASM_ERR_UNSUPPORTED);
  GLWASM_Snapshot *snapshot = nullptr;
  GLWASM_Runtime *restored = nullptr;
  if (tail_result == GLWASM_OK) {
    ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
        GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
    count = 2;
    ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, "call", nullptr, 0,
        results, &count), GLWASM_OK);
    EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 7u); EXPECT_EQ(results[1], 99u);
    count = 2;
    GLWASM_Result compiled_tail_result = glwasm_runtime_invoke_export_results(
        runtime, "tail", nullptr, 0, results, &count);
    if (compiled_tail_result != GLWASM_OK) {
      GLWASM_TestRuntimeFailureInfo failure{};
      if (glwasm_runtime_test_failure_info(runtime, 2, &failure) == GLWASM_OK) {
        std::fprintf(stderr, "compiled tail failure: result=%d run=%d "
            "core=%d outcome=%d function=%u pc=%u opcode=%u reason=%u "
            "compile=%d attempted=%u lower=%u/%u/%u site=%u\n",
            static_cast<int>(compiled_tail_result),
            static_cast<int>(failure.run_error),
            static_cast<int>(failure.core_result),
            static_cast<int>(failure.outcome), failure.last_function,
            failure.last_pc, failure.last_opcode, failure.diagnostic_reason,
            static_cast<int>(failure.jit_compile_result),
            failure.jit_compile_attempted, failure.jit_lower_pc,
            failure.jit_lower_raw_result, failure.jit_lower_stack_count,
            failure.jit_lower_site);
      }
    }
    EXPECT_EQ(compiled_tail_result, GLWASM_OK);
    EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 7u); EXPECT_EQ(results[1], 99u);
    GLWASM_JitCacheStats compiled_call{}, compiled_tail{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &compiled_call), GLWASM_OK);
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &compiled_tail), GLWASM_OK);
    EXPECT_EQ(compiled_call.guest_visible_exits, 0u);
    EXPECT_EQ(compiled_tail.guest_visible_exits, 0u);

    ASSERT_EQ(glwasm_runtime_request_test_pause(runtime), GLWASM_OK);
    count = 2;
    EXPECT_EQ(glwasm_runtime_invoke_export_results(runtime, "call", nullptr, 0,
        results, &count), GLWASM_ERR_PAUSED);
    ASSERT_EQ(glwasm_runtime_snapshot(runtime, &snapshot), GLWASM_OK);
    ASSERT_EQ(glwasm_runtime_create(module, &restored), GLWASM_OK);
    ASSERT_EQ(glwasm_runtime_restore(restored, snapshot), GLWASM_OK);
    count = 2; int paused = 0;
    GLWASM_Result resume_result = glwasm_runtime_resume_results(restored,
        results, &count, &paused);
    if (resume_result != GLWASM_OK) {
      GLWASM_TestRuntimeFailureInfo failure{};
      if (glwasm_runtime_test_failure_info(restored, 1, &failure) == GLWASM_OK) {
        std::fprintf(stderr, "snapshot resume failure: result=%d run=%d "
            "core=%d outcome=%d function=%u pc=%u opcode=%u reason=%u "
            "call=%u sp=%llu base=%llu required=%llu step=%u state=%u\n",
            static_cast<int>(resume_result), static_cast<int>(failure.run_error),
            static_cast<int>(failure.core_result),
            static_cast<int>(failure.outcome), failure.last_function,
            failure.last_pc, failure.last_opcode, failure.diagnostic_reason,
            failure.call_target, static_cast<unsigned long long>(failure.call_sp),
            static_cast<unsigned long long>(failure.call_base),
            static_cast<unsigned long long>(failure.call_required),
            failure.last_step, failure.context_state_after_step);
      }
    }
    EXPECT_EQ(resume_result, GLWASM_OK);
    EXPECT_EQ(paused, 0); EXPECT_EQ(count, 2u);
    EXPECT_EQ(results[0], 7u); EXPECT_EQ(results[1], 99u);
  }
  glwasm_snapshot_release(snapshot);
  glwasm_runtime_destroy(restored);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, HostBoundarySupportsMultiValueResultSlices) {
  static const uint8_t source[] = "(module"
      " (import \"test\" \"pair\" (func $pair (result i32 i64)))"
      " (func (export \"run\") (result i32 i64) (call $pair))"
      " (func (export \"tail\") (result i32 i64) (return_call $pair))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  uint32_t calls = 0;
  GLWASM_HostImport binding{};
  binding.module = "test"; binding.field = "pair";
  binding.kind = GLWASM_HOST_IMPORT_FUNCTION;
  binding.function_results = jit_test_host_multi; binding.user = &calls;
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t results[2] = {}; uint32_t count = 2;
  ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, "run", nullptr, 0,
      results, &count), GLWASM_OK);
  EXPECT_EQ(calls, 1u); EXPECT_EQ(count, 2u);
  EXPECT_EQ(results[0], 31u);
  EXPECT_EQ(results[1], UINT64_C(0x123456789abcdef0));
  GLWASM_JitCacheStats host_call_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &host_call_stats),
      GLWASM_OK);
  EXPECT_EQ(host_call_stats.compiled_entries, 1u);
  EXPECT_EQ(host_call_stats.guest_visible_exits, 0u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  count = 2;
  ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, "tail", nullptr, 0,
      results, &count), GLWASM_OK);
  EXPECT_EQ(calls, 2u); EXPECT_EQ(count, 2u);
  EXPECT_EQ(results[0], 31u);
  EXPECT_EQ(results[1], UINT64_C(0x123456789abcdef0));
  glwasm_runtime_destroy(runtime);

  JitHostMultiPauseState pause_state{};
  binding.function_results = jit_test_host_multi_pause;
  binding.resume_results = jit_test_host_multi_resume;
  binding.user = &pause_state;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  count = 2;
  EXPECT_EQ(glwasm_runtime_invoke_export_results(runtime, "run", nullptr, 0,
      results, &count), GLWASM_ERR_PAUSED);
  EXPECT_EQ(pause_state.initial, 1u);
  int paused = 0;
  ASSERT_EQ(glwasm_runtime_resume_results(runtime, results, &count, &paused),
      GLWASM_OK);
  EXPECT_EQ(paused, 0); EXPECT_EQ(pause_state.initial, 1u);
  EXPECT_EQ(pause_state.resumes, 1u);
  EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 8u); EXPECT_EQ(results[1], 13u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, NestedBoundaryReturnsMultiValueSlices) {
  static const uint8_t source[] =
      "(module (func (export \"pair\") (result i32 i64)"
      " (i32.const 17) (i64.const 29)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  GRCORE_Group *group = nullptr;
  GRCORE_Context *context = nullptr;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &group), GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group, nullptr, &context), GRCORE_OK);
  GRCORE_EngineId caller = 0;
  ASSERT_EQ(grcore_engine_register(context, &external_caller, &caller),
      GRCORE_OK);
  uint64_t results[2] = {}; uint32_t count = 2;
  ASSERT_EQ(glwasm_runtime_invoke_nested_results(runtime, context, caller,
      0x9912, "pair", nullptr, 0, results, &count), GLWASM_OK);
  EXPECT_EQ(count, 2u); EXPECT_EQ(results[0], 17u); EXPECT_EQ(results[1], 29u);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(runtime), GLWASM_OK);
  EXPECT_EQ(grcore_context_destroy(context), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierEntersRegisteredCompiledCode) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  policy.max_jit_code_bytes = 1024u * 1024u;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t args[] = {20, 22};
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, args, 2, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, ModulePolicySelectsBaselineOrHostSupportedCpuFeatures) {
  static const uint8_t source[] =
      "(module (func (param i32) (result i32)"
      " (local.get 0) (i32.popcnt)))";
  const uint64_t input[] = {UINT64_C(0xaaaaaaaa)};
  for (GRJIT_Features disabled : {GRJIT_FEATURES_ALL,
                                   GRJIT_FEATURES_NONE}) {
    GLWASM_Policy policy{};
    ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy),
        GLWASM_OK);
    EXPECT_EQ(policy.jit_disabled_cpu_features, GRJIT_FEATURES_ALL);
    policy.jit_disabled_cpu_features = disabled;
    GLWASM_Module *module = nullptr;
    ASSERT_EQ(glwasm_wat_module_parse_with_policy(source,
        sizeof(source) - 1u, &policy, nullptr, nullptr, &module), GLWASM_OK);
    ASSERT_NE(module, nullptr);
    ASSERT_NE(module->jit_cache, nullptr);
    GLWASM_JitCacheKey key{};
    ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_key)(module->jit_cache,
        &key), GLWASM_OK);
    EXPECT_EQ(key.disabled_cpu_features, disabled);

    GLWASM_Runtime *runtime = nullptr;
    ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
    ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
        GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
    uint64_t result = 0;
    ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, input, 1, &result), GLWASM_OK);
    EXPECT_EQ(result, 16u);
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u);
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
    EXPECT_EQ(stats.guest_visible_exits, 0u);

    GRCORE_Code *handle = GHOTIIO_LANG_WASM(glwasm_jit_cache_retain_code)(
        module->jit_cache, 0);
    ASSERT_NE(handle, nullptr);
    GRJIT_Code *code = glwasm_jit_artifact_code(handle);
    ASSERT_NE(code, nullptr);
    const GRJIT_Features recorded = grjit_code_features(code);
    const GRJIT_Features effective = grjit_features_effective(disabled);
    std::printf("wasm compiled CPU mask=%u detected=%u effective=%u recorded=%u result=%llu\n",
        static_cast<unsigned>(disabled),
        static_cast<unsigned>(grjit_features_detected()),
        static_cast<unsigned>(effective), static_cast<unsigned>(recorded),
        static_cast<unsigned long long>(result));
    EXPECT_TRUE(grjit_features_subset(recorded, effective));
    EXPECT_EQ(recorded, disabled == GRJIT_FEATURES_ALL
        ? GRJIT_FEATURES_NONE
        : grjit_features_detected() & GRJIT_FEATURE_POPCNT);
    grcore_code_release(handle);
    glwasm_runtime_destroy(runtime);
    glwasm_module_destroy(module);
  }
}

TEST(JitCache, DefaultDecodeCachesPortableBaselineCpuMask) {
  static const uint8_t source[] =
      "(module (func (result i32) (i32.const 1)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1u,
      nullptr, nullptr, nullptr, &module), GLWASM_OK);
  ASSERT_NE(module, nullptr);
  GLWASM_JitCacheKey key{};
  ASSERT_EQ(GHOTIIO_LANG_WASM(glwasm_jit_cache_key)(module->jit_cache,
      &key), GLWASM_OK);
  EXPECT_EQ(key.disabled_cpu_features, GRJIT_FEATURES_ALL);
  glwasm_module_destroy(module);
}

TEST(JitCache, ModulePolicyRejectsReservedCpuFeatureMaskBits) {
  static const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy),
      GLWASM_OK);
  policy.jit_disabled_cpu_features |= UINT32_C(1) << 31;
  GLWASM_Module *module = nullptr;
  EXPECT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_ERR_INVALID);
  EXPECT_EQ(module, nullptr);
}

TEST(JitCache, RequiredTierCompilesReturnAndSkipsUnreachableTail) {
  static const uint8_t source[] = "(module"
      " (func (param i32) (result i32)"
      " (local.get 0) (return) (i32.const 99))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {27};
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, argument, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 27u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);

}

TEST(JitCache, RequiredTierPauseAndFuelUnwindRebuildExactPreInstructionState) {
  static const uint8_t source[] = "(module"
      " (func (result i32) (i32.const 40) (i32.const 2) (i32.add))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_request_test_pause(runtime), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result),
      GLWASM_ERR_PAUSED);
  int paused = 0;
  EXPECT_EQ(glwasm_runtime_resume(runtime, &result, &paused), GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime), 4u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  /* The pause exits compiled code; resume may use the reconstructed
   * interpreter frame at the exact instruction boundary. */
  EXPECT_GE(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_PAUSE], 1u);
  const uint64_t entries_after_pause = stats.compiled_entries;
  glwasm_runtime_destroy(runtime);

  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(runtime, 0, 64, 0), GLWASM_OK);
  const GLWASM_Result limit_result = glwasm_runtime_invoke(runtime, 0,
      nullptr, 0, &result);
  GLWASM_TestRuntimeFailureInfo limit_failure{};
  ASSERT_EQ(glwasm_runtime_test_failure_info(runtime, 0, &limit_failure),
      GLWASM_OK);
  EXPECT_EQ(limit_result, GLWASM_ERR_LIMIT);
  EXPECT_EQ(limit_failure.run_error, GLWASM_ERR_LIMIT);
  EXPECT_EQ(limit_failure.core_result, GRCORE_ERR_LIMIT);
  EXPECT_EQ(limit_failure.outcome, GRCORE_OUTCOME_FINISHED);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  /* The module-owned cache retains the first instance's entry count. */
  EXPECT_EQ(stats.compiled_entries, entries_after_pause + 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_LIMIT_UNWIND], 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompilesIntegerWidthConversions) {
  static const uint8_t source[] = "(module"
      " (func (param i64) (result i32) (i32.wrap_i64 (local.get 0)))"
      " (func (param i32) (result i64) (i64.extend_i32_s (local.get 0)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t wide[] = {UINT64_C(0x100000005)};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, wide, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 5u);
  const uint64_t negative[] = {UINT32_MAX};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, negative, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, UINT64_MAX);
  GLWASM_JitCacheStats wrap_stats{}, extend_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &wrap_stats), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &extend_stats), GLWASM_OK);
  EXPECT_EQ(wrap_stats.compiled_entries, 1u);
  EXPECT_EQ(extend_stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompilesGuardedDivisionAndResumesExactTraps) {
  static const uint8_t source[] = "(module"
      " (func (param i32 i32) (result i32)"
      "   (i32.div_s (local.get 0) (local.get 1)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t result = 0;
  uint64_t interpreted = 0;
  const uint64_t ordinary[] = {static_cast<uint64_t>(static_cast<int64_t>(-27)), 4};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, ordinary, 2, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, ordinary, 2, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, static_cast<uint32_t>(-6));
  const uint64_t zero[] = {7, 0};
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, zero, 2, &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_DIVIDE_BY_ZERO);
  EXPECT_EQ(glwasm_runtime_invoke(interpreter, 0, zero, 2, &interpreted), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(interpreter), GLWASM_TRAP_DIVIDE_BY_ZERO);
  const uint64_t overflow[] = {UINT32_C(0x80000000), UINT32_MAX};
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, overflow, 2, &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_INTEGER_OVERFLOW);
  EXPECT_EQ(glwasm_runtime_invoke(interpreter, 0, overflow, 2, &interpreted), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(interpreter), GLWASM_TRAP_INTEGER_OVERFLOW);
  /* First observe traps below the retirement threshold, then continue past
   * the full 64-entry window with only ordinary Wasm traps. They must remain
   * a distinct reconstruction exit, never a named unsupported deopt. */
  for (unsigned i = 0; i < 4; ++i) {
    const uint64_t *trap_args = (i & 1u) ? zero : overflow;
    EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, trap_args, 2, &result),
        GLWASM_ERR_GUEST);
  }
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 7u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION], 6u);
  EXPECT_EQ(stats.window_exits, 0u);
  EXPECT_EQ(stats.interpreter_only, 0u);
  for (unsigned i = 0; i < 57; ++i) {
    const uint64_t *trap_args = (i & 1u) ? zero : overflow;
    EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, trap_args, 2, &result),
        GLWASM_ERR_GUEST);
    EXPECT_EQ(glwasm_runtime_last_trap(runtime), (i & 1u)
        ? GLWASM_TRAP_DIVIDE_BY_ZERO : GLWASM_TRAP_INTEGER_OVERFLOW);
  }
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 64u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION], 63u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(stats.window_entries, 64u);
  EXPECT_EQ(stats.window_exits, 0u);
  EXPECT_EQ(stats.interpreter_only, 0u);
  glwasm_runtime_destroy(interpreter);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompilesFloatArithmeticAndUnorderedComparison) {
  static const uint8_t source[] = "(module"
      " (func (result i32)"
      "   (f32.const 1.5) (f32.const 2.25) (f32.add)"
      "   (f32.const 3.75) (f32.eq))"
      " (func (result i32) (f64.const nan) (f64.const 0) (f64.ne))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t result = 0;
  uint64_t interpreted = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 1, nullptr, 0, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, 1u);
  GLWASM_JitCacheStats arithmetic{}, unordered{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &arithmetic), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &unordered), GLWASM_OK);
  EXPECT_EQ(arithmetic.compiled_entries, 1u);
  EXPECT_EQ(unordered.compiled_entries, 1u);
  EXPECT_EQ(arithmetic.guest_visible_exits, 0u);
  EXPECT_EQ(unordered.guest_visible_exits, 0u);
  glwasm_runtime_destroy(interpreter);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompilesTotalFloatConversionsAndSaturatingTruncation) {
  static const uint8_t source[] = "(module"
      " (func (result i32)"
      "   (i32.reinterpret_f32"
      "     (f32.convert_i32_s (i32.const -7))))"
      " (func (result i32)"
      "   (i32.trunc_sat_f64_u (f64.const nan)))"
      " (func (result i64)"
      "   (i64.reinterpret_f64 (f64.promote_f32 (f32.const 1.5))))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t result = 0;
  uint64_t interpreted = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, UINT32_C(0xc0e00000));
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 1, nullptr, 0, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, 0u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, nullptr, 0, &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 2, nullptr, 0, &interpreted), GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, UINT64_C(0x3ff8000000000000));
  for (uint32_t i = 0; i < 3; ++i) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, i, &stats), GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u);
    EXPECT_EQ(stats.guest_visible_exits, 0u);
  }
  glwasm_runtime_destroy(interpreter);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierReturnsEveryMultiValueResult) {
  static const uint8_t source[] = "(module"
      " (func (result i32 i64) (i32.const 7) (i64.const 9)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t results[2] = {};
  uint32_t result_count = 2;
  ASSERT_EQ(glwasm_runtime_invoke_results(runtime, 0, nullptr, 0, results,
      &result_count), GLWASM_OK);
  ASSERT_EQ(result_count, 2u);
  EXPECT_EQ(results[0], 7u);
  EXPECT_EQ(results[1], 9u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.completed_without_deopt, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierPropagatesMultiValueDirectAndIndirectCalls) {
  static const uint8_t source[] = "(module"
      " (type $pair (func (result i32 i64)))"
      " (table 1 funcref)"
      " (func $pair (type $pair) (i32.const 17) (i64.const 29))"
      " (elem (i32.const 0) func $pair)"
      " (func (export \"direct\") (type $pair) (call $pair))"
      " (func (export \"indirect\") (type $pair)"
      "   (i32.const 0) (call_indirect (type $pair))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t results[2] = {};
  for (const char *name : {"direct", "indirect"}) {
    uint32_t count = 2;
    ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, name, nullptr, 0,
        results, &count), GLWASM_OK) << name;
    ASSERT_EQ(count, 2u);
    EXPECT_EQ(results[0], 17u);
    EXPECT_EQ(results[1], 29u);
  }
  GLWASM_JitCacheStats direct{}, indirect{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &direct), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &indirect), GLWASM_OK);
  EXPECT_EQ(direct.compiled_entries, 1u);
  EXPECT_EQ(indirect.compiled_entries, 1u);
  EXPECT_EQ(direct.guest_visible_exits, 0u);
  EXPECT_EQ(indirect.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCatchesCompiledThrowAcrossCallsByClausePriority) {
  static const uint8_t source[] = "(module"
      " (type $call (func)) (table 1 funcref)"
      " (tag $a) (tag $b)"
      " (func $raise (throw $b))"
      " (elem (i32.const 0) func $raise)"
      " (func (export \"direct\") (result i32)"
      "   (block $outer"
      "     (block $inner"
      "       (try_table (catch $a $outer) (catch $b $outer)"
      "         (catch $b $inner) (call $raise)))"
      "     (i32.const 3) (return))"
      "   (i32.const 7))"
      " (func (export \"indirect\") (result i32)"
      "   (block $outer"
      "     (block $inner"
      "       (try_table (catch $a $outer) (catch $b $outer)"
      "         (catch $b $inner)"
      "         (i32.const 0) (call_indirect (type $call))))"
      "     (i32.const 3) (return))"
      "   (i32.const 7)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  for (const char *name : {"direct", "indirect"}) {
    EXPECT_EQ(glwasm_runtime_invoke_export(runtime, name, nullptr, 0,
        &result), GLWASM_OK) << name;
    EXPECT_EQ(result, 7u) << name;
  }
  for (uint32_t index = 0; index < 3; ++index) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, index, &stats),
        GLWASM_OK);
    EXPECT_GT(stats.compiled_entries, 0u) << index;
    EXPECT_EQ(stats.guest_visible_exits, 0u) << index;
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierPropagatesUnmatchedExceptionThroughCompiledFrames) {
  static const uint8_t source[] = "(module (tag $a) (tag $b)"
      " (func $raise (throw $b))"
      " (func $inner"
      "   (block $wrong"
      "     (try_table (catch $a $wrong) (call $raise))))"
      " (func (export \"run\") (result i32)"
      "   (block $right"
      "     (try_table (catch $b $right) (call $inner)))"
      "   (i32.const 37)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "run", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 37u);
  for (uint32_t index = 0; index < 3; ++index) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, index, &stats),
        GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u) << index;
    EXPECT_EQ(stats.guest_visible_exits, 0u) << index;
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierTransfersTypedExceptionPayloadAndReference) {
  static const uint8_t source[] = "(module"
      " (type $pair (func (param i32 i64))) (tag $t (type $pair))"
      " (global $wide (mut i64) (i64.const 0))"
      " (func (export \"payload\") (result i32)"
      "   (block $h (result i32 i64)"
      "     (try_table (result i32 i64) (catch $t $h)"
      "       (throw $t (i32.const 7) (i64.const 9))"
      "       (i32.const 2) (i64.const 3)))"
      "   (global.set $wide))"
      " (func (export \"wide\") (result i64) (global.get $wide))"
      " (func (export \"reference\") (result i32) (local $ex exnref)"
      "   (block $h (result i32 i64 exnref)"
      "     (try_table (result i32 i64) (catch_ref $t $h)"
      "       (throw $t (i32.const 19) (i64.const 23))"
      "       (i32.const 2) (i64.const 3))"
      "     (drop) (return))"
      "   (local.set $ex) (drop) (drop)"
      "   (block $outer"
      "     (try_table (catch_all $outer)"
      "       (local.get $ex) (throw_ref)))"
      "   (i32.const 31)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(runtime, UINT64_MAX, 64, 1),
      GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "payload", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "wide", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 9u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "reference", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 31u);
  for (uint32_t index = 0; index < 3; ++index) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, index, &stats),
        GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u) << index;
    EXPECT_EQ(stats.guest_visible_exits, 0u) << index;
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierKeepsUncaughtAndNullThrowRefDistinct) {
  static const uint8_t source[] = "(module"
      " (tag $t)"
      " (func (export \"uncaught\") (throw $t))"
      " (func (export \"null_ref\") (ref.null exn) (throw_ref)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "uncaught", nullptr, 0,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_NONE);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "null_ref", nullptr, 0,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),
      GLWASM_TRAP_INVALID_REFERENCE);
  for (uint32_t index = 0; index < 2; ++index) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, index, &stats),
        GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u) << index;
    EXPECT_EQ(stats.guest_visible_exits, 0u) << index;
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierLimitUnwindBypassesCatchAll) {
  static const uint8_t source[] = "(module (tag $t)"
      " (func (export \"run\") (local $count i32)"
      "   (loop $again"
      "     (block $caught"
      "       (try_table (catch_all $caught) (throw $t)))"
      "     (local.get $count) (i32.const 1) (i32.add)"
      "     (local.set $count) (br $again))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(runtime, 64, 64, 0), GLWASM_OK);
  uint64_t result = 0;
  const GLWASM_Result limit_result = glwasm_runtime_invoke_export(runtime,
      "run", nullptr, 0, &result);
  EXPECT_EQ(limit_result, GLWASM_ERR_LIMIT);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierPauseAcrossTryTableResumesWithoutCatch) {
  static const uint8_t source[] = "(module"
      " (import \"host.v1\" \"value\" (func $value (result i32)))"
      " (func $inner (result i32) (local $saved i32)"
      "   (block $caught"
      "     (try_table (catch_all $caught)"
      "       (local.set $saved (call $value))))"
      "   (local.get $saved))"
      " (func (export \"run\") (result i32) (call $inner)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  JitHostPauseState state;
  GLWASM_HostImport binding{};
  binding.module = "host.v1";
  binding.field = "value";
  binding.function = jit_test_host_pause;
  binding.resume = jit_test_host_resume;
  binding.user = &state;
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "run", nullptr, 0,
      &result), GLWASM_ERR_PAUSED);
  EXPECT_EQ(state.initial, 1u);
  int paused = 0;
  EXPECT_EQ(glwasm_runtime_resume(runtime, &result, &paused), GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(state.initial, 1u);
  EXPECT_EQ(state.resumes, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierMemoryCopyFillAndLoadStayCompiled) {
  static const uint8_t source[] = "(module (memory 1)"
      " (data (i32.const 0) \"\\2a\\00\\00\\00\")"
      " (func (result i32)"
      " (memory.copy (i32.const 4) (i32.const 0) (i32.const 4))"
      " (memory.fill (i32.const 8) (i32.const 43) (i32.const 4))"
      " (i32.add (i32.load (i32.const 4)) (i32.load (i32.const 8)))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t result = 0;
  uint64_t interpreted = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &interpreted),
      GLWASM_OK);
  EXPECT_EQ(result, interpreted);
  EXPECT_EQ(result, UINT32_C(0x2b2b2b55));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.completed_without_deopt, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime,
      UINT16_C(0xfc0a)), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime,
      UINT16_C(0xfc0b)), 0u);
  glwasm_runtime_destroy(interpreter);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierNarrowI64StoresStayCompiled) {
  static const uint8_t source[] = "(module (memory 1)"
      " (func (result i64)"
      "  (i64.store8 (i32.const 0) (i64.const 1234605616436508552))"
      "  (i64.store16 (i32.const 1) (i64.const 1234605616436508552))"
      "  (i64.load32_u (i32.const 0))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, UINT64_C(0x00778888));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x3c), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x3d), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierMemoryBulkBoundsExitBeforeMutation) {
  static const uint8_t source[] = "(module (memory 1)"
      " (func (result i32)"
      "  (memory.fill (i32.const 65535) (i32.const 42) (i32.const 2))"
      "  (i32.const 0)))";
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError parse_error{};
  GLWASM_Result parse_result = glwasm_wat_module_parse(source,
      sizeof(source) - 1, nullptr, nullptr, &parse_error, &module);
  ASSERT_EQ(parse_result, GLWASM_OK)
      << (parse_error.message ? parse_error.message : "no WAT parse detail")
      << " (" << static_cast<int>(parse_result) << ")";
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),
      GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0b), 0u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION], 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierLongMemoryFillPollsAndResumesWithoutReplay) {
  static const uint8_t source[] = "(module (memory 1)"
      " (func (result i32)"
      "  (memory.fill (i32.const 0) (i32.const 42) (i32.const 4097))"
      "  (i32.load8_u (i32.const 4096))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  uint64_t result = 0;
  uint64_t expected = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, expected);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime),
      glwasm_runtime_test_fuel_used(interpreter));
  GLWASM_Runtime *paused = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &paused), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(paused,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_pause_after_bulk_chunks(paused, 1),
      GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(paused, 0, nullptr, 0, &result),
      GLWASM_ERR_PAUSED);
  int still_paused = 1;
  ASSERT_EQ(glwasm_runtime_resume(paused, &result, &still_paused),
      GLWASM_OK);
  EXPECT_EQ(still_paused, 0);
  EXPECT_EQ(result, expected);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(paused),
      glwasm_runtime_test_fuel_used(interpreter));
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0b), 0u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  glwasm_runtime_destroy(paused);
  glwasm_runtime_destroy(interpreter);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierLongOverlappingMemoryCopyMatchesInterpreter) {
  static const uint8_t source[] = "(module (memory 1)"
      " (func (result i32)"
      "  (memory.fill (i32.const 0) (i32.const 17) (i32.const 8192))"
      "  (memory.fill (i32.const 4096) (i32.const 34) (i32.const 4096))"
      "  (memory.copy (i32.const 1024) (i32.const 0) (i32.const 8192))"
      "  (i32.add (i32.add (i32.load8_u (i32.const 1024))"
      "   (i32.load8_u (i32.const 5120)))"
      "   (i32.load8_u (i32.const 9215)))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr, *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t expected = 0, actual = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 85u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &actual),
      GLWASM_OK);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime),
      glwasm_runtime_test_fuel_used(interpreter));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.completed_without_deopt, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0a), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0b), 0u);
  /* The first counted chunk is the long fill; the second is the high half
   * of the overlapping copy. Resume must copy the remaining low half once. */
  GLWASM_Runtime *paused = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &paused), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(paused,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_pause_after_bulk_chunks(paused, 2),
      GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(paused, 0, nullptr, 0, &actual),
      GLWASM_ERR_PAUSED);
  int still_paused = 1;
  ASSERT_EQ(glwasm_runtime_resume(paused, &actual, &still_paused),
      GLWASM_OK);
  EXPECT_EQ(still_paused, 0);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(paused),
      glwasm_runtime_test_fuel_used(interpreter));
  glwasm_runtime_destroy(paused);
  glwasm_runtime_destroy(runtime);
  glwasm_runtime_destroy(interpreter);
  glwasm_module_destroy(module);
}

TEST(JitCache, Wasm32BulkOperandsIgnoreOldUpperSpillBits) {
  static const uint8_t source[] = "(module (memory 1)"
      " (func (result i32)"
      "  (i64.const 4294967296) (i64.const 4294967296)"
      "  (i64.const 4294967296) drop drop drop"
      "  (memory.fill (i32.const 0) (i32.const 7) (i32.const 8))"
      "  (memory.copy (i32.const 16) (i32.const 0) (i32.const 8))"
      "  (i32.load8_u (i32.const 16))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr, *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t expected = 0, actual = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, nullptr, 0, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 7u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &actual), GLWASM_OK);
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime),
      glwasm_runtime_test_fuel_used(interpreter));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0a), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0b), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_runtime_destroy(interpreter);
  glwasm_module_destroy(module);
}

TEST(JitCache, ObservedGuestFunctionsIncludeCompiledInternalCallees) {
  static const uint8_t source[] = "(module"
      " (func $inner (result i32) (i32.const 42))"
      " (func (export \"run\") (result i32) (call $inner)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_reset_observed_functions(runtime), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  uint32_t required = 0;
  uint8_t too_small[1] = {0xa5};
  EXPECT_EQ(glwasm_runtime_test_observed_functions(runtime, too_small, 1,
      &required), GLWASM_ERR_LIMIT);
  EXPECT_EQ(required, 2u);
  EXPECT_EQ(too_small[0], 0xa5u);
  uint8_t observed[2] = {};
  ASSERT_EQ(glwasm_runtime_test_observed_functions(runtime, observed, 2,
      &required), GLWASM_OK);
  EXPECT_EQ(observed[0], 1u);
  EXPECT_EQ(observed[1], 1u);
  ASSERT_EQ(glwasm_runtime_test_reset_observed_functions(runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_observed_functions(runtime, observed, 2,
      &required), GLWASM_OK);
  EXPECT_EQ(observed[0], 0u);
  EXPECT_EQ(observed[1], 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierDirectAndTailCallsReturnThroughCompiledCallee) {
  static const uint8_t source[] = "(module"
      " (func $callee (param i32) (result i32)"
      " (local.get 0) (i32.const 1) (i32.add))"
      " (func (param i32) (result i32) (local i32)"
      " (local.get 0) (local.set 1)"
      " (local.get 1) (call $callee) (local.get 1) (i32.add))"
      " (func (param i32) (result i32)"
      " (local.get 0) (return_call $callee))"
      " (func (param i32) (result i32)"
      " (local.get 0) (call 2))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {41};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 83u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats direct{}, tail{}, callee{}, nested{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &direct), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &tail), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 3, &nested), GLWASM_OK);
  EXPECT_EQ(direct.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(direct.guest_visible_exits, 0u);
  EXPECT_EQ(direct.completed_without_deopt, 1u);
  EXPECT_EQ(tail.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(direct.compiled_entries, 1u);
  EXPECT_EQ(tail.compiled_entries, 2u);
  EXPECT_EQ(tail.guest_visible_exits, 0u);
  EXPECT_EQ(tail.completed_without_deopt, 2u);
  EXPECT_EQ(nested.compiled_entries, 1u);
  EXPECT_GT(callee.compiled_entries, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierIndirectTailCallReturnsThroughCompiledCallee) {
  static const uint8_t source[] = "(module"
      " (type $answer (func (param i32) (result i32)))"
      " (table 1 funcref)"
      " (func $callee (type $answer) (local.get 0) (i32.const 1) (i32.add))"
      " (elem (i32.const 0) $callee)"
      " (func (export \"tail\") (type $answer)"
      "   (local.get 0) (i32.const 0) (return_call_indirect (type $answer))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {41};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "tail", argument, 1,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, WideCallUsesNamedPreEffectExitAboveBackendValueLimit) {
  std::string source = "(module (type $wide (func";
  for (unsigned i = 0; i < 17; ++i) source += " (param i32)";
  source += " (result i32))) (func $wide (type $wide) (i32.const 7))"
      " (func (export \"caller\") (result i32)";
  for (unsigned i = 0; i < 17; ++i)
    source += " (i32.const " + std::to_string(i) + ")";
  source += " (call $wide)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(), nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_AUTO), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "caller", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x10), 1u);
  GLWASM_JitCacheStats caller{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &caller), GLWASM_OK);
  EXPECT_EQ(caller.compiled_entries, 1u);
  EXPECT_EQ(caller.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, WideNestedSignatureHasCountedRefusalBeyondInlineArguments) {
  /* A nested interpreter call still attempts tiering after it has copied the
   * full guest argument list. The callable IR currently refuses >16 values;
   * the 100-parameter spec case must receive that counted refusal instead of
   * silently bypassing the JIT because a staging buffer has 64 words. */
  std::string source = "(module (func $wide";
  for (unsigned i = 0; i < 100; ++i) source += " (param i32)";
  source += " (result i32) (local.get 99))"
      " (func (export \"caller\") (result i32)";
  for (unsigned i = 0; i < 100; ++i)
    source += " (i32.const " + std::to_string(i) + ")";
  source += " (call $wide)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(), nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_AUTO), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "caller", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 99u);
  GLWASM_JitCacheStats caller{}, callee{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &caller), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  EXPECT_EQ(caller.compiled_entries, 1u);
  EXPECT_EQ(caller.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
  EXPECT_EQ(callee.compiled_entries, 0u);
  EXPECT_EQ(callee.signature_refusal_count, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, WideResultSignatureUsesInterpreterAfterCountedRefusal) {
  std::string source = "(module (func (export \"wide\")";
  for (unsigned i = 0; i < 17; ++i) source += " (result i32)";
  source += " ";
  for (unsigned i = 0; i < 17; ++i)
    source += " (i32.const " + std::to_string(i) + ")";
  source += "))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(), nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_AUTO), GLWASM_OK);
  uint64_t results[17] = {};
  uint32_t result_count = 17;
  ASSERT_EQ(glwasm_runtime_invoke_export_results(runtime, "wide", nullptr, 0,
      results, &result_count), GLWASM_OK);
  ASSERT_EQ(result_count, 17u);
  for (uint32_t i = 0; i < result_count; ++i) EXPECT_EQ(results[i], i);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_GT(stats.signature_refusal_count, 0u);
  EXPECT_EQ(stats.compiled_entries, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, ExternrefRefusesCompilationWhileExnrefTokenCompiles) {
  static const uint8_t source[] = "(module"
      " (func (export \"external_ref\") (param externref) (result i32)"
      "   (i32.const 31))"
      " (func (export \"exception_ref\") (param exnref) (result i32)"
      "   (i32.const 32)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_AUTO), GLWASM_OK);
  const uint64_t null_reference[] = {0};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "external_ref",
      null_reference, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 31u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "exception_ref",
      null_reference, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 32u);
  GLWASM_JitCacheStats external{}, exception{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &external), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &exception), GLWASM_OK);
  EXPECT_EQ(external.signature_refusal_count, 1u);
  EXPECT_EQ(external.compiled_entries, 0u);
  EXPECT_EQ(exception.signature_refusal_count, 0u);
  EXPECT_EQ(exception.compiled_entries, 1u);
  EXPECT_EQ(exception.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
  EXPECT_EQ(exception.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierNamesGlobalTableSizeAndUnreachableExits) {
  static const uint8_t source[] = "(module"
      " (global $g (mut i32) (i32.const 9))"
      " (memory 1) (table 1 funcref)"
      " (func (export \"dead\") unreachable)"
      " (func (export \"get_global\") (result i32) (global.get $g))"
      " (func (export \"set_global\") (i32.const 11) (global.set $g))"
      " (func (export \"memory_size\") (result i32) (memory.size))"
      " (func (export \"table_get\") (result i32)"
      "   (ref.is_null (table.get 0 (i32.const 0)))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "dead", nullptr, 0,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_UNREACHABLE);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "get_global", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 9u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "set_global", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "get_global", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 11u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "memory_size", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "table_get", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x00), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x23), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x24), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x25), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x3f), 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierSupportsAllNumericGlobalTypes) {
  static const uint8_t source[] = "(module"
      " (global $i32 (mut i32) (i32.const 0))"
      " (global $i64 (mut i64) (i64.const 0))"
      " (global $f32 (mut f32) (f32.const 0))"
      " (global $f64 (mut f64) (f64.const 0))"
      " (func (export \"i32\") (param i32) (result i32)"
      "   (local.get 0) (global.set $i32) (global.get $i32))"
      " (func (export \"i64\") (param i64) (result i64)"
      "   (local.get 0) (global.set $i64) (global.get $i64))"
      " (func (export \"f32\") (param f32) (result f32)"
      "   (local.get 0) (global.set $f32) (global.get $f32))"
      " (func (export \"f64\") (param f64) (result f64)"
      "   (local.get 0) (global.set $f64) (global.get $f64)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const struct {
    const char *name;
    uint64_t value;
  } cases[] = {
    {"i32", UINT64_C(0x89abcdef)},
    {"i64", UINT64_C(0x89abcdef01234567)},
    {"f32", UINT64_C(0x3f812345)},
    {"f64", UINT64_C(0x3ff02468acf13579)},
  };
  for (uint32_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    uint64_t result = 0;
    ASSERT_EQ(glwasm_runtime_invoke_export(runtime, cases[i].name,
        &cases[i].value, 1, &result), GLWASM_OK) << cases[i].name;
    EXPECT_EQ(result, cases[i].value) << cases[i].name;
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, i, &stats), GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u) << cases[i].name;
    EXPECT_EQ(stats.completed_without_deopt, 1u) << cases[i].name;
    EXPECT_EQ(stats.guest_visible_exits, 0u) << cases[i].name;
  }
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x23), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x24), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierTableSetExitsBeforeMutationAndResumesExactly) {
  static const uint8_t source[] = "(module"
      " (type $value (func (result i32)))"
      " (table 1 funcref)"
      " (func $value (type $value) (result i32) (i32.const 37))"
      " (elem declare func $value)"
      " (func (export \"set\")"
      "   (i32.const 0) (ref.func $value) (table.set 0))"
      " (func (export \"read\") (result i32)"
      "   (call_indirect (type $value) (i32.const 0))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "set", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "read", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 37u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x26), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x11), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierMemoryAndTableSegmentOperationsExitBeforeEffect) {
  static const uint8_t source[] = "(module"
      " (memory 1) (table 2 funcref)"
      " (data $bytes \"x\")"
      " (func $target)"
      " (elem $items func $target)"
      " (func (export \"memory_init\")"
      "   (memory.init $bytes (i32.const 0) (i32.const 0) (i32.const 1)))"
      " (func (export \"data_drop\") (data.drop $bytes))"
      " (func (export \"table_init\")"
      "   (table.init $items (i32.const 0) (i32.const 0) (i32.const 1)))"
      " (func (export \"elem_drop\") (elem.drop $items))"
      " (func (export \"table_size\") (result i32) (table.size 0))"
      " (func (export \"table_grow\") (result i32)"
      "   (table.grow 0 (ref.null func) (i32.const 1))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  for (const char *name : {"memory_init", "data_drop", "table_init",
      "elem_drop"})
    ASSERT_EQ(glwasm_runtime_invoke_export(runtime, name, nullptr, 0,
        &result), GLWASM_OK) << name;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "table_size", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 2u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "table_grow", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 2u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc08), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc09), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0c), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0d), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0f), 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc10), 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierNestedCalleeDeoptResumesBeforeUnsupportedEffect) {
  static const uint8_t source[] = "(module"
      " (func $callee (param i32) (result i32)"
      " (i32.trunc_f32_s"
      "  (f32.div (f32.convert_i32_s (local.get 0)) (f32.const 2))))"
      " (func (param i32) (result i32)"
      " (local.get 0) (call $callee))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {42};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 21u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xa8), 1u);
  GLWASM_JitCacheStats callee{}, caller{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &caller), GLWASM_OK);
  EXPECT_EQ(callee.compiled_entries, 1u);
  EXPECT_EQ(callee.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
  EXPECT_EQ(callee.guest_visible_exits, 1u);
  EXPECT_EQ(caller.compiled_entries, 1u);
  EXPECT_EQ(caller.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT], 1u);
  EXPECT_EQ(caller.guest_visible_exits, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RecursiveNamedExitIsAttributedOnlyToExecutingFrame) {
  static const uint8_t source[] = "(module"
      " (func $recur (param i32) (result i32)"
      "  (local.get 0) (i32.eqz)"
      "  (if"
      "   (then (f32.const 42) (i32.trunc_f32_s) (drop))"
      "   (else (local.get 0) (i32.const 1) (i32.sub) (call $recur) (drop)))"
      "  (i32.const 7))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {3};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, argument, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 7u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xa8), 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
  EXPECT_GE(stats.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT], 1u);
  EXPECT_GT(stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompiledCalleeExceptionReachesCompiledHandler) {
  static const uint8_t source[] = "(module"
      " (tag $value (param i32))"
      " (func $raise (param i32) (local.get 0) (throw $value))"
      " (func (param i32) (result i32)"
      "  (block $handled (result i32)"
      "   (try_table (result i32) (catch $value $handled)"
      "    (call $raise (local.get 0)) (i32.const 0))))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {42};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  /* The callee raises a Wasm exception and the compiled caller selects the
   * first matching handler and moves its payload into the target label. */
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x08), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x1f), 0u);
  GLWASM_JitCacheStats callee{}, caller{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &caller), GLWASM_OK);
  EXPECT_EQ(callee.compiled_entries, 1u);
  EXPECT_EQ(callee.guest_visible_exits, 0u);
  EXPECT_EQ(caller.compiled_entries, 1u);
  EXPECT_EQ(caller.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, ExnrefSignatureUsesStableCompiledReferenceToken) {
  static const uint8_t source[] = "(module"
      " (func (export \"null_ref\") (param exnref) (throw_ref (local.get 0))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t null_reference[] = {0};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "null_ref", null_reference, 1,
      &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_INVALID_REFERENCE);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.signature_refusal_count, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompiledImportedBoundaryInvokesHostOnce) {
  static const uint8_t source[] = "(module"
      " (import \"host.v1\" \"value\" (func $value (result i32)))"
      " (func (result i32) (call $value))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  uint32_t calls = 0;
  GLWASM_HostImport binding{};
  binding.module = "host.v1";
  binding.field = "value";
  binding.function = jit_test_host_call;
  binding.user = &calls;
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(calls, 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x10), 0u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &stats), GLWASM_OK);
  EXPECT_EQ(stats.completed_without_deopt, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, HostI32AndF32ArgumentsIgnoreOldUpperSpillBits) {
  static const uint8_t source[] = "(module"
      " (import \"host.v1\" \"check\""
      "  (func $check (param i32 f32) (result i32)))"
      " (func (result i32)"
      "  (i64.const 4294967296) (i64.const 4294967296) drop drop"
      "  (call $check (i32.const 7) (f32.const 1))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_HostImport binding{};
  binding.module = "host.v1";
  binding.field = "check";
  binding.function = jit_test_host_narrow_bits;
  GLWASM_Runtime *interpreter = nullptr, *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t expected = 0, actual = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 1, nullptr, 0, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &actual),
      GLWASM_OK);
  EXPECT_EQ(actual, expected);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &stats), GLWASM_OK);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_runtime_destroy(interpreter);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierHostPauseReconstructsCallAndUsesContinuation) {
  static const uint8_t source[] = "(module"
      " (import \"host.v1\" \"value\" (func $value (result i32)))"
      " (func (result i32) (call $value))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  JitHostPauseState state;
  GLWASM_HostImport binding{};
  binding.module = "host.v1";
  binding.field = "value";
  binding.function = jit_test_host_pause;
  binding.resume = jit_test_host_resume;
  binding.user = &state;
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(state.initial, 1u);
  int paused = 0;
  EXPECT_EQ(glwasm_runtime_resume(runtime, &result, &paused), GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(state.initial, 1u);
  EXPECT_EQ(state.resumes, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierHostErrorDoesNotReplayCompletedCallback) {
  static const uint8_t source[] = "(module"
      " (import \"host.v1\" \"value\" (func $value (result i32)))"
      " (func (result i32) (call $value))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  uint32_t calls = 0;
  GLWASM_HostImport binding{};
  binding.module = "host.v1";
  binding.field = "value";
  binding.function = jit_test_host_error;
  binding.user = &calls;
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module, &binding, 1, 0,
      nullptr, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result),
      GLWASM_ERR_LINK);
  EXPECT_EQ(calls, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierIndirectCallCompilesAndDispatchesTableTarget) {
  static const uint8_t source[] = "(module"
      " (type $ti (func (param i32) (result i32)))"
      " (table 1 funcref)"
      " (func $callee (type $ti) (local.get 0) (i32.const 2) (i32.add))"
      " (elem (i32.const 0) $callee)"
      " (func (param i32) (result i32)"
      " (local.get 0) (i32.const 0) (call_indirect (type $ti)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {40};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats caller{}, callee{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &caller), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  EXPECT_EQ(caller.compiled_entries, 1u);
  EXPECT_EQ(caller.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(caller.completed_without_deopt, 1u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x11), 0u);
  EXPECT_EQ(callee.compile_successes, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, DynamicIndirectDispatchIsBoundedAndInstanceLocal) {
  std::string source = "(module"
      " (type $answer (func (result i32)))"
      " (table 2 funcref)"
      " (func $seven (type $answer) (i32.const 7))"
      " (func $nine (type $answer) (i32.const 9))"
      " (elem (i32.const 0) func $seven $nine)";
  for (unsigned i = 0; i < 600; ++i) source += " (func)";
  source += " (func (export \"run\") (param i32) (result i32)"
      " (i32.const 0)";
  for (unsigned i = 0; i < 8; ++i)
    source += " (local.get 0) (call_indirect (type $answer)) (i32.add)";
  source += ") (func (export \"tail\") (param i32) (result i32)"
      " (local.get 0) (return_call_indirect (type $answer))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      nullptr, nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *first = nullptr, *second = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &first), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &second), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(first,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t zero[] = {0}, one[] = {1};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(first, "run", zero, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 56u);
  ASSERT_EQ(glwasm_runtime_invoke_export(second, "run", one, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 72u);
  ASSERT_EQ(glwasm_runtime_invoke_export(first, "tail", one, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 9u);
  ASSERT_EQ(glwasm_runtime_invoke_export(second, "tail", zero, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 7u);
  GLWASM_JitCacheStats run{}, tail{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(first, 602, &run), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(first, 603, &tail), GLWASM_OK);
  EXPECT_EQ(run.guest_visible_exits, 0u);
  EXPECT_EQ(tail.guest_visible_exits, 0u);
  glwasm_runtime_destroy(second);
  glwasm_runtime_destroy(first);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierTableCopyFillAndNullFunctionReferenceStayCompiled) {
  static const uint8_t source[] = "(module"
      " (type $answer (func (result i32)))"
      " (table 4 4 funcref)"
      " (func $answer_fn (type $answer) (result i32) (i32.const 42))"
      " (elem (i32.const 0) func $answer_fn)"
      " (func (export \"run\") (param $n i32) (result i64)"
      "  (local $i i32) (local $sum i64)"
      "  (block $done (loop $again"
      "   (br_if $done (i32.ge_u (local.get $i) (local.get $n)))"
      "   (table.copy 0 0 (i32.const 1) (i32.const 0) (i32.const 1))"
      "   (table.fill 0 (i32.const 2) (ref.null func) (i32.const 1))"
      "   (local.set $sum (i64.add (local.get $sum)"
      "    (i64.extend_i32_u (call_indirect (type $answer)"
      "      (table 0) (i32.const 1)))))"
      "   (local.set $i (i32.add (local.get $i) (i32.const 1)))"
      "   (br $again)))"
      "  (local.get $sum)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {4};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, argument, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 168u);
  GLWASM_JitCacheStats run{}, callee{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &run), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee), GLWASM_OK);
  EXPECT_EQ(run.completed_without_deopt, 1u);
  EXPECT_EQ(run.guest_visible_exits, 0u);
  EXPECT_EQ(run.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(callee.compiled_entries, 4u);
  EXPECT_EQ(callee.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xd0), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0e), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc11), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x11), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, Table32BulkOperandsIgnoreOldUpperSpillBits) {
  static const uint8_t source[] = "(module"
      " (type $answer (func (result i32)))"
      " (table 4 funcref)"
      " (func $answer_fn (type $answer) (result i32) (i32.const 42))"
      " (elem (i32.const 0) func $answer_fn)"
      " (func (result i32)"
      "  (i64.const 4294967296) (i64.const 4294967296)"
      "  (i64.const 4294967296) drop drop drop"
      "  (table.copy 0 0 (i32.const 1) (i32.const 0) (i32.const 1))"
      "  (i64.const 4294967296) (i64.const 4294967296)"
      "  (i64.const 4294967296) drop drop drop"
      "  (table.fill 0 (i32.const 2) (ref.null func) (i32.const 1))"
      "  (call_indirect (type $answer) (table 0) (i32.const 1))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr, *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t expected = 0, actual = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 1, nullptr, 0, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &actual),
      GLWASM_OK);
  EXPECT_EQ(actual, expected);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &stats), GLWASM_OK);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0e), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc11), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_runtime_destroy(interpreter);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierTableBulkBoundsExitBeforeMutation) {
  static const uint8_t source[] = "(module (table 1 1 funcref)"
      " (func (result i32)"
      "  (table.copy 0 0 (i32.const 1) (i32.const 0) (i32.const 1))"
      "  (i32.const 0)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION], 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc0e), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierLongTableFillExitsBeforeInterpreterContinuation) {
  static const uint8_t source[] = "(module"
      " (type $value (func (result i32)))"
      " (table 2048 2048 funcref)"
      " (func $value (type $value) (result i32) (i32.const 7))"
      " (elem (i32.const 0) func $value)"
      " (func (result i32)"
      "  (table.fill 0 (i32.const 0) (ref.null func) (i32.const 1025))"
      "  (i32.const 0) (call_indirect (type $value)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),
      GLWASM_TRAP_UNINITIALIZED_ELEMENT);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0xfc11), 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierStructuredControlCompilesResultJoins) {
  static const uint8_t source[] = "(module"
      " (func (result i32) (block (result i32) (i32.const 7)))"
      " (func (param i32) (result i32) (local.get 0) (br 0))"
      " (func (param i32) (result i32)"
      " (local.get 0) (local.get 0) (br_if 0))"
      " (func (param i32) (result i32)"
      " (i32.const 33) (local.get 0) (br_table 0 0))"
      " (func (param i32) (result i32)"
      " (if (result i32) (local.get 0)"
      " (then (i32.const 1)) (else (i32.const 2))))"
      " (func (param i32) (result i32)"
      " (block (result i32) (loop (result i32) (local.get 0))))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  const uint64_t interpreter_true[] = {7}, interpreter_false[] = {0};
  uint64_t interpreter_result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 4, interpreter_true, 1,
      &interpreter_result), GLWASM_OK);
  EXPECT_EQ(interpreter_result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 4, interpreter_false, 1,
      &interpreter_result), GLWASM_OK);
  EXPECT_EQ(interpreter_result, 2u);
  glwasm_runtime_destroy(interpreter);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  const uint64_t seven[] = {7};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, seven, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, seven, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  const uint64_t zero[] = {0};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 0u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 33u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, seven, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 33u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 4, seven, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 4, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 2u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 5, seven, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  /* Result-bearing controls and their branch joins remain compiled. */
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x02), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x03), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x04), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x0c), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x0d), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x0e), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierPreservesTypedControlParametersAndBranchPayloads) {
  static const uint8_t source[] = "(module"
      " (func (param i32) (result i32)"
      "  (i32.const 41)"
      "  (block (result i32)"
      "   (i32.const 99) (i32.const 7) (local.get 0) (br_if 0)"
      "   (drop) (drop) (i32.const 3))"
      "  (i32.add))"
      " (func (param $n i32) (result i32) (local $i i32)"
      "  (i32.const 0)"
      "  (loop (param i32) (result i32)"
      "   (local.set $i) (i32.const 99)"
      "   (local.get $i) (i32.const 1) (i32.add) (local.tee $i)"
      "   (local.get $i) (local.get $n) (i32.lt_u) (br_if 0)"
      "   (local.set $i) (drop) (local.get $i)))"
      " (func (param i32) (result i64)"
      "  (i64.const 10) (i64.const 7) (local.get 0)"
      "  (if (param i64) (result i64)"
      "   (then (i64.const 1) (i64.add))"
      "   (else (i64.const 2) (i64.add)))"
      "  (i64.add))"
      " (func (param i32) (result i32)"
      "  (block (result i32)"
      "   (i32.const 41)"
      "   (block (result i32)"
      "    (i32.const 99) (i32.const 7) (local.get 0)"
      "    (br_table 0 1) (i32.const 0))"
      "   (i32.add))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t zero[] = {0}, one[] = {1}, four[] = {4};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 44u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, one, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 48u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, four, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 4u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 19u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, one, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 18u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, zero, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 48u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, one, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  for (uint32_t i = 0; i < 4; ++i) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, i, &stats), GLWASM_OK);
    EXPECT_EQ(stats.guest_visible_exits, 0u) << "function " << i;
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u)
        << "function " << i;
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierRunsLoopBackedgesWithExactFrameControlState) {
  static const uint8_t source[] = "(module"
      " (func (param $n i32) (result i64)"
      " (local $i i32) (local $sum i64)"
      " (block $done (loop $again"
      "   (br_if $done (i32.ge_u (local.get $i) (local.get $n)))"
      "   (local.set $sum (i64.add (local.get $sum)"
      "     (i64.extend_i32_u (local.get $i))))"
      "   (local.set $i (i32.add (local.get $i) (i32.const 1)))"
      "   (br $again)))"
      " (local.get $sum))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t argument[] = {10};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, argument, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 45u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.completed_without_deopt, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, MutableLocalKeepsEarlierStackCopyAcrossLoopAndDeopt) {
  static const uint8_t source[] = "(module"
      " (table 1 funcref)"
      " (func (param $n i32) (result i32)"
      "  (local $x i32) (local $i i32)"
      "  (local.set $x (i32.const 3))"
      "  (local.get $x)"
      "  (block $done (loop $again"
      "   (br_if $done (i32.ge_u (local.get $i) (local.get $n)))"
      "   (local.set $x (i32.add (local.get $x) (i32.const 2)))"
      "   (local.set $i (i32.add (local.get $i) (i32.const 1)))"
      "   (br $again)))"
      "  (i32.const 0) (ref.null func) (table.set 0)"
      "  (i32.add (local.get $x))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *interpreter = nullptr, *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &interpreter), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(interpreter,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t iterations[] = {4};
  uint64_t expected = 0, actual = 0;
  ASSERT_EQ(glwasm_runtime_invoke(interpreter, 0, iterations, 1, &expected),
      GLWASM_OK);
  EXPECT_EQ(expected, 14u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, iterations, 1, &actual),
      GLWASM_OK);
  EXPECT_EQ(actual, expected);
  /* table.set exits after the local changed. Rebuilding requires both its
   * current value (11) and the older operand copy (3). */
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x26), 1u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 1u);
  GLWASM_Runtime *paused_runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &paused_runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(paused_runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_request_test_pause(paused_runtime), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(paused_runtime, 0, iterations, 1, &actual),
      GLWASM_ERR_PAUSED);
  int paused = 1;
  ASSERT_EQ(glwasm_runtime_resume(paused_runtime, &actual, &paused),
      GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(actual, expected);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(paused_runtime, 0, &stats),
      GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_PAUSE], 1u);
  glwasm_runtime_destroy(paused_runtime);
  glwasm_runtime_destroy(runtime);
  glwasm_runtime_destroy(interpreter);
  glwasm_module_destroy(module);
}

TEST(JitCache, ConditionalBranchDiscardsOnlyTakenPathOperands) {
  static const uint8_t source[] = "(module"
      " (func (export \"branch\") (param i32) (result i32)"
      "  (local $saved i32)"
      "  (block $done"
      "   (i32.const 40) (local.get 0) (br_if $done)"
      "   (local.set $saved))"
      "  (local.get $saved)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t taken[] = {1}, fallthrough[] = {0};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, taken, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 0u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, fallthrough, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 40u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 2u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, DeadFinalEndAfterEarlyReturnKeepsCompiledReturn) {
  static const uint8_t source[] = "(module"
      " (func (export \"early\") (result i32)"
      "  (i32.const 7) (return) (i32.const 11)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, BranchJoinRestoresOperandShapeAfterOtherPathReturns) {
  static const uint8_t source[] = "(module"
      " (func (export \"join\") (param i32) (result i32)"
      "  (block $done"
      "   (i32.const 99) (local.get 0) (br_if $done)"
      "   (drop) (i32.const 7) (return))"
      "  (i32.const 42)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t taken[] = {1}, fallthrough[] = {0};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, taken, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, fallthrough, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, 7u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 2u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, LongShallowFunctionUsesVerifiedFrameDepthAndPcBlockBudget) {
  std::string source = "(module (func (result i32)";
  source += " (local";
  for (unsigned i = 0; i < 64; ++i) source += " i32";
  source += ")";
  for (unsigned i = 0; i < 4100; ++i) source += " nop";
  source += " (i32.const 9)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      nullptr, nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 9u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, VerifiedFrameAboveDefaultJitSlotBudgetCompiles) {
  std::string source = "(module (func (result i32) (local $value i32) (local";
  for (unsigned i = 0; i < 4089; ++i) source += " i32";
  source += ") (i32.const 9) (local.set $value) (local.get $value)))";
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_TRUSTED, &policy), GLWASM_OK);
  GLWASM_Module *module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse_with_policy(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      &policy, nullptr, &error, &module), GLWASM_OK) << error.message;
  ASSERT_GT(glwasm_frame_slots(&module->funcs[0]), 4096u);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 9u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierSelectMergesCompiledValuesForBothConditions) {
  static const uint8_t source[] = "(module"
      " (func (param i32 i32 i32) (result i32)"
      "  (local.get 0) (local.get 1) (local.get 2) select)"
      " (func (param i64 i64 i32) (result i64)"
      "  (local.get 0) (local.get 1) (local.get 2)"
      "  (select (result i64))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t yes[] = {7, 9, 1};
  uint64_t no[] = {7, 9, 0};
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, yes, 3, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, no, 3, &result), GLWASM_OK);
  EXPECT_EQ(result, 9u);
  uint64_t wide_yes[] = {UINT64_C(0x100000001), UINT64_C(0x200000002), 1};
  uint64_t wide_no[] = {UINT64_C(0x100000001), UINT64_C(0x200000002), 0};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, wide_yes, 3, &result), GLWASM_OK);
  EXPECT_EQ(result, wide_yes[0]);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, wide_no, 3, &result), GLWASM_OK);
  EXPECT_EQ(result, wide_no[1]);
  GLWASM_JitCacheStats untyped{}, typed{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &untyped), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &typed), GLWASM_OK);
  EXPECT_EQ(untyped.guest_visible_exits, 0u);
  EXPECT_EQ(typed.guest_visible_exits, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierMemoryGrowthAndTableExitPreserveInterpreterEffects) {
  static const uint8_t source[] = "(module"
      " (memory 1 2)"
      " (table 1 funcref)"
      " (type $value (func (result i32)))"
      " (func $value (type $value) (result i32) (i32.const 42))"
      " (elem (i32.const 0) $value)"
      " (func (result i32) (memory.grow (i32.const 1)))"
      " (func (param i32) (result i32)"
      " (ref.is_null (table.get 0 (local.get 0))))"
      " (func (result i32)"
      " (i32.store (i32.const 0) (i32.const 42))"
      " (i32.load (i32.const 0)))"
      " (func (result i32) (i32.load (i32.const 131072)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 1, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 1u);
  const uint64_t index[] = {0};
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 2, index, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 0u);
  const uint64_t out_of_bounds[] = {1};
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 2, out_of_bounds, 1, &result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 3, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 4, nullptr, 0, &result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),
      GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
  GLWASM_JitCacheStats growth{}, table{}, memory{}, bad_memory{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &growth), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &table), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 3, &memory), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 4, &bad_memory), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x40), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x25), 2u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x36), 0u);
  EXPECT_EQ(growth.compiled_entries, 1u);
  EXPECT_EQ(growth.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
  EXPECT_EQ(growth.guest_visible_exits, 0u);
  EXPECT_EQ(table.compiled_entries, 2u);
  EXPECT_EQ(table.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 2u);
  EXPECT_EQ(memory.compiled_entries, 1u);
  EXPECT_EQ(memory.guest_visible_exits, 0u);
  EXPECT_EQ(bad_memory.exit_counts[GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION], 1u);
  EXPECT_EQ(bad_memory.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x11), 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x28), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, NoMaximumMemoryDeclarationHonorsRuntimeResourceCap) {
  static const uint8_t source[] = "(module"
      " (memory 1)"
      " (func (result i32) (memory.grow (i32.const 1)))"
      ")";
  GLWASM_Limits limits{};
  ASSERT_EQ(glwasm_limits_default(&limits), GLWASM_OK);
  limits.memory_initial = 65536;
  limits.memory_maximum = 65536;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, &limits,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, UINT32_MAX);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);

  static const uint8_t too_large[] = "(module (memory 2))";
  module = nullptr;
  EXPECT_EQ(glwasm_wat_module_parse(too_large, sizeof(too_large) - 1,
      &limits, nullptr, nullptr, &module), GLWASM_ERR_LIMIT);
  EXPECT_EQ(module, nullptr);
}

TEST(JitCache, DeoptPreservesExactFloatingPointPayloadBits) {
  // Types: (f32) -> f32 and (f64) -> f64. Each wrapper keeps its float below
  // memory.size's result. JIT lowers memory.size through a named exit, so
  // reconstruction must preserve that raw value.
  static const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x0b,0x02,
        0x60,0x01,0x7d,0x01,0x7d,
        0x60,0x01,0x7c,0x01,0x7c,
      0x03,0x03,0x02,0x00,0x01,
      0x05,0x04,0x01,0x01,0x01,0x01,
      0x0a,0x15,0x02,
        0x09,0x00,0x20,0x00,0x01,0x01,0x3f,0x00,0x1a,0x0b,
        0x09,0x00,0x20,0x00,0x01,0x01,0x3f,0x00,0x1a,0x0b,
  };
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode(wasm, sizeof(wasm), nullptr, nullptr, nullptr,
      &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER), GLWASM_OK);
  const uint64_t f32_payload[] = {UINT32_C(0x7fc12345)};
  const uint64_t f64_payload[] = {UINT64_C(0x7ff8123456789abc)};
  uint64_t result = 0;
  // First execute through the interpreter so the binary fixture's semantics
  // are checked independently of JIT reconstruction.
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, f32_payload, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, f32_payload[0]);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, f64_payload, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, f64_payload[0]);
  glwasm_runtime_destroy(runtime);
  runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, f32_payload, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, f32_payload[0]);
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 1, f64_payload, 1, &result),
      GLWASM_OK);
  EXPECT_EQ(result, f64_payload[0]);
  for (uint32_t function = 0; function <= 1; ++function) {
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, function, &stats),
        GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u);
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
  }
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x3f), 2u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierFloatSignatureExecutesCompiledArithmetic) {
  static const uint8_t source[] = "(module"
      " (func (param f32 f32) (result f32)"
      " (f32.add (local.get 0) (local.get 1)))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t arguments[] = {UINT32_C(0x3fc00000), UINT32_C(0x40100000)};
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, arguments, 2, &result),
      GLWASM_OK);
  EXPECT_EQ(result, UINT32_C(0x40700000));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.signature_refusal_count, 0u);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 0u);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x92), 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCountsEveryTrappingFloatConversionBeforeExecution) {
  struct Case {
    const char *op;
    const char *type;
    const char *value;
    uint16_t opcode;
  };
  const Case cases[] = {
      {"i32.trunc_f32_s", "i32", "f32", 0xa8},
      {"i32.trunc_f32_u", "i32", "f32", 0xa9},
      {"i32.trunc_f64_s", "i32", "f64", 0xaa},
      {"i32.trunc_f64_u", "i32", "f64", 0xab},
      {"i64.trunc_f32_s", "i64", "f32", 0xae},
      {"i64.trunc_f32_u", "i64", "f32", 0xaf},
      {"i64.trunc_f64_s", "i64", "f64", 0xb0},
      {"i64.trunc_f64_u", "i64", "f64", 0xb1},
  };
  std::string source = "(module";
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    source += " (func (export \"convert" + std::to_string(i) +
        "\") (result " + cases[i].type + ") (" + cases[i].op +
        " (" + cases[i].value + ".const nan)))";
  }
  source += ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      nullptr, nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    SCOPED_TRACE(cases[i].op);
    const std::string name = "convert" + std::to_string(i);
    EXPECT_EQ(glwasm_runtime_invoke_export(runtime, name.c_str(), nullptr, 0,
        &result), GLWASM_ERR_GUEST);
    EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_INTEGER_OVERFLOW);
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime,
        static_cast<uint32_t>(i), &stats), GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries, 1u);
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED], 1u);
    EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime,
        cases[i].opcode), 1u);
  }
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierMemoryGrowReturnsOldPagesWithoutReplay) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x06,0x01,0x60,0x01,0x7f,0x01,0x7f,
      0x03,0x02,0x01,0x00,
      0x05,0x03,0x01,0x00,0x01,
      0x0a,0x08,0x01,0x06,0x00,0x20,0x00,0x40,0x00,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  policy.max_jit_code_bytes = 1024u * 1024u;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t args[] = {1};
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, args, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 1u) << "memory.grow returns the old page count";
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_test_jit_opcode_exit_count(runtime, 0x40), 0u);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_RETURN], 1u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  EXPECT_EQ(glwasm_runtime_test_fuel_used(runtime), 3u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierReportsUnsupportedValueSignature) {
  static const uint8_t source[] = "(module"
      " (func (param externref) (result i32) (i32.const 1))"
      ")";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  const uint64_t argument[] = {0};
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, argument, 1, &result),
      GLWASM_ERR_UNSUPPORTED);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.signature_refusal_count, 1u);
  EXPECT_EQ(stats.compiled_entries, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierReportsActivationLimitInsteadOfInterpreting) {
  const uint8_t wasm[] = {
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x04,0x01,0x60,0x00,0x00,
      0x03,0x02,0x01,0x00,
      0x0a,0x04,0x01,0x02,0x00,0x0b};
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_STANDARD, &policy), GLWASM_OK);
  policy.default_native_depth = 0;
  policy.max_jit_code_bytes = 1024u * 1024u;
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm, sizeof(wasm), &policy,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime, 0, nullptr, 0, &result),
      GLWASM_ERR_LIMIT);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierRunsInAnExternalNestedContext) {
  static const uint8_t source[] =
      "(module (func (export \"run\") (result i32) (i32.const 42)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  GRCORE_Group *group = nullptr;
  GRCORE_Context *context = nullptr;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &group), GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group, nullptr, &context), GRCORE_OK);
  GRCORE_EngineId caller = 0;
  ASSERT_EQ(grcore_engine_register(context, &external_caller, &caller),
      GRCORE_OK);
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_nested(runtime, context, caller, 0x9911,
      "run", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compiled_entries, 1u);
  EXPECT_EQ(stats.signature_refusal_count, 0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
  EXPECT_EQ(grcore_context_destroy(context), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
}

TEST(JitCache, RequiredTierCompilesI32BitwiseOperations) {
  static const uint8_t source[] =
      "(module (func (export \"run\") (param i32 i32) (result i32) "
      "(i32.and (local.get 0) (local.get 1))))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
  const uint64_t arguments[] = {UINT64_C(0x55aa), UINT64_C(0x0ff0)};
  uint64_t result = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "run", arguments, 2,
      &result), GLWASM_OK);
  EXPECT_EQ(result, UINT64_C(0x05a0));
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compile_successes, 1u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, RequiredTierCompilesEveryIntegerBitwiseOperatorAtItsWidth) {
  struct Case {
    const char *name;
    const char *left_type;
    const char *right_type;
    const char *op;
    uint64_t left;
    uint64_t right;
    uint64_t expected;
  };
  const Case cases[] = {
      {"i32.and", "i32", "i32", "and", 0xf0f0u, 0x0ff0u, 0x00f0u},
      {"i32.or", "i32", "i32", "or", 0xf0f0u, 0x0ff0u, 0xfff0u},
      {"i32.xor", "i32", "i32", "xor", 0xf0f0u, 0x0ff0u, 0xff00u},
      {"i32.shl", "i32", "i32", "shl", 1u, 4u, 16u},
      {"i32.shr_s", "i32", "i32", "shr_s", UINT32_C(0x80000000), 1u,
          UINT32_C(0xc0000000)},
      {"i32.shr_u", "i32", "i32", "shr_u", UINT32_C(0x80000000), 1u,
          UINT32_C(0x40000000)},
      {"i32.rotl", "i32", "i32", "rotl", UINT32_C(0x80000001), 1u, 3u},
      {"i32.rotr", "i32", "i32", "rotr", 1u, 1u, UINT32_C(0x80000000)},
      {"i64.and", "i64", "i64", "and", UINT64_C(0xf0f0f0f0f0f0f0f0),
          UINT64_C(0x0ff00ff00ff00ff0), UINT64_C(0x00f000f000f000f0)},
      {"i64.or", "i64", "i64", "or", UINT64_C(0xf0f0f0f0f0f0f0f0),
          UINT64_C(0x0ff00ff00ff00ff0), UINT64_C(0xfff0fff0fff0fff0)},
      {"i64.xor", "i64", "i64", "xor", UINT64_C(0xf0f0f0f0f0f0f0f0),
          UINT64_C(0x0ff00ff00ff00ff0), UINT64_C(0xff00ff00ff00ff00)},
      {"i64.shl", "i64", "i64", "shl", 1u, 63u,
          UINT64_C(0x8000000000000000)},
      {"i64.shr_s", "i64", "i64", "shr_s", UINT64_C(0x8000000000000000),
          1u, UINT64_C(0xc000000000000000)},
      {"i64.shr_u", "i64", "i64", "shr_u", UINT64_C(0x8000000000000000),
          1u, UINT64_C(0x4000000000000000)},
      {"i64.rotl", "i64", "i64", "rotl", UINT64_C(0x8000000000000000),
          1u, 1u},
      {"i64.rotr", "i64", "i64", "rotr", 1u, 1u,
          UINT64_C(0x8000000000000000)},
  };
  for (const Case &test : cases) {
    SCOPED_TRACE(test.name);
    const std::string source = std::string("(module (func (export \"run\") ")+
        "(param "+test.left_type+" "+test.right_type+") (result "+
        test.left_type+") (local.get 0) (local.get 1) ("+test.name+")))";
    GLWASM_Module *module = nullptr;
    ASSERT_EQ(glwasm_wat_module_parse(
        reinterpret_cast<const uint8_t *>(source.data()), source.size(),
        nullptr, nullptr, nullptr, &module), GLWASM_OK);
    GLWASM_Runtime *runtime = nullptr;
    ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
    ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
        GLWASM_TEST_JIT_REQUIRE), GLWASM_OK);
    const uint64_t arguments[] = {test.left, test.right};
    uint64_t result = 0;
    EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "run", arguments, 2,
        &result), GLWASM_OK);
    EXPECT_EQ(result, test.expected);
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &stats), GLWASM_OK);
    EXPECT_EQ(stats.compile_successes, 1u);
    EXPECT_EQ(stats.compiled_entries, 1u);
    EXPECT_EQ(stats.guest_visible_exits, 0u);
    glwasm_runtime_destroy(runtime);
    glwasm_module_destroy(module);
  }
}

TEST(JitCache, RetiredNestedTargetsDeoptDirectAndIndirectCallersSafely) {
  static const char source[] =
      "(module "
      "(type $sig (func (result i32))) "
      "(table 1 funcref) "
      "(func $callee (type $sig) (i32.const 42)) "
      "(func (export \"run\") (result i32) (call $callee)) "
      "(func (export \"tail\") (result i32) (return_call $callee)) "
      "(func (export \"indirect\") (result i32) (i32.const 0) "
      "(call_indirect (type $sig))) "
      "(func (export \"tail_indirect\") (result i32) (i32.const 0) "
      "(return_call_indirect (type $sig))) "
      "(elem (i32.const 0) $callee))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source), sizeof(source) - 1,
      nullptr, nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "run", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "tail", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "indirect", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime, "tail_indirect", nullptr, 0,
      &result), GLWASM_OK);
  ASSERT_EQ(result, 42u);
  GLWASM_JitCacheStats direct_stats{}, tail_stats{}, indirect_stats{},
      tail_indirect_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &direct_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &tail_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 3, &indirect_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 4, &tail_indirect_stats),
      GLWASM_OK);
  ASSERT_EQ(direct_stats.compile_successes, 1u);
  ASSERT_EQ(tail_stats.compile_successes, 1u);
  ASSERT_EQ(indirect_stats.compile_successes, 1u);
  ASSERT_EQ(tail_indirect_stats.compile_successes, 1u);

  ASSERT_EQ(glwasm_runtime_test_retire_jit_function(runtime, 0), GLWASM_OK);
  GLWASM_JitCacheStats callee_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 0, &callee_stats),
      GLWASM_OK);
  ASSERT_TRUE(callee_stats.interpreter_only);

  /* All caller artifacts remain installed. Their direct/indirect call hooks
   * and tail-call slot helpers must deopt to the interpreter instead of
   * invoking a cleared entry pointer. */
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "run", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "tail", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "indirect", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime, "tail_indirect", nullptr, 0,
      &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 1, &direct_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 2, &tail_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 3, &indirect_stats),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime, 4, &tail_indirect_stats),
      GLWASM_OK);
  EXPECT_GT(direct_stats.guest_visible_exits, 0u);
  EXPECT_GT(tail_stats.guest_visible_exits, 0u);
  EXPECT_GT(indirect_stats.guest_visible_exits, 0u);
  EXPECT_GT(tail_indirect_stats.guest_visible_exits, 0u);

  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(JitCache, NestedAndTailCallsDriveTheGuestVisibleRetirementWindow) {
  static const uint8_t source[] = "(module"
      " (global $g i32 (i32.const 1))"
      " (memory 1)"
      " (func $callee (param i32) (result i32)"
      "   (local.get 0)"
      "   (if (then (memory.size) (drop)))"
      "   (i32.const 42))"
      " (func (export \"run\") (param i32) (result i32)"
      "   (local.get 0) (call $callee))"
      " (func (export \"tail\") (param i32) (result i32)"
      "   (local.get 0) (return_call $callee)))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  uint64_t result=0;
  const uint64_t no_exit[]={0};
  const uint64_t deopt[]={1};

  /* Fill 56 slots with ordinary nested returns. The callee must remain
   * compiled while the 64-entry observation window is not yet full. */
  for(size_t call=0;call<56;call++) {
    ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"run",no_exit,1,&result),
        GLWASM_OK)<<"normal nested return "<<call;
    ASSERT_EQ(result,42u);
  }
  GLWASM_JitCacheStats callee{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime,0,&callee),GLWASM_OK);
  EXPECT_FALSE(callee.interpreter_only);
  EXPECT_EQ(callee.window_entries,56u);
  EXPECT_EQ(callee.window_exits,0u);

  /* Four direct calls and three tail handoffs take the named memory.size exit.
   * The interpreter resumes and completes; both paths attribute the deopt to
   * the callee. */
  for(size_t call=0;call<4;call++) {
    ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"run",deopt,1,&result),
        GLWASM_OK)<<"deopting nested call "<<call;
    ASSERT_EQ(result,42u);
  }
  for(size_t call=0;call<3;call++) {
    ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"tail",deopt,1,&result),
        GLWASM_OK)<<"deopting tail handoff "<<call;
    ASSERT_EQ(result,42u);
  }
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime,0,&callee),GLWASM_OK);
  EXPECT_FALSE(callee.interpreter_only);
  EXPECT_EQ(callee.window_entries,63u);
  EXPECT_EQ(callee.window_exits,7u);
  EXPECT_EQ(callee.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED],7u);
  GLWASM_JitCacheStats tail{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime,2,&tail),GLWASM_OK);
  EXPECT_EQ(tail.completed_without_deopt,3u);

  ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"tail",deopt,1,&result),
      GLWASM_OK);
  ASSERT_EQ(result,42u);
  ASSERT_EQ(glwasm_runtime_test_jit_stats(runtime,0,&callee),GLWASM_OK);
  EXPECT_TRUE(callee.interpreter_only);
  EXPECT_EQ(callee.window_entries,64u);
  EXPECT_EQ(callee.window_exits,8u);
  EXPECT_EQ(callee.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED],8u);
  EXPECT_EQ(callee.refusal_counts[GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE],1u);

  /* Retirement removes only the native entry. The same nested guest code
   * remains callable through the interpreter on every caller path. */
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"run",deopt,1,&result),
      GLWASM_OK);
  EXPECT_EQ(result,42u);
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"tail",deopt,1,&result),
      GLWASM_OK);
  EXPECT_EQ(result,42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

} // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
