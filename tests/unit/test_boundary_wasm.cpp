/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/host.h>
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/runtime-core/a/boundary.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/builder.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/natives.h>
#include <ghoti.io/runtime-jit/verify.h>
#include "runtime/runtime.h"
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {
struct Fixture {
  GRCORE_EngineId engine = 0;
  unsigned initial_calls = 0;
  unsigned resume_calls = 0;
  uintptr_t token = 0;
  uint64_t exception_id = 0;
  bool fail_after_boundary = false;
};

GLWASM_Result cross_boundary(const GLWASM_HostCall *call, Fixture *f) {
  auto *stack = grcore_context_stack(call->context);
  GRCORE_BoundaryRef boundary{};
  if (grcore_boundary_enter(stack, call->engine, f->engine,
          call->instance_token, true, &boundary) != GRCORE_OK)
    return GLWASM_ERR_INTERNAL;
  uintptr_t active = 0;
  GRCORE_EngineId origin = 0;
  uint64_t identity = 0;
  GRCORE_Result result = grcore_boundary_active_instance(stack, f->engine,
      &active);
  if (result == GRCORE_OK && active != call->instance_token)
    result = GRCORE_ERR_INVALID;
  const uint64_t expected = static_cast<uint64_t>(call->instance_token) ^
      UINT64_C(0x5a5a1234);
  if (result == GRCORE_OK)
    result = grcore_boundary_set_foreign_exception(stack, boundary,
        f->engine, expected);
  if (result == GRCORE_OK)
    result = grcore_boundary_get_foreign_exception(stack, boundary, &origin,
        &identity);
  if (result == GRCORE_OK && (origin != f->engine || identity != expected))
    result = GRCORE_ERR_INVALID;
  if (f->token && f->token != active) return GLWASM_ERR_INTERNAL;
  f->token = active;
  f->exception_id = identity;
  GRCORE_Result left = grcore_boundary_leave(stack, boundary);
  if (result != GRCORE_OK || left != GRCORE_OK) return GLWASM_ERR_INTERNAL;
  return f->fail_after_boundary ? GLWASM_ERR_GUEST : GLWASM_OK;
}

GLWASM_Result initial(const GLWASM_HostCall *call, uint64_t *out) {
  auto *f = static_cast<Fixture *>(call->user);
  ++f->initial_calls;
  GLWASM_Result result = cross_boundary(call, f);
  if (result == GLWASM_OK) result = GLWASM_ERR_PAUSED;
  if (out) *out = 42;
  return result;
}

GLWASM_Result resume(const GLWASM_HostCall *call, uint64_t *out) {
  auto *f = static_cast<Fixture *>(call->user);
  ++f->resume_calls;
  GLWASM_Result result = cross_boundary(call, f);
  if (out) *out = 42;
  return result;
}

static const GRCORE_EngineDescriptor fixture_engine =
    GRCORE_ENGINE_DESCRIPTOR_INIT("wasm-boundary-fixture", nullptr, nullptr,
        nullptr, {nullptr, nullptr, nullptr}, {0, 0, 0}, nullptr, nullptr,
        nullptr, nullptr);

GLWASM_Module *module() {
  static const uint8_t source[] =
      "(module (import \"tang.v1\" \"op\" (func $op (result i32))) "
      "(func (export \"run\") (result i32) (call $op)))";
  GLWASM_Module *m = nullptr;
  if (glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr, nullptr,
          nullptr, &m) != GLWASM_OK)
    return nullptr;
  return m;
}

GLWASM_Runtime *runtime(Fixture *fixture, GLWASM_Module *m) {
  GLWASM_HostImport import{};
  import.module = "tang.v1";
  import.field = "op";
  import.function = initial;
  import.resume = resume;
  import.user = fixture;
  GLWASM_Runtime *r = nullptr;
  if (glwasm_runtime_create_with_imports(m, &import, 1, 0, nullptr, &r) !=
      GLWASM_OK)
    return nullptr;
  if (glwasm_runtime_register_boundary_engine(r, &fixture_engine,
          &fixture->engine) != GLWASM_OK) {
    glwasm_runtime_destroy(r);
    return nullptr;
  }
  return r;
}

struct TangDispatchFixture {
  GRCORE_Context *context = nullptr;
  GRCORE_EngineId tang_engine = 0;
  GLWASM_Runtime *wasm = nullptr;
  uintptr_t token = 0;
  unsigned compiled_dispatches = 0;
  unsigned interpreter_exits = 0;
  GLWASM_Result nested_result = GLWASM_ERR_INTERNAL;
  uint64_t nested_value = 0;
  bool callback_saw_shared_context = false;
  bool callback_saw_instance = false;
  struct HandleTable {
    std::vector<uint64_t> elements{0};
    unsigned leases=0,active_roots=0,owned_roots=0,promotions=0,relocations=0;
    uintptr_t resolved_object=0x1000;
  } table;
  GLWASM_HostTableProvider table_provider{};
};

