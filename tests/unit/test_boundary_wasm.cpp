/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/host.h>
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/runtime-core/a/boundary.h>
#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <ghoti.io/runtime-core/a/registry.h>
#include <ghoti.io/runtime-core/a/stack.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/b/key.h>
#include <ghoti.io/runtime-core/b/request.h>
#include <ghoti.io/runtime-jit/backend.h>
#include <ghoti.io/runtime-jit/builder.h>
#include <ghoti.io/runtime-jit/code.h>
#include <ghoti.io/runtime-jit/natives.h>
#include <ghoti.io/runtime-jit/verify.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>
#include <ghoti.io/runtime-heap/id.h>
#ifdef GLWASM_BOUNDARY_RELOCATE
#include <ghoti.io/runtime-heap/relocate.h>
#endif
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
  GRCORE_BoundaryRef boundary{};
  GRCORE_ActivationRef callback_activation{};
  GRCORE_Result unwind_result=GRCORE_ERR_INTERNAL;
  GRCORE_EngineId unwind_origin=0;
  uint64_t unwind_identity=0;
  size_t unwind_popped=0;
  GRCORE_Result activation_after_unwind=GRCORE_ERR_INTERNAL;
  GRCORE_Result activation_top_after_unwind=GRCORE_ERR_INTERNAL;
  GRCORE_ActivationRef top_after_unwind{};
  GRCORE_ActivationInfo activation_info_after_unwind{};
  size_t activation_count_after_unwind=0;
  size_t frame_count_after_unwind=0;
  size_t budget_scope_count_after_unwind=0;
  bool boundary_live = false;
  bool fail_after_boundary = false;
};

void observe_wasm_boundary_exception(GRCORE_Context *context,
    const GRCORE_AbstractFrame *frame) {
  if(!frame||frame->slot_count<2)return;
  GRCORE_Stack *stack=grcore_context_stack(context);
  uint64_t boundary_id=0,fixture_pointer=0;
  if(grcore_stack_slot_get(stack,frame->frame,0,&boundary_id)!=GRCORE_OK||
      grcore_stack_slot_get(stack,frame->frame,1,&fixture_pointer)!=GRCORE_OK)
    return;
  auto *fixture=reinterpret_cast<Fixture *>(static_cast<uintptr_t>(fixture_pointer));
  if(!fixture)return;
  GRCORE_BoundaryRef boundary{boundary_id};
  fixture->unwind_result=grcore_boundary_get_foreign_exception(stack,boundary,
      &fixture->unwind_origin,&fixture->unwind_identity);
}

GLWASM_Result cross_boundary(const GLWASM_HostCall *call, Fixture *f) {
  auto *stack = grcore_context_stack(call->context);
  if (!f->boundary_live) {
    if(grcore_activation_top(stack,&f->callback_activation)!=GRCORE_OK)
      return GLWASM_ERR_INTERNAL;
    if (grcore_boundary_enter(stack, call->engine, f->engine,
            call->instance_token, true, &f->boundary) != GRCORE_OK)
      return GLWASM_ERR_INTERNAL;
    f->boundary_live = true;
  }
  uintptr_t active = 0;
  GRCORE_EngineId origin = 0;
  uint64_t identity = 0;
  GRCORE_Result result = grcore_boundary_active_instance(stack, f->engine,
      &active);
  if (result == GRCORE_OK && active != call->instance_token)
    result = GRCORE_ERR_INVALID;
  const uint64_t expected = static_cast<uint64_t>(call->instance_token) ^
      UINT64_C(0x5a5a1234);
  const uint64_t exception_identity=f->exception_id
      ?f->exception_id:expected;
  if (result == GRCORE_OK)
    result = grcore_boundary_set_foreign_exception(stack, f->boundary,
        f->engine, exception_identity);
  if (result == GRCORE_OK)
    result = grcore_boundary_get_foreign_exception(stack, f->boundary, &origin,
        &identity);
  if (result == GRCORE_OK && (origin != f->engine ||
      identity != (f->exception_id ? f->exception_id : expected)))
    result = GRCORE_ERR_INVALID;
  if (f->token && f->token != active) return GLWASM_ERR_INTERNAL;
  f->token = active;
  f->exception_id = identity;
  if (result != GRCORE_OK) return GLWASM_ERR_INTERNAL;
  GRCORE_Result left=grcore_boundary_leave(stack,f->boundary);
  f->boundary_live=false;
  if(left!=GRCORE_OK)return GLWASM_ERR_INTERNAL;
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
        nullptr, {nullptr, nullptr, nullptr}, {0, 0, 0}, nullptr,
        observe_wasm_boundary_exception,
        nullptr, nullptr);

GLWASM_Module *module() {
  static const uint8_t source[] =
      "(module (import \"tang.v1\" \"op\" (func $op (result i32))) "
      "(global $after (mut i32) (i32.const 0)) "
      "(func (export \"run\") (result i32) (call $op) "
      "(global.set $after) (global.get $after)) "
      "(func (export \"after\") (result i32) (global.get $after)))";
  GLWASM_Policy policy{};
  if (glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED,&policy)!=GLWASM_OK)
    return nullptr;
  policy.allow_tang_v1_imports=1;
  GLWASM_Module *m = nullptr;
  if (glwasm_wat_module_parse_with_policy(source, sizeof(source) - 1,
          &policy, nullptr, nullptr, &m) != GLWASM_OK)
    return nullptr;
  return m;
}

GLWASM_Runtime *runtime(Fixture *fixture, GLWASM_Module *m) {
  GLWASM_HostImport import{};
  static const GLWASM_HostValueType result_i32[] = {GLWASM_HOST_I32};
  import.module = "tang.v1";
  import.field = "op";
  import.function = initial;
  import.resume = resume;
  import.user = fixture;
  import.results = result_i32;
  import.result_count = 1;
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
  unsigned operation_count = 0;
  bool callback_saw_shared_context = false;
  bool callback_saw_instance = false;
  bool callback_roots_match = false;
  GRCORE_Result callback_active_result = GRCORE_ERR_INVALID;
  GRCORE_EngineId callback_engine = 0;
  uintptr_t callback_instance_token = 0;
  uintptr_t callback_active_token = 0;
  struct HandleTable {
    std::vector<uint64_t> elements{0};
    unsigned leases=0,active_roots=0,owned_roots=0,promotions=0,relocations=0;
    GRHEAP_Heap *heap=nullptr;
    void *resolved_object=nullptr;
    uint64_t object_id=0;
    uintptr_t encoded_object_address=0;
  } table;
  GLWASM_HostTableProvider table_provider{};
};

