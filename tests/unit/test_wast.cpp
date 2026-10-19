/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * Focused interpreter, host API and exception tests. The pinned spec suite
 * itself runs in test_spec_suite.cpp.
 */
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/script.h>
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/policy.h>
#include "module_internal.h"
#include "runtime/runtime.h"
#include <ghoti.io/lang-wasm/parse.h>
#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <limits>
#include <vector>

namespace fs = std::filesystem;

static const GLWASM_HostValueType kHostI32[] = {GLWASM_HOST_I32};
static const GLWASM_HostValueType kHostI32I32[] = {GLWASM_HOST_I32,
    GLWASM_HOST_I32};

TEST(WasmRuntime, ValidatesArgumentsTrapsAndExecutesResolvedBrTable) {
  const std::string source =
      "(module"
      " (func (export \"after_block\") (result i32)"
      "   (block (result i32) (block (result i32) (i32.const 1))"
      "     (i32.const 2) (br 0) (i32.const 3)))"
      " (func (export \"table\") (param i32) (result i32)"
      "   (block (result i32) (i32.const 7) (local.get 0) (br_table 0 0)))"
      " (func (export \"nested_if\") (param i32) (result i32)"
      "   (block (result i32) (i32.const 42)"
      "     (if (local.get 0)"
      "       (then (if (i32.const 0) (then (i32.const 1) (drop))"
      "         (else (i32.const 2) (drop)))))"
      "     (br 0)))"
      " (func (export \"nan_min\") (result f32)"
      "   (f32.min (f32.const nan) (f32.const 1)))"
      " (func (export \"nan_min_f64\") (result f64)"
      "   (f64.min (f64.const nan) (f64.const 1)))"
      " (func (export \"nan_max_f64\") (result f64)"
      "   (f64.max (f64.const nan) (f64.const 1)))"
      " (func (export \"zero_min_f64\") (result f64)"
      "   (f64.min (f64.const 0) (f64.const -0)))"
      " (func (export \"zero_max_f64\") (result f64)"
      "   (f64.max (f64.const -0) (f64.const 0)))"
      " (func (export \"trap\") (result i32) unreachable))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError parse_error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      nullptr, nullptr, &parse_error, &module), GLWASM_OK)
      << parse_error.message;
  GLWASM_Runtime * runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  auto invoke = [&](const char * name, const uint64_t * args, uint32_t nargs,
                    uint64_t * result) {
    for (uint32_t i = 0; i < module->nexports; ++i) {
      const GLWASM_Export & exported = module->exports[i];
      if (exported.kind == GLWASM_EXT_FUNC &&
          std::string(exported.name.bytes, exported.name.length) == name)
        return glwasm_runtime_invoke(runtime, exported.index, args, nargs,
                                     result);
    }
    return GLWASM_ERR_INVALID;
  };
  uint64_t result = 0;
  EXPECT_EQ(invoke("after_block", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 2u);
  const uint64_t selector0[] = {0};
  const uint64_t selector1[] = {1};
  EXPECT_EQ(invoke("nested_if", selector0, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(invoke("nested_if", selector1, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(invoke("table", selector0, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  EXPECT_EQ(invoke("table", selector1, 1, &result), GLWASM_OK);
  EXPECT_EQ(result, 7u);
  EXPECT_EQ(invoke("nan_min", nullptr, 0, &result), GLWASM_OK);
  float nan_result = 0;
  std::memcpy(&nan_result, &result, sizeof(nan_result));
  EXPECT_TRUE(std::isnan(nan_result));
  EXPECT_EQ(invoke("nan_min_f64", nullptr, 0, &result), GLWASM_OK);
  double nan_result_f64 = 0;
  std::memcpy(&nan_result_f64, &result, sizeof(nan_result_f64));
  EXPECT_TRUE(std::isnan(nan_result_f64));
  EXPECT_EQ(invoke("nan_max_f64", nullptr, 0, &result), GLWASM_OK);
  std::memcpy(&nan_result_f64, &result, sizeof(nan_result_f64));
  EXPECT_TRUE(std::isnan(nan_result_f64));
  EXPECT_EQ(invoke("zero_min_f64", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, UINT64_C(0x8000000000000000));
  EXPECT_EQ(invoke("zero_max_f64", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, UINT64_C(0));
  EXPECT_EQ(invoke("table", nullptr, 0, &result), GLWASM_ERR_INVALID);
  EXPECT_EQ(invoke("table", selector0, 2, &result), GLWASM_ERR_INVALID);
  EXPECT_EQ(invoke("trap", nullptr, 0, &result), GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime), GLWASM_TRAP_UNREACHABLE);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, LinearMemoryIsInstanceLocalAndBoundsChecked) {
  const std::string source =
      "(module (memory 1 2)"
      " (func (export \"write\") (param i32 i32)"
      "   (local.get 0) (local.get 1) i32.store)"
      " (func (export \"read\") (param i32) (result i32)"
      "   (local.get 0) i32.load)"
      " (func (export \"grow\") (param i32) (result i32)"
      "   (local.get 0) memory.grow)"
      " (func (export \"size\") (result i32) memory.size))";
  GLWASM_Module *module=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()),source.size(),
      nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << " at "
      << error.location.line << ':' << error.location.column;
  GLWASM_Runtime *first=nullptr,*second=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&first),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module,&second),GLWASM_OK);
  auto call=[&](GLWASM_Runtime *runtime,const char *name,
      const uint64_t *args,uint32_t nargs,uint64_t *result) {
    for(uint32_t i=0;i<module->nexports;i++) {
      const GLWASM_Export &e=module->exports[i];
      if(e.kind==GLWASM_EXT_FUNC&&std::string(e.name.bytes,e.name.length)==name)
        return glwasm_runtime_invoke(runtime,e.index,args,nargs,result);
    }
    return GLWASM_ERR_INVALID;
  };
  uint64_t result=0;
  const uint64_t write_args[]={0,0x12345678};
  EXPECT_EQ(call(first,"write",write_args,2,&result),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(first,UINT64_MAX,64,1),GLWASM_OK);
  const uint64_t address[]={0};
  EXPECT_EQ(call(first,"read",address,1,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(0x12345678));
  EXPECT_EQ(call(second,"read",address,1,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(0));
  const uint64_t grow[]={1};
  EXPECT_EQ(call(first,"grow",grow,1,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(1));
  EXPECT_EQ(call(first,"size",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(2));
  const uint64_t edge[]={131072};
  EXPECT_EQ(call(first,"read",edge,1,&result),GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(first),GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
  glwasm_runtime_destroy(first);
  glwasm_runtime_destroy(second);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, BulkMemoryInitializesAndDropsPassiveData) {
  const std::string source =
      "(module (memory 1 1) (data \"abc\")"
      " (func (export \"bulk\") (result i32)"
      "   (memory.init 0 (memory 0) (i32.const 16) (i32.const 0) (i32.const 3))"
      "   (memory.copy (i32.const 32) (i32.const 16) (i32.const 3))"
      "   (memory.fill (i32.const 36) (i32.const 255) (i32.const 4))"
      "   (i32.load (i32.const 32))"
      "   (data.drop 0))"
      " (func (export \"zero\")"
      "   (memory.init 0 (memory 0) (i32.const 65536) (i32.const 0) (i32.const 0))))";
  GLWASM_Module *module=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()),source.size(),
      nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << " at "
      << error.location.line << ':' << error.location.column;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  uint64_t result=0;
  const GLWASM_Export &bulk=module->exports[0];
  ASSERT_EQ(glwasm_runtime_invoke(runtime,bulk.index,nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(0x00636261));
  uint32_t zero_function=module->exports[1].index;
  EXPECT_EQ(glwasm_runtime_invoke(runtime,zero_function,nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke(runtime,bulk.index,nullptr,0,&result),GLWASM_ERR_GUEST);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, Memory64AndTable64KeepWideAddressesAndReferences) {
  const std::string source =
      "(module (memory i64 1 2)"
      " (type $result (func (result i32)))"
      " (table $dispatch i64 1 2 funcref)"
      " (func $answer (type $result) (result i32) i32.const 42)"
      " (elem (table $dispatch) (i64.const 0) func $answer)"
      " (func (export \"store\") (param i64 i64)"
      "   (local.get 0) (local.get 1) i64.store)"
      " (func (export \"load\") (param i64) (result i64)"
      "   (local.get 0) i64.load)"
      " (func (export \"call\") (result i32)"
      "   (i64.const 0) (call_indirect (type $result) (table $dispatch)))"
      " (func (export \"call_at\") (param i64) (result i32)"
      "   (local.get 0) (call_indirect (type $result) (table $dispatch)))"
      " (func (export \"grow\") (result i64)"
      "   (table.grow $dispatch (ref.func $answer) (i64.const 1)) ))";
  GLWASM_Module *module=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()),source.size(),
      nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << " at "
      << error.location.line << ':' << error.location.column;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  auto call=[&](const char *name,const uint64_t *args,uint32_t nargs,uint64_t *result) {
    for(uint32_t i=0;i<module->nexports;i++) {
      const GLWASM_Export &e=module->exports[i];
      if(e.kind==GLWASM_EXT_FUNC&&std::string(e.name.bytes,e.name.length)==name)
        return glwasm_runtime_invoke(runtime,e.index,args,nargs,result);
    }
    return GLWASM_ERR_INVALID;
  };
  uint64_t result=0;
  const uint64_t store_args[]={0,UINT64_C(0xfedcba9876543210)};
  ASSERT_EQ(call("store",store_args,2,&result),GLWASM_OK);
  const uint64_t address[]={0};
  ASSERT_EQ(call("load",address,1,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(0xfedcba9876543210));
  ASSERT_EQ(call("call",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(42));
  ASSERT_EQ(call("grow",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(1));
  ASSERT_EQ(glwasm_runtime_set_test_limits(runtime,UINT64_MAX,64,1),GLWASM_OK);
  const uint64_t second_slot[]={1};
  ASSERT_EQ(call("call_at",second_slot,1,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(42));
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, BulkTableOperationsRespectReferenceAndDropSemantics) {
  const std::string source =
      "(module (type $result (func (result i32)))"
      " (table $tab i64 4 8 funcref)"
      " (func $answer (type $result) (result i32) i32.const 42)"
      " (elem $passive func $answer)"
      " (func (export \"bulk\")"
      "   (table.init $passive (table $tab) (i64.const 0) (i32.const 0) (i32.const 1))"
      "   (table.copy $tab $tab (i64.const 1) (i64.const 0) (i64.const 1))"
      "   (table.fill $tab (i64.const 2) (ref.null func) (i64.const 1))"
      "   (elem.drop $passive))"
      " (func (export \"call_copy\") (result i32)"
      "   (i64.const 1) (call_indirect (type $result) (table $tab)))"
      " (func (export \"call_null\") (result i32)"
      "   (i64.const 2) (call_indirect (type $result) (table $tab)))"
      " (func (export \"zero\")"
      "   (table.init $passive (table $tab) (i64.const 4) (i32.const 0) (i32.const 0))))";
  GLWASM_Module *module=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()),source.size(),
      nullptr,nullptr,&error,&module),GLWASM_OK) << error.message << " at "
      << error.location.line << ':' << error.location.column;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  auto call=[&](const char *name,uint64_t *result) {
    for(uint32_t i=0;i<module->nexports;i++) {
      const GLWASM_Export &e=module->exports[i];
      if(e.kind==GLWASM_EXT_FUNC&&std::string(e.name.bytes,e.name.length)==name)
        return glwasm_runtime_invoke(runtime,e.index,nullptr,0,result);
    }
    return GLWASM_ERR_INVALID;
  };
  uint64_t result=0;
  ASSERT_EQ(call("bulk",&result),GLWASM_OK);
  ASSERT_EQ(call("call_copy",&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(42));
  EXPECT_EQ(call("call_null",&result),GLWASM_ERR_GUEST);
  EXPECT_EQ(call("zero",&result),GLWASM_OK);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

static GLWASM_Result host_add(const GLWASM_HostCall *call,uint64_t *result) {
  if(!call||call->argument_count!=2||call->result_count!=1||
      call->types[0]!=GLWASM_HOST_I32||call->types[1]!=GLWASM_HOST_I32)
    return GLWASM_ERR_INVALID;
  *result=(uint32_t)call->arguments[0]+(uint32_t)call->arguments[1];
  return GLWASM_OK;
}

struct HostPauseFixture { int initial_calls=0; int resume_calls=0; };
static GLWASM_Result host_pause_once(const GLWASM_HostCall *call,uint64_t *) {
  if(!call||!call->user)return GLWASM_ERR_INVALID;
  auto *state=static_cast<HostPauseFixture *>(call->user);
  ++state->initial_calls;
  return GLWASM_ERR_PAUSED;
}
static GLWASM_Result host_resume_once(const GLWASM_HostCall *call,uint64_t *result) {
  if(!call||!call->user||!result)return GLWASM_ERR_INVALID;
  auto *state=static_cast<HostPauseFixture *>(call->user);
  ++state->resume_calls;
  *result=77;
  return GLWASM_OK;
}

static GLWASM_Runtime *parse_and_link(const std::string &source,
    const GLWASM_HostImport *imports,size_t count,GLWASM_Module **out_module,
    GLWASM_LinkError *link_error,GLWASM_Result *result);

TEST(WastAdapter, HostCallbackPauseUsesResumeContinuationExactlyOnce) {
  HostPauseFixture state;
  const GLWASM_HostImport binding={.module="host.v1",.field="step",
      .function=host_pause_once,.resume=host_resume_once,.user=&state,
      .kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
      .function_results=nullptr,.resume_results=nullptr,
      .parameters=nullptr,.parameter_count=0,.results=kHostI32,.result_count=1};
  const std::string source="(module (import \"host.v1\" \"step\" (func $step (result i32)))"
      " (func (export \"run\") (result i32) (call $step)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result result;
  GLWASM_Runtime *runtime=parse_and_link(source,&binding,1,&module,&link,&result);
  ASSERT_EQ(result,GLWASM_OK)<<link.reason;
  uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&value),GLWASM_ERR_PAUSED);
  EXPECT_EQ(state.initial_calls,1);EXPECT_EQ(state.resume_calls,0);
  GLWASM_Snapshot *snapshot=nullptr;
  EXPECT_EQ(glwasm_runtime_snapshot(runtime,&snapshot),GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(snapshot,nullptr);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&value,&paused),GLWASM_OK);
  EXPECT_EQ(paused,0);EXPECT_EQ(value,77u);
  EXPECT_EQ(state.initial_calls,1);EXPECT_EQ(state.resume_calls,1);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, PausedInstanceSnapshotRestoresMemoryAndExecution) {
  const std::string source=
      "(module (type $answer (func (result i32)))"
      " (memory 1 1)"
      " (global $g (mut i32) (i32.const 7))"
      " (table $tab i64 1 2 funcref)"
      " (func $answer_fn (type $answer) (result i32) (i32.const 42))"
      " (elem $passive func $answer_fn)"
      " (data (i32.const 8) \"abc\")"
      " (func (export \"pause\")"
      "   (global.get $g) (i32.const 1) (i32.add) (global.set $g)"
      "   (i32.store (i32.const 8) (i32.const 99))"
      "   (table.init $passive (table $tab) (i64.const 0) (i32.const 0) (i32.const 1))"
      "   (elem.drop $passive))"
      " (func (export \"getglobal\") (result i32) (global.get $g))"
      " (func (export \"load\") (result i32) (i32.load (i32.const 8)))"
      " (func (export \"call\") (result i32)"
      "   (i64.const 0) (call_indirect (type $answer) (table $tab)))"
      " (func (export \"init_dropped\")"
      "   (table.init $passive (table $tab) (i64.const 0) (i32.const 0) (i32.const 1)))"
      ")";
  GLWASM_Module *module=nullptr;GLWASM_ParseError parse_error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source.data()),
      source.size(),nullptr,nullptr,&parse_error,&module),GLWASM_OK)<<parse_error.message;
  GLWASM_Runtime *source_runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&source_runtime),GLWASM_OK);
  uint64_t value=0;
  ASSERT_EQ(glwasm_runtime_request_test_pause(source_runtime),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(source_runtime,"pause",nullptr,0,&value),GLWASM_ERR_PAUSED);
  GLWASM_Snapshot *snapshot=nullptr;
  ASSERT_EQ(glwasm_runtime_snapshot(source_runtime,&snapshot),GLWASM_OK);
  GLWASM_Runtime *restored=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&restored),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(restored,1000,64,0),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_restore(restored,snapshot),GLWASM_OK);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(restored,&value,&paused),GLWASM_OK);
  EXPECT_EQ(paused,0);
  ASSERT_EQ(glwasm_runtime_invoke_export(restored,"getglobal",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,8u);
  ASSERT_EQ(glwasm_runtime_invoke_export(restored,"load",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,99u);
  ASSERT_EQ(glwasm_runtime_invoke_export(restored,"call",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(restored,"init_dropped",nullptr,0,&value),GLWASM_ERR_GUEST);
  glwasm_snapshot_release(snapshot);glwasm_runtime_destroy(restored);
  glwasm_runtime_destroy(source_runtime);glwasm_module_destroy(module);
}

struct HostMemoryFixture { std::vector<uint8_t> bytes; int leases=0; };
static GLWASM_Result host_memory_begin(void *opaque,GLWASM_HostMemoryView *view) {
  auto *memory=static_cast<HostMemoryFixture *>(opaque);
  if(!memory||!view||memory->leases)return GLWASM_ERR_INTERNAL;
  ++memory->leases;view->bytes=memory->bytes.data();view->length_bytes=memory->bytes.size();
  return GLWASM_OK;
}
static void host_memory_end(void *opaque,GLWASM_HostMemoryView *) {
  auto *memory=static_cast<HostMemoryFixture *>(opaque);if(memory)--memory->leases;
}
static GLWASM_Result host_memory_grow(void *opaque,uint64_t delta,uint64_t *old) {
  auto *memory=static_cast<HostMemoryFixture *>(opaque);
  if(!memory||!old||memory->leases||delta>SIZE_MAX/65536u)return GLWASM_ERR_LIMIT;
  *old=memory->bytes.size()/65536u;
  try { memory->bytes.resize(memory->bytes.size()+static_cast<size_t>(delta)*65536u); }
  catch (...) { return GLWASM_ERR_OOM; }
  return GLWASM_OK;
}

struct HostTableFixture { std::vector<uint64_t> elements; int leases=0; };
static GLWASM_Result host_table_begin(void *opaque,GLWASM_HostTableView *view) {
  auto *table=static_cast<HostTableFixture *>(opaque);
  if(!table||!view||table->leases)return GLWASM_ERR_INTERNAL;
  ++table->leases;view->elements=table->elements.data();view->length=table->elements.size();
  return GLWASM_OK;
}
static void host_table_end(void *opaque,GLWASM_HostTableView *) {
  auto *table=static_cast<HostTableFixture *>(opaque);if(table)--table->leases;
}
static GLWASM_Result host_table_grow(void *opaque,uint64_t delta,uint64_t initial,uint64_t *old) {
  auto *table=static_cast<HostTableFixture *>(opaque);
  if(!table||!old||table->leases||delta>SIZE_MAX-table->elements.size())return GLWASM_ERR_LIMIT;
  *old=table->elements.size();
  try { table->elements.resize(table->elements.size()+static_cast<size_t>(delta),initial); }
  catch (...) { return GLWASM_ERR_OOM; }
  return GLWASM_OK;
}
static GLWASM_Result host_reference_check(void *,uint64_t token) {
  return token==55?GLWASM_OK:GLWASM_ERR_LINK;
}

struct BulkPauseFixture {
  GRCORE_Context *context=nullptr;
  int end_calls=0;
  bool posted=false;
};
static GLWASM_Result host_capture_bulk_context(const GLWASM_HostCall *call,uint64_t *) {
  if(!call||!call->user||!call->context)return GLWASM_ERR_INVALID;
  static_cast<BulkPauseFixture *>(call->user)->context=call->context;
  return GLWASM_OK;
}
static void post_bulk_pause(BulkPauseFixture *state) {
  if(!state||!state->context||state->posted||++state->end_calls!=2)return;
  GRCORE_Port *port=nullptr;
  if(grcore_context_port(state->context,&port)!=GRCORE_OK)return;
  state->posted=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT)==GRCORE_OK;
  grcore_port_release(port);
}
struct BulkPauseMemory { BulkPauseFixture pause; std::vector<uint8_t> bytes; int leases=0; int begins=0; };
static GLWASM_Result bulk_pause_memory_begin(void *opaque,GLWASM_HostMemoryView *view) {
  auto *memory=static_cast<BulkPauseMemory *>(opaque);
  if(!memory||!view||memory->leases)return GLWASM_ERR_INTERNAL;
  ++memory->leases;++memory->begins;view->bytes=memory->bytes.data();view->length_bytes=memory->bytes.size();
  return GLWASM_OK;
}
static void bulk_pause_memory_end(void *opaque,GLWASM_HostMemoryView *) {
  auto *memory=static_cast<BulkPauseMemory *>(opaque);
  if(!memory)return;
  --memory->leases;post_bulk_pause(&memory->pause);
}
struct BulkPauseTable { BulkPauseFixture pause; std::vector<uint64_t> elements; int leases=0; int begins=0; int validations=0; };
static GLWASM_Result bulk_pause_table_begin(void *opaque,GLWASM_HostTableView *view) {
  auto *table=static_cast<BulkPauseTable *>(opaque);
  if(!table||!view||table->leases)return GLWASM_ERR_INTERNAL;
  ++table->leases;++table->begins;view->elements=table->elements.data();view->length=table->elements.size();
  return GLWASM_OK;
}
static void bulk_pause_table_end(void *opaque,GLWASM_HostTableView *) {
  auto *table=static_cast<BulkPauseTable *>(opaque);
  if(!table)return;
  --table->leases;post_bulk_pause(&table->pause);
}
static GLWASM_Result bulk_pause_reference_check(void *opaque,uint64_t token) {
  auto *table=static_cast<BulkPauseTable *>(opaque);
  if(!table)return GLWASM_ERR_INVALID;
  ++table->validations;
  return token==55?GLWASM_OK:GLWASM_ERR_LINK;
}

static GLWASM_Runtime * parse_and_link(const std::string &source,
    const GLWASM_HostImport *imports,size_t count,GLWASM_Module **out_module,
    GLWASM_LinkError *link_error,GLWASM_Result *result) {
  GLWASM_ParseError error{};*out_module=nullptr;
  *result=glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source.data()),
      source.size(),nullptr,nullptr,&error,out_module);
  if(*result!=GLWASM_OK)return nullptr;
  GLWASM_Runtime *runtime=nullptr;
  *result=glwasm_runtime_create_with_imports(*out_module,imports,count,1,
      link_error,&runtime);
  if(*result!=GLWASM_OK){glwasm_module_destroy(*out_module);*out_module=nullptr;}
  return runtime;
}

TEST(WasmExceptions, FuelAndDepthLimitsBypassCatchAll) {
  const std::string source=
      "(module"
      " (func $recurse (block $h (try_table (catch_all $h) (call $recurse))))"
      " (func (export \"fuel\") (result i32)"
      "   (block $h (try_table (catch_all $h)"
      "     (loop $again (br $again)))) (i32.const 1))"
      " (func (export \"depth\") (result i32)"
      "   (block $h (try_table (catch_all $h) (call $recurse)))"
      "   (i32.const 2)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *fuel_runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  ASSERT_EQ(glwasm_runtime_set_test_limits(fuel_runtime,12,64,0),GLWASM_OK);
  const GLWASM_Result fuel_result=glwasm_runtime_invoke_export(fuel_runtime,
      "fuel",nullptr,0,&result);
  GLWASM_TestRuntimeFailureInfo fuel_failure{};
  ASSERT_EQ(glwasm_runtime_test_failure_info(fuel_runtime,1,&fuel_failure),
      GLWASM_OK);
  EXPECT_EQ(fuel_result,GLWASM_ERR_LIMIT);
  EXPECT_EQ(fuel_failure.run_error,GLWASM_ERR_LIMIT);
  EXPECT_EQ(fuel_failure.core_result,GRCORE_ERR_LIMIT);
  EXPECT_EQ(fuel_failure.outcome,GRCORE_OUTCOME_FINISHED);
  glwasm_runtime_destroy(fuel_runtime);

  GLWASM_Runtime *depth_runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&depth_runtime),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_set_test_limits(depth_runtime,UINT64_MAX,4,0),
      GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(depth_runtime,"depth",nullptr,0,
      &result),GLWASM_ERR_LIMIT);
  glwasm_runtime_destroy(depth_runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, ExceptionAllocationLimitBypassesCatchAll) {
  const std::string source=
      "(module (tag $t)"
      " (func (export \"run\") (local $count i32)"
      "   (loop $again"
      "     (block $caught"
      "       (try_table (catch_all $caught) (throw $t)))"
      "     (local.get $count) (i32.const 1) (i32.add)"
      "     (local.set $count) (br $again))))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  const GLWASM_Result limit_result=glwasm_runtime_invoke_export(runtime,
      "run",nullptr,0,&result);
  EXPECT_EQ(limit_result,GLWASM_ERR_LIMIT);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, PropagatesAcrossFramesAndHonorsClausePriority) {
  const std::string source=
      "(module (tag $a) (tag $b)"
      " (func $raise (throw $b))"
      " (func (export \"run\") (result i32)"
      "   (block $outer"
      "     (block $inner"
      "       (try_table (catch $a $outer) (catch $b $outer)"
      "         (catch $b $inner) (call $raise)))"
      "     (i32.const 3) (return))"
      "   (i32.const 7)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,7u);  // The first matching clause branches to $outer.
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, ImportedTagsMatchByHostIdentity) {
  const GLWASM_HostValueType parameters[]={GLWASM_HOST_I32};
  GLWASM_HostTag host_tag{UINT64_C(0xabc123), parameters, 1};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="first",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&host_tag},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0},
      {.module="host.v1",.field="second",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&host_tag},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0}};
  const std::string source=
      "(module"
      " (import \"host.v1\" \"first\" (tag $first (param i32)))"
      " (import \"host.v1\" \"second\" (tag $second (param i32)))"
      " (func $raise (i32.const 42) (throw $first))"
      " (func (export \"run\") (result i32)"
      "   (block $handled (result i32)"
      "     (try_table (result i32) (catch $second $handled)"
      "       (call $raise) (i32.const 3)))))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,imports,2,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),
      GLWASM_OK);
  EXPECT_EQ(result,42u);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, ImportedTagIdentityRequiresMatchingPayloadSignature) {
  const GLWASM_HostValueType first_parameter[]={GLWASM_HOST_I32};
  const GLWASM_HostValueType second_parameter[]={GLWASM_HOST_I64};
  GLWASM_HostTag first_tag{UINT64_C(0xabc123), first_parameter, 1};
  GLWASM_HostTag second_tag{UINT64_C(0xabc123), second_parameter, 1};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="first",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&first_tag},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0},
      {.module="host.v1",.field="second",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&second_tag},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0}};
  const std::string source=
      "(module"
      " (import \"host.v1\" \"first\" (tag $first (param i32)))"
      " (import \"host.v1\" \"second\" (tag $second (param i64)))"
      " (func $raise (i32.const 42) (throw $first))"
      " (func (export \"run\") (result i64)"
      "   (block $handled (result i64)"
      "     (try_table (result i64) (catch $second $handled)"
      "       (call $raise) (i64.const 3)))))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,imports,2,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),GLWASM_TRAP_NONE);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, TransfersTypedPayloadInDeclaredOrder) {
  const std::string source=
      "(module (type $pair (func (param i32 i64))) (tag $t (type $pair))"
      " (global $small (mut i32) (i32.const 0))"
      " (global $wide (mut i64) (i64.const 0))"
      " (func (export \"run\") (result i32)"
      "   (block $h (result i32 i64)"
      "     (try_table (result i32 i64) (catch $t $h)"
      "       (throw $t (i32.const 7) (i64.const 9))"
      "       (i32.const 2) (i64.const 3)))"
      "   (global.set $wide) (global.set $small) (global.get $small))"
      " (func (export \"wide\") (result i64) (global.get $wide)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,7u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"wide",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,9u);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, CatchRefPreservesIdentityForThrowRef) {
  const std::string source=
      "(module (tag $t (param i32))"
      " (func (export \"run\") (result i32) (local $ex exnref)"
      "   (block $h (result i32 exnref)"
      "     (try_table (result i32) (catch_ref $t $h)"
      "       (throw $t (i32.const 19)) (i32.const 2))"
      "     (return))"
      "   (drop) (return))"
      " (func (export \"rethrow\") (result i32) (local $ex exnref)"
      "   (block $h (result i32 exnref)"
      "     (try_table (result i32) (catch_ref $t $h)"
      "       (throw $t (i32.const 23)) (i32.const 2))"
      "     (return))"
      "   (local.set $ex) (local.get $ex) (throw_ref)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,19u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"rethrow",nullptr,0,&result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),GLWASM_TRAP_NONE);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, UncaughtAndNullReferenceOutcomesStayDistinct) {
  const std::string source=
      "(module (type $empty (func)) (tag $t (type $empty))"
      " (func $raise (throw $t))"
      " (func (export \"uncaught\") (call $raise))"
      " (func (export \"null_ref\") (ref.null exn) (throw_ref)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"uncaught",nullptr,0,&result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),GLWASM_TRAP_NONE);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"null_ref",nullptr,0,&result),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_trap(runtime),GLWASM_TRAP_INVALID_REFERENCE);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WasmExceptions, CatchAllRefCanBeRethrownAsTheSameLiveValue) {
  const std::string source=
      "(module (tag $t)"
      " (func (export \"run\") (result i32) (local $ex exnref)"
      "   (block $h (result exnref)"
      "     (try_table (result exnref) (catch_all_ref $h)"
      "       (throw $t) (ref.null exn)))"
      "   (local.set $ex)"
      "   (block $outer"
      "     (try_table (catch_all $outer) (local.get $ex) (throw_ref)))"
      "   (i32.const 31)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,nullptr,0,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,31u);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, MemoryBulkFillPausesAfterAChunkAndResumesRemainingBytesOnce) {
  BulkPauseMemory memory;memory.bytes.resize(65536);
  GLWASM_HostMemoryProvider provider{bulk_pause_memory_begin,
      bulk_pause_memory_end,host_memory_grow,&memory,1,1,0};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="arm",.function=host_capture_bulk_context,
       .resume=nullptr,.user=&memory.pause,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0},
      {.module="host.v1",.field="mem",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_MEMORY,
       .resource={.memory=&provider},.function_results=nullptr,
       .resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0}};
  const std::string source="(module (import \"host.v1\" \"arm\" (func $arm))"
      " (import \"host.v1\" \"mem\" (memory 1 1))"
      " (func (export \"run\") (call $arm)"
      " (memory.fill (i32.const 0) (i32.const 42) (i32.const 8192))))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,imports,2,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  const int begins_before=memory.begins;
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(memory.pause.posted);EXPECT_EQ(memory.leases,0);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_EQ(paused,0);EXPECT_EQ(memory.leases,0);
  EXPECT_EQ(memory.begins-begins_before,4);
  EXPECT_EQ(memory.bytes[0],42u);EXPECT_EQ(memory.bytes[4095],42u);
  EXPECT_EQ(memory.bytes[4096],42u);EXPECT_EQ(memory.bytes[8191],42u);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, TableBulkFillPausesAfterAChunkAndResumesRemainingEntriesOnce) {
  BulkPauseTable table;table.elements.resize(2048);
  GLWASM_HostTableProvider provider{bulk_pause_table_begin,bulk_pause_table_end,
      host_table_grow,GLWASM_HOST_EXTERNREF,bulk_pause_reference_check,&table,
      0,0,0};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="arm",.function=host_capture_bulk_context,
       .resume=nullptr,.user=&table.pause,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0},
      {.module="host.v1",.field="tab",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_TABLE,
       .resource={.table=&provider},.function_results=nullptr,
       .resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0}};
  const std::string source="(module (import \"host.v1\" \"arm\" (func $arm))"
      " (import \"host.v1\" \"tab\" (table 2048 externref))"
      " (func (export \"run\") (param externref) (call $arm)"
      " (table.fill (i32.const 0) (local.get 0) (i32.const 2048))))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result status;
  GLWASM_Runtime *runtime=parse_and_link(source,imports,2,&module,&link,&status);
  ASSERT_EQ(status,GLWASM_OK)<<link.reason;
  const int begins_before=table.begins;
  const uint64_t args[]={55};uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",args,1,&result),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(table.pause.posted);EXPECT_EQ(table.leases,0);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_EQ(paused,0);EXPECT_EQ(table.leases,0);
  EXPECT_EQ(table.begins-begins_before,4);EXPECT_EQ(table.validations,2);
  ASSERT_EQ(table.elements.size(),2048u);
  EXPECT_TRUE(std::all_of(table.elements.begin(),table.elements.end(),
      [](uint64_t value){return value==55u;}));
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, PublicHostAPIHonorsStartFunctionGrant) {
  const std::string source="(module (global $g (mut i32) (i32.const 0))"
      " (func $start (i32.const 7) (global.set $g)) (start $start)"
      " (func (export \"get\") (result i32) (global.get $g)))";
  GLWASM_Module *module=nullptr;GLWASM_ParseError parse_error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source.data()),
      source.size(),nullptr,nullptr,&parse_error,&module),GLWASM_OK)
      << parse_error.message;
  GLWASM_LinkError link{};GLWASM_Runtime *runtime=nullptr;
  EXPECT_EQ(glwasm_runtime_create_with_imports(module,nullptr,0,0,&link,
      &runtime),GLWASM_ERR_LINK);
  EXPECT_EQ(link.reason,GLWASM_LINK_START_NOT_ALLOWED);
  EXPECT_EQ(runtime,nullptr);
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,nullptr,0,1,&link,
      &runtime),GLWASM_OK)<<link.reason;
  uint64_t value=0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"get",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,7u);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, ImportedMemoryProviderUsesShortRelocationSafeLeases) {
  const std::string source="(module (import \"host.v1\" \"mem\" (memory 1 2))"
      " (func (export \"store\") (param i32 i32) (local.get 0) (local.get 1) (i32.store))"
      " (func (export \"load\") (param i32) (result i32) (local.get 0) (i32.load))"
      " (func (export \"grow\") (result i32) (i32.const 1) (memory.grow 0))"
      " (data (i32.const 8) \"abc\"))";
  HostMemoryFixture memory;memory.bytes.resize(65536);
  GLWASM_HostMemoryProvider provider{host_memory_begin,host_memory_end,host_memory_grow,&memory,2,1,0};
  GLWASM_HostImport binding{.module="host.v1",.field="mem",.function=nullptr,
      .resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_MEMORY,.resource={.memory=&provider},
      .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0};
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result result;
  GLWASM_Runtime *runtime=parse_and_link(source,&binding,1,&module,&link,&result);
  ASSERT_EQ(result,GLWASM_OK)<<link.reason;ASSERT_NE(runtime,nullptr);
  const uint64_t store_args[]={0,UINT32_C(0x12345678)};uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"store",store_args,2,&value),GLWASM_OK);
  EXPECT_EQ(memory.leases,0);EXPECT_EQ(memory.bytes[0],0x78);
  EXPECT_EQ(memory.bytes[8],'a');EXPECT_EQ(memory.bytes[10],'c');
  const uint64_t load_args[]={0};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"load",load_args,1,&value),GLWASM_OK);
  EXPECT_EQ(value,UINT32_C(0x12345678));
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"grow",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,1u);EXPECT_EQ(memory.bytes.size(),2u*65536u);EXPECT_EQ(memory.leases,0);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);
}