GLWASM_Result handle_table_begin(void *opaque,GLWASM_HostTableView *view) {
  auto *table=static_cast<TangDispatchFixture::HandleTable *>(opaque);
  if(!table||!view||table->leases)return GLWASM_ERR_INTERNAL;
  ++table->leases;
  table->active_roots=0;
  for(uint64_t token:table->elements)if(token)++table->active_roots;
  /* The fixture's torture collector relocates its object on every lease while
   * preserving the opaque handle token. No raw object address escapes. */
  std::vector<uint64_t> moved=table->elements;
  table->elements.swap(moved);
  table->resolved_object+=0x100;
  ++table->relocations;
  view->elements=table->elements.data();view->length=table->elements.size();
  return GLWASM_OK;
}

void handle_table_end(void *opaque,GLWASM_HostTableView *) {
  auto *table=static_cast<TangDispatchFixture::HandleTable *>(opaque);
  if(!table)return;
  table->leases=0;table->active_roots=0;table->owned_roots=0;
  for(uint64_t token:table->elements)if(token)++table->owned_roots;
}

GLWASM_Result handle_table_grow(void *opaque,uint64_t delta,uint64_t initial,
    uint64_t *old_size) {
  auto *table=static_cast<TangDispatchFixture::HandleTable *>(opaque);
  if(!table||!old_size||table->leases||delta>SIZE_MAX-table->elements.size())
    return GLWASM_ERR_LIMIT;
  *old_size=table->elements.size();
  try { table->elements.resize(table->elements.size()+delta,initial); }
  catch (...) { return GLWASM_ERR_OOM; }
  return GLWASM_OK;
}

GLWASM_Result handle_table_validate(void *opaque,uint64_t token) {
  auto *table=static_cast<TangDispatchFixture::HandleTable *>(opaque);
  if(!table||!table->leases)return GLWASM_ERR_INTERNAL;
  if(token!=55)return GLWASM_ERR_LINK;
  ++table->promotions;
  ++table->active_roots;
  return GLWASM_OK;
}

GLWASM_Result boundary_probe(const GLWASM_HostCall *call,uint64_t *out) {
  auto *state=static_cast<TangDispatchFixture *>(call->user);
  uintptr_t active=0;
  state->callback_saw_shared_context=call->context==state->context;
  state->callback_saw_instance=call->instance_token==state->token&&
      call->engine!=state->tang_engine&&
      grcore_boundary_active_instance(grcore_context_stack(call->context),
          call->engine,&active)==GRCORE_OK&&
      active==state->token&&call->argument_count==1&&
      call->arguments[0]==55&&state->table.owned_roots==1;
  if(out)*out=call->arguments[0];
  return state->callback_saw_shared_context&&state->callback_saw_instance?
      GLWASM_OK:GLWASM_ERR_INTERNAL;
}

GRJIT_NativeResult tang_descriptor_trampoline(void *opaque,uint64_t token) {
  auto *state=static_cast<TangDispatchFixture *>(opaque);
  ++state->compiled_dispatches;
  state->token=static_cast<uintptr_t>(token);
  state->nested_result=glwasm_runtime_invoke_nested(state->wasm,
      state->context,state->tang_engine,state->token,"run",&token,1,
      &state->nested_value);
  return GRJIT_NativeResult{state->nested_value,GRJIT_NATIVE_OK,0};
}

uint64_t tang_interpreter_fallback(TangDispatchFixture *state,uint64_t token) {
  ++state->interpreter_exits;
  return tang_descriptor_trampoline(state,token).value;
}

uint32_t no_push(void *,void *,uint64_t,const uint64_t *,uint64_t,uint64_t){return 0;}
void no_pop(void *){}
uint32_t no_compile(void *,uint64_t){return 0;}
uint32_t no_deopt(void *,uint64_t){return 0;}

static const GRCORE_EngineDescriptor tang_descriptor =
    GRCORE_ENGINE_DESCRIPTOR_INIT("tang-test-descriptor",nullptr,nullptr,
        nullptr,{nullptr,nullptr,nullptr},{0,0,0},nullptr,nullptr,nullptr,nullptr);