static const GRCORE_Key tang_fixture_key=GRCORE_KEY_INIT(
    "lang-wasm-story12-test-instance",GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_NONE,nullptr,nullptr,nullptr,nullptr,nullptr);

GLWASM_Result handle_table_begin(void *opaque,GLWASM_HostTableView *view) {
  auto *table=static_cast<TangDispatchFixture::HandleTable *>(opaque);
  if(!table||!view||table->leases)
    return GLWASM_ERR_INTERNAL;
  ++table->leases;
  table->active_roots=0;
  for(uint64_t token:table->elements)if(token)++table->active_roots;
  /* This is a real runtime-heap collection. The provider roots the object in
   * its owner slot; the opaque table token remains unchanged when relocation
   * updates that slot. */
  if(table->heap&&table->resolved_object){
#ifdef GLWASM_BOUNDARY_RELOCATE
    constexpr uintptr_t address_mask=static_cast<uintptr_t>(
        UINT64_C(0x9e3779b97f4a7c15));
    const uintptr_t address_before=table->encoded_object_address;
    GRHEAP_RelocationStats before{};
    if(grheap_relocation_stats(table->heap,&before)!=GRHEAP_OK){
      table->leases=0;
      table->active_roots=0;
      return GLWASM_ERR_INTERNAL;
    }
#endif
    if(grheap_collect(table->heap)!=GRHEAP_OK){
      table->leases=0;
      table->active_roots=0;
      return GLWASM_ERR_INTERNAL;
    }
#ifdef GLWASM_BOUNDARY_RELOCATE
    GRHEAP_RelocationStats after{};
    uint64_t resolved_id=0;
    if(grheap_relocation_stats(table->heap,&after)!=GRHEAP_OK||
        grheap_id_of(table->heap,table->resolved_object,&resolved_id)!=GRHEAP_OK||
        resolved_id!=table->object_id){
      table->leases=0;
      table->active_roots=0;
      return GLWASM_ERR_INTERNAL;
    }
    const uintptr_t address_after=
        reinterpret_cast<uintptr_t>(table->resolved_object)^address_mask;
    if(after.moved>before.moved&&address_after!=address_before)
      ++table->relocations;
    table->encoded_object_address=address_after;
#endif
  }
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
  if(!token)return GLWASM_OK;
  if(token!=55||!table->resolved_object||
      *static_cast<uint64_t *>(table->resolved_object)!=UINT64_C(0x74616e672d726f6f))
    return GLWASM_ERR_LINK;
  ++table->promotions;
  ++table->active_roots;
  return GLWASM_OK;
}

GLWASM_Result boundary_probe(const GLWASM_HostCall *call,uint64_t *out) {
  auto *state=static_cast<TangDispatchFixture *>(call->user);
  uintptr_t active=0;
  state->callback_saw_shared_context=call->context==state->context;
  state->callback_engine=call->engine;
  state->callback_instance_token=call->instance_token;
  state->callback_active_result=grcore_boundary_active_instance(
      grcore_context_stack(call->context),call->engine,&active);
  state->callback_active_token=active;
  state->callback_roots_match=state->table.owned_roots==1;
  state->callback_saw_instance=call->instance_token==state->token&&
      call->engine!=state->tang_engine&&
      state->callback_active_result==GRCORE_OK&&active==state->token&&
      call->argument_count==1&&call->arguments[0]==55&&
      state->callback_roots_match;
  if(out)*out=call->arguments[0];
  return state->callback_saw_shared_context&&state->callback_saw_instance?
      GLWASM_OK:GLWASM_ERR_INTERNAL;
}

GRJIT_NativeResult tang_descriptor_trampoline(void *opaque_context,
    uint64_t operation,uint64_t token) {
  auto *context=static_cast<GRCORE_Context *>(opaque_context);
  auto *state=static_cast<TangDispatchFixture *>(context
      ?grcore_context_slot(context,&tang_fixture_key):nullptr);
  if(!state)return GRJIT_NativeResult{0,GRJIT_NATIVE_UNWIND,0};
  ++state->compiled_dispatches;
  if(operation>3)return GRJIT_NativeResult{0,GRJIT_NATIVE_UNWIND,0};
  const char *export_name=operation==0?"run":operation==1?"increment":
      operation==2?"read":"compiled_probe";
  state->token=static_cast<uintptr_t>(token);
  state->nested_result=glwasm_runtime_invoke_nested(state->wasm,context,
      state->tang_engine,state->token,export_name,operation==0?&token:nullptr,
      operation==0?1u:0u,&state->nested_value);
  if(operation==1&&state->nested_result==GLWASM_OK)
    ++state->operation_count;
  const uint32_t status=state->nested_result==GLWASM_OK
      ?GRJIT_NATIVE_OK:state->nested_result==GLWASM_ERR_LIMIT
      ?GRJIT_NATIVE_UNWIND:GRJIT_NATIVE_DEOPT;
  return GRJIT_NativeResult{state->nested_value,status,0};
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
  GRCORE_RequestKind control=GRCORE_REQUEST_INTERRUPT;
  GRCORE_Result control_result=GRCORE_ERR_INVALID;
  bool control_entered=false;
  unsigned control_calls=0;
};
GLWASM_Result nested_boundary_control(const GLWASM_HostCall *call,uint64_t *) {
  auto *state=static_cast<NestedBoundaryDriver *>(call->user);
  if(!state||!call->context)return GLWASM_ERR_INTERNAL;
  state->control_entered=true;
  GRCORE_Port *port=nullptr;
  GRCORE_Result result=grcore_context_port(call->context,&port);
  state->control_result=result;
  if(result!=GRCORE_OK)return GLWASM_ERR_INTERNAL;
  result=grcore_port_post(port,state->control);
  state->control_result=result;
  grcore_port_release(port);
  if(result==GRCORE_OK)++state->control_calls;
  return result==GRCORE_OK?GLWASM_OK:GLWASM_ERR_INTERNAL;
}
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