TEST(WastAdapter, ImportedProvidersCannotExceedDeclaredMaximums) {
  GLWASM_LinkError link{};GLWASM_Result status;GLWASM_Module *module=nullptr;
  HostMemoryFixture oversized_memory;oversized_memory.bytes.resize(2u*65536u);
  GLWASM_HostMemoryProvider memory_provider{host_memory_begin,host_memory_end,
      host_memory_grow,&oversized_memory,2,1,0};
  const GLWASM_HostImport memory_import={.module="host.v1",.field="mem",
      .function=nullptr,.resume=nullptr,.user=nullptr,
      .kind=GLWASM_HOST_IMPORT_MEMORY,.resource={.memory=&memory_provider},
      .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0};
  GLWASM_Runtime *runtime=parse_and_link(
      "(module (import \"host.v1\" \"mem\" (memory 1 1)))",
      &memory_import,1,&module,&link,&status);
  EXPECT_EQ(status,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  EXPECT_EQ(oversized_memory.leases,0);

  HostTableFixture oversized_table{{0,0,0}};
  GLWASM_HostTableProvider table_provider{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_EXTERNREF,host_reference_check,&oversized_table,
      3,1,0};
  const GLWASM_HostImport table_import={.module="host.v1",.field="tab",
      .function=nullptr,.resume=nullptr,.user=nullptr,
      .kind=GLWASM_HOST_IMPORT_TABLE,.resource={.table=&table_provider},
      .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0};
  runtime=parse_and_link(
      "(module (import \"host.v1\" \"tab\" (table 1 2 externref)))",
      &table_import,1,&module,&link,&status);
  EXPECT_EQ(status,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  EXPECT_EQ(oversized_table.leases,0);
}

TEST(WastAdapter, ImportedTableAndGlobalProvidersValidateTypesAndTokens) {
  HostTableFixture table{{0,0}};
  GLWASM_HostTableProvider table_provider{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_EXTERNREF,host_reference_check,&table,0,0,0};
  uint64_t global_value=7;
  GLWASM_HostGlobal global{GLWASM_HOST_I32,1,&global_value,nullptr,nullptr};
  const GLWASM_HostImport bindings[]={
      {.module="host.v1",.field="tab",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_TABLE,.resource={.table=&table_provider},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0},
      {.module="host.v1",.field="g",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_GLOBAL,.resource={.global=&global},
       .function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0}};
  const std::string source="(module (import \"host.v1\" \"tab\" (table $tab 2 externref))"
      " (import \"host.v1\" \"g\" (global $g (mut i32)))"
      " (func (export \"setref\") (param externref) (i32.const 0) (local.get 0) (table.set 0))"
      " (func (export \"fillref\") (param externref) (table.fill $tab (i32.const 0) (local.get 0) (i32.const 2)))"
      " (func (export \"isnull\") (result i32) (i32.const 0) (table.get 0) (ref.is_null))"
      " (func (export \"setglobal\") (param i32) (local.get 0) (global.set $g))"
      " (func (export \"getglobal\") (result i32) (global.get $g)))";
  GLWASM_Module *module=nullptr;GLWASM_LinkError link{};GLWASM_Result result;
  GLWASM_Runtime *runtime=parse_and_link(source,bindings,2,&module,&link,&result);
  ASSERT_EQ(result,GLWASM_OK)<<link.reason;
  const uint64_t ref_arg[]={55};uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"setref",ref_arg,1,&value),GLWASM_OK);
  EXPECT_EQ(table.elements[0],55u);EXPECT_EQ(table.leases,0);
  const uint64_t stale_arg[]={99};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"setref",stale_arg,1,&value),GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"fillref",stale_arg,1,&value),GLWASM_ERR_GUEST);
  EXPECT_EQ(table.elements[0],55u);EXPECT_EQ(table.leases,0);
  EXPECT_EQ(table.elements[1],0u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"isnull",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,0u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_REQUIRE),GLWASM_OK);
  const uint64_t global_arg[]={42};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"setglobal",global_arg,1,&value),GLWASM_OK);
  EXPECT_EQ(global_value,42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"getglobal",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,42u);
  const uint64_t negative_global_arg[]={UINT32_MAX};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"setglobal",
      negative_global_arg,1,&value),GLWASM_OK);
  EXPECT_EQ(global_value,UINT32_MAX);
  global_value=84;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"getglobal",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,84u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(runtime,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_request_test_pause(runtime),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"isnull",nullptr,0,&value),GLWASM_ERR_PAUSED);
  GLWASM_Snapshot *unsupported_snapshot=nullptr;
  EXPECT_EQ(glwasm_runtime_snapshot(runtime,&unsupported_snapshot),GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(unsupported_snapshot,nullptr);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);

  GLWASM_HostGlobal wrong{GLWASM_HOST_I64,1,&global_value,nullptr,nullptr};
  const GLWASM_HostImport mismatch={.module="host.v1",.field="g",
      .function=nullptr,.resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_GLOBAL,
      .resource={.global=&wrong},.function_results=nullptr,.resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0};
  const std::string global_source="(module (import \"host.v1\" \"g\" (global (mut i32))))";
  runtime=parse_and_link(global_source,&mismatch,1,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);

  HostTableFixture wrong_table_state{{0,0}};
  GLWASM_HostTableProvider wrong_table{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_FUNCREF,nullptr,&wrong_table_state,0,0,0};
  const GLWASM_HostImport table_mismatch={.module="host.v1",.field="tab",
      .function=nullptr,.resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_TABLE,
      .resource={.table=&wrong_table},.function_results=nullptr,
      .resume_results=nullptr,.parameters=nullptr,.parameter_count=0,.results=nullptr,.result_count=0};
  const std::string table_source="(module (import \"host.v1\" \"tab\" (table 2 externref)))";
  runtime=parse_and_link(table_source,&table_mismatch,1,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  runtime=parse_and_link(table_source,nullptr,0,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(link.reason,GLWASM_LINK_UNRESOLVED);
}

TEST(WastAdapter, PublicHostAPIRequiresExplicitVersionedImportGrant) {
  auto imported_sum = [](const char *version) {
    std::vector<uint8_t> wasm={
      0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
      0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
      0x02,0x0f,0x01,0x07,
      static_cast<uint8_t>(version[0]),static_cast<uint8_t>(version[1]),
      static_cast<uint8_t>(version[2]),static_cast<uint8_t>(version[3]),
      static_cast<uint8_t>(version[4]),static_cast<uint8_t>(version[5]),
      static_cast<uint8_t>(version[6]),
      0x03,'s','u','m',0x00,0x00};
    return wasm;
  };
  std::vector<uint8_t> wasm=imported_sum("tang.v1");
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED,&policy),GLWASM_OK);
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm.data(),wasm.size(),&policy,
      nullptr,nullptr,&module),GLWASM_OK);
  GLWASM_Runtime *runtime=nullptr;
  GLWASM_LinkError link_error{};
  const GLWASM_HostImport imports[]={
      {.module="tang.v1",.field="sum",.function=host_add,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
       .function_results=nullptr,.resume_results=nullptr,
       .parameters=kHostI32I32,.parameter_count=2,.results=kHostI32,.result_count=1}};
  EXPECT_EQ(glwasm_runtime_create_with_imports(module,imports,1,1,
      &link_error,&runtime),GLWASM_ERR_LINK);
  EXPECT_EQ(link_error.reason,GLWASM_LINK_UNRESOLVED);
  EXPECT_EQ(link_error.module,std::string("tang.v1"));
  ASSERT_EQ(runtime,nullptr);
  glwasm_module_destroy(module);

  policy.allow_tang_v1_imports=1;
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm.data(),wasm.size(),&policy,
      nullptr,nullptr,&module),GLWASM_OK);
  const GLWASM_HostImport wrong_field[]={
      {.module="tang.v1",.field="other",.function=host_add,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
       .function_results=nullptr,.resume_results=nullptr,
       .parameters=kHostI32I32,.parameter_count=2,.results=kHostI32,.result_count=1}};
  EXPECT_EQ(glwasm_runtime_create_with_imports(module,wrong_field,1,1,
      &link_error,&runtime),GLWASM_ERR_LINK);
  EXPECT_EQ(link_error.reason,GLWASM_LINK_UNRESOLVED);
  ASSERT_EQ(runtime,nullptr);
  GLWASM_Runtime *linked=nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,imports,1,1,
      &link_error,&linked),GLWASM_OK);
  const uint64_t arguments[]={40,2};
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke(linked,0,arguments,2,&result),GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(42));
  glwasm_runtime_destroy(linked);
  glwasm_module_destroy(module);

  wasm=imported_sum("tang.v2");
  ASSERT_EQ(glwasm_module_decode_with_policy(wasm.data(),wasm.size(),&policy,
      nullptr,nullptr,&module),GLWASM_OK);
  GLWASM_HostImport wrong_version[]={
      {.module="tang.v2",.field="sum",.function=host_add,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={},
       .function_results=nullptr,.resume_results=nullptr,
       .parameters=kHostI32I32,.parameter_count=2,.results=kHostI32,.result_count=1}};
  EXPECT_EQ(glwasm_runtime_create_with_imports(module,wrong_version,1,1,
      &link_error,&runtime),GLWASM_ERR_LINK);
  EXPECT_EQ(link_error.reason,GLWASM_LINK_UNRESOLVED);
  EXPECT_EQ(link_error.module,std::string("tang.v2"));
  ASSERT_EQ(runtime,nullptr);
  glwasm_module_destroy(module);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