struct NestedBoundaryDriver {
  GLWASM_Runtime *wasm=nullptr;
  GRCORE_Context *context=nullptr;
  GRCORE_EngineId caller=0;
  uintptr_t instance=0;
  bool pending=false;
  GLWASM_Result result=GLWASM_ERR_INTERNAL;
  uint64_t value=0;
};
GRCORE_Step nested_boundary_driver(GRCORE_Context *,void *opaque) {
  auto *state=static_cast<NestedBoundaryDriver *>(opaque);
  state->result=state->pending?
      glwasm_runtime_resume_nested(state->wasm,state->context,state->caller,
          state->instance,&state->value):
      glwasm_runtime_invoke_nested(state->wasm,state->context,state->caller,
          state->instance,"run",nullptr,0,&state->value);
  if(state->result==GLWASM_ERR_PAUSED){state->pending=true;return GRCORE_STEP_PAUSED;}
  state->pending=false;
  if(state->result==GLWASM_ERR_LIMIT)return GRCORE_STEP_UNWOUND;
  return state->result==GLWASM_OK?GRCORE_STEP_FINISHED:GRCORE_STEP_UNWOUND;
}
}  // namespace

TEST(WasmBoundary, RealModuleResumesOnceAndKeepsInstanceAndExceptionIdentity) {
  GLWASM_Module *m = module();
  ASSERT_NE(m, nullptr);
  Fixture a, b;
  GLWASM_Runtime *ra = runtime(&a, m);
  GLWASM_Runtime *rb = runtime(&b, m);
  ASSERT_NE(ra, nullptr);
  ASSERT_NE(rb, nullptr);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(ra, "run", nullptr, 0, &value),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(a.initial_calls, 1u);
  int paused = 0;
  EXPECT_EQ(glwasm_runtime_resume(ra, &value, &paused), GLWASM_OK);
  EXPECT_EQ(paused, 0);
  EXPECT_EQ(a.initial_calls, 1u);
  EXPECT_EQ(a.resume_calls, 1u);
  EXPECT_EQ(value, 42u);

  EXPECT_EQ(glwasm_runtime_invoke_export(rb, "run", nullptr, 0, &value),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(glwasm_runtime_resume(rb, &value, &paused), GLWASM_OK);
  EXPECT_NE(a.token, b.token);
  EXPECT_NE(a.exception_id, b.exception_id);
  glwasm_runtime_destroy(ra);
  glwasm_runtime_destroy(rb);
  glwasm_module_destroy(m);
}

TEST(WasmBoundary, CallbackUnwindPropagatesAsGuestFailure) {
  GLWASM_Module *m = module();
  ASSERT_NE(m, nullptr);
  Fixture fixture;
  fixture.fail_after_boundary = true;
  GLWASM_Runtime *r = runtime(&fixture, m);
  ASSERT_NE(r, nullptr);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(r, "run", nullptr, 0, &value),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(fixture.initial_calls, 1u);
  glwasm_runtime_destroy(r);
  glwasm_module_destroy(m);
}

TEST(WasmBoundary, NestedInvocationPauseResumesAndLimitUnwindStaysLimit) {
  static const uint8_t source[]=
      "(module (func (export \"run\") (result i32) (i32.const 42)))";
  GLWASM_Module *m=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&m),GLWASM_OK);
  GLWASM_Runtime *wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create(m,&wasm),GLWASM_OK);
  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group,nullptr,&context),GRCORE_OK);
  GRCORE_EngineId caller=0;
  ASSERT_EQ(grcore_engine_register(context,&fixture_engine,&caller),GRCORE_OK);
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(context,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke_nested(wasm,context,caller,0x1234,"run",
      nullptr,0,&value),GLWASM_ERR_PAUSED);
  EXPECT_EQ(glwasm_runtime_resume_nested(wasm,context,caller,0x9999,&value),
      GLWASM_ERR_INVALID);
  EXPECT_EQ(glwasm_runtime_resume_nested(wasm,context,caller,0x1234,&value),
      GLWASM_OK);
  EXPECT_EQ(value,42u);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);

  GRCORE_Options *options=nullptr;context=nullptr;group=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_options_create(nullptr,&options),GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group,options,&context),GRCORE_OK);
  ASSERT_EQ(grcore_engine_register(context,&fixture_engine,&caller),GRCORE_OK);
  GRCORE_EngineId nested_engine=0;
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(wasm,context,&nested_engine),GLWASM_OK);
  ASSERT_EQ(grcore_context_port(context,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_TERMINATE),GRCORE_OK);
  grcore_port_release(port);
  EXPECT_EQ(glwasm_runtime_invoke_nested(wasm,context,caller,0x1235,"run",
      nullptr,0,&value),GLWASM_ERR_LIMIT);
  EXPECT_NE(glwasm_runtime_last_trap(wasm),GLWASM_TRAP_UNREACHABLE);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  grcore_options_destroy(options);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
  glwasm_runtime_destroy(wasm);glwasm_module_destroy(m);
}