/* An adopted context as an embedder makes one: with a native stack budget,
 * which compiled Wasm needs before it runs at all. */
static GRCORE_Result budgeted_context(GRCORE_Group *group,
    GRCORE_Context **out) {
  GRCORE_Options *options = nullptr;
  GRCORE_Result result = grcore_options_create(nullptr, &options);
  if (result == GRCORE_OK)
    result = grcore_options_set_native_stack_bytes(options, UINT64_C(1) << 20);
  if (result == GRCORE_OK) result = grcore_context_create(group, options, out);
  if (options) grcore_options_destroy(options);
  return result;
}

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

TEST(WasmBoundary, CallbackFailurePropagatesAsGuestFailure) {
  GLWASM_Module *m = module();
  ASSERT_NE(m, nullptr);
  Fixture fixture;
  fixture.fail_after_boundary = true;
  GLWASM_Runtime *r = runtime(&fixture, m);
  ASSERT_NE(r, nullptr);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(r,GLWASM_TEST_JIT_REQUIRE),
      GLWASM_OK);
  uint64_t value = 0;
  EXPECT_EQ(glwasm_runtime_invoke_export(r, "run", nullptr, 0, &value),
      GLWASM_ERR_GUEST);
  EXPECT_EQ(fixture.initial_calls, 1u);
  EXPECT_FALSE(fixture.boundary_live);
  GLWASM_JitCacheStats stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(r, 1, &stats), GLWASM_OK);
  EXPECT_EQ(stats.compile_successes, 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_HOST_CALL], 1u);
  EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT], 0u);
  EXPECT_EQ(stats.guest_visible_exits, 0u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(r,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(r,"after",nullptr,0,&value),
      GLWASM_OK);
  EXPECT_EQ(value,0u);
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
  ASSERT_EQ(budgeted_context(group,&context),GRCORE_OK);
  GRCORE_EngineId caller=0;
  ASSERT_EQ(grcore_engine_register(context,&fixture_engine,&caller),GRCORE_OK);
  GRCORE_Port *port=nullptr;
  ASSERT_EQ(grcore_context_port(context,&port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(port);
  uint64_t value=0;
  EXPECT_EQ(glwasm_runtime_invoke_nested(wasm,context,caller,0x1234,"run",
      nullptr,0,&value),GLWASM_ERR_PAUSED);
  ASSERT_EQ(grcore_context_state(context),GRCORE_CONTEXT_PAUSED);
  const size_t paused_frames=grcore_stack_frame_count(
      grcore_context_stack(context));
  GLWASM_Runtime *other_wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create(m,&other_wasm),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_nested(other_wasm,context,caller,0x1236,
      "run",nullptr,0,&value),GLWASM_ERR_INVALID);
  EXPECT_EQ(grcore_context_state(context),GRCORE_CONTEXT_PAUSED);
  EXPECT_EQ(grcore_stack_frame_count(grcore_context_stack(context)),
      paused_frames);
  glwasm_runtime_destroy(other_wasm);
  EXPECT_EQ(glwasm_runtime_resume_nested(wasm,context,caller,0x9999,&value),
      GLWASM_ERR_INVALID);
  EXPECT_EQ(glwasm_runtime_resume_nested(wasm,context,caller,0x1234,&value),
      GLWASM_OK);
  EXPECT_EQ(value,42u);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(wasm),GLWASM_OK);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);

  GRCORE_Options *options=nullptr;context=nullptr;group=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(grcore_options_create(nullptr,&options),GRCORE_OK);
  ASSERT_EQ(grcore_options_set_native_stack_bytes(options,UINT64_C(1)<<20),GRCORE_OK);
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
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(wasm),GLWASM_OK);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  grcore_options_destroy(options);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
  glwasm_runtime_destroy(wasm);glwasm_module_destroy(m);
}

TEST(WasmBoundary, NestedPauseAndLimitPropagateThroughTheOuterCoreRun) {
  static const uint8_t source[]=
      "(module (import \"host.v1\" \"control\" (func $control)) "
      "(func $inner (result i32) (call $control) (i32.const 42)) "
      "(func $outer (result i32) (call $inner)) "
      "(func (export \"run\") (result i32) (call $outer)))";
  GLWASM_Module *m=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&m),GLWASM_OK);
  NestedBoundaryDriver state;
  state.control=GRCORE_REQUEST_INTERRUPT;
  GLWASM_HostImport control_import{};
  control_import.module="host.v1";control_import.field="control";
  control_import.function=nested_boundary_control;control_import.user=&state;
  GLWASM_Runtime *wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(m,&control_import,1,0,nullptr,
      &wasm),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(wasm,
      GLWASM_TEST_JIT_REQUIRE),GLWASM_OK);
  state.wasm=wasm;

  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(budgeted_context(group,&context),GRCORE_OK);
  state.context=context;state.instance=0x2345;
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,&state.caller),GRCORE_OK);
  GRCORE_EngineId wasm_engine=0;
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(wasm,context,&wasm_engine),GLWASM_OK);
  EXPECT_NE(wasm_engine,0u);
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result run_result=grcore_run(context,nested_boundary_driver,&state,&outcome);
  ASSERT_EQ(run_result,GRCORE_OK)<<"nested result="<<state.result
      <<" pending="<<state.pending<<" control_entered="
      <<state.control_entered<<" control_result="<<state.control_result;
  EXPECT_EQ(outcome,GRCORE_OUTCOME_PAUSED);EXPECT_TRUE(state.pending);
  const uint32_t active_functions[]={1,2,3};
  for(uint32_t function:active_functions){
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(wasm,function,&stats),GLWASM_OK);
    EXPECT_EQ(stats.compiled_entries,1u);
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_PAUSE],1u);
  }
  ASSERT_EQ(grcore_resume(context,&outcome),GRCORE_OK);
  EXPECT_EQ(outcome,GRCORE_OUTCOME_FINISHED);
  EXPECT_EQ(state.result,GLWASM_OK);EXPECT_EQ(state.value,42u);
  EXPECT_FALSE(state.pending);
  EXPECT_EQ(state.control_calls,1u);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(wasm),GLWASM_OK);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);

  group=nullptr;context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(budgeted_context(group,&context),GRCORE_OK);
  state.context=context;state.caller=0;state.instance=0x2346;
  state.pending=false;state.value=0;state.result=GLWASM_ERR_INTERNAL;
  state.control=GRCORE_REQUEST_TERMINATE;state.control_calls=0;
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,&state.caller),GRCORE_OK);
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(wasm,context,&wasm_engine),GLWASM_OK);
  EXPECT_EQ(grcore_run(context,nested_boundary_driver,&state,&outcome),
      GRCORE_ERR_LIMIT);
  EXPECT_EQ(state.result,GLWASM_ERR_LIMIT);
  EXPECT_EQ(glwasm_runtime_last_trap(wasm),GLWASM_TRAP_NONE);
  EXPECT_EQ(state.control_calls,1u);
  for(uint32_t function:active_functions){
    GLWASM_JitCacheStats stats{};
    ASSERT_EQ(glwasm_runtime_test_jit_stats(wasm,function,&stats),GLWASM_OK);
    EXPECT_EQ(stats.exit_counts[GLWASM_JIT_EXIT_LIMIT_UNWIND],1u);
  }
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(wasm),GLWASM_OK);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
  glwasm_runtime_destroy(wasm);glwasm_module_destroy(m);
}

TEST(WasmBoundary, CompiledTangDescriptorDispatchesIntoWasmOnSharedContext) {
  static const uint8_t source[]=
      "(module (import \"host.v1\" \"tab\" (table 1 externref)) "
      "(import \"tang.v1\" \"probe\" (func $probe (param externref) (result externref))) "
      "(global $count (mut i32) (i32.const 0)) "
      "(func (export \"read\") (result i32) (global.get $count)) "
      "(func (export \"increment\") (result i32) "
      "(global.get $count) (i32.const 1) (i32.add) (global.set $count) "
      "(global.get $count)) "
      "(func (export \"reset\") (i32.const 0) (global.set $count)) "
      "(func (export \"run\") (param externref) (result externref) "
      "(i32.const 0) (local.get 0) (table.set 0) "
      "(call $probe (table.get 0 (i32.const 0)))) "
      "(func (export \"compiled_probe\") (result i32) (i32.const 55)))";
  GLWASM_Policy policy{};
  ASSERT_EQ(glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED,&policy),GLWASM_OK);
  policy.allow_tang_v1_imports=1;
  GLWASM_Module *m=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse_with_policy(source,sizeof(source)-1,
      &policy,nullptr,nullptr,&m),GLWASM_OK);
  TangDispatchFixture state;
  state.table_provider={handle_table_begin,handle_table_end,handle_table_grow,
      GLWASM_HOST_EXTERNREF,handle_table_validate,&state.table,0,0,0};
  GLWASM_HostImport imports[2]{};
  imports[0].module="host.v1";imports[0].field="tab";
  imports[0].kind=GLWASM_HOST_IMPORT_TABLE;
  imports[0].resource.table=&state.table_provider;
  imports[1].module="tang.v1";imports[1].field="probe";
  imports[1].function=boundary_probe;imports[1].user=&state;
  static const GLWASM_HostValueType externref[]={GLWASM_HOST_EXTERNREF};
  imports[1].parameters=externref;imports[1].parameter_count=1;
  imports[1].results=externref;imports[1].result_count=1;
  ASSERT_EQ(glwasm_runtime_create_with_imports(m,imports,2,0,nullptr,
      &state.wasm),GLWASM_OK);

  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(budgeted_context(group,&context),GRCORE_OK);
  state.context=context;
  GRHEAP_Options *heap_options=nullptr;
  ASSERT_EQ(grheap_options_create(nullptr,&heap_options),GRHEAP_OK);
  ASSERT_EQ(grheap_options_set_torture(heap_options,true),GRHEAP_OK);
#ifdef GLWASM_BOUNDARY_RELOCATE
  ASSERT_TRUE(grheap_relocation_available());
  ASSERT_EQ(grheap_options_set_relocate(heap_options,true),GRHEAP_OK);
#endif
  ASSERT_EQ(grheap_heap_create(context,heap_options,&state.table.heap),GRHEAP_OK);
  grheap_options_destroy(heap_options);
  static const GRHEAP_Type probe_type=GRHEAP_TYPE_INIT(
      "tang-boundary-reference-probe",sizeof(uint64_t),nullptr,nullptr,
      nullptr,nullptr,nullptr);
  ASSERT_EQ(grheap_alloc(state.table.heap,&probe_type,
      &state.table.resolved_object),GRHEAP_OK);
  *static_cast<uint64_t *>(state.table.resolved_object)=
      UINT64_C(0x74616e672d726f6f);
  ASSERT_EQ(grheap_id_of(state.table.heap,state.table.resolved_object,
      &state.table.object_id),GRHEAP_OK);
#ifdef GLWASM_BOUNDARY_RELOCATE
  state.table.encoded_object_address=
      reinterpret_cast<uintptr_t>(state.table.resolved_object)^
      static_cast<uintptr_t>(UINT64_C(0x9e3779b97f4a7c15));