TEST(WasmBoundary, NestedPauseAndLimitPropagateThroughTheOuterCoreRun) {
  static const uint8_t source[]=
      "(module (func (export \"run\") (result i32) (i32.const 42)))";
  GLWASM_Module *m=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&m),GLWASM_OK);
  GLWASM_Runtime *wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create(m,&wasm),GLWASM_OK);

  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group,nullptr,&context),GRCORE_OK);
  NestedBoundaryDriver state{wasm,context,0,0x2345};
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,&state.caller),GRCORE_OK);
  GRCORE_EngineId wasm_engine=0;
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(wasm,context,&wasm_engine),GLWASM_OK);
  EXPECT_NE(wasm_engine,0u);
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(context,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result run_result=grcore_run(context,nested_boundary_driver,&state,&outcome);
  ASSERT_EQ(run_result,GRCORE_OK)<<"nested result="<<state.result
      <<" pending="<<state.pending;
  EXPECT_EQ(outcome,GRCORE_OUTCOME_PAUSED);EXPECT_TRUE(state.pending);
  ASSERT_EQ(grcore_resume(context,&outcome),GRCORE_OK);
  EXPECT_EQ(outcome,GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(state.result,GLWASM_OK);EXPECT_EQ(state.value,42u);
  EXPECT_FALSE(state.pending);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);

  group=nullptr;context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group,nullptr,&context),GRCORE_OK);
  state={wasm,context,0,0x2346};
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,&state.caller),GRCORE_OK);
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(wasm,context,&wasm_engine),GLWASM_OK);
  ASSERT_EQ(grcore_context_port(context,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_TERMINATE),GRCORE_OK);
  grcore_port_release(port);
  EXPECT_EQ(grcore_run(context,nested_boundary_driver,&state,&outcome),
      GRCORE_ERR_LIMIT);
  EXPECT_EQ(state.result,GLWASM_ERR_LIMIT);
  EXPECT_EQ(glwasm_runtime_last_trap(wasm),GLWASM_TRAP_NONE);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
  glwasm_runtime_destroy(wasm);glwasm_module_destroy(m);
}