#endif
  ASSERT_EQ(grheap_root_add(state.table.heap,&state.table.resolved_object),
      GRHEAP_OK);
  state.table.elements[0]=55;
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,
      &state.tang_engine),GRCORE_OK);
  GRCORE_EngineId wasm_engine=0;
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(state.wasm,context,
      &wasm_engine),GLWASM_OK);
  ASSERT_EQ(grcore_context_register(context,&tang_fixture_key,&state),
      GRCORE_OK);

  GRJIT_NativeTable *natives=nullptr;
  ASSERT_EQ(grjit_native_table_create(nullptr,nullptr,&natives),GRJIT_OK);
  const GRJIT_Type native_params[]={GRJIT_TYPE_I64,GRJIT_TYPE_I64};
  GRJIT_NativeDesc native{};
  native.address=reinterpret_cast<uintptr_t>(&tang_descriptor_trampoline);
  native.params=native_params;native.param_count=2;
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
  ASSERT_EQ(grjit_builder_set_guest_function(builder,91),GRJIT_OK);
  ASSERT_EQ(grjit_builder_set_natives(builder,natives),GRJIT_OK);
  GRJIT_VReg token=GRJIT_NO_VREG,value=GRJIT_NO_VREG;
  ASSERT_EQ(grjit_builder_param(builder,GRJIT_TYPE_I64,&token),GRJIT_OK);
  GRJIT_VReg operation=GRJIT_NO_VREG;
  ASSERT_EQ(grjit_builder_param(builder,GRJIT_TYPE_I64,&operation),GRJIT_OK);
  ASSERT_EQ(grjit_builder_vreg(builder,GRJIT_TYPE_I64,&value),GRJIT_OK);
  GRJIT_BlockId block=0;
  ASSERT_EQ(grjit_builder_block(builder,&block),GRJIT_OK);
  ASSERT_EQ(grjit_builder_set_block(builder,block),GRJIT_OK);
  const GRJIT_Operand args[]={grjit_operand_vreg(operation),
      grjit_operand_vreg(token)};
  const GRJIT_FrameSlot state_slots[]={grjit_frame_slot_vreg(token),
      grjit_frame_slot_vreg(operation)};
  const GRJIT_FrameSlot after_slots[]={grjit_frame_slot_vreg(token),
      grjit_frame_slot_vreg(value)};
  const GRCORE_PollIdentity site{91,4,0};
  const GRJIT_FrameState frame_state{site,2,state_slots};
  const GRCORE_PollIdentity after_site{91,5,0};
  const GRJIT_FrameState after_state{after_site,2,after_slots};
  ASSERT_EQ(grjit_builder_call_native(builder,value,native_id,args,2,
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
  GRCORE_Code *code_handle=nullptr;
  ASSERT_EQ(grcore_code_create(nullptr,code,[](void *payload){
      grjit_code_destroy(static_cast<GRJIT_Code *>(payload));
    },&code_handle),GRCORE_OK);
  const uintptr_t code_address=(uintptr_t)grjit_code_address(code);
  ASSERT_EQ(grcore_code_register(context,state.tang_engine,code_handle,
      code_address,grjit_code_size(code),grjit_code_meta(code)),GRCORE_OK);

  GRCORE_Stack *stack=grcore_context_stack(context);
  GRCORE_FrameRef tang_frame{};
  ASSERT_EQ(grcore_stack_push(stack,state.tang_engine,2,&tang_frame),
      GRCORE_OK);
  const GRCORE_PollIdentity tang_identity{91,4,state.tang_engine};
  ASSERT_EQ(grcore_stack_set_identity(stack,tang_frame,tang_identity),
      GRCORE_OK);
  ASSERT_EQ(grcore_stack_set_guest_function(stack,tang_frame,91),GRCORE_OK);
  uint64_t output[1]={0};
  // Raw code-entry calls return to this test between guest invocations. Give
  // each call its own activation so the collector never walks a frame that
  // returned from an earlier call but still occupies the same native stack.
  auto invoke_tang=[&](GRCORE_Stack *call_stack,GRCORE_EngineId engine,
      GRCORE_Context *call_context,const uint64_t *args) {
    GRCORE_ActivationRef activation{};
    const GRCORE_Result entered=grcore_activation_enter(call_stack,
        GRCORE_ACTIVATION_JIT,engine,false,nullptr,&activation);
    EXPECT_EQ(entered,GRCORE_OK);
    if(entered!=GRCORE_OK)return UINT32_MAX;
    const uint32_t result=grjit_code_entry(code)(call_context,nullptr,args,
        output);
    EXPECT_EQ(grcore_activation_leave(call_stack,activation),GRCORE_OK);
    return result;
  };
  const uint64_t arguments[]={55,0};
  const uint32_t exit=invoke_tang(stack,state.tang_engine,context,arguments);
  EXPECT_EQ(exit,GRJIT_EXIT_RETURNED);
  EXPECT_EQ(state.interpreter_exits,0u);
  EXPECT_EQ(state.compiled_dispatches,1u);
  EXPECT_EQ(state.nested_result,GLWASM_OK)<<"shared_context="
      <<state.callback_saw_shared_context<<" callback_engine="
      <<state.callback_engine<<" tang_engine="<<state.tang_engine
      <<" instance="<<state.callback_instance_token<<" expected="
      <<state.token<<" active_result="<<state.callback_active_result
      <<" active="<<state.callback_active_token<<" roots="
      <<state.callback_roots_match;
  EXPECT_EQ(state.nested_value,55u);
  EXPECT_EQ(output[0],55u);
  EXPECT_TRUE(state.callback_saw_shared_context);
  EXPECT_TRUE(state.callback_saw_instance);
  const uint64_t increment[]={55,1};
  EXPECT_EQ(invoke_tang(stack,state.tang_engine,context,increment),
      GRJIT_EXIT_RETURNED);
  EXPECT_EQ(output[0],1u);
  EXPECT_EQ(invoke_tang(stack,state.tang_engine,context,increment),
      GRJIT_EXIT_RETURNED);
  EXPECT_EQ(output[0],2u);
  EXPECT_EQ(state.operation_count,2u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(state.wasm,
      GLWASM_TEST_JIT_REQUIRE),GLWASM_OK);
  const uint64_t compiled_dispatch[]={55,3};
  const uint32_t compiled_probe_exit_one =
      invoke_tang(stack,state.tang_engine,context,compiled_dispatch);
  EXPECT_EQ(compiled_probe_exit_one,GRJIT_EXIT_RETURNED)
      << "nested_result=" << state.nested_result
      << " nested_value=" << state.nested_value
      << " dispatches=" << state.compiled_dispatches
      << " operation_count=" << state.operation_count;
  EXPECT_EQ(state.nested_result,GLWASM_OK)
      << "first compiled-probe dispatch failed";
  EXPECT_EQ(output[0],55u);
  const uint32_t compiled_probe_exit_two =
      invoke_tang(stack,state.tang_engine,context,compiled_dispatch);
  EXPECT_EQ(compiled_probe_exit_two,GRJIT_EXIT_RETURNED)
      << "nested_result=" << state.nested_result
      << " nested_value=" << state.nested_value
      << " dispatches=" << state.compiled_dispatches
      << " operation_count=" << state.operation_count;
  EXPECT_EQ(state.nested_result,GLWASM_OK)
      << "second compiled-probe dispatch failed";
  EXPECT_EQ(output[0],55u);
  EXPECT_TRUE(state.callback_saw_shared_context);
  EXPECT_TRUE(state.callback_saw_instance);
  EXPECT_GT(state.table.promotions,0u);
  EXPECT_EQ(state.operation_count,2u);
  GLWASM_JitCacheStats compiled_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(state.wasm,5,&compiled_stats),
      GLWASM_OK);
  EXPECT_EQ(compiled_stats.compile_attempts,1u);
  EXPECT_EQ(compiled_stats.compile_successes,1u);
  EXPECT_EQ(compiled_stats.exit_counts[GLWASM_JIT_EXIT_RETURN],2u);
  EXPECT_EQ(compiled_stats.guest_visible_exits,0u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(state.wasm,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  const uint64_t read[]={55,2};
  EXPECT_EQ(invoke_tang(stack,state.tang_engine,context,read),
      GRJIT_EXIT_RETURNED);
  EXPECT_EQ(output[0],2u);
  EXPECT_EQ(state.table.leases,0u);
  EXPECT_EQ(state.table.active_roots,0u);
  EXPECT_EQ(state.table.owned_roots,1u);
  EXPECT_GT(state.table.promotions,0u);
#ifdef GLWASM_BOUNDARY_RELOCATE
  EXPECT_GT(state.table.relocations,0u);
  void *object_by_id=nullptr;
  ASSERT_EQ(grheap_object_of_id(state.table.heap,state.table.object_id,
      &object_by_id),GRHEAP_OK);
  EXPECT_EQ(object_by_id,state.table.resolved_object);
  GRHEAP_RelocationStats relocation{};
  ASSERT_EQ(grheap_relocation_stats(state.table.heap,&relocation),GRHEAP_OK);
  EXPECT_GT(relocation.collections,0u);
  EXPECT_GT(relocation.moved,0u);
#else
  GRHEAP_Stats heap_stats{};
  ASSERT_EQ(grheap_stats(state.table.heap,&heap_stats),GRHEAP_OK);
  EXPECT_GT(heap_stats.collections,0u);
#endif
  const uint64_t stale=99;
  uint64_t stale_result=0;
  // This path deliberately uses externref table operations, which remain
  // interpreter-only; keep the semantic stale-handle assertion independent
  // of the forced-JIT checks above.
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(state.wasm,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(state.wasm,"run",&stale,1,
      &stale_result),GLWASM_ERR_GUEST);
  EXPECT_EQ(state.table.leases,0u);
  EXPECT_EQ(state.table.active_roots,0u);
  EXPECT_EQ(state.table.owned_roots,1u);
  EXPECT_EQ(state.table.elements[0],55u);
  EXPECT_EQ(grheap_root_remove(state.table.heap,&state.table.resolved_object),
      GRHEAP_OK);
  TangDispatchFixture second;
  second.token=56;
  second.table.elements[0]=55;
  second.table_provider={handle_table_begin,handle_table_end,
      handle_table_grow,GLWASM_HOST_EXTERNREF,handle_table_validate,
      &second.table,0,0,0};
  GLWASM_HostImport second_imports[2]{};
  second_imports[0].module="host.v1";second_imports[0].field="tab";
  second_imports[0].kind=GLWASM_HOST_IMPORT_TABLE;
  second_imports[0].resource.table=&second.table_provider;
  second_imports[1].module="tang.v1";second_imports[1].field="probe";
  second_imports[1].function=boundary_probe;second_imports[1].user=&second;
  second_imports[1].parameters=externref;second_imports[1].parameter_count=1;
  second_imports[1].results=externref;second_imports[1].result_count=1;
  ASSERT_EQ(glwasm_runtime_create_with_imports(m,second_imports,2,0,nullptr,
      &second.wasm),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second.wasm,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  GRCORE_Group *second_group=nullptr;GRCORE_Context *second_context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&second_group),GRCORE_OK);
  ASSERT_EQ(budgeted_context(second_group,&second_context),
      GRCORE_OK);
  second.context=second_context;
  ASSERT_EQ(grcore_engine_register(second_context,&tang_descriptor,
      &second.tang_engine),GRCORE_OK);
  ASSERT_EQ(grcore_code_register(second_context,second.tang_engine,
      code_handle,code_address,grjit_code_size(code),grjit_code_meta(code)),
      GRCORE_OK);
  GRCORE_EngineId second_wasm_engine=0;
  ASSERT_EQ(glwasm_runtime_bind_boundary_context(second.wasm,second_context,
      &second_wasm_engine),GLWASM_OK);
  ASSERT_EQ(grcore_context_register(second_context,&tang_fixture_key,&second),
      GRCORE_OK);
  GRCORE_FrameRef second_tang_frame{};
  ASSERT_EQ(grcore_stack_push(grcore_context_stack(second_context),
      second.tang_engine,2,&second_tang_frame),GRCORE_OK);
  const GRCORE_PollIdentity second_tang_identity{91,4,second.tang_engine};
  ASSERT_EQ(grcore_stack_set_identity(grcore_context_stack(second_context),
      second_tang_frame,second_tang_identity),
      GRCORE_OK);
  ASSERT_EQ(grcore_stack_set_guest_function(
      grcore_context_stack(second_context),second_tang_frame,91),GRCORE_OK);
  uint64_t reset_result=0;
  // Capture the exact pause identity from an interpreter call, resume it,
  // then repeat with REQUIRE-JIT and compare the Wasm frame identity.
  GRCORE_Port *pause_port=nullptr;
  ASSERT_EQ(grcore_context_port(second_context,&pause_port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(pause_port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(pause_port);
  uint64_t paused_result=0;
  EXPECT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
      second.tang_engine,56,"compiled_probe",nullptr,0,&paused_result),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(grcore_activation_count(grcore_context_stack(second_context)),1u);
  GRCORE_PollIdentity interpreter_poll{};
  ASSERT_EQ(grcore_context_poll_identity(second_context,&interpreter_poll),
      GRCORE_OK);
  ASSERT_EQ(glwasm_runtime_resume_nested(second.wasm,second_context,
      second.tang_engine,56,&paused_result),GLWASM_OK);
  EXPECT_EQ(paused_result,55u);
  EXPECT_EQ(grcore_activation_count(grcore_context_stack(second_context)),0u);
  ASSERT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
      second.tang_engine,56,"reset",nullptr,0,&reset_result),GLWASM_OK);
  EXPECT_EQ(grcore_activation_count(grcore_context_stack(second_context)),0u);
  uint64_t interpreted_results[4]={0};
  uint64_t interpreted_fuel[4]={0};
  const char *measured_exports[]={"increment","increment","read",
      "compiled_probe"};
  for(size_t i=0;i<4;i++){
    const uint64_t before=grcore_context_fuel_used(second_context);
    ASSERT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
        second.tang_engine,56,measured_exports[i],nullptr,0,
        &interpreted_results[i]),GLWASM_OK);
    interpreted_fuel[i]=grcore_context_fuel_used(second_context)-before;
  }
  EXPECT_EQ(interpreted_results[0],1u);
  EXPECT_EQ(interpreted_results[1],2u);
  EXPECT_EQ(interpreted_results[2],2u);
  EXPECT_EQ(interpreted_results[3],55u);
  ASSERT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
      second.tang_engine,56,"reset",nullptr,0,&reset_result),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second.wasm,
      GLWASM_TEST_JIT_REQUIRE),GLWASM_OK);
  pause_port=nullptr;
  ASSERT_EQ(grcore_context_port(second_context,&pause_port),GRCORE_OK);
  ASSERT_EQ(grcore_port_post(pause_port,GRCORE_REQUEST_INTERRUPT),GRCORE_OK);
  grcore_port_release(pause_port);
  EXPECT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
      second.tang_engine,56,"compiled_probe",nullptr,0,&paused_result),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(grcore_activation_count(grcore_context_stack(second_context)),1u);
  GRCORE_PollIdentity compiled_poll{};
  ASSERT_EQ(grcore_context_poll_identity(second_context,&compiled_poll),
      GRCORE_OK);
  EXPECT_EQ(compiled_poll.engine,interpreter_poll.engine);
  EXPECT_EQ(compiled_poll.function,interpreter_poll.function);
  EXPECT_EQ(compiled_poll.offset,interpreter_poll.offset);
  ASSERT_EQ(glwasm_runtime_resume_nested(second.wasm,second_context,
      second.tang_engine,56,&paused_result),GLWASM_OK);
  EXPECT_EQ(paused_result,55u);
  EXPECT_EQ(grcore_activation_count(grcore_context_stack(second_context)),0u);
  GLWASM_JitCacheStats second_probe_before{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(second.wasm,5,
      &second_probe_before),GLWASM_OK);
  const uint64_t second_probe[]={56,3};
  uint64_t before=grcore_context_fuel_used(second_context);
  EXPECT_EQ(invoke_tang(grcore_context_stack(second_context),
      second.tang_engine,second_context,second_probe),
      GRJIT_EXIT_RETURNED);
  EXPECT_EQ(output[0],55u);
  EXPECT_EQ(grcore_context_fuel_used(second_context)-before,
      interpreted_fuel[3]);
  before=grcore_context_fuel_used(second_context);
  EXPECT_EQ(invoke_tang(grcore_context_stack(second_context),
      second.tang_engine,second_context,second_probe),
      GRJIT_EXIT_RETURNED);
  EXPECT_EQ(output[0],55u);
  EXPECT_EQ(grcore_context_fuel_used(second_context)-before,
      interpreted_fuel[3]);
  EXPECT_EQ(second.operation_count,0u);
  GLWASM_JitCacheStats second_probe_stats{};
  ASSERT_EQ(glwasm_runtime_test_jit_stats(second.wasm,5,
      &second_probe_stats),GLWASM_OK);
  EXPECT_EQ(second_probe_stats.compile_successes,1u);
  EXPECT_EQ(second_probe_stats.exit_counts[GLWASM_JIT_EXIT_RETURN],
      second_probe_before.exit_counts[GLWASM_JIT_EXIT_RETURN]+2u);
  EXPECT_EQ(second_probe_stats.guest_visible_exits,0u);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second.wasm,
      GLWASM_TEST_JIT_INTERPRETER),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_invoke_nested(second.wasm,second_context,
      second.tang_engine,56,"read",nullptr,0,&output[0]),GLWASM_OK);
  EXPECT_EQ(output[0],0u);
  ASSERT_EQ(grcore_stack_pop(grcore_context_stack(second_context)),GRCORE_OK);
  ASSERT_EQ(grcore_stack_pop(stack),GRCORE_OK);
  EXPECT_EQ(grcore_code_unregister(second_context,code_address),GRCORE_OK);
  EXPECT_EQ(grcore_code_unregister(context,code_address),GRCORE_OK);
  grcore_code_release(code_handle);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(state.wasm),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(second.wasm),GLWASM_OK);
  grjit_native_table_free(natives);
  glwasm_runtime_destroy(state.wasm);
  glwasm_runtime_destroy(second.wasm);
  glwasm_module_destroy(m);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
  EXPECT_EQ(grcore_context_destroy(second_context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(second_group),GRCORE_OK);
}

TEST(WasmBoundary, WasmExceptionIdentityReturnsAcrossTheForeignBoundary) {
  static const uint8_t source[]=
      "(module (tag $value (param i32))"
      " (func (export \"raise\") (throw $value (i32.const 23)))"
      " (func (export \"roundtrip\") (param exnref) (result exnref) (local $caught exnref)"
      "   (block $handler (result i32 exnref)"
      "     (try_table (result i32) (catch_ref $value $handler)"
      "       (local.get 0) (throw_ref) (i32.const 0))"
      "     (ref.null exn)) (local.set $caught) (drop)"
      "   (local.get $caught)))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source,sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  GLWASM_Runtime *wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&wasm),GLWASM_OK);
  GRCORE_Group *group=nullptr;GRCORE_Context *context=nullptr;
  ASSERT_EQ(grcore_group_create(nullptr,nullptr,&group),GRCORE_OK);
  ASSERT_EQ(budgeted_context(group,&context),GRCORE_OK);
  GRCORE_EngineId tang=0;
  ASSERT_EQ(grcore_engine_register(context,&tang_descriptor,&tang),GRCORE_OK);

  uint64_t ignored=0;
  EXPECT_EQ(glwasm_runtime_invoke_nested(wasm,context,tang,0x7711,"raise",
      nullptr,0,&ignored),GLWASM_ERR_GUEST);
  GLWASM_ForeignException foreign{};
  ASSERT_EQ(glwasm_runtime_last_foreign_exception(wasm,&foreign),GLWASM_OK);
  EXPECT_NE(foreign.engine,0u);EXPECT_NE(foreign.engine,tang);
  ASSERT_NE(foreign.identity,0u);

  const uint64_t arguments[]={foreign.identity};
  uint64_t results[1]={0};uint32_t result_count=1;
  EXPECT_EQ(glwasm_runtime_invoke_nested_results(wasm,context,tang,0x7711,
      "roundtrip",arguments,1,results,&result_count),GLWASM_OK);
  EXPECT_EQ(result_count,1u);
  EXPECT_EQ(results[0],foreign.identity);
  EXPECT_EQ(glwasm_runtime_last_foreign_exception(wasm,&foreign),
      GLWASM_ERR_INVALID);

  // A token belongs to the runtime that minted it. Equal module contents and
  // a shared core context do not grant another runtime access to its exnref.
  GLWASM_Runtime *other_wasm=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&other_wasm),GLWASM_OK);
  GLWASM_ForeignException foreign_owner{};
  // Preserve the token before asking the owner runtime for another result.
  const uint64_t owner_identity=results[0];
  uint64_t other_results[1]={0};uint32_t other_result_count=1;
  GRCORE_EngineId other_engine=0;
  EXPECT_EQ(glwasm_runtime_bind_boundary_context(other_wasm,context,
      &other_engine),GLWASM_OK);
  EXPECT_NE(other_engine,0u);
  EXPECT_EQ(glwasm_runtime_invoke_nested_results(other_wasm,context,
      tang,0x7711,"roundtrip",&owner_identity,1,other_results,
      &other_result_count),GLWASM_ERR_GUEST);
  EXPECT_EQ(glwasm_runtime_last_foreign_exception(other_wasm,&foreign_owner),
      GLWASM_ERR_INVALID);
  // The nested result carries this guest failure. last_trap reports the
  // restored caller Run, which had no trap before the nested invocation.
  EXPECT_EQ(glwasm_runtime_last_trap(other_wasm),
      GLWASM_TRAP_NONE);
  glwasm_runtime_destroy(other_wasm);

  glwasm_runtime_destroy(wasm);glwasm_module_destroy(module);
  EXPECT_EQ(grcore_context_destroy(context),GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group),GRCORE_OK);
}