TEST(WasmBoundary, CompiledTangDescriptorDispatchesIntoWasmOnSharedContext) {
  static const uint8_t source[]=
      "(module (import \"host.v1\" \"tab\" (table 1 externref)) "
      "(import \"tang.v1\" \"probe\" (func $probe (param externref) (result externref))) "
      "(func (export \"run\") (param externref) (result externref) "
      "(i32.const 0) (local.get 0) (table.set 0) "
      "(call $probe (table.get 0 (i32.const 0)))))";
  GLWASM_Module *m=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&m),GLWASM_OK);
  TangDispatchFixture state;
  state.table.elements[0]=55;
  state.table_provider={handle_table_begin,handle_table_end,handle_table_grow,
      GLWASM_HOST_EXTERNREF,handle_table_validate,&state.table};
  GLWASM_HostImport imports[2]{};
  imports[0].module="host.v1";imports[0].field="tab";
  imports[0].kind=GLWASM_HOST_IMPORT_TABLE;
  imports[0].resource.table=&state.table_provider;
  imports[1].module="tang.v1";imports[1].field="probe";
  imports[1].function=boundary_probe;imports[1].user=&state;
  ASSERT_EQ(glwasm_runtime_create_with_imports(m,imports,2,0,nullptr,
      &state.wasm),GLWASM_OK);

  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_context_create(group,nullptr,&context),GRCORE_OK);
  state.context=context;
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,
      &state.tang_engine),GRCORE_OK);

  GRJIT_NativeTable *natives=nullptr;
  ASSERT_EQ(grjit_native_table_create(nullptr,nullptr,&natives),GRJIT_OK);
  const GRJIT_Type native_params[]={GRJIT_TYPE_I64};
  GRJIT_NativeDesc native{};
  native.address=reinterpret_cast<uintptr_t>(&tang_descriptor_trampoline);
  native.params=native_params;native.param_count=1;
  native.result=GRJIT_TYPE_I64;
  native.flags=GRJIT_NATIVE_REENTERS|GRJIT_NATIVE_STATUS;
  uint32_t native_id=0;
  ASSERT_EQ(grjit_native_table_add(natives,&native,&native_id),GRJIT_OK);
  GRJIT_Builder *builder=nullptr;
  ASSERT_EQ(grjit_builder_create("tang.compiled.boundary.dispatch",2,nullptr,
      nullptr,&builder),GRJIT_OK);
  GRJIT_CallHooks hooks{};
  hooks.push=no_push;hooks.pop=no_pop;hooks.compile=no_compile;hooks.deopt=no_deopt;
  ASSERT_EQ(grjit_builder_set_callable(builder,&hooks,1),GRJIT_OK);
  ASSERT_EQ(grjit_builder_set_natives(builder,natives),GRJIT_OK);
  GRJIT_VReg token=GRJIT_NO_VREG,value=GRJIT_NO_VREG;
  ASSERT_EQ(grjit_builder_param(builder,GRJIT_TYPE_I64,&token),GRJIT_OK);
  ASSERT_EQ(grjit_builder_vreg(builder,GRJIT_TYPE_I64,&value),GRJIT_OK);
  GRJIT_BlockId block=0;
  ASSERT_EQ(grjit_builder_block(builder,&block),GRJIT_OK);
  ASSERT_EQ(grjit_builder_set_block(builder,block),GRJIT_OK);
  const GRJIT_Operand args[]={grjit_operand_vreg(token)};
  const GRJIT_FrameSlot state_slots[]={grjit_frame_slot_vreg(token),
      grjit_frame_slot_dead()};
  const GRJIT_FrameSlot after_slots[]={grjit_frame_slot_vreg(token),
      grjit_frame_slot_vreg(value)};
  const GRCORE_PollIdentity site{91,4};
  const GRJIT_FrameState frame_state{site,2,state_slots};
  const GRCORE_PollIdentity after_site{91,5};
  const GRJIT_FrameState after_state{after_site,2,after_slots};
  ASSERT_EQ(grjit_builder_call_native(builder,value,native_id,args,1,
      &frame_state,&after_state),GRJIT_OK);
  ASSERT_EQ(grjit_builder_ret(builder,grjit_operand_vreg(value)),GRJIT_OK);
  GRJIT_Function *function=nullptr;
  ASSERT_EQ(grjit_builder_finish(builder,&function),GRJIT_OK);
  char verify_reason[256]={0};
  ASSERT_EQ(grjit_function_verify(function,nullptr,verify_reason,
      sizeof(verify_reason)),GRJIT_OK)<<verify_reason;
  GRJIT_CompileOptions options{};
  options.pages=grcore_context_page_provider(context);
  GRJIT_Code *code=nullptr;
  ASSERT_EQ(grjit_compile(&options,function,&code),GRJIT_OK);
  grjit_function_destroy(function);

  GRCORE_Stack *stack=grcore_context_stack(context);
  GRCORE_ActivationRef tang_activation{};
  ASSERT_EQ(grcore_activation_enter(stack,GRCORE_ACTIVATION_NATIVE,
      state.tang_engine,false,nullptr,&tang_activation),GRCORE_OK);
  const uint64_t arguments[]={55};
  uint64_t output[1]={0};
  const uint32_t exit=grjit_code_entry(code)(&state,nullptr,arguments,output);
  if(exit!=GRJIT_EXIT_RETURNED)
    output[0]=tang_interpreter_fallback(&state,arguments[0]);
  EXPECT_EQ(exit,GRJIT_EXIT_RETURNED);
  EXPECT_EQ(state.interpreter_exits,0u);
  EXPECT_EQ(state.compiled_dispatches,1u);
  EXPECT_EQ(state.nested_result,GLWASM_OK);
  EXPECT_EQ(state.nested_value,55u);
  EXPECT_EQ(output[0],55u);
  EXPECT_TRUE(state.callback_saw_shared_context);
  EXPECT_TRUE(state.callback_saw_instance);
  EXPECT_EQ(state.table.leases,0u);
  EXPECT_EQ(state.table.active_roots,0u);
  EXPECT_EQ(state.table.owned_roots,1u);
  EXPECT_GT(state.table.promotions,0u);
  EXPECT_GT(state.table.relocations,0u);
  const uint64_t stale=99;
  uint64_t stale_result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(state.wasm,"run",&stale,1,
      &stale_result),GLWASM_ERR_GUEST);
  EXPECT_EQ(state.table.leases,0u);
  EXPECT_EQ(state.table.active_roots,0u);
  EXPECT_EQ(state.table.owned_roots,1u);
  EXPECT_EQ(state.table.elements[0],55u);
  ASSERT_EQ(grcore_activation_leave(stack,tang_activation),GRCORE_OK);
  grjit_code_destroy(code);
  grjit_native_table_free(natives);
  glwasm_runtime_destroy(state.wasm);
  glwasm_module_destroy(m);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
}