TEST(WasmBoundary, SharedCompiledModuleRemainsRegisteredUntilLastRuntimeUnbinds) {
  static const uint8_t source[] =
      "(module (func (export \"answer\") (result i32) (i32.const 42)))";
  GLWASM_Module *module = nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(source, sizeof(source) - 1, nullptr,
      nullptr, nullptr, &module), GLWASM_OK);
  GLWASM_Runtime *first = nullptr;
  GLWASM_Runtime *second = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &first), GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module, &second), GLWASM_OK);
  GRCORE_Group *group = nullptr;
  GRCORE_Context *context = nullptr;
  ASSERT_EQ(grcore_group_create(nullptr, nullptr, &group), GRCORE_OK);
  ASSERT_EQ(budgeted_context(group,&context), GRCORE_OK);
  GRCORE_EngineId caller = 0;
  ASSERT_EQ(grcore_engine_register(context, &tang_descriptor, &caller),
      GRCORE_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(first, GLWASM_TEST_JIT_REQUIRE),
      GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_test_set_jit_mode(second, GLWASM_TEST_JIT_REQUIRE),
      GLWASM_OK);

  uint64_t result = 0;
  ASSERT_EQ(glwasm_runtime_invoke_nested(first, context, caller, 0x7711,
      "answer", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_invoke_nested(second, context, caller, 0x7711,
      "answer", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  EXPECT_EQ(grcore_code_registered_count(context), 1u);

  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(first), GLWASM_OK);
  glwasm_runtime_destroy(first);
  EXPECT_EQ(grcore_code_registered_count(context), 1u);
  ASSERT_EQ(glwasm_runtime_invoke_nested(second, context, caller, 0x7711,
      "answer", nullptr, 0, &result), GLWASM_OK);
  EXPECT_EQ(result, 42u);
  ASSERT_EQ(glwasm_runtime_unbind_boundary_context(second), GLWASM_OK);
  EXPECT_EQ(grcore_code_registered_count(context), 0u);
  glwasm_runtime_destroy(second);
  glwasm_module_destroy(module);
  EXPECT_EQ(grcore_context_destroy(context), GRCORE_OK);
  EXPECT_EQ(grcore_group_destroy(group), GRCORE_OK);
}
