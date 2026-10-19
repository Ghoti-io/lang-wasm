/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/runtime-core/a/boundary.h>
#include <ghoti.io/runtime-core/a/activation.h>
#include <ghoti.io/runtime-core/a/budget_scope.h>
#include <ghoti.io/runtime-core/a/compiled.h>
#include <ghoti.io/runtime-core/a/deopt.h>
#include <ghoti.io/runtime-core/a/registry.h>
#include <ghoti.io/runtime-core/a/unwind.h>
#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/safemath.h>
#include "runtime.h"
#include "runtime_layout.h"
#include "../jit/artifact_internal.h"
#include "../jit/lower_internal.h"
#include "../jit/cache_internal.h"
#include <ghoti.io/runtime-jit/runtime-jit.h>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdatomic.h>
#include <string.h>

#define operand_capacity glwasm_operand_capacity
#define frame_slots glwasm_frame_slots
#define control_base glwasm_control_base
static uint32_t block_results(const GLWASM_Func*f,uint32_t op){return op<f->nblocks?f->blocks[op].nresults:0;}
static uint32_t branch_arity(const GLWASM_Func*f,uint32_t opener){if(opener>=f->nins)return 0;uint32_t block=f->ins[opener].a;if(block>=f->nblocks)return 0;return f->ins[opener].op==0x03?f->blocks[block].nparams:f->blocks[block].nresults;}
static uint32_t arithmetic_shift_right32(uint32_t value,uint32_t count){count&=31u;if(!count)return value;uint32_t shifted=value>>count;return value&UINT32_C(0x80000000)?shifted|(UINT32_MAX<<(32u-count)):shifted;}
static uint64_t arithmetic_shift_right64(uint64_t value,uint64_t count){count&=63u;if(!count)return value;uint64_t shifted=value>>count;return value&UINT64_C(0x8000000000000000)?shifted|(UINT64_MAX<<(64u-(unsigned)count)):shifted;}
static int32_t wasm_i32(uint32_t bits){int32_t value;memcpy(&value,&bits,sizeof(value));return value;}
static int64_t wasm_i64(uint64_t bits){int64_t value;memcpy(&value,&bits,sizeof(value));return value;}
static uint32_t wasm_sign_extend32(uint32_t value,unsigned bits){uint32_t mask=(UINT32_C(1)<<bits)-1u;value&=mask;return value&(UINT32_C(1)<<(bits-1u))?value|~mask:value;}
static uint64_t wasm_sign_extend64(uint64_t value,unsigned bits){uint64_t mask=(UINT64_C(1)<<bits)-1u;value&=mask;return value&(UINT64_C(1)<<(bits-1u))?value|~mask:value;}
static int same_value_type(GLWASM_ValType a,GLWASM_ValType b){return a.kind==b.kind&&a.nullable==b.nullable&&a.heap==b.heap&&a.mut==b.mut&&a.index==b.index;}
static int same_function_type(const GLWASM_TypeDef*a,const GLWASM_TypeDef*b){if(a->nparams!=b->nparams||a->nresults!=b->nresults)return 0;for(uint32_t i=0;i<a->nparams;i++)if(!same_value_type(a->params[i],b->params[i]))return 0;for(uint32_t i=0;i<a->nresults;i++)if(!same_value_type(a->results[i],b->results[i]))return 0;return 1;}
static GLWASM_Result map_core_result(GRCORE_Result result){switch(result){case GRCORE_OK:return GLWASM_OK;case GRCORE_ERR_UNSUPPORTED:return GLWASM_ERR_UNSUPPORTED;case GRCORE_ERR_LIMIT:return GLWASM_ERR_LIMIT;case GRCORE_ERR_OOM:return GLWASM_ERR_OOM;case GRCORE_ERR_INVALID:return GLWASM_ERR_INVALID;case GRCORE_ERR_GUEST:return GLWASM_ERR_GUEST;case GRCORE_ERR_INTERNAL:default:return GLWASM_ERR_INTERNAL;}}

typedef struct GLWASM_DebugModule {
  struct GLWASM_DebugModule *next;
  uint32_t id;
  GRCORE_EngineId engine;
  char *name;
  char **functions;
  uint32_t function_count;
  uint64_t **offsets;
  uint32_t *offset_counts;
  size_t runtime_refs;
} GLWASM_DebugModule;
typedef struct GLWASM_DebugRegistry {
  const GCU_Allocator *allocator;
  GLWASM_DebugModule *modules;
} GLWASM_DebugRegistry;
static GLWASM_Result map_core_result(GRCORE_Result result);
static uint64_t * slots(GRCORE_Stack *stack, GRCORE_FrameRef frame);

static GLWASM_Result create_jit_slots(GLWASM_Module *module,
    GRCORE_Context *context, GRCORE_EntrySlot ***out_slots) {
  *out_slots = NULL;
  if (!module->nfuncs) return GLWASM_OK;
  if (module->nfuncs != 0 &&
      sizeof(**out_slots) > SIZE_MAX / (size_t)module->nfuncs)
    return GLWASM_ERR_LIMIT;
  GRCORE_EntrySlot **slots = gcu_allocator_calloc(&module->alloc,
      module->nfuncs, sizeof(*slots));
  if (!slots) return GLWASM_ERR_OOM;
  for (uint32_t i = module->nimported_funcs; i < module->nfuncs; ++i) {
    GRCORE_Result result = grcore_entry_slot_create(context, &slots[i]);
    if (result != GRCORE_OK) {
      for (uint32_t j = module->nimported_funcs; j < i; ++j)
        if (slots[j]) (void)grcore_entry_slot_clear(context, slots[j]);
      gcu_allocator_free(&module->alloc, slots);
      return map_core_result(result);
    }
  }
  *out_slots = slots;
  return GLWASM_OK;
}

static void clear_jit_slots(GLWASM_Module *module, GRCORE_Context *context,
    GRCORE_EntrySlot **slots) {
  if (!slots) return;
  for (uint32_t i = module->nimported_funcs; i < module->nfuncs; ++i)
    if (slots[i]) (void)grcore_entry_slot_clear(context, slots[i]);
  gcu_allocator_free(&module->alloc, slots);
}

static void debug_registry_destroy(GRCORE_Context *context, void *value) {
  (void)context;
  GLWASM_DebugRegistry *registry=value;
  if (!registry) return;
  GLWASM_DebugModule *module=registry->modules;
  while (module) {
    GLWASM_DebugModule *next=module->next;
    gcu_allocator_free(registry->allocator,module->name);
    for (uint32_t i=0;i<module->function_count;i++)
      gcu_allocator_free(registry->allocator,module->functions[i]);
    gcu_allocator_free(registry->allocator,module->functions);
    if(module->offsets)for(uint32_t i=0;i<module->function_count;i++)
      gcu_allocator_free(registry->allocator,module->offsets[i]);
    gcu_allocator_free(registry->allocator,module->offsets);
    gcu_allocator_free(registry->allocator,module->offset_counts);
    gcu_allocator_free(registry->allocator,module);
    module=next;
  }
  gcu_allocator_free(grcore_context_allocator(context),registry);
}
static const GRCORE_Key debug_registry_key=GRCORE_KEY_INIT(
    "lang-wasm debug names",GRCORE_CARDINALITY_ONE,GRCORE_PHASE_YIELD,
    debug_registry_destroy,NULL,NULL,NULL,NULL);

static char *copy_debug_name(const GCU_Allocator *allocator,
    const GLWASM_Name *name) {
  if (!name||!name->bytes) return NULL;
  const char *bytes=name->debug_bytes?name->debug_bytes:name->bytes;
  size_t length=name->debug_bytes?name->debug_length:name->length;
  char *copy=gcu_allocator_malloc(allocator,length+1u);
  if(copy)memcpy(copy,bytes,length+1u);
  return copy;
}
static char *copy_fallback_function_name(const GCU_Allocator *allocator,
    uint32_t index) {
  char text[32];
  int length=snprintf(text,sizeof(text),"function[%u]",index);
  if(length<0||(size_t)length>=sizeof(text))return NULL;
  char *copy=gcu_allocator_malloc(allocator,(size_t)length+1u);
  if(copy)memcpy(copy,text,(size_t)length+1u);
  return copy;
}
static GLWASM_Result ensure_debug_names(GRCORE_Context *context,
    const GLWASM_Module *module,GRCORE_EngineId engine) {
  if(!engine)return GLWASM_ERR_INVALID;
  GLWASM_DebugRegistry *registry=grcore_context_slot(context,&debug_registry_key);
  if(!registry){
    const GCU_Allocator *allocator=grcore_context_allocator(context);
    registry=gcu_allocator_calloc(allocator,1,sizeof(*registry));
    if(!registry)return GLWASM_ERR_OOM;
    registry->allocator=allocator;
    GRCORE_Result result=grcore_context_register(context,&debug_registry_key,registry);
    if(result!=GRCORE_OK){gcu_allocator_free(allocator,registry);return map_core_result(result);}
  }
  for(GLWASM_DebugModule *it=registry->modules;it;it=it->next)
    if(it->id==module->debug_id)
      return it->engine==engine?GLWASM_OK:GLWASM_ERR_INVALID;
  GLWASM_DebugModule *entry=gcu_allocator_calloc(registry->allocator,1,
      sizeof(*entry));
  if(!entry)return GLWASM_ERR_OOM;
  entry->id=module->debug_id;entry->engine=engine;
  entry->function_count=module->nfuncs;
  if(module->module_name.bytes){
    entry->name=copy_debug_name(registry->allocator,&module->module_name);
    if(!entry->name){gcu_allocator_free(registry->allocator,entry);return GLWASM_ERR_OOM;}
  }
  if(entry->function_count){
    entry->functions=gcu_allocator_calloc(registry->allocator,
        entry->function_count,sizeof(*entry->functions));
    entry->offsets=gcu_allocator_calloc(registry->allocator,
        entry->function_count,sizeof(*entry->offsets));
    entry->offset_counts=gcu_allocator_calloc(registry->allocator,
        entry->function_count,sizeof(*entry->offset_counts));
    if(!entry->functions||!entry->offsets||!entry->offset_counts){
      gcu_allocator_free(registry->allocator,entry->functions);
      gcu_allocator_free(registry->allocator,entry->offsets);
      gcu_allocator_free(registry->allocator,entry->offset_counts);
      gcu_allocator_free(registry->allocator,entry->name);
      gcu_allocator_free(registry->allocator,entry);return GLWASM_ERR_OOM;}
    for(uint32_t i=0;i<entry->function_count;i++){
      const GLWASM_Name *name=i<module->function_names_count?
          &module->function_names[i]:NULL;
      entry->functions[i]=name&&name->bytes?
          copy_debug_name(registry->allocator,name):
          copy_fallback_function_name(registry->allocator,i);
      if(!entry->functions[i]){
        for(uint32_t j=0;j<i;j++)
          gcu_allocator_free(registry->allocator,entry->functions[j]);
        gcu_allocator_free(registry->allocator,entry->functions);
        for(uint32_t j=0;j<i;j++)
          gcu_allocator_free(registry->allocator,entry->offsets[j]);
        gcu_allocator_free(registry->allocator,entry->offsets);
        gcu_allocator_free(registry->allocator,entry->offset_counts);
        gcu_allocator_free(registry->allocator,entry->name);
        gcu_allocator_free(registry->allocator,entry);
        return GLWASM_ERR_OOM;
      }
      const GLWASM_TranslatedFunc *translated=module->funcs[i].translated;
      if(translated&&translated->offset_count){
        if(translated->offset_count != 0 &&
            sizeof(uint64_t)>SIZE_MAX/(size_t)translated->offset_count){
          for(uint32_t j=0;j<=i;j++)
            gcu_allocator_free(registry->allocator,entry->functions[j]);
          gcu_allocator_free(registry->allocator,entry->functions);
          for(uint32_t j=0;j<i;j++)
            gcu_allocator_free(registry->allocator,entry->offsets[j]);
          gcu_allocator_free(registry->allocator,entry->offsets);
          gcu_allocator_free(registry->allocator,entry->offset_counts);
          gcu_allocator_free(registry->allocator,entry->name);
          gcu_allocator_free(registry->allocator,entry);
          return GLWASM_ERR_LIMIT;
        }
        size_t bytes=(size_t)translated->offset_count*sizeof(uint64_t);
        entry->offsets[i]=gcu_allocator_malloc(registry->allocator,bytes);
        if(!entry->offsets[i]){
          for(uint32_t j=0;j<=i;j++)
            gcu_allocator_free(registry->allocator,entry->functions[j]);
          gcu_allocator_free(registry->allocator,entry->functions);
          for(uint32_t j=0;j<i;j++)
            gcu_allocator_free(registry->allocator,entry->offsets[j]);
          gcu_allocator_free(registry->allocator,entry->offsets);
          gcu_allocator_free(registry->allocator,entry->offset_counts);
          gcu_allocator_free(registry->allocator,entry->name);
          gcu_allocator_free(registry->allocator,entry);
          return GLWASM_ERR_OOM;
        }
        memcpy(entry->offsets[i],translated->offsets,bytes);
        entry->offset_counts[i]=translated->offset_count;
      }
    }
  }
  entry->next=registry->modules;registry->modules=entry;
  return GLWASM_OK;
}
static void destroy_debug_module(GLWASM_DebugRegistry *registry,
    GLWASM_DebugModule *module) {
  gcu_allocator_free(registry->allocator,module->name);
  for(uint32_t i=0;i<module->function_count;i++)
    gcu_allocator_free(registry->allocator,module->functions[i]);
  gcu_allocator_free(registry->allocator,module->functions);
  if(module->offsets)for(uint32_t i=0;i<module->function_count;i++)
    gcu_allocator_free(registry->allocator,module->offsets[i]);
  gcu_allocator_free(registry->allocator,module->offsets);
  gcu_allocator_free(registry->allocator,module->offset_counts);
  gcu_allocator_free(registry->allocator,module);
}
static GLWASM_Result retain_debug_names(GRCORE_Context *context,
    const GLWASM_Module *module,GRCORE_EngineId engine) {
  GLWASM_DebugRegistry *registry=grcore_context_slot(context,&debug_registry_key);
  for(GLWASM_DebugModule *it=registry?registry->modules:NULL;it;it=it->next)
    if(it->id==module->debug_id&&it->engine==engine){
      if(it->runtime_refs==SIZE_MAX)return GLWASM_ERR_LIMIT;
      it->runtime_refs++;return GLWASM_OK;
    }
  return GLWASM_ERR_INTERNAL;
}
static void release_debug_names(GRCORE_Context *context,uint32_t module_id,
    GRCORE_EngineId engine) {
  GLWASM_DebugRegistry *registry=grcore_context_slot(context,&debug_registry_key);
  if(!registry)return;
  GLWASM_DebugModule **link=&registry->modules;
  while(*link){
    GLWASM_DebugModule *entry=*link;
    if(entry->id==module_id&&entry->engine==engine){
      if(entry->runtime_refs&&--entry->runtime_refs==0){
        *link=entry->next;destroy_debug_module(registry,entry);
      }
      return;
    }
    link=&entry->next;
  }
}
static GLWASM_DebugModule *find_debug_module(const GRCORE_Context *context,
    uint64_t function) {
  GLWASM_DebugRegistry *registry=grcore_context_slot(context,&debug_registry_key);
  uint32_t id=(uint32_t)(function>>32);
  for(GLWASM_DebugModule *it=registry?registry->modules:NULL;it;it=it->next)
    if(it->id==id)return it;
  return NULL;
}
static GRCORE_Location locate_scoped(const GRCORE_Context *context,
    GRCORE_EngineId engine,uint64_t function,uint64_t offset) {
  (void)offset;
  GLWASM_DebugModule *module=find_debug_module(context,function);
  if(module&&module->engine!=engine)module=NULL;
  return (GRCORE_Location){module&&module->name?module->name:"wasm",0};
}
static const char *function_name_scoped(const GRCORE_Context *context,
    GRCORE_EngineId engine,uint64_t function) {
  GLWASM_DebugModule *module=find_debug_module(context,function);
  if(module&&module->engine!=engine)return NULL;
  uint32_t index=(uint32_t)function;
  return module&&index<module->function_count?module->functions[index]:NULL;
}
static GRCORE_InstructionValidity instruction_valid(const GRCORE_Context *context,
    GRCORE_EngineId engine,uint64_t function,uint64_t offset) {
  GLWASM_DebugModule *debug_module=find_debug_module(context,function);
  if(!debug_module)return GRCORE_INSTRUCTION_UNKNOWN;
  if(debug_module->engine!=engine)return GRCORE_INSTRUCTION_UNKNOWN;
  uint32_t index=(uint32_t)function;
  if(index>=debug_module->function_count)
    return GRCORE_INSTRUCTION_INVALID;
  uint64_t *offsets=debug_module->offsets[index];
  uint32_t offset_count=debug_module->offset_counts[index];
  if(!offsets||!offset_count)return GRCORE_INSTRUCTION_INVALID;
  /* offset_count includes the terminal boundary after the final instruction;
   * that position is useful for resumption metadata, but is not executable. */
  for(uint32_t i=0;i+1u<offset_count;i++)
    if(offsets[i]==offset)return GRCORE_INSTRUCTION_VALID;
  return GRCORE_INSTRUCTION_INVALID;
}
static uint64_t wasm_function_identity(const GLWASM_Module *module,
    uint32_t function) {
  return ((uint64_t)module->debug_id<<32)|function;
}
static GRCORE_Result wasm_set_guest_frame_identity(GRCORE_Stack *stack,
    GRCORE_FrameRef frame,const GLWASM_Module *module,uint32_t function,
    uint64_t offset) {
  if(grcore_stack_set_identity(stack,frame,
      (GRCORE_PollIdentity){wasm_function_identity(module,function),offset,0})
      !=GRCORE_OK)
    return GRCORE_ERR_INVALID;
  return grcore_stack_set_guest_function(stack,frame,function);
}
static size_t wasm_scope_count(const GRCORE_AbstractFrame *frame);
static GRCORE_Result wasm_scope(const GRCORE_AbstractFrame *frame,
    size_t index,GRCORE_ScopeInfo *out);
static GRCORE_Result wasm_variable(const GRCORE_AbstractFrame *frame,
    size_t scope,size_t index,GRCORE_Variable *out);
static size_t wasm_memory_count(const GRCORE_AbstractFrame *frame);
static GRCORE_Result wasm_memory_info(const GRCORE_AbstractFrame *frame,
    size_t index,GRCORE_MemoryInfo *out);
static GRCORE_Result wasm_memory_read(const GRCORE_AbstractFrame *frame,
    size_t index,uint64_t offset,void *buffer,size_t size);
/* Wasm's current numeric values are represented in one guest slot as their
 * raw bits (i32/f32 in the low word; i64/f64 in the full word). These hooks
 * only copy bits: they allocate, poll, collect, and fail neither before nor
 * after writing. Runtime-core validates metadata before invoking `convert`,
 * so the default arm is unreachable for a valid conversion; zeroing it is a
 * defensive fail-closed result. Wasm references need a distinct policy. */
static void wasm_deopt_convert(GRCORE_Context *context,
    GRCORE_Representation representation, uint64_t raw, uint64_t *out_root) {
  (void)context;
  switch (representation) {
  case GRCORE_REPR_I32:
  case GRCORE_REPR_F32:
    *out_root = (uint32_t)raw;
    break;
  case GRCORE_REPR_I64:
  case GRCORE_REPR_F64:
    *out_root = raw;
    break;
  case GRCORE_REPR_BITS:
  case GRCORE_REPR_COUNT:
  default:
    *out_root = 0;
    break;
  }
}
static bool wasm_deopt_reverse(GRCORE_Context *context,
    GRCORE_Representation representation, uint64_t value, uint64_t *out_raw) {
  (void)context;
  if (!out_raw) return false;
  uint64_t raw;
  switch (representation) {
  case GRCORE_REPR_I32:
  case GRCORE_REPR_F32:
    raw = (uint32_t)value;
    break;
  case GRCORE_REPR_I64:
  case GRCORE_REPR_F64:
    raw = value;
    break;
  case GRCORE_REPR_BITS:
  case GRCORE_REPR_COUNT:
  default:
    return false;
  }
  *out_raw = raw;
  return true;
}
static const GRCORE_EngineDescriptor descriptor = {
    sizeof(GRCORE_EngineDescriptor), "lang-wasm", NULL, NULL, NULL,
    {wasm_scope_count, wasm_scope, wasm_variable}, {0, 0, 0}, NULL, NULL,
    wasm_deopt_convert, wasm_deopt_reverse, NULL,
    {wasm_memory_count, wasm_memory_info, wasm_memory_read}, instruction_valid,
    locate_scoped,function_name_scoped};
static GRCORE_EngineId find_wasm_engine(const GRCORE_Context *context) {
  for(GRCORE_EngineId id=1;id<=grcore_engine_count(context);id++)
    if(grcore_engine_descriptor(context,id)==&descriptor)return id;
  return 0;
}

/* A host function's callbacks and the types its import declared. */
typedef struct HostBinding {
  GLWASM_HostFunction function;
  GLWASM_HostResumeFunction resume;
  GLWASM_HostFunctionResults function_results;
  GLWASM_HostResumeFunctionResults resume_results;
  void *user;
  GLWASM_HostValueType *parameter_types;
  GLWASM_HostValueType *result_types;
} HostBinding;

/* Store objects (the spec's "store"). An instance's index spaces point
 * into them, so two instances that link share the object itself. A funcref
 * value is a store function address plus one: it names the same function in
 * every instance and every table of the store. */
typedef struct StoreFunc {
  GLWASM_Runtime *instance;    /* the defining instance; NULL for the host */
  uint32_t index;              /* its function index in instance->module */
  const GLWASM_TypeDef *type;  /* the module's type, or host_type */
  GLWASM_TypeDef host_type;    /* a host function's declared type */
  HostBinding host;
} StoreFunc;

typedef struct StoreMemory {
  uint8_t *bytes;
  uint64_t length;             /* bytes */
  uint64_t pages;
  uint64_t max;                /* pages, when has_max */
  uint8_t has_max, addr64;
  uint8_t hosted;              /* backed by a host provider's leases */
  GLWASM_HostMemoryProvider provider;
} StoreMemory;

typedef struct StoreTable {
  uint64_t *elements;
  uint64_t length;
  GLWASM_ValType elem;
  uint64_t max;                /* elements, when has_max */
  uint8_t has_max, addr64;
  uint8_t hosted;              /* backed by a host provider's leases */
  GLWASM_HostTableProvider provider;
} StoreTable;

typedef struct StoreGlobal {
  uint64_t value;              /* unless host: then *host->value */
  GLWASM_ValType type;
  uint8_t mut;
  GLWASM_HostGlobal *host;
} StoreGlobal;

/* A tag's identity is the object. Host tags with one identity and one
 * payload type share an object, so they catch each other's exceptions. */
typedef struct StoreTag {
  uint64_t host_identity;      /* nonzero for a host tag */
  GLWASM_ValType *params;
  uint32_t nparams;
} StoreTag;

typedef struct ExceptionValue {
  struct ExceptionValue *next;
  const GCU_Allocator *allocator;
  uint64_t identity;
  StoreTag *tag;
  uint32_t payload_count;
  uint64_t payload[];
} ExceptionValue;

/* The store's one execution. `instance` and `m` are the instance whose
 * frame is on top; every frame transition rebinds them (run_bind), so a call
 * into another instance's function is an ordinary frame push. */
typedef struct Run {
  GLWASM_Module *m; GRCORE_Context *ctx; GRCORE_EngineId engine;
  GLWASM_Runtime *instance;
  GLWASM_Runtime *entry_instance; /* the instance whose function was invoked */
  GLWASM_Store *store;
  const GCU_Allocator *guest_alloc;
  uint32_t entry; uint64_t *args; uint32_t nargs; uint64_t result;
  uint64_t *results; uint32_t result_capacity, result_arity, result_count;
  uint8_t jit_entry_attempted;
  uint32_t jit_pending_function;
  uint8_t jit_pending;
  size_t stack_base;
  uint32_t pending_host_function; int pending_host;
  ExceptionValue *pending_exception;
  void *snapshot_restore;
  uint32_t bulk_func, bulk_pc, bulk_op;
  uint64_t bulk_dst, bulk_src, bulk_len, bulk_done, bulk_value;
  int bulk_active;
  /* Retained only to explain a failed invocation after runtime-core has
   * unwound its guest frames. These fields are internal test diagnostics. */
  uint32_t diagnostic_function, diagnostic_pc;
  uint32_t diagnostic_call_target;
  uint64_t diagnostic_call_sp, diagnostic_call_base, diagnostic_call_required;
  uint32_t diagnostic_caller_params, diagnostic_caller_locals;
  uint32_t diagnostic_callee_params;
  uint32_t diagnostic_frame_entry_function;
  uint64_t diagnostic_frame_entry_sp, diagnostic_frame_entry_base;
  uint32_t diagnostic_first_bad_sp_function, diagnostic_first_bad_sp_pc;
  uint64_t diagnostic_first_bad_sp, diagnostic_first_bad_base;
  uint32_t last_jit_lower_pc, last_jit_lower_raw_result;
  uint32_t last_jit_lower_stack_count, last_jit_lower_site;
  uint16_t last_jit_lower_opcode;
  uint16_t diagnostic_opcode;
  uint8_t diagnostic_has_instruction, diagnostic_reason;
  uint8_t diagnostic_step, diagnostic_context_state;
  GLWASM_Result error; GLWASM_Trap trap; int active; int paused;
} Run;

static int global_reference_valid(const Run *r,uint32_t index,
    uint64_t token);

static void diagnostic_record_frame_entry(Run *run, uint32_t function,
    uint64_t sp, uint64_t base);

static void diagnostic_record_sp(Run *run, uint32_t function, uint32_t pc,
    uint64_t sp, uint64_t base) {
  if (sp >= base || run->diagnostic_first_bad_sp_function != UINT32_MAX)
    return;
  run->diagnostic_first_bad_sp_function = function;
  run->diagnostic_first_bad_sp_pc = pc;
  run->diagnostic_first_bad_sp = sp;
  run->diagnostic_first_bad_base = base;
}

/* One instance of a module. Its index spaces hold store addresses: what it
 * defined and what it imported are the same kind of entry. */
struct GLWASM_Runtime {
  GLWASM_Store *store;
  GLWASM_Module *module;
  uint32_t *funcs;             /* function index -> store function address */
  StoreTable **tables;
  StoreMemory **memories;
  StoreGlobal **globals;
  StoreTag **tags;
  uint8_t *data_dropped;
  /* Element instances: each segment's references, evaluated once at
   * instantiation. A dropped (or declarative, or applied active) segment
   * has none left. */
  uint64_t **elem_values;
  uint32_t *elem_counts;
  char **memory_debug_names;
  uintptr_t boundary_token;    /* the instance's context-unique identity */
  GRCORE_EntrySlot **jit_slots;
  uint8_t *jit_registered;
  uint64_t **jit_spills;
  size_t *jit_spill_capacities;
  size_t *jit_deopt_cells;
  size_t jit_spill_count;
  size_t jit_call_depth;
  size_t jit_root_deopt_cells;
  uint32_t jit_helper_status;
  uint32_t jit_test_pause_bulk_chunks;
  uint8_t *test_observed_functions;
  GRCORE_DeoptReservation *jit_reservation;
  GRCORE_EntrySlot **boundary_jit_slots;
  uint8_t *boundary_jit_registered;
  GRCORE_DeoptReservation *boundary_jit_reservation;
  uint64_t jit_deopt_cause;
  GLWASM_TestJitMode jit_mode;
  uint32_t jit_exit_function;
  uint16_t jit_exit_opcode;
  uint8_t jit_exit_trap_reconstruction;
  uint64_t test_deopt_count;
  GLWASM_TestDeoptOrigin test_first_deopt;
  GLWASM_TestDeoptOrigin test_last_deopt;
  GLWASM_Result last_jit_compile_result;
  uint8_t last_jit_compile_attempted;
};

/* The store: one runtime-core context, its one Run, and every object. */
struct GLWASM_Store {
  const GCU_Allocator *alloc;  /* bookkeeping */
  const GCU_Allocator *guest_alloc; /* the context's budgeted allocator */
  GLWASM_Policy policy;
  GRCORE_Group *group; GRCORE_Options *options;
  GRCORE_Context *context; GRCORE_EngineId engine;
  Run run;
  StoreFunc **funcs; uint32_t nfuncs, cfuncs;
  StoreTable **tables; uint32_t ntables, ctables;
  StoreMemory **memories; uint32_t nmemories, cmemories;
  StoreGlobal **globals; uint32_t nglobals, cglobals;
  StoreTag **tags; uint32_t ntags, ctags;
  GLWASM_Runtime **instances; uint32_t ninstances, cinstances;
  ExceptionValue *exceptions;
  /* An adopted runtime-core context (boundary calls, Story 12). */
  GRCORE_Context *boundary_context;
  GRCORE_EngineId boundary_engine;
  Run nested_saved_run;
  uintptr_t nested_saved_token;
  GLWASM_Runtime *nested_runtime;
  GRCORE_Context *nested_context;
  GRCORE_EngineId nested_caller;
  uintptr_t nested_instance;
  size_t nested_base;
  GRCORE_BoundaryRef nested_boundary;
  int nested_active;
  int nested_owns_run;
  GLWASM_ForeignException last_foreign_exception;
  GRCORE_Result last_core_result;
  GRCORE_Outcome last_outcome;
  uint8_t solo;                /* test seam: made by glwasm_runtime_create */
  uint8_t test_fuel_pauses;    /* test seam: exhausted fuel pauses */
};

/* Bind the run to the instance whose frame is now on top. */
static void run_bind(Run *run, GLWASM_Runtime *instance) {
  run->instance = instance;
  run->m = instance->module;
}

static StoreFunc *instance_func(const GLWASM_Runtime *instance,
    uint32_t index) {
  return instance->store->funcs[instance->funcs[index]];
}

/* The funcref token of an instance's function `index`. */
static uint64_t instance_funcref(const GLWASM_Runtime *instance,
    uint32_t index) {
  return (uint64_t)instance->funcs[index] + 1u;
}

static void elem_drop(GLWASM_Runtime *instance, uint32_t index) {
  gcu_allocator_free(instance->store->alloc, instance->elem_values[index]);
  instance->elem_values[index] = NULL;
  instance->elem_counts[index] = 0;
}

static void diagnostic_record_frame_entry(Run *run, uint32_t function,
    uint64_t sp, uint64_t base) {
  run->diagnostic_frame_entry_function = function;
  run->diagnostic_frame_entry_sp = sp;
  run->diagnostic_frame_entry_base = base;
  /* All guest frame entry paths (root, nested, direct, indirect and tail)
   * converge here after allocating and identifying the guest frame. Keep a
   * bounded per-instance observation set for the strict WAST tier audit. */
  if (run->instance && run->instance->test_observed_functions &&
      function < run->m->nfuncs && !run->m->funcs[function].imported)
    run->instance->test_observed_functions[function] = 1u;
}

static atomic_uintptr_t next_boundary_token=1;
static atomic_uint_fast64_t next_exception_identity=ATOMIC_VAR_INIT(1);
static _Thread_local GLWASM_Runtime *active_jit_runtime;

static uint64_t allocate_exception_identity(void) {
  uint_fast64_t identity=atomic_load_explicit(&next_exception_identity,
      memory_order_relaxed);
  for(;;){
    if(identity==0||identity>=UINT64_MAX)return 0;
    if(atomic_compare_exchange_weak_explicit(&next_exception_identity,
        &identity,identity+1u,memory_order_relaxed,memory_order_relaxed))
      return (uint64_t)identity;
  }
}

static int jit_state_for_context(GLWASM_Runtime *runtime,
    GRCORE_Context *context, GRCORE_EntrySlot ***slots_out,
    uint8_t **registered_out, GRCORE_DeoptReservation **reservation_out) {
  if(!runtime||!context)return 0;
  if(context==runtime->store->context){
    if(slots_out)*slots_out=runtime->jit_slots;
    if(registered_out)*registered_out=runtime->jit_registered;
    if(reservation_out)*reservation_out=runtime->jit_reservation;
    return 1;
  }
  if(context==runtime->store->boundary_context){
    if(slots_out)*slots_out=runtime->boundary_jit_slots;
    if(registered_out)*registered_out=runtime->boundary_jit_registered;
    if(reservation_out)*reservation_out=runtime->boundary_jit_reservation;
    return runtime->boundary_jit_slots!=NULL;
  }
  return 0;
}

static void clear_jit_context_state(GLWASM_Module *module,
    GRCORE_Context *context,GRCORE_EntrySlot ***slots_pointer,
    uint8_t **registered_pointer,
    GRCORE_DeoptReservation **reservation_pointer) {
  if(!module||!context)return;
  GRCORE_EntrySlot **slots=slots_pointer?*slots_pointer:NULL;
  uint8_t *registered=registered_pointer?*registered_pointer:NULL;
  if(slots&&registered)for(uint32_t i=module->nimported_funcs;
      i<module->nfuncs;i++)if(registered[i]&&slots[i]){
    GRCORE_Code *code=grcore_entry_slot_code(slots[i]);
    uintptr_t address=code?(uintptr_t)grjit_code_address(
        glwasm_jit_artifact_code(code)):0;
    (void)grcore_entry_slot_clear(context,slots[i]);
    if(address)(void)grcore_code_unregister(context,address);
    registered[i]=0;
  }
  clear_jit_slots(module,context,slots);
  if(slots_pointer)*slots_pointer=NULL;
  if(registered_pointer){
    gcu_allocator_free(&module->alloc,registered);
    *registered_pointer=NULL;
  }
  if(reservation_pointer&&*reservation_pointer){
    (void)grcore_deopt_release(context,*reservation_pointer);
    *reservation_pointer=NULL;
  }
}

static GLWASM_Result register_jit_runtime(GLWASM_Runtime *runtime,
    GRCORE_Context *context, GRCORE_DeoptReservation **reservation) {
  (void)runtime;
  GRCORE_Result result = grcore_deopt_reserve(context, 0, reservation);
  return map_core_result(result);
}

/* A rebuild needs one rooted conversion cell for every converting location
 * among all of this function's deopt sites. Reserve the maximum before the
 * compiled frame runs; nested calls extend it before entering their callee. */
static size_t jit_code_max_deopt_cells(const GRJIT_Code *code) {
  const GRCORE_CodeMeta *meta = code ? grjit_code_meta(code) : NULL;
  if (!meta) return SIZE_MAX;
  size_t maximum = 0;
  for (size_t i = 0; i < meta->site_count; ++i) {
    size_t cells = grcore_deopt_converting_count(&meta->sites[i]);
    if (cells > maximum) maximum = cells;
  }
  return maximum;
}

static GLWASM_Result jit_extend_deopt_reservation(GLWASM_Runtime *runtime,
    GRCORE_Context *context, size_t cells) {
  GRCORE_DeoptReservation *reservation = NULL;
  if (!runtime || !jit_state_for_context(runtime, context, NULL, NULL,
      &reservation) || !reservation || cells == SIZE_MAX)
    return GLWASM_ERR_INTERNAL;
  return map_core_result(grcore_deopt_reservation_extend(context,
      reservation, cells));
}

static void jit_retract_deopt_reservation(GLWASM_Runtime *runtime,
    GRCORE_Context *context, size_t cells) {
  GRCORE_DeoptReservation *reservation = NULL;
  if (cells && runtime && jit_state_for_context(runtime, context, NULL, NULL,
      &reservation) && reservation)
    grcore_deopt_reservation_retract(reservation, cells);
}

static void jit_release_compiled_reservations(GLWASM_Runtime *runtime,
    GRCORE_Context *context) {
  if (!runtime) return;
  for (size_t depth = runtime->jit_call_depth; depth > 0; --depth) {
    size_t cells = runtime->jit_deopt_cells[depth];
    runtime->jit_deopt_cells[depth] = 0;
    jit_retract_deopt_reservation(runtime, context, cells);
  }
  runtime->jit_call_depth = 0;
  size_t root_cells = runtime->jit_root_deopt_cells;
  runtime->jit_root_deopt_cells = 0;
  jit_retract_deopt_reservation(runtime, context, root_cells);
}

static void jit_record_active_nested_deopts(GLWASM_Runtime *runtime,
    uint64_t cause);

static uint32_t wasm_jit_deopt(void *opaque_context, uint64_t cause) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime=active_jit_runtime;
  GRCORE_DeoptReservation *reservation=NULL;
  if (!runtime || runtime->store->run.ctx!=context||
      !jit_state_for_context(runtime,context,NULL,NULL,&reservation)||
      !reservation) {
    return (uint32_t)GRCORE_ERR_INVALID;
  }
  size_t rebuilt = 0;
  size_t keep = cause == 2u ? 0u : SIZE_MAX;
  jit_record_active_nested_deopts(runtime, cause);
  /* The rebuild writes slots, not identities: each compiled frame's guest
   * frame still has the identity it was pushed with, its function's entry.
   * Give it its compiled frame's (a caller's call site, the innermost
   * frame's exit site) before the rebuild clears the record. Only a poll
   * sets the innermost identity itself, and a named exit is not a poll:
   * the interpreter resumes that instruction without polling it again. */
  {
    GRCORE_Stack *stack = grcore_context_stack(context);
    const size_t frame_count = grcore_stack_frame_count(stack);
    GRCORE_CompiledWalk walk;
    GRCORE_CompiledFrame frame;
    if (frame_count && grcore_compiled_walk_begin(context, &walk) == GRCORE_OK) {
      GRCORE_FrameRef guest = grcore_stack_top(stack);
      size_t guest_index = frame_count - 1u, record = 0;
      int any = 0;
      while (grcore_compiled_walk_next(&walk, &frame) == GRCORE_CWALK_FRAME) {
        if (any && frame.record != record) break; /* not this rebuild's run */
        any = 1;
        record = frame.record;
        const size_t index = frame.base_frames - 1u +
            (frame.run_length - 1u - frame.run_depth);
        while (guest_index > index) {
          guest = grcore_stack_caller(stack, guest);
          --guest_index;
        }
        if (index < frame_count)
          (void)grcore_stack_set_identity(stack, guest, frame.identity);
      }
    }
  }
  GRCORE_Result result = grcore_compiled_rebuild(context,
      reservation, keep, &rebuilt);
  if (result == GRCORE_OK) runtime->jit_deopt_cause = cause;
  return (uint32_t)result;
}

static uint32_t wasm_jit_push(void *context, void *callee_environment,
    uint64_t callee, const uint64_t *args, uint64_t arg_count,
    uint64_t result_count);
static uint32_t wasm_jit_tail(void *context, void *callee_environment,
    uint64_t callee, const uint64_t *args, uint64_t arg_count,
    uint64_t result_count);
static void wasm_jit_pop(void *context);
static uint32_t wasm_jit_catch_exception(void *context,
    void *caller_environment, uint64_t callee, uint32_t landing_block);
static uint32_t wasm_jit_compile(void *context, uint64_t callee);
static int ensure_jit_spill(GLWASM_Runtime *runtime, uint32_t function,
    size_t depth);

static const GRJIT_CallHooks wasm_jit_hooks = {
  .push = wasm_jit_push,
  .pop = wasm_jit_pop,
  .compile = wasm_jit_compile,
  .deopt = wasm_jit_deopt,
  .tail = wasm_jit_tail,
  .catch_exception = wasm_jit_catch_exception
};
static void wasm_jit_charge(void *environment, uint64_t amount);
static uint32_t wasm_jit_poll(void *context, uint64_t function,
    uint64_t offset);
static uint64_t wasm_jit_memory_access(void *environment,
    uint64_t function, uint64_t pc, uint64_t address, uint64_t value);
static uint32_t wasm_jit_host_call(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base);
static uint32_t wasm_jit_helper_succeeded(void *environment);
static uint64_t wasm_jit_global_get(void *environment, uint64_t index);
static uint32_t wasm_jit_global_set(void *environment, uint64_t index,
    uint64_t value);
static uintptr_t wasm_jit_slot_entry(void *environment, uint64_t callee);
static uint32_t wasm_jit_indirect_target(void *environment,
    uint64_t function, uint64_t pc, uint64_t table_index);
static uint32_t wasm_jit_table_bulk(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base);
static uint32_t wasm_jit_memory_bulk(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base);
static uint32_t wasm_jit_take_exception(void *environment, uint64_t function,
    uint64_t clause, uint64_t *spill, uint64_t base, uint64_t arity);
static uint32_t wasm_jit_raise_tag(void *environment, uint64_t function,
    uint64_t tag, uint64_t *spill, uint64_t payload_base);
static uint32_t wasm_jit_raise_ref(void *environment, uint64_t identity);
static uint64_t wasm_jit_funcref(void *environment, uint64_t function);
static int dispatch_exception(Run *run, ExceptionValue *exception);
static GRCORE_Step finish_uncaught_exception(GRCORE_Context *context,
    Run *run, uint32_t function, uint32_t pc, ExceptionValue *exception);

static uint32_t wasm_jit_record_exit(void *environment, uint64_t function,
    uint64_t opcode, int trap_reconstruction) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || function >= runtime->module->nfuncs || opcode > UINT16_MAX ||
      (opcode > UINT8_MAX &&
       ((opcode & UINT64_C(0xff00)) != UINT64_C(0xfc00) ||
        (opcode & UINT64_C(0x00ff)) > 17u)))
    return 1u;
  runtime->jit_exit_function = (uint32_t)function;
  runtime->jit_exit_opcode = (uint16_t)opcode;
  if (trap_reconstruction) runtime->jit_exit_trap_reconstruction = 1;
  if (!trap_reconstruction)
    glwasm_jit_cache_note_opcode_exit(runtime->module->jit_cache,
        (uint32_t)function, (uint16_t)opcode);
  return 0u;
}

static uint32_t wasm_jit_named_exit(void *environment, uint64_t function,
    uint64_t opcode) {
  GLWASM_Runtime *runtime = environment;
  return wasm_jit_record_exit(environment, function, opcode,
      runtime && runtime->jit_exit_trap_reconstruction);
}

static uint32_t wasm_jit_trap_exit(void *environment, uint64_t function,
    uint64_t opcode) {
  return wasm_jit_record_exit(environment, function, opcode, 1);
}

static uintptr_t wasm_jit_spill_base(void *environment, uint64_t function) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || function >= runtime->module->nfuncs ||
      runtime->jit_call_depth >= runtime->jit_spill_count ||
      !runtime->jit_spills[runtime->jit_call_depth]) return 0;
  return (uintptr_t)runtime->jit_spills[runtime->jit_call_depth];
}

static int ensure_jit_spill(GLWASM_Runtime *runtime, uint32_t function,
    size_t depth) {
  if (!runtime || function >= runtime->module->nfuncs || depth == SIZE_MAX)
    return 0;
  size_t needed = depth + 1u;
  if (needed > runtime->jit_spill_count) {
    if (needed > SIZE_MAX / sizeof(*runtime->jit_spills) ||
        needed > SIZE_MAX / sizeof(*runtime->jit_spill_capacities) ||
        needed > SIZE_MAX / sizeof(*runtime->jit_deopt_cells)) return 0;
    uint64_t **spills = gcu_allocator_calloc(&runtime->module->alloc,
        needed, sizeof(*spills));
    size_t *capacities = gcu_allocator_calloc(&runtime->module->alloc,
        needed, sizeof(*capacities));
    size_t *deopt_cells = gcu_allocator_calloc(&runtime->module->alloc,
        needed, sizeof(*deopt_cells));
    if (!spills || !capacities || !deopt_cells) {
      gcu_allocator_free(&runtime->module->alloc, spills);
      gcu_allocator_free(&runtime->module->alloc, capacities);
      gcu_allocator_free(&runtime->module->alloc, deopt_cells);
      return 0;
    }
    if (runtime->jit_spill_count) {
      memcpy(spills, runtime->jit_spills,
          runtime->jit_spill_count * sizeof(*spills));
      memcpy(capacities, runtime->jit_spill_capacities,
          runtime->jit_spill_count * sizeof(*capacities));
      memcpy(deopt_cells, runtime->jit_deopt_cells,
          runtime->jit_spill_count * sizeof(*deopt_cells));
    }
    gcu_allocator_free(&runtime->module->alloc, runtime->jit_spills);
    gcu_allocator_free(&runtime->module->alloc,
        runtime->jit_spill_capacities);
    gcu_allocator_free(&runtime->module->alloc, runtime->jit_deopt_cells);
    runtime->jit_spills = spills;
    runtime->jit_spill_capacities = capacities;
    runtime->jit_deopt_cells = deopt_cells;
    runtime->jit_spill_count = needed;
  }
  size_t words = glwasm_jit_spill_words(&runtime->module->funcs[function]);
  size_t result_words = 0;
  for (uint32_t i = 0; i < runtime->module->ntypes; ++i)
    if (runtime->module->types[i].nresults > result_words)
      result_words = runtime->module->types[i].nresults;
  if (words > SIZE_MAX - result_words) return 0;
  words += result_words;
  if (words > SIZE_MAX / sizeof(uint64_t)) return 0;
  if (runtime->jit_spill_capacities[depth] < words) {
    uint64_t *storage = gcu_allocator_calloc(&runtime->module->alloc, words,
        sizeof(*storage));
    if (!storage) return 0;
    gcu_allocator_free(&runtime->module->alloc, runtime->jit_spills[depth]);
    runtime->jit_spills[depth] = storage;
    runtime->jit_spill_capacities[depth] = words;
  }
  return 1;
}

static GLWASM_Result jit_install_for_runtime(GLWASM_Runtime *runtime,
    uint32_t function_index);
static void jit_clear_runtime_entry(GLWASM_Runtime *runtime,
    uint32_t function);

static int decode_jit_function(GLWASM_Runtime *runtime, uint64_t token,
    uint32_t *function) {
  if (!runtime || !function || (uint32_t)(token >> 32) !=
      runtime->module->debug_id) return 0;
  uint32_t index = (uint32_t)token;
  if (index < runtime->module->nimported_funcs ||
      index >= runtime->module->nfuncs ||
      runtime->module->funcs[index].imported) return 0;
  *function = index;
  return 1;
}

static void jit_record_nested_exit(GLWASM_Runtime *runtime,
    uint32_t function, GLWASM_JitExit exit) {
  if (!runtime || !runtime->module->jit_cache ||
      function >= runtime->module->nfuncs) return;
  if (glwasm_jit_cache_record_entry(runtime->module->jit_cache, function,
      exit)) {
    /* A nested callee has returned to its caller now, so its per-context slot
     * can be retired before the next compiled call observes it. Keep the
     * mapping pinned by any active caller handle until that frame returns. */
    jit_clear_runtime_entry(runtime, function);
    glwasm_jit_cache_finish_retirement(runtime->module->jit_cache, function);
  }
}

static void jit_clear_runtime_entry(GLWASM_Runtime *runtime,
    uint32_t function) {
  GRCORE_EntrySlot **jit_slots=NULL;
  uint8_t *jit_registered=NULL;
  if (!runtime || function >= runtime->module->nfuncs ||
      !jit_state_for_context(runtime,runtime->store->run.ctx,&jit_slots,
          &jit_registered,NULL)||!jit_registered||
      !jit_registered[function]||!jit_slots||!jit_slots[function])return;
  GRCORE_Code *code = grcore_entry_slot_code(jit_slots[function]);
  const uintptr_t address = code
      ? (uintptr_t)grjit_code_address(glwasm_jit_artifact_code(code)) : 0;
  (void)grcore_entry_slot_clear(runtime->store->run.ctx,
      jit_slots[function]);
  if (address)
    (void)grcore_code_unregister(runtime->store->run.ctx, address);
  jit_registered[function] = 0;
}

static void jit_record_active_nested_deopts(GLWASM_Runtime *runtime,
    uint64_t cause) {
  if (!runtime || runtime->jit_call_depth == 0) return;
  GLWASM_JitExit propagated_exit = runtime->store->run.paused || cause == 1u
      ? GLWASM_JIT_EXIT_PAUSE
      : cause == 2u ? GLWASM_JIT_EXIT_LIMIT_UNWIND
      : GLWASM_JIT_EXIT_GUARD_DEOPT;
  GRCORE_Stack *stack = grcore_context_stack(runtime->store->run.ctx);
  GRCORE_FrameRef frame = grcore_stack_top(stack);
  for (size_t depth = runtime->jit_call_depth; depth > 0; --depth) {
    uint64_t *frame_slots_ptr = slots(stack, frame);
    if (!frame_slots_ptr) break;
    uint32_t function = (uint32_t)frame_slots_ptr[H_FUNC];
    /* The named instruction belongs to the frame that executed it. Active
     * compiled callers are reconstructed because of that deopt, but did not
     * themselves take the named exit; account them as guard deopts. */
    GLWASM_JitExit exit = propagated_exit;
    if (exit == GLWASM_JIT_EXIT_GUARD_DEOPT && depth ==
        runtime->jit_call_depth && runtime->jit_exit_opcode != UINT16_MAX &&
        runtime->jit_exit_function == function)
      exit = runtime->jit_exit_trap_reconstruction
          ? GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION
          : GLWASM_JIT_EXIT_NAMED_UNSUPPORTED;
    jit_record_nested_exit(runtime, function, exit);
    frame = grcore_stack_caller(stack, frame);
  }
}

static uint32_t wasm_jit_push(void *opaque_context, void *callee_environment,
    uint64_t callee, const uint64_t *args, uint64_t arg_count,
    uint64_t result_count) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime = callee_environment;
  GRCORE_EntrySlot **jit_slots = NULL;
  uint32_t function = 0;
  if (!runtime || runtime->store->run.ctx!=context||active_jit_runtime!=runtime||
      !decode_jit_function(runtime, callee, &function) ||
      !jit_state_for_context(runtime, context, &jit_slots, NULL, NULL)) {
    if (runtime && runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GLWASM_Func *func = &runtime->module->funcs[function];
  GLWASM_TypeDef *type = &runtime->module->types[func->type_index];
  if (arg_count != type->nparams || result_count != type->nresults ||
      (arg_count && !args)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >=
      grcore_context_guest_depth(context)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  if (runtime->jit_call_depth == SIZE_MAX ||
      !ensure_jit_spill(runtime, function, runtime->jit_call_depth + 1u)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  GRCORE_Code *callee_handle = jit_slots[function]
      ? grcore_entry_slot_code(jit_slots[function]) : NULL;
  size_t deopt_cells = jit_code_max_deopt_cells(callee_handle
      ? glwasm_jit_artifact_code(callee_handle) : NULL);
  if (deopt_cells == SIZE_MAX) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  /* Compiled frames are rebuilt as a chain, so reserve each callee's full
   * conversion maximum before its guest frame or any guest effect exists. */
  GLWASM_Result reserved = jit_extend_deopt_reservation(runtime, context,
      deopt_cells);
  if (reserved != GLWASM_OK) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = reserved;
    return 1u;
  }
  size_t callee_depth = runtime->jit_call_depth + 1u;
  runtime->jit_deopt_cells[callee_depth] = deopt_cells;
  GRCORE_Stack *stack = grcore_context_stack(context);
  GRCORE_FrameRef frame;
  if (grcore_stack_push(stack, runtime->store->run.engine, frame_slots(func), &frame) !=
      GRCORE_OK) {
    runtime->jit_deopt_cells[callee_depth] = 0;
    jit_retract_deopt_reservation(runtime, context, deopt_cells);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  uint64_t *slots_ptr = slots(stack, frame);
  if (!slots_ptr) {
    (void)grcore_stack_pop(stack);
    runtime->jit_deopt_cells[callee_depth] = 0;
    jit_retract_deopt_reservation(runtime, context, deopt_cells);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  memset(slots_ptr, 0, frame_slots(func) * sizeof(*slots_ptr));
  slots_ptr[H_FUNC] = function;
  slots_ptr[H_PC] = 0;
  slots_ptr[H_SP] = H_OPERANDS + func->nlocals;
  slots_ptr[H_NLOCALS] = func->nlocals;
  diagnostic_record_frame_entry(&runtime->store->run,function,slots_ptr[H_SP],
      H_OPERANDS+func->nlocals);
  slots_ptr[H_CTRL_DEPTH] = 0;
  slots_ptr[H_INSTANCE] = (uintptr_t)runtime;
  for (uint32_t i = 0; i < func->nparams; ++i)
    slots_ptr[H_OPERANDS + i] = args[i];
  if (wasm_set_guest_frame_identity(stack,frame,runtime->module,function,0)
      !=GRCORE_OK) {
    (void)grcore_stack_pop(stack);
    runtime->jit_deopt_cells[callee_depth] = 0;
    jit_retract_deopt_reservation(runtime, context, deopt_cells);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  ++runtime->jit_call_depth;
  return 0u;
}

static void wasm_jit_pop(void *opaque_context) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime=active_jit_runtime;
  if (!runtime || runtime->store->run.ctx!=context||runtime->jit_call_depth == 0) {
    if (runtime) runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return;
  }
  GRCORE_Stack *stack = grcore_context_stack(context);
  uint64_t *frame_slots_ptr = slots(stack, grcore_stack_top(stack));
  if (!frame_slots_ptr) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return;
  }
  jit_record_nested_exit(runtime, (uint32_t)frame_slots_ptr[H_FUNC],
      GLWASM_JIT_EXIT_RETURN);
  if (!grcore_stack_frame_count(stack) ||
      grcore_stack_pop(stack) != GRCORE_OK) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return;
  }
  size_t deopt_cells = runtime->jit_deopt_cells[runtime->jit_call_depth];
  runtime->jit_deopt_cells[runtime->jit_call_depth] = 0;
  jit_retract_deopt_reservation(runtime, context, deopt_cells);
  --runtime->jit_call_depth;
}

static uint32_t wasm_jit_catch_exception(void *opaque_context,
    void *caller_environment, uint64_t callee, uint32_t landing_block) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime = caller_environment;
  (void)landing_block;
  if (!runtime || runtime != active_jit_runtime ||
      runtime->store->run.ctx != context || !runtime->store->run.pending_exception ||
      runtime->jit_call_depth == 0 ||
      (uint32_t)(callee >> 32) != runtime->module->debug_id) {
    if (runtime) runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GRCORE_Stack *stack = grcore_context_stack(context);
  uint64_t *frame_slots_ptr = slots(stack, grcore_stack_top(stack));
  if (!frame_slots_ptr || frame_slots_ptr[H_FUNC] >= runtime->module->nfuncs ||
      frame_slots_ptr[H_INSTANCE] != (uintptr_t)runtime) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  const uint32_t function = (uint32_t)frame_slots_ptr[H_FUNC];
  /* A tail call may replace the callee named at the original call site.
   * The top guest frame remains the one returned by this native edge. */
  jit_record_nested_exit(runtime, function,
      GLWASM_JIT_EXIT_WASM_EXCEPTION);
  if (grcore_stack_pop(stack) != GRCORE_OK) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  size_t deopt_cells = runtime->jit_deopt_cells[runtime->jit_call_depth];
  runtime->jit_deopt_cells[runtime->jit_call_depth] = 0;
  jit_retract_deopt_reservation(runtime, context, deopt_cells);
  --runtime->jit_call_depth;
  return 0u;
}

static uint32_t wasm_jit_tail(void *opaque_context, void *callee_environment,
    uint64_t callee, const uint64_t *args, uint64_t arg_count,
    uint64_t result_count) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime = callee_environment;
  GRCORE_EntrySlot **jit_slots=NULL;
  uint8_t *jit_registered=NULL;
  uint32_t target = 0;
  if (!runtime||runtime->store->run.ctx!=context||active_jit_runtime!=runtime||
      !decode_jit_function(runtime, callee, &target) ||
      (arg_count && !args) || runtime->jit_call_depth >=
          runtime->jit_spill_count||
      !jit_state_for_context(runtime,context,&jit_slots,&jit_registered,NULL))
    return 1u;
  GRCORE_Stack *stack = grcore_context_stack(context);
  GRCORE_FrameRef caller_ref = grcore_stack_top(stack);
  uint64_t *caller_slots = slots(stack, caller_ref);
  if (!caller_slots) return 1u;
  uint32_t caller_index = (uint32_t)caller_slots[H_FUNC];
  if (caller_index >= runtime->module->nfuncs) return 1u;
  GLWASM_Func *caller = &runtime->module->funcs[caller_index];
  GLWASM_Func *callee_func = &runtime->module->funcs[target];
  if (caller->type_index >= runtime->module->ntypes ||
      callee_func->type_index >= runtime->module->ntypes)
    return 1u;
  GLWASM_TypeDef *caller_type = &runtime->module->types[caller->type_index];
  GLWASM_TypeDef *callee_type = &runtime->module->types[callee_func->type_index];
  if (arg_count != callee_type->nparams ||
      result_count != callee_type->nresults ||
      result_count != caller_type->nresults)
    return 1u;
  GLWASM_Result installed = jit_install_for_runtime(runtime, target);
  if (installed != GLWASM_OK) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = installed;
    return 1u;
  }
  /* Retirement can clear a target's per-context entry while compiled callers
   * still exist. Exit to the interpreter before generated call code loads that
   * empty slot. */
  if (!jit_registered[target] || !jit_slots[target] ||
      jit_slots[target]->entry <= GRCORE_ENTRY_REFUSED) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_UNSUPPORTED;
    return 1u;
  }
  if (!ensure_jit_spill(runtime, target, runtime->jit_call_depth)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  GRCORE_Code *target_handle = grcore_entry_slot_code(jit_slots[target]);
  size_t target_deopt_cells = jit_code_max_deopt_cells(target_handle
      ? glwasm_jit_artifact_code(target_handle) : NULL);
  if (target_deopt_cells == SIZE_MAX) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  /* Tail transfer replaces this frame, but reserve the target's full maximum
   * additively before mutating the stack. Rebuild needs the sum of every active
   * frame's converting locations; the old allowance is retracted on commit. */
  size_t added_deopt_cells = target_deopt_cells;
  size_t caller_slots_count = frame_slots(caller);
  if (caller_slots_count > SIZE_MAX / sizeof(uint64_t)) {
    runtime->store->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  uint64_t *saved = gcu_allocator_calloc(&runtime->module->alloc,
      caller_slots_count, sizeof(*saved));
  if (!saved) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  memcpy(saved, caller_slots, caller_slots_count * sizeof(*saved));
  GRCORE_PollIdentity caller_identity = {0};
  if (grcore_stack_identity(stack, caller_ref, &caller_identity) != GRCORE_OK) {
    gcu_allocator_free(&runtime->module->alloc, saved);
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GLWASM_Result deopt_reserved = jit_extend_deopt_reservation(runtime,
      context, added_deopt_cells);
  if (deopt_reserved != GLWASM_OK) {
    gcu_allocator_free(&runtime->module->alloc, saved);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = deopt_reserved;
    return 1u;
  }
  if (grcore_stack_pop(stack) != GRCORE_OK) {
    jit_retract_deopt_reservation(runtime, context, added_deopt_cells);
    gcu_allocator_free(&runtime->module->alloc, saved);
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GRCORE_FrameRef target_ref;
  GRCORE_Result pushed = grcore_stack_push(stack, runtime->store->run.engine,
      frame_slots(callee_func), &target_ref);
  int target_pushed = pushed == GRCORE_OK;
  if (pushed == GRCORE_OK) {
    uint64_t *target_slots = slots(stack, target_ref);
    if (target_slots) {
      memset(target_slots, 0, frame_slots(callee_func) * sizeof(*target_slots));
      target_slots[H_FUNC] = target;
      target_slots[H_PC] = 0;
      target_slots[H_SP] = H_OPERANDS + callee_func->nlocals;
      target_slots[H_NLOCALS] = callee_func->nlocals;
      target_slots[H_CTRL_DEPTH] = 0;
      target_slots[H_INSTANCE] = (uintptr_t)runtime;
      diagnostic_record_frame_entry(&runtime->store->run,target,
          target_slots[H_SP],H_OPERANDS+callee_func->nlocals);
      for (uint32_t i = 0; i < callee_func->nparams; ++i)
        target_slots[H_OPERANDS + i] = args[i];
      pushed = wasm_set_guest_frame_identity(stack,target_ref,runtime->module,
          target,0);
    } else pushed = GRCORE_ERR_INVALID;
  }
  if (pushed != GRCORE_OK) {
    if (target_pushed) (void)grcore_stack_pop(stack);
    GRCORE_FrameRef restored_ref;
    GRCORE_Result restored = grcore_stack_push(stack, runtime->store->run.engine,
        caller_slots_count, &restored_ref);
    if (restored == GRCORE_OK) {
      uint64_t *restored_slots = slots(stack, restored_ref);
      if (!restored_slots || wasm_set_guest_frame_identity(stack,restored_ref,
          runtime->module,(uint32_t)caller_identity.function,
          caller_identity.offset)!=GRCORE_OK)
        restored = GRCORE_ERR_INVALID;
      else memcpy(restored_slots, saved,
          caller_slots_count * sizeof(*restored_slots));
    }
    jit_retract_deopt_reservation(runtime, context, added_deopt_cells);
    gcu_allocator_free(&runtime->module->alloc, saved);
    if (restored != GRCORE_OK) runtime->store->run.error = GLWASM_ERR_INTERNAL;
    else if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->store->run.error = map_core_result(pushed);
    return 1u;
  }
  gcu_allocator_free(&runtime->module->alloc, saved);
  /* Tail transfer replaces this guest frame. Extend before touching the
   * stack, then retire the old frame's allowance only after replacement has
   * committed. This also handles a smaller target exactly. */
  if (runtime->jit_call_depth) {
    size_t old_cells = runtime->jit_deopt_cells[runtime->jit_call_depth];
    runtime->jit_deopt_cells[runtime->jit_call_depth] = target_deopt_cells;
    jit_retract_deopt_reservation(runtime, context, old_cells);
  } else {
    size_t old_cells = runtime->jit_root_deopt_cells;
    runtime->jit_root_deopt_cells = target_deopt_cells;
    jit_retract_deopt_reservation(runtime, context, old_cells);
  }
  /* A committed tail transfer completes the caller's compiled entry. Count it
   * as a normal completion; a later deopt in the target is attributed to the
   * target frame that remains active. */
  jit_record_nested_exit(runtime, caller_index, GLWASM_JIT_EXIT_RETURN);
  return 0u;
}

static uint32_t wasm_jit_compile(void *opaque_context, uint64_t callee) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime=active_jit_runtime;
  GRCORE_EntrySlot **jit_slots=NULL;
  uint32_t function = 0;
  if(!runtime||runtime->store->run.ctx!=context||
      !decode_jit_function(runtime,callee,&function)||
      !jit_state_for_context(runtime,context,&jit_slots,NULL,NULL))return 1u;
  GLWASM_Result result = jit_install_for_runtime(runtime, function);
  if (result == GLWASM_OK && jit_slots[function]->entry >
      GRCORE_ENTRY_REFUSED) return 0u;
  if (result == GLWASM_OK && runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE) {
    GLWASM_JitCacheStats stats;
    runtime->store->run.error = glwasm_jit_cache_stats(runtime->module->jit_cache,
        function, &stats) &&
        stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT]
        ? GLWASM_ERR_LIMIT : GLWASM_ERR_UNSUPPORTED;
  }
  if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
    if (result != GLWASM_OK) runtime->store->run.error = result;
  return 1u;
}

static GLWASM_Result jit_install_for_runtime(GLWASM_Runtime *runtime,
    uint32_t function_index) {
  GLWASM_Module *module = runtime->module;
  GRCORE_Context *context=runtime->store->run.ctx;
  GRCORE_EngineId engine=runtime->store->run.engine;
  GRCORE_EntrySlot **jit_slots=NULL;
  uint8_t *jit_registered=NULL;
  if (!module->jit_cache ||
      !jit_state_for_context(runtime,context,&jit_slots,&jit_registered,NULL)||
      !jit_slots||!jit_registered||
      function_index < module->nimported_funcs ||
      function_index >= module->nfuncs) return GLWASM_ERR_UNSUPPORTED;
  GLWASM_JitCacheStats current_stats;
  if (glwasm_jit_cache_stats(module->jit_cache, function_index,
      &current_stats) && current_stats.interpreter_only) {
    jit_clear_runtime_entry(runtime, function_index);
    glwasm_jit_cache_finish_retirement(module->jit_cache, function_index);
    return GLWASM_OK;
  }
  if (jit_registered[function_index]) return GLWASM_OK;
  GLWASM_JitRefusal refusal = GLWASM_JIT_REFUSAL_NONE;
  GLWASM_JitCompileDecision decision = glwasm_jit_cache_begin(
      module->jit_cache, function_index, &refusal);
  if (decision == GLWASM_JIT_COMPILE_OWNER) {
    GRJIT_Code *jit_code = NULL;
    uint16_t unsupported_opcode = UINT16_MAX;
    runtime->store->run.last_jit_lower_pc = UINT32_MAX;
    runtime->store->run.last_jit_lower_raw_result = UINT32_MAX;
    runtime->store->run.last_jit_lower_opcode = UINT16_MAX;
    runtime->store->run.last_jit_lower_stack_count = UINT32_MAX;
    runtime->store->run.last_jit_lower_site = UINT32_MAX;
    GLWASM_Result compiled = glwasm_jit_lower_function(module,
        function_index, module->jit_pages, (GRJIT_CallHooks *)&wasm_jit_hooks,
        wasm_jit_poll, (uintptr_t)wasm_jit_charge,
        (uintptr_t)wasm_jit_named_exit, (uintptr_t)wasm_jit_trap_exit,
        (uintptr_t)wasm_jit_spill_base,
        (uintptr_t)wasm_jit_memory_access,
        (uintptr_t)wasm_jit_global_get,
        (uintptr_t)wasm_jit_global_set,
        (uintptr_t)wasm_jit_helper_succeeded,
        (uintptr_t)wasm_jit_slot_entry,
        (uintptr_t)wasm_jit_indirect_target,
        (uintptr_t)wasm_jit_host_call,
        (uintptr_t)wasm_jit_table_bulk,
        (uintptr_t)wasm_jit_memory_bulk,
        (uintptr_t)wasm_jit_take_exception,
        (uintptr_t)wasm_jit_raise_tag,
        (uintptr_t)wasm_jit_raise_ref,
        (uintptr_t)wasm_jit_funcref,
        &jit_code,
        &unsupported_opcode, &runtime->store->run.last_jit_lower_pc,
        &runtime->store->run.last_jit_lower_raw_result,
        &runtime->store->run.last_jit_lower_opcode,
        &runtime->store->run.last_jit_lower_stack_count,
        &runtime->store->run.last_jit_lower_site);
    runtime->last_jit_compile_result = compiled;
    runtime->last_jit_compile_attempted = 1;
    if (compiled == GLWASM_OK) {
      GRCORE_Code *handle = NULL;
      compiled = glwasm_jit_artifact_create(jit_code, module->jit_pages,
          &handle);
      if (compiled == GLWASM_OK) {
        jit_code = NULL; /* the artifact owns the mapping now */
        if (glwasm_jit_cache_publish(module->jit_cache, function_index,
            handle) == GLWASM_OK) {
          decision = GLWASM_JIT_COMPILE_READY;
        } else {
          grcore_code_release(handle);
          compiled = GLWASM_ERR_INTERNAL;
        }
      }
      if (jit_code) grjit_code_destroy(jit_code);
    }
    if (compiled != GLWASM_OK) {
      if (compiled == GLWASM_ERR_UNSUPPORTED) {
        if (unsupported_opcode <= UINT8_MAX)
          glwasm_jit_cache_note_unsupported_opcode(module->jit_cache,
              function_index, unsupported_opcode);
        else
          glwasm_jit_cache_note_unsupported_signature(module->jit_cache,
              function_index);
      }
      GLWASM_JitRefusal why = compiled == GLWASM_ERR_UNSUPPORTED
          ? GLWASM_JIT_REFUSAL_UNSUPPORTED_IR
          : compiled == GLWASM_ERR_LIMIT || compiled == GLWASM_ERR_OOM
          ? GLWASM_JIT_REFUSAL_RESOURCE_LIMIT
          : GLWASM_JIT_REFUSAL_BACKEND_UNAVAILABLE;
      (void)glwasm_jit_cache_refuse(module->jit_cache, function_index, why);
      (void)grcore_entry_slot_refuse(context,jit_slots[function_index]);
      return GLWASM_OK; /* interpreter is the specified refusal path */
    }
  } else if (decision == GLWASM_JIT_COMPILE_BUSY) {
    return GLWASM_OK;
  } else if (decision == GLWASM_JIT_COMPILE_INTERPRET) {
    (void)grcore_entry_slot_refuse(context,jit_slots[function_index]);
    return GLWASM_OK;
  } else if (decision != GLWASM_JIT_COMPILE_READY) {
    return GLWASM_ERR_INTERNAL;
  }

  GRCORE_Code *handle = glwasm_jit_cache_retain_code(module->jit_cache,
      function_index);
  if (!handle) return GLWASM_ERR_INTERNAL;
  GRJIT_Code *jit_code = glwasm_jit_artifact_code(handle);
  const void *address = grjit_code_address(jit_code);
  GRCORE_Result registered = grcore_code_register(context,
      engine, handle, (uintptr_t)address, grjit_code_size(jit_code),
      grjit_code_meta(jit_code));
  if (registered == GRCORE_OK) {
    GRJIT_Result installed = grjit_entry_slot_install(context,
        jit_slots[function_index], handle, jit_code,
        grjit_code_token(jit_code), grjit_code_param_count(jit_code));
    if (installed == GRJIT_OK) jit_registered[function_index] = 1;
    else {
      (void)grcore_code_unregister(context, (uintptr_t)address);
      registered = installed == GRJIT_ERR_OOM ? GRCORE_ERR_OOM
          : installed == GRJIT_ERR_LIMIT ? GRCORE_ERR_LIMIT : GRCORE_ERR_INVALID;
    }
  }
  grcore_code_release(handle);
  return registered == GRCORE_OK ? GLWASM_OK : map_core_result(registered);
}

static GLWASM_Result jit_try_enter(GLWASM_Runtime *runtime,
    uint32_t function_index, const uint64_t *arguments, uint64_t *results,
    uint32_t result_capacity, uint32_t *result_count, int *entered,
    int *returned) {
  *entered = 0;
  *returned = 0;
  if (runtime->jit_mode == GLWASM_TEST_JIT_INTERPRETER) return GLWASM_OK;
  /* With no native stack budget there is nothing to bound compiled
   * recursion against, so nothing is entered compiled (the policy says so). */
  if (grcore_context_native_stack_bytes(runtime->store->run.ctx) == GRCORE_UNLIMITED)
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_UNSUPPORTED : GLWASM_OK;
  GRCORE_EntrySlot **jit_slots=NULL;
  if (!jit_state_for_context(runtime,runtime->store->run.ctx,&jit_slots,NULL,NULL)||
      (runtime->store->run.ctx==runtime->store->context&&
       runtime->store->run.engine!=runtime->store->engine)||
      (runtime->store->run.ctx==runtime->store->boundary_context&&
       runtime->store->run.engine!=runtime->store->boundary_engine)) {
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_UNSUPPORTED : GLWASM_OK;
  }
  GLWASM_JitCacheStats cache_stats;
  if (glwasm_jit_cache_stats(runtime->module->jit_cache, function_index,
      &cache_stats) && cache_stats.interpreter_only) {
    jit_clear_runtime_entry(runtime, function_index);
    glwasm_jit_cache_finish_retirement(runtime->module->jit_cache,
        function_index);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_UNSUPPORTED : GLWASM_OK;
  }
  GLWASM_Result prepared = jit_install_for_runtime(runtime, function_index);
  if (prepared != GLWASM_OK) return prepared;
  if (!jit_slots||!jit_slots[function_index]||
      jit_slots[function_index]->entry <= GRCORE_ENTRY_REFUSED) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE) {
      GLWASM_JitCacheStats stats;
      if (glwasm_jit_cache_stats(runtime->module->jit_cache,
          function_index, &stats) &&
          stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT])
        return GLWASM_ERR_LIMIT;
      return GLWASM_ERR_UNSUPPORTED;
    }
    return prepared;
  }
  GLWASM_Module *module = runtime->module;
  GRCORE_Code *handle = grcore_code_retain(
      grcore_entry_slot_code(jit_slots[function_index]));
  if (!handle) return GLWASM_ERR_INTERNAL;
  GRJIT_Code *code = glwasm_jit_artifact_code(handle);
  size_t code_results = grjit_code_result_count(code);
  if (code_results > UINT32_MAX || code_results > result_capacity ||
      (code_results && !results)) {
    grcore_code_release(handle);
    return GLWASM_ERR_LIMIT;
  }
  GRCORE_Stack *stack = grcore_context_stack(runtime->store->run.ctx);
  runtime->jit_call_depth = 0;
  runtime->jit_root_deopt_cells = 0;
  if (!ensure_jit_spill(runtime, function_index, 0)) {
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_OOM : GLWASM_OK;
  }
  size_t root_deopt_cells = jit_code_max_deopt_cells(code);
  if (root_deopt_cells == SIZE_MAX) {
    grcore_code_release(handle);
    return GLWASM_ERR_INTERNAL;
  }
  GLWASM_Result deopt_reserved = jit_extend_deopt_reservation(runtime,
      runtime->store->run.ctx, root_deopt_cells);
  if (deopt_reserved != GLWASM_OK) {
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? deopt_reserved : GLWASM_OK;
  }
  runtime->jit_root_deopt_cells = root_deopt_cells;
  GRCORE_ActivationRef activation = {0};
  runtime->jit_exit_function = UINT32_MAX;
  runtime->jit_exit_opcode = UINT16_MAX;
  runtime->jit_exit_trap_reconstruction = 0;
  GRCORE_Result cr = grcore_activation_enter(stack, GRCORE_ACTIVATION_JIT,
      runtime->store->run.engine, false, NULL, &activation);
  if (cr != GRCORE_OK) {
    jit_release_compiled_reservations(runtime, runtime->store->run.ctx);
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? map_core_result(cr) : GLWASM_OK;
  }
  size_t words = grjit_code_out_words(code);
  if (!words || words > SIZE_MAX / sizeof(uint64_t)) {
    (void)grcore_activation_leave(stack, activation);
    jit_release_compiled_reservations(runtime, runtime->store->run.ctx);
    grcore_code_release(handle);
    return GLWASM_ERR_INTERNAL;
  }
  uint64_t *out = gcu_allocator_calloc(&runtime->module->alloc, words,
      sizeof(*out));
  if (!out) {
    (void)grcore_activation_leave(stack, activation);
    jit_release_compiled_reservations(runtime, runtime->store->run.ctx);
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_OOM : GLWASM_OK;
  }
  GLWASM_Runtime *previous_jit_runtime=active_jit_runtime;
  active_jit_runtime=runtime;
  uint32_t exit = grjit_code_call(code, runtime->store->run.ctx, runtime,
      arguments, out);
  active_jit_runtime=previous_jit_runtime;
  if (exit == GRJIT_EXIT_DEOPT) {
    /* Capture the rebuilt guest frame before activation cleanup or a later
     * interpreter step overwrites the useful origin. This is diagnostic
     * state only; it never changes execution or cache policy. */
    GLWASM_TestDeoptOrigin origin = {0};
    origin.cause = out[0];
    origin.function = UINT32_MAX;
    origin.pc = UINT32_MAX;
    origin.named_function = runtime->jit_exit_function;
    origin.named_opcode = runtime->jit_exit_opcode;
    origin.compiled_depth = (uint32_t)runtime->jit_call_depth;
    uint64_t *top_slots = slots(stack, grcore_stack_top(stack));
    if (top_slots && top_slots[H_FUNC] < module->nfuncs &&
        top_slots[H_PC] <= module->funcs[top_slots[H_FUNC]].nins) {
      origin.function = (uint32_t)top_slots[H_FUNC];
      origin.pc = (uint32_t)top_slots[H_PC];
      origin.has_frame = 1;
    }
    if (runtime->test_deopt_count == 0)
      runtime->test_first_deopt = origin;
    runtime->test_last_deopt = origin;
    ++runtime->test_deopt_count;
  }
  int had_nested_compiled_frames = runtime->jit_call_depth != 0;
  /* A root return_call replaces the guest frame without increasing the native
   * call depth. Attribute the final exit to the function left in that frame,
   * including when an inner compiled callee is still active at deopt. */
  uint32_t root_exit_function = function_index;
  GRCORE_FrameRef root_frame = grcore_stack_top(stack);
  for (size_t depth = runtime->jit_call_depth; depth > 0; --depth)
    root_frame = grcore_stack_caller(stack, root_frame);
  uint64_t *root_slots = slots(stack, root_frame);
  if (root_slots && root_slots[H_FUNC] < module->nfuncs)
    root_exit_function = (uint32_t)root_slots[H_FUNC];
  /* A deopt/unwind can bypass the normal compiled-call pop hooks. The entire
   * compiled chain has returned to this boundary, so no lease remains active. */
  jit_release_compiled_reservations(runtime, runtime->store->run.ctx);
  if(exit==GRJIT_EXIT_REBUILD_FAILED){
    /* A failed rebuild leaves compiled frames on the guest stack. The chain
     * has returned permanently, so discard frames created inside this JIT
     * activation before leaving it. finish_run/finish_nested will then unwind
     * the caller's remaining guest frames while preserving run.error. */
    GRCORE_Result unwind_result=grcore_unwind_to_activation(stack,activation,
        NULL);
    if(unwind_result!=GRCORE_OK && runtime->store->run.error==GLWASM_OK)
      runtime->store->run.error=GLWASM_ERR_INTERNAL;
  } else if (exit == GRJIT_EXIT_TRAP && runtime->store->run.error != GLWASM_OK) {
    /* A terminal host failure skips post-call reconstruction. If it occurred
     * in a nested compiled callee, discard its guest frames with the runtime-
     * core unwind contract before closing this activation. */
    GRCORE_Result unwind_result = grcore_unwind_to_activation(stack, activation,
        NULL);
    /* The callback's guest error remains authoritative if cleanup itself
     * reports a problem; finish_run still discards guest frames. */
    (void)unwind_result;
  }
  cr = grcore_activation_leave(stack, activation);
  if (cr != GRCORE_OK) {
    jit_release_compiled_reservations(runtime, runtime->store->run.ctx);
    gcu_allocator_free(&runtime->module->alloc, out);
    grcore_code_release(handle);
    if (runtime->store->run.error == GLWASM_OK)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return runtime->store->run.error;
  }
  grcore_code_release(handle);
  *entered = 1;
  GLWASM_JitExit cache_exit = GLWASM_JIT_EXIT_RETURN;
  if (exit == GRJIT_EXIT_RETURNED) {
    *returned = 1;
    if (code_results) memcpy(results, out, code_results * sizeof(*results));
    *result_count = (uint32_t)code_results;
    cache_exit = GLWASM_JIT_EXIT_RETURN;
  } else if (exit == GRJIT_EXIT_DEOPT) {
    uint64_t cause = out[0];
    if (runtime->store->run.paused || cause == 1u)
      cache_exit = GLWASM_JIT_EXIT_PAUSE;
    else if (cause == 2u) cache_exit = GLWASM_JIT_EXIT_LIMIT_UNWIND;
    else if (!had_nested_compiled_frames &&
        runtime->jit_exit_opcode != UINT16_MAX &&
        runtime->jit_exit_function == root_exit_function) {
      cache_exit = runtime->jit_exit_trap_reconstruction
          ? GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION
          : GLWASM_JIT_EXIT_NAMED_UNSUPPORTED;
    } else cache_exit = GLWASM_JIT_EXIT_GUARD_DEOPT;
    if (cause == 1u) runtime->store->run.paused = 1;
    else if (cause == 2u) runtime->store->run.error = GLWASM_ERR_LIMIT;
  } else if (exit == GRJIT_EXIT_REBUILD_FAILED) {
    /* A host callback can complete with a guest-visible failure after its
     * compiled caller has already become impossible to rebuild. Preserve that
     * authoritative callback status; the failed chain is discarded by
     * finish_run. With no status to preserve, a failed rebuild is internal. */
    if (runtime->store->run.error == GLWASM_OK)
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
  } else if (exit == GRJIT_EXIT_TRAP) {
    if (runtime->store->run.error == GLWASM_OK) {
      runtime->store->run.error = GLWASM_ERR_GUEST;
      cache_exit = GLWASM_JIT_EXIT_WASM_TRAP;
    } else {
      cache_exit = GLWASM_JIT_EXIT_HOST_CALL;
    }
  } else if (exit == GRJIT_EXIT_EXCEPTION) {
    ExceptionValue *exception = runtime->store->run.pending_exception;
    if (!exception) {
      runtime->store->run.error = GLWASM_ERR_INTERNAL;
      cache_exit = GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE;
    } else {
      uint32_t origin_pc = 0;
      uint64_t *origin = slots(stack, grcore_stack_top(stack));
      if (origin && origin[H_FUNC] == root_exit_function &&
          origin[H_PC] <= module->funcs[root_exit_function].nins)
        origin_pc = (uint32_t)origin[H_PC];
      if (!dispatch_exception(&runtime->store->run, exception))
        (void)finish_uncaught_exception(runtime->store->run.ctx, &runtime->store->run,
            root_exit_function, origin_pc, exception);
      cache_exit = GLWASM_JIT_EXIT_WASM_EXCEPTION;
    }
  } else {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    cache_exit = GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE;
  }
  if (glwasm_jit_cache_record_entry(module->jit_cache, root_exit_function,
      cache_exit)) {
    jit_clear_runtime_entry(runtime, root_exit_function);
    glwasm_jit_cache_finish_retirement(module->jit_cache,
        root_exit_function);
  }
  runtime->jit_exit_opcode = UINT16_MAX;
  runtime->jit_exit_trap_reconstruction = 0;
  gcu_allocator_free(&runtime->module->alloc, out);
  return GLWASM_OK;
}

static void wasm_jit_charge(void *environment, uint64_t amount) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime) return;
  GRCORE_Context *context = runtime->store->run.ctx
      ? runtime->store->run.ctx : runtime->store->context;
  (void)grcore_context_charge_fuel(context, amount);
}

static uint32_t wasm_jit_poll(void *opaque_context, uint64_t function,
    uint64_t offset) {
  GRCORE_Context *context = opaque_context;
  GRCORE_Verdict verdict = grcore_stack_poll(context, function, offset);
  if (verdict == GRCORE_VERDICT_PAUSE) return 1u;
  if (verdict == GRCORE_VERDICT_UNWIND) return 2u;
  return 0u;
}

/* A paused store: every store object's contents, every instance's segment
 * state, the run, and runtime-core's frames. It restores into a store whose
 * instances were made from the same modules, in the same order, with the
 * same links (same object counts), so every address means the same. */
struct GLWASM_Snapshot {
  const GCU_Allocator *alloc;
  GLWASM_Module **modules;     /* retained, one per instance, in order */
  uintptr_t *instances;        /* source instance identities, by index */
  uint32_t ninstances, nfuncs, nmemories, ntables, nglobals, ntags;
  GRCORE_Snapshot *core;
  uint8_t **memories;
  uint64_t *memory_bytes, *memory_pages;
  uint64_t **tables;
  uint64_t *table_sizes;
  uint64_t *globals;
  uint8_t **data_dropped;      /* per instance */
  uint32_t **elem_counts;      /* per instance; values are the module's */
  uint32_t entry_instance;
  uint64_t *args;
  uint32_t nargs,entry;
  uint8_t jit_entry_attempted,jit_pending;
  uint32_t jit_pending_function;
  uint64_t result;
  uint32_t result_capacity,result_arity,result_count;
  uint32_t bulk_func,bulk_pc,bulk_op;
  uint64_t bulk_dst,bulk_src,bulk_len,bulk_done,bulk_value;
  int bulk_active;
};

static void free_exceptions(GLWASM_Store *store) {
  ExceptionValue *exception=store?store->exceptions:NULL;
  while(exception) {
    ExceptionValue *next=exception->next;
    gcu_allocator_free(exception->allocator,exception);
    exception=next;
  }
  if(store){store->exceptions=NULL;store->run.pending_exception=NULL;}
}

static GLWASM_HostValueType host_value_type(GLWASM_ValType type) {
  switch(type.kind) {
    case GLWASM_VK_I32:return GLWASM_HOST_I32;
    case GLWASM_VK_I64:return GLWASM_HOST_I64;
    case GLWASM_VK_F32:return GLWASM_HOST_F32;
    case GLWASM_VK_F64:return GLWASM_HOST_F64;
    case GLWASM_VK_REF:
      if(type.heap==GLWASM_HT_FUNC||type.heap==GLWASM_HT_NOFUNC)
        return GLWASM_HOST_FUNCREF;
      if(type.heap==GLWASM_HT_EXTERN||type.heap==GLWASM_HT_NOEXTERN)
        return GLWASM_HOST_EXTERNREF;
      return (GLWASM_HostValueType)0;
    default:return (GLWASM_HostValueType)0;
  }
}

/* Call a host function on behalf of `caller`, the instance whose code made
 * the call. */
static GLWASM_Result call_host(GLWASM_Runtime *caller, const StoreFunc *f,
    const uint64_t *arguments, uint64_t *results, int resume) {
  if(!caller||!f||f->instance||(!f->host.function&&!f->host.function_results))
    return GLWASM_ERR_LINK;
  const GLWASM_TypeDef *type=f->type;
  const HostBinding *binding=&f->host;
  GLWASM_Store *store=caller->store;
  if(type->nresults>1&&!binding->function_results)return GLWASM_ERR_UNSUPPORTED;
  GLWASM_HostCall call={.instance=caller,
      .context=store->run.ctx?store->run.ctx:store->context,
      .engine=store->run.engine?store->run.engine:store->engine,
      .instance_token=caller->boundary_token,
      .has_resume=binding->resume!=NULL||binding->resume_results!=NULL,
      .types=binding->parameter_types,
      .result_types=binding->result_types,
      .arguments=arguments,.argument_count=type->nparams,
      .result_count=type->nresults,
      .result_type=type->nresults==1?host_value_type(type->results[0]):GLWASM_HOST_I32,
      .user=binding->user};
  if(type->nresults==1&&call.result_type==0)return GLWASM_ERR_UNSUPPORTED;
  uint64_t ignored=0;
  uint64_t *output=type->nresults?results:&ignored;
  if(type->nresults&&!output)return GLWASM_ERR_INVALID;
  GRCORE_Stack *stack=grcore_context_stack(call.context);
  GRCORE_ActivationRef activation={0};
  GRCORE_Result cr=grcore_activation_enter(stack,GRCORE_ACTIVATION_NATIVE,
      call.engine,true,NULL,&activation);
  if(cr!=GRCORE_OK)return map_core_result(cr);
  GLWASM_Result result_code;
  if(binding->function_results){
    result_code=resume
        ?(binding->resume_results?binding->resume_results(&call,output):GLWASM_ERR_INVALID)
        :binding->function_results(&call,output);
  }else{
    result_code=resume
        ?(binding->resume?binding->resume(&call,output):GLWASM_ERR_INVALID)
        :binding->function(&call,output);
  }
  cr=grcore_activation_leave(stack,activation);
  if(cr!=GRCORE_OK){
    return map_core_result(cr);
  }
  /* A host callback that returns PAUSED is a resumable state-machine step.
   * Its C activation is closed here; execute() restores the Wasm call operands
   * and retries this callback when the context resumes. */
  if(result_code==GLWASM_ERR_PAUSED && !binding->resume &&
      !binding->resume_results)return GLWASM_ERR_INVALID;
  /* A funcref the host returns must name a function of this store. */
  if(result_code==GLWASM_OK)
    for(uint32_t i=0;i<type->nresults;i++)
      if(type->results[i].kind==GLWASM_VK_REF&&
          (type->results[i].heap==GLWASM_HT_FUNC||
           type->results[i].heap==GLWASM_HT_NOFUNC)&&
          output[i]>store->nfuncs)return GLWASM_ERR_INVALID;
  return result_code;
}

/* A callback's resumable return closes its native activation before yielding.
 * Turn that return into an ordinary runtime-core poll pause so the context and
 * saved Wasm frames share the usual paused-state invariants. */
static GRCORE_Verdict request_host_pause(GRCORE_Context *context,
    const GLWASM_Module *module,uint32_t function,uint64_t offset,
    int has_frame) {
  GRCORE_Port *port=NULL;
  if(grcore_context_port(context,&port)!=GRCORE_OK)
    return GRCORE_VERDICT_UNWIND;
  GRCORE_Result posted=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);
  grcore_port_release(port);
  if(posted!=GRCORE_OK)return GRCORE_VERDICT_UNWIND;
  return has_frame?grcore_stack_poll(context,
      wasm_function_identity(module,function),offset):
      grcore_poll(context,(GRCORE_Location){"wasm",0});
}

/* Successful numeric imports execute inside a registered JIT GC point. On
 * pause, the pre-call frame is reconstructed and the interpreter invokes the
 * registered continuation exactly once. Other host failures resume after the
 * completed callback with run.error set, so the callback is never repeated. */
static uint32_t wasm_jit_host_call(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime) return 2u;
  Run *run = &runtime->store->run;
  if (function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins ||
      runtime->jit_call_depth >= runtime->jit_spill_count ||
      !runtime->jit_spills[runtime->jit_call_depth]) {
    run->error = GLWASM_ERR_INTERNAL;
    return 2u;
  }
  GLWASM_Func *caller = &runtime->module->funcs[function];
  GLWASM_Instr instruction = caller->translated->ins[pc].source;
  if (instruction.op != 0x10 || instruction.a >= runtime->module->nimported_funcs ||
      instruction.a >= runtime->module->nfuncs) {
    run->error = GLWASM_ERR_INTERNAL;
    return 2u;
  }
  GLWASM_Func *callee = &runtime->module->funcs[instruction.a];
  if (!callee->imported || callee->type_index >= runtime->module->ntypes) {
    run->error = GLWASM_ERR_INTERNAL;
    return 2u;
  }
  GLWASM_TypeDef *type = &runtime->module->types[callee->type_index];
  if (argument_base > glwasm_operand_capacity(caller) ||
      type->nparams > glwasm_operand_capacity(caller) - argument_base) {
    run->error = GLWASM_ERR_INTERNAL;
    return 2u;
  }
  uint64_t *spill = runtime->jit_spills[runtime->jit_call_depth];
  uint64_t *arguments = spill + caller->nlocals +
      argument_base;
  uint64_t *result_values = spill + caller->nlocals +
      glwasm_operand_capacity(caller);
  /* Host ABI words carry i32/f32 in their low 32 bits. Native i32/f32
   * stores into reusable spill slots need not clear an earlier high word. */
  for (uint32_t i = 0; i < type->nparams; ++i)
    if (type->params[i].kind == GLWASM_VK_I32 ||
        type->params[i].kind == GLWASM_VK_F32)
      arguments[i] = (uint32_t)arguments[i];
  const StoreFunc *target = instance_func(runtime, instruction.a);
  if (target->instance) {
    /* The import is another instance's Wasm function. Compiled code runs
     * one instance only, so it leaves at the call (a named exit) and the
     * interpreter makes the call as an ordinary frame push. The pre-call
     * state is rebuilt; nothing has happened yet. */
    (void)wasm_jit_record_exit(runtime, function, instruction.op, 0);
    return 1u;
  }
  int resume = run->pending_host &&
      run->pending_host_function == instruction.a;
  GLWASM_Result called = call_host(runtime, target, arguments,
      result_values, resume);
  if (called == GLWASM_OK) {
    run->pending_host = 0;
    return 0u;
  }
  if (called == GLWASM_ERR_PAUSED) {
    run->pending_host = 1;
    run->pending_host_function = instruction.a;
    GRCORE_Verdict verdict = request_host_pause(runtime->store->run.ctx,
        runtime->module, (uint32_t)function,
        caller->translated->offsets[pc], 1);
    if (verdict == GRCORE_VERDICT_PAUSE) {
      run->paused = 1;
      return 1u;
    }
    run->error = GLWASM_ERR_LIMIT;
    return 2u;
  }
  run->error = called;
  return 2u;
}

static uint64_t load_le(const uint8_t *p, size_t n) {
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v |= (uint64_t)p[i] << (8u * i);
  return v;
}

static void store_le(uint8_t *p, size_t n, uint64_t v) {
  for (size_t i = 0; i < n; ++i) p[i] = (uint8_t)(v >> (8u * i));
}

static uint64_t sign_extend(uint64_t value, unsigned bits) {
  const uint64_t sign = UINT64_C(1) << (bits - 1u);
  const uint64_t mask = (UINT64_C(1) << bits) - 1u;
  value &= mask;
  return (value ^ sign) - sign;
}

static size_t memory_access_size(uint16_t op) {
  switch (op) {
    case 0x28: case 0x2a: case 0x36: case 0x38: return 4;
    case 0x29: case 0x2b: case 0x37: case 0x39: return 8;
    case 0x2c: case 0x2d: case 0x30: case 0x31:
    case 0x3a: case 0x3c: return 1;
    case 0x2e: case 0x2f: case 0x32: case 0x33:
    case 0x3b: case 0x3d: return 2;
    case 0x34: case 0x35: case 0x3e: return 4;
    default: return 0;
  }
}

static StoreMemory *run_memory(const Run *r, uint32_t index) {
  return r->instance->memories[index];
}

/* Address range check against the memory's current length. A hosted
 * memory's length is current only inside a lease. */
static int memory_range(const Run *r, uint32_t index, uint64_t address,
    uint64_t offset, uint64_t width, uint64_t *out) {
  if (index >= r->m->nmems || offset > UINT64_MAX - address) return 0;
  const StoreMemory *memory = run_memory(r, index);
  uint64_t effective = address + offset;
  if (effective > memory->length ||
      (uint64_t)width > memory->length - effective) return 0;
  *out = effective;
  return 1;
}

typedef struct MemoryLease {
  GLWASM_HostMemoryView view;
  StoreMemory *memory;
} MemoryLease;

/* Open a view of a memory. A hosted memory's provider pins its bytes for
 * the lease and its size is re-read and checked against its declared type;
 * a store-owned memory is its own view. */
static int memory_lease(StoreMemory *memory, MemoryLease *lease) {
  memset(lease, 0, sizeof(*lease));
  lease->memory = memory;
  if (!memory->hosted) {
    lease->view.bytes = memory->bytes;
    lease->view.length_bytes = memory->length;
    return 1;
  }
  if (memory->provider.begin(memory->provider.user, &lease->view) != GLWASM_OK)
    return 0;
  uint64_t pages = lease->view.length_bytes / 65536u;
  if ((lease->view.length_bytes && !lease->view.bytes) ||
      lease->view.length_bytes % 65536u ||
      (memory->has_max && pages > memory->max)) {
    memory->provider.end(memory->provider.user, &lease->view);
    memset(lease, 0, sizeof(*lease));
    return 0;
  }
  memory->bytes = lease->view.bytes;
  memory->length = lease->view.length_bytes;
  memory->pages = pages;
  return 1;
}

static int memory_acquire(Run *r, uint32_t index, MemoryLease *lease) {
  if (!r || !lease || index >= r->m->nmems) return 0;
  return memory_lease(run_memory(r, index), lease);
}

static void memory_release(MemoryLease *lease) {
  if (lease && lease->memory && lease->memory->hosted)
    lease->memory->provider.end(lease->memory->provider.user, &lease->view);
  if (lease) memset(lease, 0, sizeof(*lease));
}

static int memory_grow(Run *run, uint32_t index, uint64_t delta,
    uint64_t *old_pages);
static StoreTable *run_table(const Run *r, uint32_t index);
static int table_reference_valid(const Run *r, uint32_t index, uint64_t token);
static int table_object_grow(GLWASM_Store *store, StoreTable *table,
    uint64_t delta, uint64_t value, uint64_t *old_length);

static uint64_t wasm_jit_memory_access(void *environment, uint64_t function,
    uint64_t pc, uint64_t address, uint64_t value) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins) return 0;
  runtime->jit_exit_trap_reconstruction = 0;
  GLWASM_Func *func = &runtime->module->funcs[function];
  const GLWASM_Instr instruction = func->translated->ins[pc].source;
  runtime->jit_helper_status = 1u;
  Run *run = &runtime->store->run;
  if (instruction.op == 0x40u) {
    if (instruction.a >= run->m->nmems ||
        run_memory(run, instruction.a)->hosted)
      return 0;
    uint64_t old_pages = 0;
    int grew = memory_grow(run, instruction.a, address, &old_pages);
    runtime->jit_helper_status = 0u;
    return grew ? old_pages :
        (run->m->mems[instruction.a].addr64 ? UINT64_MAX : UINT32_MAX);
  }
  if (instruction.op == 0x3fu) { /* memory.size */
    if (instruction.a >= run->m->nmems ||
        run_memory(run, instruction.a)->hosted)
      return 0;
    runtime->jit_helper_status = 0u;
    return run_memory(run, instruction.a)->pages;
  }
  if (instruction.op == 0xfc09u || instruction.op == 0xfc0du) {
    /* data.drop / elem.drop */
    if (instruction.op == 0xfc09u) {
      if (instruction.a >= run->m->ndatas) return 0;
      if (run->instance->data_dropped)
        run->instance->data_dropped[instruction.a] = 1;
    } else {
      if (instruction.a >= run->m->nelems) return 0;
      elem_drop(run->instance, instruction.a);
    }
    runtime->jit_helper_status = 0u;
    return 0;
  }
  if (instruction.op == 0x25u || instruction.op == 0x26u ||
      instruction.op == 0xfc0fu || instruction.op == 0xfc10u) {
    /* table.get / table.set / table.grow / table.size. `address` is the
     * index or delta; `value` the reference stored or grown with. As for
     * memory, an imported table's callbacks belong to the interpreter. */
    if (instruction.a >= run->m->ntables ||
        run_table(run, instruction.a)->hosted)
      return 0;
    if (!run->m->tables[instruction.a].addr64) address = (uint32_t)address;
    if (instruction.op == 0xfc0fu) {
      if (!table_reference_valid(run, instruction.a, value)) {
        runtime->jit_exit_trap_reconstruction = 1;
        return 0;
      }
      uint64_t old = 0;
      int grew = table_object_grow(run->store, run_table(run, instruction.a),
          address, value, &old);
      runtime->jit_helper_status = 0u;
      return grew ? old :
          (run->m->tables[instruction.a].addr64 ? UINT64_MAX : UINT32_MAX);
    }
    /* A table the store holds is its own view; no lease is needed. */
    StoreTable *table = run_table(run, instruction.a);
    uint64_t result = 0;
    if (instruction.op == 0xfc10u) result = table->length;
    else if (address >= table->length) {
      runtime->jit_exit_trap_reconstruction = 1;
      return 0;
    } else if (instruction.op == 0x25u) {
      result = table->elements[address];
      if (!table_reference_valid(run, instruction.a, result)) {
        runtime->jit_exit_trap_reconstruction = 1;
        return 0;
      }
    } else {
      if (!table_reference_valid(run, instruction.a, value)) {
        runtime->jit_exit_trap_reconstruction = 1;
        return 0;
      }
      table->elements[address] = value;
    }
    runtime->jit_helper_status = 0u;
    return result;
  }
  const size_t width = memory_access_size(instruction.op);
  MemoryLease lease;
  uint64_t effective = 0;
  /* An imported provider's begin/end callbacks may be observable. Avoid
   * probing them before a guard that would replay the instruction. */
  if (instruction.b >= run->m->nmems ||
      run_memory(run, instruction.b)->hosted)
    return 0;
  if (!width || !memory_acquire(run, instruction.b, &lease)) return 0;
  if (!memory_range(run, instruction.b, address, instruction.c, width,
      &effective)) {
    runtime->jit_exit_trap_reconstruction = 1;
    memory_release(&lease);
    return 0;
  }
  uint64_t result = 0;
  if (instruction.op >= 0x28 && instruction.op <= 0x35) {
    result = load_le(lease.view.bytes + effective, width);
    if ((instruction.op == 0x2c || instruction.op == 0x2e ||
         instruction.op == 0x30 || instruction.op == 0x32 ||
         instruction.op == 0x34) && width < 8u)
      result = sign_extend(result, (unsigned)(width * 8u));
  } else {
    store_le(lease.view.bytes + effective, width, value);
  }
  memory_release(&lease);
  runtime->jit_helper_status = 0u;
  return result;
}

/* One interpreter-sized memory.init/copy/fill chunk. The compiled caller polls
 * after every non-final chunk; bulk_active and bulk_done let a paused or
 * deoptimized frame continue at this instruction without replaying effects.
 * Results: 0 complete, 1 pre-effect fallback, 2 poll then continue,
 * 3 post-effect fallback through the saved bulk continuation. */
static uint32_t wasm_jit_memory_bulk(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base) {
  GLWASM_Runtime *runtime = environment;
  if (runtime) runtime->jit_exit_trap_reconstruction = 0;
  if (!runtime || function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins ||
      runtime->jit_call_depth >= runtime->jit_spill_count ||
      !runtime->jit_spills[runtime->jit_call_depth]) return 1u;
  GLWASM_Func *func = &runtime->module->funcs[function];
  GLWASM_Instr instruction = func->translated->ins[pc].source;
  uint32_t sub = instruction.op & 0xffu;
  /* memory.init names its data segment first and its memory second. */
  if ((instruction.op != 0xfc08u && instruction.op != 0xfc0au &&
       instruction.op != 0xfc0bu) ||
      (sub == 8u ? instruction.b >= runtime->module->nmems ||
          instruction.a >= runtime->module->ndatas
       : instruction.a >= runtime->module->nmems) ||
      (sub == 10u && instruction.b >= runtime->module->nmems)) return 1u;
  size_t capacity = glwasm_operand_capacity(func);
  if (argument_base > capacity || capacity - argument_base < 3u) return 1u;
  uint32_t destination_memory = sub == 8u ? instruction.b : instruction.a;
  uint32_t source_memory = instruction.b;
  Run *run = &runtime->store->run;
  if (run_memory(run, destination_memory)->hosted ||
      (sub == 10u && run_memory(run, source_memory)->hosted))
    return 1u;
  int distinct_source = sub == 10u && destination_memory != source_memory;
  if (!run->bulk_active) {
    uint64_t *spill = runtime->jit_spills[runtime->jit_call_depth];
    uint64_t *operands = spill + func->nlocals;
    uint64_t destination = operands[argument_base];
    uint64_t second = operands[argument_base + 1u];
    uint64_t length = operands[argument_base + 2u];
    /* Native i32 spill stores overwrite only the low word. An earlier i64
     * occupant of the same operand slot may leave nonzero upper bits; Wasm32
     * addresses and lengths are i32 values, so normalize before range checks
     * or saving the bulk continuation. */
    if (!runtime->module->mems[destination_memory].addr64) {
      destination = (uint32_t)destination;
      if (sub == 10u) second = (uint32_t)second;
      length = (uint32_t)length;
    }
    /* memory.init's segment offset and length are i32 for every memory. */
    if (sub == 8u) {
      second = (uint32_t)second;
      length = (uint32_t)length;
    }
    MemoryLease dst_check, src_check;
    if (!memory_acquire(run, destination_memory, &dst_check)) return 1u;
    if (distinct_source && !memory_acquire(run, source_memory, &src_check)) {
      memory_release(&dst_check);
      return 1u;
    }
    uint64_t checked = 0;
    int valid = memory_range(run, destination_memory, destination, 0, length,
        &checked);
    if (sub == 10u)
      valid = valid && memory_range(run, source_memory, second, 0, length,
          &checked);
    if (sub == 8u) {
      const uint64_t data_length =
          runtime->data_dropped && runtime->data_dropped[instruction.a]
          ? 0 : runtime->module->datas[instruction.a].nbytes;
      valid = valid && second <= data_length &&
          length <= data_length - second;
    }
    if (distinct_source) memory_release(&src_check);
    memory_release(&dst_check);
    if (!valid) {
      runtime->jit_exit_trap_reconstruction = 1;
      return 1u;
    }
    run->bulk_func = (uint32_t)function;
    run->bulk_pc = (uint32_t)pc;
    run->bulk_op = sub;
    run->bulk_dst = destination;
    run->bulk_src = sub == 10u || sub == 8u ? second : 0;
    run->bulk_len = length;
    run->bulk_done = 0;
    run->bulk_value = second & 0xffu;
    run->bulk_active = 1;
  } else if (run->bulk_func != function || run->bulk_pc != pc ||
      run->bulk_op != sub) {
    return 3u;
  }
  if (run->bulk_done > run->bulk_len) return 3u;
  if (run->bulk_done == run->bulk_len) {
    run->bulk_active = 0;
    return 0u;
  }
  uint64_t left = run->bulk_len - run->bulk_done;
  size_t chunk = (size_t)(left > 4096u ? 4096u : left);
  uint64_t offset = sub == 10u && run->bulk_dst > run->bulk_src &&
      run->bulk_dst - run->bulk_src < run->bulk_len
      ? run->bulk_len - run->bulk_done - chunk : run->bulk_done;
  MemoryLease dst_lease, src_lease;
  if (!memory_acquire(run, destination_memory, &dst_lease)) return 3u;
  if (distinct_source && !memory_acquire(run, source_memory, &src_lease)) {
    memory_release(&dst_lease);
    return 3u;
  }
  uint64_t dst_offset = 0, src_offset = 0;
  int valid = memory_range(run, destination_memory, run->bulk_dst, offset,
      chunk, &dst_offset);
  if (sub == 10u)
    valid = valid && memory_range(run, source_memory, run->bulk_src, offset,
        chunk, &src_offset);
  if (valid) {
    uint8_t *dst = dst_lease.view.bytes + dst_offset;
    if (sub == 8u) {
      memcpy(dst, runtime->module->datas[instruction.a].bytes +
          run->bulk_src + offset, chunk);
    } else if (sub == 10u) {
      uint8_t *src = (distinct_source ? src_lease.view.bytes :
          dst_lease.view.bytes) + src_offset;
      memmove(dst, src, chunk);
    } else memset(dst, (int)run->bulk_value, chunk);
  }
  if (distinct_source) memory_release(&src_lease);
  memory_release(&dst_lease);
  if (!valid) return 3u;
  run->bulk_done += chunk;
  if (run->bulk_done == run->bulk_len) {
    run->bulk_active = 0;
    return 0u;
  }
  grcore_context_charge_fuel(run->ctx, chunk);
  if (runtime->jit_test_pause_bulk_chunks &&
      --runtime->jit_test_pause_bulk_chunks == 0) {
    GRCORE_Port *port = NULL;
    if (grcore_context_port(run->ctx, &port) != GRCORE_OK) return 3u;
    GRCORE_Result posted = grcore_port_post(port, GRCORE_REQUEST_INTERRUPT);
    grcore_port_release(port);
    if (posted != GRCORE_OK) return 3u;
  }
  return 2u;
}

static uint64_t wasm_jit_funcref(void *environment, uint64_t function) {
  GLWASM_Runtime *runtime = environment;
  return runtime && function < runtime->module->nfuncs
      ? instance_funcref(runtime, (uint32_t)function) : 0;
}

static uint32_t wasm_jit_helper_succeeded(void *environment) {
  GLWASM_Runtime *runtime = environment;
  /* GRJIT guards deopt on zero, so invert the helper's zero-on-success status. */
  return runtime && runtime->jit_helper_status == 0u;
}

static uint64_t global_read(const StoreGlobal *global) {
  return global->host ? *global->host->value : global->value;
}

static void global_write(StoreGlobal *global, uint64_t value) {
  if (global->host) *global->host->value = value;
  else global->value = value;
}

/* Is `token` a value this global may hold? A funcref names a function of
 * the store; a host global vouches for the other references it is given.
 * A store-owned global holds only values that were already valid. */
static int global_value_valid(const GLWASM_Store *store,
    const StoreGlobal *global, uint64_t token) {
  if (global->type.kind != GLWASM_VK_REF || !token) return 1;
  if (global->type.heap == GLWASM_HT_FUNC ||
      global->type.heap == GLWASM_HT_NOFUNC)
    return token <= store->nfuncs;
  if (!global->host) return 1;
  return global->host->validate_token &&
      global->host->validate_token(global->host->user, token) == GLWASM_OK;
}

static uint64_t wasm_jit_global_get(void *environment, uint64_t index) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || index >= runtime->module->nglobals) {
    if (runtime) runtime->jit_helper_status = 1u;
    return 0;
  }
  const StoreGlobal *global = runtime->globals[index];
  const uint64_t value = global_read(global);
  if (!global_value_valid(runtime->store, global, value)) {
    runtime->jit_exit_trap_reconstruction = 1;
    runtime->jit_helper_status = 1u;
    return 0;
  }
  runtime->jit_helper_status = 0u;
  return value;
}

static uint32_t wasm_jit_global_set(void *environment, uint64_t index,
    uint64_t value) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || index >= runtime->module->nglobals ||
      !runtime->globals[index]->mut) {
    if (runtime) runtime->jit_helper_status = 1u;
    return 1u;
  }
  StoreGlobal *global = runtime->globals[index];
  if (!global_value_valid(runtime->store, global, value)) {
    runtime->jit_exit_trap_reconstruction = 1;
    runtime->jit_helper_status = 1u;
    return 1u;
  }
  global_write(global, value);
  runtime->jit_helper_status = 0u;
  return 0u;
}

static uintptr_t wasm_jit_slot_entry(void *environment, uint64_t callee) {
  GLWASM_Runtime *runtime = environment;
  uint32_t function = 0;
  GRCORE_EntrySlot **jit_slots=NULL;
  if (!runtime||!jit_state_for_context(runtime,runtime->store->run.ctx,&jit_slots,
      NULL,NULL)||!jit_slots||
      !decode_jit_function(runtime, callee, &function) ||
      !jit_slots[function]) return 0;
  /* Tail-call pointer operations enter the slot directly and therefore do not
   * pass through runtime-core's ordinary compile-on-call callback. Install the
   * target before exposing its entry word. */
  if (jit_install_for_runtime(runtime, function) != GLWASM_OK) return 0;
  if (!jit_slots[function]->entry ||
      jit_slots[function]->entry <= GRCORE_ENTRY_REFUSED) return 0;
  return (uintptr_t)&jit_slots[function]->entry;
}

static GLWASM_Runtime *debug_frame_runtime(const GRCORE_AbstractFrame *frame) {
  uint64_t value=0;
  if(!frame||grcore_frame_slot(frame,H_INSTANCE,NULL,&value)!=GRCORE_OK)
    return NULL;
  return (GLWASM_Runtime *)(uintptr_t)value;
}
static uint32_t debug_frame_function(const GRCORE_AbstractFrame *frame) {
  uint64_t value=0;
  if(!frame||grcore_frame_slot(frame,H_FUNC,NULL,&value)!=GRCORE_OK)
    return UINT32_MAX;
  return (uint32_t)value;
}
static size_t wasm_scope_count(const GRCORE_AbstractFrame *frame) {
  return debug_frame_runtime(frame)?3u:0u;
}
static GRCORE_Result wasm_scope(const GRCORE_AbstractFrame *frame,
    size_t index,GRCORE_ScopeInfo *out) {
  if(!out||index>=wasm_scope_count(frame))return GRCORE_ERR_INVALID;
  GLWASM_Runtime *runtime=debug_frame_runtime(frame);
  uint64_t value=0;
  if(index==0){
    uint32_t function=debug_frame_function(frame);
    if(function>=runtime->module->nfuncs)
      return GRCORE_ERR_INVALID;
    *out=(GRCORE_ScopeInfo){GRCORE_SCOPE_LOCAL,"Locals",
      runtime->module->funcs[function].nlocals};
  }else if(index==1){
    uint64_t sp=0;
    if(grcore_frame_slot(frame,H_SP,NULL,&sp)!=GRCORE_OK||
        grcore_frame_slot(frame,H_NLOCALS,NULL,&value)!=GRCORE_OK)
      return GRCORE_ERR_INVALID;
    size_t base=H_OPERANDS+(size_t)value;
    *out=(GRCORE_ScopeInfo){GRCORE_SCOPE_LOCAL,"Operand stack",
      sp>=base?(size_t)(sp-base):0};
  }else{
    *out=(GRCORE_ScopeInfo){GRCORE_SCOPE_GLOBAL,"Globals",
      runtime->module->nglobals};
  }
  return GRCORE_OK;
}
static const char *wasm_variable_name(const GLWASM_Module *module,
    uint32_t function,size_t scope,size_t index) {
  static _Thread_local char fallback[32];
  if(scope==0&&function<module->local_names_count){
    GLWASM_LocalNameMap *map=&module->local_names[function];
    if(index<=UINT32_MAX&&map->index_slots&&map->index_capacity){
      uint32_t local=(uint32_t)index;
      uint32_t slot=(local*UINT32_C(2654435761))&
          (map->index_capacity-1u);
      while(map->index_slots[slot]){
        GLWASM_NameEntry *entry=&map->entries[map->index_slots[slot]-1u];
        if(entry->index==local){
          GLWASM_Name *name=&entry->name;
          return name->debug_bytes?name->debug_bytes:name->bytes;
        }
        slot=(slot+1u)&(map->index_capacity-1u);
      }
    }
  }
  if(scope==2){
    for(uint32_t i=0;i<module->nexports;i++)
      if(module->exports[i].kind==GLWASM_EXT_GLOBAL&&
          module->exports[i].index==index)
        return module->exports[i].name.debug_bytes?
            module->exports[i].name.debug_bytes:
            module->exports[i].name.bytes;
  }
  const char *prefix=scope==0?"local":scope==1?"stack":"global";
  (void)snprintf(fallback,sizeof(fallback),"%s[%zu]",prefix,index);
  return fallback;
}
static GRCORE_Result wasm_variable(const GRCORE_AbstractFrame *frame,
    size_t scope,size_t index,GRCORE_Variable *out) {
  if(!out||scope>=3)return GRCORE_ERR_INVALID;
  GLWASM_Runtime *runtime=debug_frame_runtime(frame);
  uint32_t function=debug_frame_function(frame);
  if(!runtime||function>=runtime->module->nfuncs)return GRCORE_ERR_INVALID;
  uint64_t nlocals=0,value=0;
  if(grcore_frame_slot(frame,H_NLOCALS,NULL,&nlocals)!=GRCORE_OK)
    return GRCORE_ERR_INVALID;
  if(scope==0){
    if(index>=nlocals||grcore_frame_slot(frame,H_OPERANDS+index,NULL,&value)!=GRCORE_OK)
      return GRCORE_ERR_INVALID;
  }else if(scope==1){
    uint64_t sp=0;
    if(grcore_frame_slot(frame,H_SP,NULL,&sp)!=GRCORE_OK)return GRCORE_ERR_INVALID;
    size_t base=H_OPERANDS+(size_t)nlocals;
    if(sp<base||index>=sp-base||
        grcore_frame_slot(frame,base+index,NULL,&value)!=GRCORE_OK)
      return GRCORE_ERR_INVALID;
  }else{
    if(index>=runtime->module->nglobals)return GRCORE_ERR_INVALID;
    const StoreGlobal *global=runtime->globals[index];
    value=global_read(global);
    if(!global_value_valid(runtime->store,global,value))
      return GRCORE_ERR_INVALID;
  }
  *out=(GRCORE_Variable){wasm_variable_name(runtime->module,function,scope,index),
      GRCORE_SLOT_RAW,value};
  return GRCORE_OK;
}
static size_t wasm_memory_count(const GRCORE_AbstractFrame *frame) {
  GLWASM_Runtime *runtime=debug_frame_runtime(frame);
  return runtime?runtime->module->nmems:0;
}
static const char *wasm_memory_name(GLWASM_Runtime *runtime,uint32_t index) {
  GLWASM_Module *module=runtime->module;
  for(uint32_t i=0;i<module->nexports;i++)
    if(module->exports[i].kind==GLWASM_EXT_MEM&&
        module->exports[i].index==index)
      return module->exports[i].name.debug_bytes?
          module->exports[i].name.debug_bytes:module->exports[i].name.bytes;
  if(!runtime->memory_debug_names){
    runtime->memory_debug_names=gcu_allocator_calloc(&module->alloc,
        module->nmems,sizeof(*runtime->memory_debug_names));
    if(!runtime->memory_debug_names)return NULL;
  }
  if(!runtime->memory_debug_names[index]){
    char fallback[32];
    int length=snprintf(fallback,sizeof(fallback),"memory[%u]",index);
    if(length<0||(size_t)length>=sizeof(fallback))return NULL;
    runtime->memory_debug_names[index]=gcu_allocator_malloc(&module->alloc,
        (size_t)length+1u);
    if(!runtime->memory_debug_names[index])return NULL;
    memcpy(runtime->memory_debug_names[index],fallback,(size_t)length+1u);
  }
  return runtime->memory_debug_names[index];
}
static GRCORE_Result wasm_memory_info(const GRCORE_AbstractFrame *frame,
    size_t index,GRCORE_MemoryInfo *out) {
  GLWASM_Runtime *runtime=debug_frame_runtime(frame);
  if(!runtime||!out||index>=runtime->module->nmems)return GRCORE_ERR_INVALID;
  MemoryLease lease;
  if(!memory_lease(runtime->memories[index],&lease))return GRCORE_ERR_INVALID;
  const char *name=wasm_memory_name(runtime,(uint32_t)index);
  if(!name){memory_release(&lease);return GRCORE_ERR_OOM;}
  *out=(GRCORE_MemoryInfo){name,lease.view.length_bytes};
  memory_release(&lease);
  return GRCORE_OK;
}
static GRCORE_Result wasm_memory_read(const GRCORE_AbstractFrame *frame,
    size_t index,uint64_t offset,void *buffer,size_t size) {
  GLWASM_Runtime *runtime=debug_frame_runtime(frame);
  if(!runtime||(size&&!buffer)||index>=runtime->module->nmems)
    return GRCORE_ERR_INVALID;
  MemoryLease lease;
  if(!memory_lease(runtime->memories[index],&lease))return GRCORE_ERR_INVALID;
  if(offset>SIZE_MAX||offset>lease.view.length_bytes||
      (uint64_t)size>lease.view.length_bytes-offset){
    memory_release(&lease);return GRCORE_ERR_INVALID;
  }
  if(size)memcpy(buffer,lease.view.bytes+(size_t)offset,size);
  memory_release(&lease);
  return GRCORE_OK;
}

static StoreTable *run_table(const Run *r, uint32_t index) {
  return r->instance->tables[index];
}

typedef struct TableLease {
  GLWASM_HostTableView view;
  StoreTable *table;
} TableLease;

/* Open a view of a table; as for memories, only a hosted table needs one. */
static int table_lease(StoreTable *table, TableLease *lease) {
  memset(lease, 0, sizeof(*lease));
  lease->table = table;
  if (!table->hosted) {
    lease->view.elements = table->elements;
    lease->view.length = table->length;
    return 1;
  }
  if (table->provider.begin(table->provider.user, &lease->view) != GLWASM_OK)
    return 0;
  if ((lease->view.length && !lease->view.elements) ||
      (table->has_max && lease->view.length > table->max)) {
    table->provider.end(table->provider.user, &lease->view);
    memset(lease, 0, sizeof(*lease));
    return 0;
  }
  table->elements = lease->view.elements;
  table->length = lease->view.length;
  return 1;
}

static int table_acquire(Run *r, uint32_t index, TableLease *lease) {
  if (!r || !lease || index >= r->m->ntables) return 0;
  return table_lease(run_table(r, index), lease);
}

static void table_release(TableLease *lease) {
  if (lease && lease->table && lease->table->hosted)
    lease->table->provider.end(lease->table->provider.user, &lease->view);
  if (lease) memset(lease, 0, sizeof(*lease));
}

/* A funcref token: zero, or a store function address plus one. */
static const StoreFunc *funcref_target(const GLWASM_Store *store,
    uint64_t token) {
  return token && token <= store->nfuncs ? store->funcs[token - 1u] : NULL;
}

static uint32_t wasm_jit_indirect_target(void *environment,
    uint64_t function, uint64_t pc, uint64_t table_index) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins) return 0;
  runtime->jit_exit_trap_reconstruction = 0;
  GLWASM_Instr instruction =
      runtime->module->funcs[function].translated->ins[pc].source;
  runtime->jit_helper_status = 1u;
  if ((instruction.op != 0x11 && instruction.op != 0x13) ||
      instruction.a >= runtime->module->ntypes ||
      instruction.b >= runtime->module->ntables) return 0;
  Run *run = &runtime->store->run;
  if (run_table(run, instruction.b)->hosted) return 0;
  TableLease lease;
  if (!table_acquire(run, instruction.b, &lease)) return 0;
  uint32_t target = 0;
  int valid = 0, trap = 1;
  if (table_index < lease.view.length) {
    const StoreFunc *callee = funcref_target(runtime->store,
        lease.view.elements[table_index]);
    /* A store function this instance owns is one it defines. Another
     * instance's or the host's function is called by the interpreter: a
     * named exit, with nothing yet done. */
    if (callee && same_function_type(&runtime->module->types[instruction.a],
        callee->type)) {
      if (callee->instance == runtime) {
        target = callee->index;
        valid = 1;
      } else trap = 0;
    }
  }
  table_release(&lease);
  if (valid) runtime->jit_helper_status = 0u;
  else runtime->jit_exit_trap_reconstruction = (uint8_t)trap;
  return target;
}

/* Is `token` a value this table may hold? As for a global. */
static int table_value_valid(const GLWASM_Store *store,
    const StoreTable *table, uint64_t token) {
  if (table->elem.kind != GLWASM_VK_REF || !token) return 1;
  if (table->elem.heap == GLWASM_HT_FUNC || table->elem.heap == GLWASM_HT_NOFUNC)
    return token <= store->nfuncs;
  if (!table->hosted) return 1;
  return table->provider.validate_token &&
      table->provider.validate_token(table->provider.user, token) == GLWASM_OK;
}

static int table_reference_valid(const Run *r,uint32_t index,uint64_t token) {
  return index < r->m->ntables &&
      table_value_valid(r->store, run_table(r, index), token);
}
/* The common small-table operations used by baseline workloads fit in one
 * interpreter chunk (1024 elements), so this helper performs exactly one
 * pre-polled chunk. Larger operations return before mutation and take the
 * named interpreter exit, which owns their incremental poll continuation. */
static uint32_t wasm_jit_table_bulk(void *environment, uint64_t function,
    uint64_t pc, uint64_t argument_base) {
  GLWASM_Runtime *runtime = environment;
  if (runtime) runtime->jit_exit_trap_reconstruction = 0;
  if (!runtime || function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins ||
      runtime->jit_call_depth >= runtime->jit_spill_count ||
      !runtime->jit_spills[runtime->jit_call_depth]) return 1u;
  GLWASM_Func *func = &runtime->module->funcs[function];
  GLWASM_Instr instruction = func->translated->ins[pc].source;
  uint32_t sub = instruction.op & 0xffu;
  /* table.init names its element segment first and its table second. */
  uint32_t dst_index = sub == 12u ? instruction.b : instruction.a;
  if ((instruction.op != 0xfc0cu && instruction.op != 0xfc0eu &&
       instruction.op != 0xfc11u) ||
      dst_index >= runtime->module->ntables ||
      (sub == 12u && instruction.a >= runtime->module->nelems) ||
      (sub == 14u && instruction.b >= runtime->module->ntables)) return 1u;
  size_t capacity = glwasm_operand_capacity(func);
  if (argument_base > capacity || capacity - argument_base < 3u) return 1u;
  uint64_t *spill = runtime->jit_spills[runtime->jit_call_depth];
  uint64_t *operands = spill + func->nlocals;
  uint64_t destination = operands[argument_base];
  uint64_t source = sub != 17u ? operands[argument_base + 1u] : 0;
  uint64_t length = operands[argument_base + 2u];
  /* Table32 operands are stored through i32 native spills, which may leave
   * upper bits from an earlier i64 value in the same reusable slot. */
  if (!runtime->module->tables[dst_index].addr64) destination =
      (uint32_t)destination;
  if (sub == 12u || !runtime->module->tables[dst_index].addr64)
    length = (uint32_t)length;
  if (sub == 12u || (sub == 14u &&
      !runtime->module->tables[instruction.b].addr64))
    source = (uint32_t)source;
  if (sub == 14u &&
      !same_value_type(runtime->module->tables[instruction.a].elem,
          runtime->module->tables[instruction.b].elem)) return 1u;
  uint64_t value = sub == 17u ? operands[argument_base + 1u] : 0;
  Run *run = &runtime->store->run;
  /* Acquiring imported table views is itself an embedder callback. Leave
   * those operations to the interpreter so a failed guard cannot replay it. */
  if (run_table(run, dst_index)->hosted ||
      (sub == 14u && run_table(run, instruction.b)->hosted))
    return 1u;
  if (sub == 17u && !table_reference_valid(run, dst_index, value))
    return 1u;
  /* Init and copy move a store table's tokens as the interpreter does;
   * fill stores a reference compiled code supplied, so only a function
   * table's. */
  GLWASM_Table *dst_decl = &runtime->module->tables[dst_index];
  if (sub == 17u && ((dst_decl->elem.heap != GLWASM_HT_FUNC &&
      dst_decl->elem.heap != GLWASM_HT_NOFUNC) ||
      value > runtime->store->nfuncs)) return 1u;
  TableLease dst_lease, src_lease;
  if (!table_acquire(run, dst_index, &dst_lease)) return 1u;
  int distinct_source = sub == 14u && instruction.a != instruction.b;
  if (distinct_source && !table_acquire(run, instruction.b, &src_lease)) {
    table_release(&dst_lease);
    return 1u;
  }
  const uint64_t dst_length = dst_lease.view.length;
  const uint64_t src_length = sub == 14u
      ? (distinct_source ? src_lease.view.length : dst_length)
      : sub == 12u ? runtime->elem_counts[instruction.a] : 0;
  int valid = destination <= dst_length && length <= dst_length - destination;
  if (sub != 17u)
    valid = valid && source <= src_length && length <= src_length - source;
  /* An invalid range traps whatever its length; a valid one longer than
   * a chunk leaves its incremental polling to the interpreter. */
  if (!valid) runtime->jit_exit_trap_reconstruction = 1;
  else if (length > 1024u) valid = 0;
  if (valid && length) {
    uint64_t *dst = dst_lease.view.elements + (size_t)destination;
    if (sub == 12u) {
      memcpy(dst, runtime->elem_values[instruction.a] + (size_t)source,
          (size_t)length * sizeof(*dst));
    } else if (sub == 14u) {
      uint64_t *src = (distinct_source ? src_lease.view.elements :
          dst_lease.view.elements) + (size_t)source;
      memmove(dst, src, (size_t)length * sizeof(*dst));
    } else {
      for (uint64_t i = 0; i < length; ++i) dst[i] = value;
    }
  }
  if (distinct_source) table_release(&src_lease);
  table_release(&dst_lease);
  return valid ? 0u : 1u;
}
static int global_reference_valid(const Run *r,uint32_t index,uint64_t token) {
  return index < r->m->nglobals &&
      global_value_valid(r->store, r->instance->globals[index], token);
}

/* Grow a memory by `delta` pages, within its own type, the address space,
 * and `limit_bytes` (the growing module's memory_maximum). The bytes come
 * from the store's budgeted allocator, so the context budget refuses too. */
static int memory_object_grow(GLWASM_Store *store, StoreMemory *memory,
    uint64_t delta, uint64_t limit_bytes, uint64_t *old_pages) {
  const uint64_t page_size = UINT64_C(65536);
  MemoryLease lease;
  if (!memory_lease(memory, &lease)) return 0;
  uint64_t old = memory->pages;
  memory_release(&lease);
  if (old_pages) *old_pages = old;
  if (delta > UINT64_MAX - old) return 0;
  uint64_t next = old + delta;
  const uint64_t address_limit = memory->addr64 ? (UINT64_C(1) << 48u) : 65536u;
  if ((memory->has_max && next > memory->max) || next > address_limit ||
      next > SIZE_MAX / page_size || next > limit_bytes / page_size)
    return 0;
  if (memory->hosted) {
    uint64_t provider_old = 0;
    if (memory->provider.grow(memory->provider.user, delta, &provider_old) !=
        GLWASM_OK || provider_old != old) return 0;
    if (!memory_lease(memory, &lease)) return 0;
    int success = memory->pages == next;
    memory_release(&lease);
    return success;
  }
  uint64_t bytes = next * page_size;
  if (bytes == memory->length) return 1;
  uint8_t *grown = gcu_allocator_calloc(store->guest_alloc, (size_t)bytes, 1);
  if (!grown) return 0;
  if (memory->length) memcpy(grown, memory->bytes, (size_t)memory->length);
  gcu_allocator_free(store->guest_alloc, memory->bytes);
  memory->bytes = grown;
  memory->length = bytes;
  memory->pages = next;
  return 1;
}

static int memory_grow(Run *r, uint32_t index, uint64_t delta,
    uint64_t *old_pages) {
  if (index >= r->m->nmems) return 0;
  return memory_object_grow(r->store, run_memory(r, index), delta,
      r->m->limits.memory_maximum, old_pages);
}

/* Grow a table by `delta` elements set to `value`, within its type and the
 * address space. Returns 0 when the request fails. */
static int table_object_grow(GLWASM_Store *store, StoreTable *table,
    uint64_t delta, uint64_t value, uint64_t *old_length) {
  TableLease lease;
  if (!table_lease(table, &lease)) return 0;
  uint64_t old = table->length;
  table_release(&lease);
  if (old_length) *old_length = old;
  uint64_t next = delta <= UINT64_MAX - old ? old + delta : UINT64_MAX;
  uint64_t address_limit = table->addr64 ? UINT64_MAX : (UINT64_C(1) << 32u);
  if ((table->has_max && next > table->max) || next > address_limit ||
      next > SIZE_MAX / sizeof(uint64_t))
    return 0;
  if (next == old) return 1;
  if (table->hosted) {
    uint64_t provider_old = 0;
    if (table->provider.grow(table->provider.user, delta, value,
        &provider_old) != GLWASM_OK || provider_old != old) return 0;
    if (!table_lease(table, &lease)) return 0;
    int success = table->length == next;
    table_release(&lease);
    return success;
  }
  uint64_t *grown = gcu_allocator_calloc(store->guest_alloc, (size_t)next,
      sizeof(*grown));
  if (!grown) return 0;
  if (old) memcpy(grown, table->elements, (size_t)old * sizeof(*grown));
  for (uint64_t j = old; j < next; j++) grown[j] = value;
  gcu_allocator_free(store->guest_alloc, table->elements);
  table->elements = grown;
  table->length = next;
  return 1;
}

static uint64_t * slots(GRCORE_Stack *s, GRCORE_FrameRef f) { return grcore_stack_slots(s,f); }

static void trap_poll(GRCORE_Context *context, void *value, GRCORE_PollCall *call) {
  Run *r=(Run *)value;
  if(r && (r->trap!=GLWASM_TRAP_NONE||r->error==GLWASM_ERR_GUEST)){
    (void)grcore_pollcall_set_unwind_result(call,GRCORE_ERR_GUEST);
    (void)grcore_pollcall_vote(call,GRCORE_VERDICT_UNWIND);
    return;
  }
  if(r){
    uint64_t fuel_limit=grcore_context_fuel_limit(context);
    uint64_t memory_limit=grcore_context_memory_bytes(context);
    uint64_t memory_used=grcore_context_memory_in_use(context);
    const int fuel_out=fuel_limit!=UINT64_MAX&&
        grcore_context_fuel_remaining(context)==0;
    /* The frame differential's stop: a resumable pause where the fuel ran
     * out, which is the same instruction in both tiers. */
    if(fuel_out&&r->store&&r->store->test_fuel_pauses){
      (void)grcore_pollcall_vote(call,GRCORE_VERDICT_PAUSE);
      return;
    }
    if(fuel_out||(memory_limit!=UINT64_MAX&&memory_used>memory_limit)){
      r->error=GLWASM_ERR_LIMIT;
      (void)grcore_pollcall_set_unwind_result(call,GRCORE_ERR_LIMIT);
      (void)grcore_pollcall_vote(call,GRCORE_VERDICT_UNWIND);
    }
  }
}
static const GRCORE_Key trap_key=GRCORE_KEY_INIT("wasm-trap",
  GRCORE_CARDINALITY_ONE,GRCORE_PHASE_DECIDE,NULL,trap_poll,NULL,NULL,NULL);

static ExceptionValue *find_exception(GLWASM_Runtime *runtime,
    uint64_t reference) {
  if(!runtime||!reference)return NULL;
  for(ExceptionValue *exception=runtime->store->exceptions;exception;
      exception=exception->next)
    if(exception->identity==reference)return exception;
  return NULL;
}

/* A catch clause matches by tag object: the same tag, whichever instance
 * defined, imported or re-exported it. */
static int exception_tag_matches(const Run *run,const ExceptionValue *exception,
    uint32_t tag_index) {
  if(!run||!exception||tag_index>=run->m->ntags)return 0;
  return run->instance->tags[tag_index]==exception->tag;
}

static ExceptionValue *new_exception(Run *run,uint32_t tag_index,
    const uint64_t *payload) {
  if(!run||!run->instance||tag_index>=run->m->ntags)return NULL;
  StoreTag *tag=run->instance->tags[tag_index];
  uint64_t bytes64=(uint64_t)sizeof(ExceptionValue)+
      (uint64_t)tag->nparams*sizeof(uint64_t);
  if(bytes64>SIZE_MAX)return NULL;
  size_t bytes=(size_t)bytes64;
  ExceptionValue *exception=gcu_allocator_calloc(run->guest_alloc,1,bytes);
  if(!exception){
    uint64_t budget=grcore_context_memory_bytes(run->ctx);
    uint64_t used=grcore_context_memory_in_use(run->ctx);
    run->error=budget!=UINT64_MAX&&
        (used>=budget||(uint64_t)bytes>budget-used)
        ?GLWASM_ERR_LIMIT:GLWASM_ERR_OOM;
    return NULL;
  }
  exception->allocator=run->guest_alloc;
  exception->identity=allocate_exception_identity();
  if(!exception->identity){
    gcu_allocator_free(exception->allocator,exception);
    run->error=GLWASM_ERR_LIMIT;
    return NULL;
  }
  exception->tag=tag;
  exception->payload_count=tag->nparams;
  if(tag->nparams)memcpy(exception->payload,payload,
      (size_t)tag->nparams*sizeof(uint64_t));
  exception->next=run->store->exceptions;
  run->store->exceptions=exception;
  return exception;
}

/* Search live try_table labels from the throwing frame outwards. Handled
 * exceptions only edit Wasm frames; the runtime-core unwinder remains reserved
 * for traps and terminal limits. */
static int dispatch_exception(Run *run,ExceptionValue *exception) {
  if(!run||!exception||!run->ctx)return 0;
  GRCORE_Stack *stack=grcore_context_stack(run->ctx);
  while(grcore_stack_frame_count(stack)>run->stack_base){
    GRCORE_FrameRef frame=grcore_stack_top(stack);
    uint64_t *slots_ptr=slots(stack,frame);
    if(!slots_ptr)return 0;
    /* Each frame's handlers and tags are its own instance's. */
    run_bind(run,(GLWASM_Runtime *)(uintptr_t)slots_ptr[H_INSTANCE]);
    uint32_t function_index=(uint32_t)slots_ptr[H_FUNC];
    if(function_index>=run->m->nfuncs)return 0;
    GLWASM_Func *function=&run->m->funcs[function_index];
    uint32_t control_depth=(uint32_t)slots_ptr[H_CTRL_DEPTH];
    for(uint32_t cursor=control_depth;cursor>0;--cursor){
      size_t control=control_base(function)+2u*(cursor-1u);
      uint32_t opener=(uint32_t)slots_ptr[control];
      if(opener>=function->nins||function->ins[opener].op!=0x1f)continue;
      GLWASM_Instr *try_table=&function->ins[opener];
      if(try_table->b>function->ncatches||
          try_table->d>function->ncatches-try_table->b)return 0;
      for(uint32_t i=0;i<try_table->d;i++){
        GLWASM_Catch *clause=&function->catches[try_table->b+i];
        if(!clause->resolved)return 0;
        if(clause->kind<2&&
            !exception_tag_matches(run,exception,clause->tag))continue;

        uint32_t target_depth=0;
        size_t destination=H_OPERANDS+function->nlocals;
        uint32_t target_arity=run->m->types[function->type_index].nresults;
        if(clause->target_opener!=UINT32_MAX){
          int found=0;
          for(uint32_t j=0;j<control_depth;j++){
            size_t target_control=control_base(function)+2u*j;
            if((uint32_t)slots_ptr[target_control]==clause->target_opener){
              destination=(size_t)slots_ptr[target_control+1u];
              target_arity=branch_arity(function,clause->target_opener);
              target_depth=function->ins[clause->target_opener].op==0x03
                  ?j+1u:j;
              found=1;
              break;
            }
          }
          if(!found)return 0;
        }
        uint32_t caught_count=clause->kind==0||clause->kind==1
            ?exception->payload_count:0;
        uint32_t output_count=caught_count+(clause->kind==1||clause->kind==3);
        if(output_count!=target_arity)return 0;
        uint32_t old_sp=(uint32_t)slots_ptr[H_SP];
        if(destination>old_sp)return 0;
        memset(&slots_ptr[destination],0,
            (size_t)(old_sp-destination)*sizeof(*slots_ptr));
        if(caught_count)memcpy(&slots_ptr[destination],exception->payload,
            (size_t)caught_count*sizeof(*slots_ptr));
        if(clause->kind==1||clause->kind==3)
          slots_ptr[destination+caught_count]=exception->identity;
        slots_ptr[H_CTRL_DEPTH]=target_depth;
        slots_ptr[H_PC]=clause->target_pc;
        slots_ptr[H_SP]=(uint32_t)(destination+output_count);
        run->pending_exception=NULL;
        return 1;
      }
    }
    (void)grcore_stack_pop(stack);
  }
  run_bind(run,run->entry_instance);
  run->pending_exception=NULL;
  run->error=GLWASM_ERR_GUEST;
  return 0;
}

static int wasm_jit_exception_access(GLWASM_Runtime *runtime,
    uint64_t function, uint64_t *spill) {
  if (!runtime || runtime != active_jit_runtime ||
      runtime->store->run.ctx == NULL || function >= runtime->module->nfuncs ||
      runtime->jit_call_depth >= runtime->jit_spill_count || !spill ||
      runtime->jit_spills[runtime->jit_call_depth] != spill)
    return 0;
  uint64_t *frame = slots(grcore_context_stack(runtime->store->run.ctx),
      grcore_stack_top(grcore_context_stack(runtime->store->run.ctx)));
  return frame && frame[H_FUNC] == function &&
      frame[H_INSTANCE] == (uintptr_t)runtime;
}

static uint32_t wasm_jit_raise_tag(void *environment, uint64_t function,
    uint64_t tag, uint64_t *spill, uint64_t payload_base) {
  GLWASM_Runtime *runtime = environment;
  if (!wasm_jit_exception_access(runtime, function, spill) ||
      tag >= runtime->module->ntags ||
      runtime->module->tags[tag].type_index >= runtime->module->ntypes) {
    if (runtime) runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  const GLWASM_TypeDef *type = &runtime->module->types[
      runtime->module->tags[tag].type_index];
  size_t capacity = runtime->jit_spill_capacities[runtime->jit_call_depth];
  if (payload_base > capacity || type->nparams > capacity - payload_base) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  ExceptionValue *exception = new_exception(&runtime->store->run, (uint32_t)tag,
      spill + payload_base);
  if (!exception) return 0u;
  for (uint32_t i = 0; i < type->nparams; ++i)
    if (type->params[i].kind == GLWASM_VK_I32 ||
        type->params[i].kind == GLWASM_VK_F32)
      exception->payload[i] &= UINT32_MAX;
  runtime->store->run.pending_exception = exception;
  return 1u;
}

static uint32_t wasm_jit_raise_ref(void *environment, uint64_t identity) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || runtime != active_jit_runtime || !runtime->store->run.ctx)
    return 0u;
  ExceptionValue *exception = find_exception(runtime, identity);
  if (!exception) {
    runtime->store->run.trap = GLWASM_TRAP_INVALID_REFERENCE;
    return 0u;
  }
  runtime->store->run.pending_exception = exception;
  return 1u;
}

static uint32_t wasm_jit_take_exception(void *environment, uint64_t function,
    uint64_t clause_index, uint64_t *spill, uint64_t base, uint64_t arity) {
  GLWASM_Runtime *runtime = environment;
  if (!wasm_jit_exception_access(runtime, function, spill)) {
    if (runtime) runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  GLWASM_Func *func = &runtime->module->funcs[function];
  ExceptionValue *exception = runtime->store->run.pending_exception;
  if (!exception || clause_index >= func->ncatches ||
      base > glwasm_operand_capacity(func) ||
      arity > glwasm_operand_capacity(func) - base ||
      func->nlocals > runtime->jit_spill_capacities[runtime->jit_call_depth] ||
      base + arity > runtime->jit_spill_capacities[
          runtime->jit_call_depth] - func->nlocals) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  GLWASM_Catch *clause = &func->catches[clause_index];
  if (!clause->resolved ||
      (clause->kind < 2u && !exception_tag_matches(&runtime->store->run,
          exception, clause->tag)))
    return 0u;
  const uint32_t caught_count = clause->kind < 2u
      ? exception->payload_count : 0u;
  const uint32_t output_count = caught_count +
      (clause->kind == 1u || clause->kind == 3u);
  if (output_count != arity) {
    runtime->store->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  uint64_t *destination = spill + func->nlocals + base;
  if (caught_count)
    memcpy(destination, exception->payload,
        (size_t)caught_count * sizeof(*destination));
  if (clause->kind == 1u || clause->kind == 3u)
    destination[caught_count] = exception->identity;
  runtime->store->run.pending_exception = NULL;
  return 1u;
}

static GRCORE_Step raise_trap(GRCORE_Context *ctx,Run *r,uint32_t fi,
    uint32_t pc,GLWASM_Trap trap){
  r->trap=trap;
  const GLWASM_Func *fn=&r->m->funcs[fi];
  GRCORE_Verdict v=grcore_stack_poll(ctx,wasm_function_identity(r->m,fi),
      fn->translated->offsets[pc]);
  return v==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:GRCORE_STEP_FINISHED;
}

/* Trapping truncation. The bounds are exclusive on both sides of the
 * representable range after truncation toward zero: -2^31 - 1 < x < 2^31
 * for i32, -1 < x < 2^32 for u32, and so on. Every bound is exact in
 * double, and every f32 input converts to double exactly. */
static int wasm_float_to_integer(double input, unsigned width, int is_signed,
    uint64_t *out) {
  if (__builtin_isnan(input)) return 0;
  if (width == 32u) {
    if (is_signed) {
      if (input <= -2147483649.0 || input >= 2147483648.0) return 0;
      *out = (uint32_t)(int32_t)trunc(input);
    } else {
      if (input <= -1.0 || input >= 4294967296.0) return 0;
      *out = (uint32_t)trunc(input);
    }
  } else if (is_signed) {
    /* -2^63 - 1 is not a double; the next double below -2^63 is out of
     * range, so -2^63 itself is the inclusive lower bound. */
    const double edge = 9223372036854775808.0;
    if (input < -edge || input >= edge) return 0;
    int64_t value = (int64_t)trunc(input);
    memcpy(out, &value, sizeof(value));
  } else {
    const double edge = 18446744073709551616.0;
    if (input <= -1.0 || input >= edge) return 0;
    *out = (uint64_t)trunc(input);
  }
  return 1;
}

static uint64_t wasm_float_to_integer_sat(double input, unsigned width,
    int is_signed) {
  if (__builtin_isnan(input)) return 0;
  if (width == 32u) {
    if (is_signed) {
      if (input <= -2147483648.0) return UINT32_C(0x80000000);
      if (input >= 2147483648.0) return UINT32_C(0x7fffffff);
      return (uint32_t)(int32_t)trunc(input);
    }
    if (input <= 0.0) return 0;
    if (input >= 4294967296.0) return UINT32_MAX;
    return (uint32_t)trunc(input);
  }
  if (is_signed) {
    const double edge = 9223372036854775808.0;
    if (input <= -edge) return UINT64_C(0x8000000000000000);
    if (input >= edge) return UINT64_C(0x7fffffffffffffff);
    int64_t value = (int64_t)trunc(input);
    uint64_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
  }
  const double edge = 18446744073709551616.0;
  if (input <= 0.0) return 0;
  if (input >= edge) return UINT64_MAX;
  return (uint64_t)trunc(input);
}

static double wasm_round_nearest(double value) {
  if (!__builtin_isfinite(value) || value == 0.0) return value;
  double lower = floor(value);
  double fraction = value - lower;
  double rounded = fraction < 0.5 ? lower : fraction > 0.5 ? lower + 1.0 :
      (fmod(lower, 2.0) == 0.0 ? lower : lower + 1.0);
  return rounded == 0.0 ? copysign(0.0, value) : rounded;
}

/* Execute the complete scalar numeric instruction families accepted by the
 * validator. Returning 1 means the opcode was handled; traps are returned via
 * step so the ordinary interpreter unwind path remains authoritative. */
static int wasm_interpret_numeric(GRCORE_Context *ctx, Run *run,
    uint32_t function, uint32_t pc, GLWASM_Instr instruction, uint64_t *stack,
    uint32_t *sp, GRCORE_Step *step) {
  uint16_t op = instruction.op;
  uint64_t a = 0, b = 0, value = 0;
  *step = GRCORE_STEP_FINISHED;
#define NUMERIC_FAIL() do { run->error = GLWASM_ERR_INTERNAL; *step = GRCORE_STEP_FINISHED; return 1; } while (0)
#define NUMERIC_TRAP(kind) do { *step = raise_trap(ctx, run, function, pc, (kind)); return 1; } while (0)
#define NUMERIC_POP2() do { if (*sp < 2u) NUMERIC_FAIL(); b = stack[--*sp]; a = stack[--*sp]; } while (0)
  if (op >= 0x45u && op <= 0x66u) {
    int result = 0;
    if (op == 0x45u || op == 0x50u) {
      if (*sp < 1u) NUMERIC_FAIL();
      a = stack[--*sp]; result = op == 0x45u ? (uint32_t)a == 0 : a == 0;
    } else if (op <= 0x4fu) {
      NUMERIC_POP2();
      uint32_t x = (uint32_t)a, y = (uint32_t)b;
      switch (op) {
        case 0x46: result = x == y; break; case 0x47: result = x != y; break;
        case 0x48: result = wasm_i32(x) < wasm_i32(y); break;
        case 0x49: result = x < y; break;
        case 0x4a: result = wasm_i32(x) > wasm_i32(y); break;
        case 0x4b: result = x > y; break;
        case 0x4c: result = wasm_i32(x) <= wasm_i32(y); break;
        case 0x4d: result = x <= y; break;
        case 0x4e: result = wasm_i32(x) >= wasm_i32(y); break;
        case 0x4f: result = x >= y; break;
        default: NUMERIC_FAIL();
      }
    } else if (op >= 0x51u && op <= 0x5au) {
      NUMERIC_POP2();
      switch (op) {
        case 0x51: result = a == b; break; case 0x52: result = a != b; break;
        case 0x53: result = wasm_i64(a) < wasm_i64(b); break;
        case 0x54: result = a < b; break;
        case 0x55: result = wasm_i64(a) > wasm_i64(b); break;
        case 0x56: result = a > b; break;
        case 0x57: result = wasm_i64(a) <= wasm_i64(b); break;
        case 0x58: result = a <= b; break;
        case 0x59: result = wasm_i64(a) >= wasm_i64(b); break;
        case 0x5a: result = a >= b; break;
        default: NUMERIC_FAIL();
      }
    } else if (op <= 0x60u) {
      NUMERIC_POP2();
      float x, y; uint32_t xb = (uint32_t)a, yb = (uint32_t)b;
      memcpy(&x, &xb, sizeof(x)); memcpy(&y, &yb, sizeof(y));
      switch (op) {
        case 0x5b: result = x == y; break; case 0x5c: result = x != y; break;
        case 0x5d: result = x < y; break; case 0x5e: result = x > y; break;
        case 0x5f: result = x <= y; break; case 0x60: result = x >= y; break;
        default: NUMERIC_FAIL();
      }
    } else {
      NUMERIC_POP2();
      double x, y; memcpy(&x, &a, sizeof(x)); memcpy(&y, &b, sizeof(y));
      switch (op) {
        case 0x61: result = x == y; break; case 0x62: result = x != y; break;
        case 0x63: result = x < y; break; case 0x64: result = x > y; break;
        case 0x65: result = x <= y; break; case 0x66: result = x >= y; break;
        default: NUMERIC_FAIL();
      }
    }
    stack[(*sp)++] = (uint64_t)result; return 1;
  }
  if ((op >= 0x67u && op <= 0x69u) || (op >= 0x79u && op <= 0x7bu)) {
    if (*sp < 1u) NUMERIC_FAIL();
    a = stack[--*sp];
    unsigned width = op < 0x70u ? 32u : 64u;
    uint64_t x = width == 32u ? (uint32_t)a : a;
    if (op == 0x67u || op == 0x79u)
      value = width == 32u ? (x ? (uint32_t)__builtin_clz((uint32_t)x) : 32u)
          : (x ? (uint64_t)__builtin_clzll(x) : 64u);
    else if (op == 0x68u || op == 0x7au)
      value = width == 32u ? (x ? (uint32_t)__builtin_ctz((uint32_t)x) : 32u)
          : (x ? (uint64_t)__builtin_ctzll(x) : 64u);
    else value = width == 32u ? (uint32_t)__builtin_popcount((uint32_t)x)
        : (uint64_t)__builtin_popcountll(x);
    stack[(*sp)++] = value; return 1;
  }
  if ((op >= 0x6au && op <= 0x78u) || (op >= 0x7cu && op <= 0x8au)) {
    NUMERIC_POP2();
    if (op <= 0x78u) {
      uint32_t x = (uint32_t)a, y = (uint32_t)b, z;
      switch (op) {
        case 0x6a: z = x + y; break; case 0x6b: z = x - y; break;
        case 0x6c: z = x * y; break;
        case 0x6d:
          if (!y) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
          if (x == UINT32_C(0x80000000) && y == UINT32_MAX)
            NUMERIC_TRAP(GLWASM_TRAP_INTEGER_OVERFLOW);
          z = (uint32_t)(wasm_i32(x) / wasm_i32(y)); break;
        case 0x6e: if (!y) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
          z = x / y; break;
        case 0x6f: if (!y) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
          z = (x == UINT32_C(0x80000000) && y == UINT32_MAX) ? 0u :
              (uint32_t)(wasm_i32(x) % wasm_i32(y)); break;
        case 0x70: if (!y) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
          z = x % y; break;
        case 0x71: z = x & y; break; case 0x72: z = x | y; break;
        case 0x73: z = x ^ y; break; case 0x74: z = x << (y & 31u); break;
        case 0x75: z = arithmetic_shift_right32(x, y); break;
        case 0x76: z = x >> (y & 31u); break;
        case 0x77: z = y ? (x << (y & 31u)) | (x >> ((32u - (y & 31u)) & 31u)) : x; break;
        case 0x78: z = y ? (x >> (y & 31u)) | (x << ((32u - (y & 31u)) & 31u)) : x; break;
        default: NUMERIC_FAIL();
      }
      stack[(*sp)++] = z; return 1;
    }
    switch (op) {
      case 0x7c: value = a + b; break; case 0x7d: value = a - b; break;
      case 0x7e: value = a * b; break;
      case 0x7f:
        if (!b) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
        if (a == UINT64_C(0x8000000000000000) && b == UINT64_MAX)
          NUMERIC_TRAP(GLWASM_TRAP_INTEGER_OVERFLOW);
        value = (uint64_t)(wasm_i64(a) / wasm_i64(b)); break;
      case 0x80: if (!b) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
        value = a / b; break;
      case 0x81: if (!b) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
        value = (a == UINT64_C(0x8000000000000000) && b == UINT64_MAX)
            ? 0u : (uint64_t)(wasm_i64(a) % wasm_i64(b)); break;
      case 0x82: if (!b) NUMERIC_TRAP(GLWASM_TRAP_DIVIDE_BY_ZERO);
        value = a % b; break;
      case 0x83: value = a & b; break; case 0x84: value = a | b; break;
      case 0x85: value = a ^ b; break; case 0x86: value = a << (b & 63u); break;
      case 0x87: value = arithmetic_shift_right64(a, b); break;
      case 0x88: value = a >> (b & 63u); break;
      case 0x89: value = b ? (a << (b & 63u)) | (a >> ((64u - (b & 63u)) & 63u)) : a; break;
      case 0x8a: value = b ? (a >> (b & 63u)) | (a << ((64u - (b & 63u)) & 63u)) : a; break;
      default: NUMERIC_FAIL();
    }
    stack[(*sp)++] = value; return 1;
  }
  if ((op >= 0x8bu && op <= 0x98u) || (op >= 0x99u && op <= 0xa6u)) {
    int f32 = op <= 0x98u;
    int binary = (op >= 0x92u && op <= 0x98u) ||
        (op >= 0xa0u && op <= 0xa6u);
    if (*sp < (binary ? 2u : 1u)) NUMERIC_FAIL();
    b = stack[--*sp];
    if (binary) a = stack[--*sp]; else a = b;
    if (f32) {
      uint32_t ab = (uint32_t)a, bb = (uint32_t)b, vb;
      float x, y, z; memcpy(&x, &ab, 4); memcpy(&y, &bb, 4);
      switch (op) {
        case 0x8b: vb = ab & UINT32_C(0x7fffffff); value = vb; break;
        case 0x8c: value = ab ^ UINT32_C(0x80000000); break;
        case 0x8d: z = ceilf(x); goto store_f32;
        case 0x8e: z = floorf(x); goto store_f32;
        case 0x8f: z = truncf(x); goto store_f32;
        case 0x90: z = (float)wasm_round_nearest((double)x); goto store_f32;
        case 0x91: z = sqrtf(x); goto store_f32;
        case 0x92: z = x + y; goto store_f32;
        case 0x93: z = x - y; goto store_f32;
        case 0x94: z = x * y; goto store_f32;
        case 0x95: z = x / y; goto store_f32;
        case 0x96: z = (isnan(x)||isnan(y))?NAN:(x==0.0f&&y==0.0f?(signbit(x)||signbit(y)?-0.0f:0.0f):fminf(x,y)); goto store_f32;
        case 0x97: z = (isnan(x)||isnan(y))?NAN:(x==0.0f&&y==0.0f?(signbit(x)&&signbit(y)?-0.0f:0.0f):fmaxf(x,y)); goto store_f32;
        case 0x98: value = (ab & UINT32_C(0x7fffffff)) | (bb & UINT32_C(0x80000000)); break;
        default: NUMERIC_FAIL();
      }
      stack[(*sp)++] = value; return 1;
store_f32:
      /* Every arithmetic result that is a NaN is an arithmetic NaN: libm's
       * rounding functions may return a signalling input unchanged. */
      { uint32_t bits; memcpy(&bits, &z, sizeof(bits));
        if (isnan(z)) bits |= UINT32_C(0x00400000);
        stack[(*sp)++] = bits; }
      return 1;
    }
    uint64_t ab = a, bb = b; double x, y, z;
    memcpy(&x, &ab, 8); memcpy(&y, &bb, 8);
    switch (op) {
      case 0x99: value = ab & UINT64_C(0x7fffffffffffffff); break;
      case 0x9a: value = ab ^ UINT64_C(0x8000000000000000); break;
      case 0x9b: z = ceil(x); goto store_f64;
      case 0x9c: z = floor(x); goto store_f64;
      case 0x9d: z = trunc(x); goto store_f64;
      case 0x9e: z = wasm_round_nearest(x); goto store_f64;
      case 0x9f: z = sqrt(x); goto store_f64;
      case 0xa0: z = x + y; goto store_f64;
      case 0xa1: z = x - y; goto store_f64;
      case 0xa2: z = x * y; goto store_f64;
      case 0xa3: z = x / y; goto store_f64;
      case 0xa4: z = (__builtin_isnan(x)||__builtin_isnan(y))?NAN:(x==0.0&&y==0.0?(__builtin_signbit(x)||__builtin_signbit(y)?-0.0:0.0):fmin(x,y)); goto store_f64;
      case 0xa5: z = (__builtin_isnan(x)||__builtin_isnan(y))?NAN:(x==0.0&&y==0.0?(__builtin_signbit(x)&&__builtin_signbit(y)?-0.0:0.0):fmax(x,y)); goto store_f64;
      case 0xa6: value = (ab & UINT64_C(0x7fffffffffffffff)) | (bb & UINT64_C(0x8000000000000000)); break;
      default: NUMERIC_FAIL();
    }
    stack[(*sp)++] = value; return 1;
store_f64:
    memcpy(&value, &z, sizeof(z));
    if (__builtin_isnan(z)) value |= UINT64_C(0x0008000000000000);
    stack[(*sp)++] = value; return 1;
  }
  if (op == 0xa7u || op == 0xacu || op == 0xadu ||
      (op >= 0xb2u && op <= 0xbfu) || (op >= 0xc0u && op <= 0xc4u) ||
      (op >= 0xa8u && op <= 0xb1u) ||
      ((op & 0xff00u) == 0xfc00u && (op & 0xffu) <= 7u)) {
    if (*sp < 1u) NUMERIC_FAIL();
    a = stack[--*sp];
    switch (op) {
      case 0xa7: value = (uint32_t)a; break;
      case 0xac: value = (uint64_t)(int64_t)wasm_i32((uint32_t)a); break;
      case 0xad: value = (uint32_t)a; break;
      case 0xc0: value = wasm_sign_extend32((uint32_t)a, 8u); break;
      case 0xc1: value = wasm_sign_extend32((uint32_t)a, 16u); break;
      case 0xc2: value = wasm_sign_extend64(a, 8u); break;
      case 0xc3: value = wasm_sign_extend64(a, 16u); break;
      case 0xc4: value = wasm_sign_extend64(a, 32u); break;
      case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb7: case 0xb8:
      case 0xb9: case 0xba: {
        /* Each conversion rounds once, straight from the integer: going
         * through double first would round a 64-bit integer twice. */
        int src64 = op == 0xb4u || op == 0xb5u || op == 0xb9u || op == 0xbau;
        int unsig = op == 0xb3u || op == 0xb5u || op == 0xb8u || op == 0xbau;
        if (op <= 0xb5u) {
          float out = src64 ? (unsig ? (float)a : (float)wasm_i64(a))
              : (unsig ? (float)(uint32_t)a : (float)wasm_i32((uint32_t)a));
          uint32_t bits; memcpy(&bits,&out,4); value=bits;
        } else {
          double out = src64 ? (unsig ? (double)a : (double)wasm_i64(a))
              : (unsig ? (double)(uint32_t)a : (double)wasm_i32((uint32_t)a));
          memcpy(&value,&out,8);
        }
        break;
      }
      case 0xb6: { double number; float out; memcpy(&number,&a,8); out=(float)number; uint32_t bits; memcpy(&bits,&out,4); value=bits; break; }
      case 0xbb: { uint32_t bits=(uint32_t)a; float input; double out; memcpy(&input,&bits,4); out=(double)input; memcpy(&value,&out,8); break; }
      case 0xbc: value = (uint32_t)a; break;
      case 0xbd: value = a; break;
      case 0xbe: value = (uint32_t)a; break;
      case 0xbf: value = a; break;
      default: {
        int trunc_op = op >= 0xa8u && op <= 0xb1u;
        unsigned sub = op & 0xffu;
        int width = trunc_op ? (op <= 0xabu ? 32 : 64) :
            (sub <= 3u ? 32 : 64);
        int is_f64 = trunc_op ? (op == 0xaau || op == 0xabu ||
            op == 0xb0u || op == 0xb1u) : (sub == 2u || sub == 3u || sub == 6u || sub == 7u);
        int unsig = trunc_op ? (op == 0xa9u || op == 0xabu || op == 0xafu || op == 0xb1u)
            : (sub & 1u) != 0;
        double input;
        if (is_f64) memcpy(&input,&a,8);
        else { uint32_t bits=(uint32_t)a; float f; memcpy(&f,&bits,4); input=(double)f; }
        if (trunc_op) {
          if (!wasm_float_to_integer(input,(unsigned)width,!unsig,&value))
            NUMERIC_TRAP(__builtin_isnan(input)
                ? GLWASM_TRAP_INVALID_CONVERSION
                : GLWASM_TRAP_INTEGER_OVERFLOW);
        } else value=wasm_float_to_integer_sat(input,(unsigned)width,!unsig);
        break;
      }
    }
    stack[(*sp)++] = value; return 1;
  }
#undef NUMERIC_POP2
#undef NUMERIC_TRAP
#undef NUMERIC_FAIL
  return 0;
}

static GRCORE_Step finish_uncaught_exception(GRCORE_Context *ctx,Run *run,
    uint32_t function,uint32_t pc,ExceptionValue *exception) {
  uint64_t offset=0;
  if(function<run->m->nfuncs&&run->m->funcs[function].translated&&
      pc<run->m->funcs[function].translated->offset_count)
    offset=run->m->funcs[function].translated->offsets[pc];
  GRCORE_Verdict verdict=grcore_poll(ctx,(GRCORE_Location){"wasm",
      offset<=INT_MAX?(int)offset:INT_MAX});
  if(verdict==GRCORE_VERDICT_UNWIND){
    run->error=GLWASM_ERR_LIMIT;
    return GRCORE_STEP_UNWOUND;
  }
  GLWASM_Runtime *runtime=run->instance;
  if(runtime&&exception&&runtime->store->nested_boundary.id){
    GRCORE_Result stored=grcore_boundary_set_foreign_exception(
        grcore_context_stack(ctx),runtime->store->nested_boundary,run->engine,
        exception->identity);
    if(stored!=GRCORE_OK){
      run->error=map_core_result(stored);
      return GRCORE_STEP_UNWOUND;
    }
    runtime->store->last_foreign_exception.engine=run->engine;
    runtime->store->last_foreign_exception.identity=exception->identity;
  }
  return GRCORE_STEP_FINISHED;
}

static GRCORE_Step execute_impl(GRCORE_Context *ctx, void *opaque) {
  Run *r=(Run *)opaque; GRCORE_Stack *s=grcore_context_stack(ctx); GRCORE_FrameRef f;
  if (grcore_stack_frame_count(s)<=r->stack_base) {
    /* The invoked function may be an import: the host's, or another
     * instance's, which is then run as that instance's own frame. */
    run_bind(r,r->entry_instance);
    const StoreFunc *entry=instance_func(r->instance,r->entry);
    if(entry->instance&&entry->instance!=r->instance){
      run_bind(r,entry->instance);
      r->entry=entry->index;
    }
    GLWASM_Func *fn=&r->m->funcs[r->entry];
    if(!entry->instance){
      uint64_t *results=r->results;
      GLWASM_Result called=call_host(r->instance,entry,r->args,results,
          r->pending_host&&r->pending_host_function==r->entry);
      if(called==GLWASM_ERR_PAUSED){
        r->pending_host=1;r->pending_host_function=r->entry;
        GRCORE_Verdict verdict=request_host_pause(ctx,r->m,r->entry,0,0);
        return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
            GRCORE_STEP_UNWOUND;
      }
      r->pending_host=0;
      if(called!=GLWASM_OK){r->error=called;return GRCORE_STEP_FINISHED;}
      GLWASM_TypeDef *type=&r->m->types[fn->type_index];
      r->result_count=type->nresults;
      r->result=type->nresults?results[0]:0;
      return GRCORE_STEP_FINISHED;
    }
    if (grcore_stack_push(s,r->engine,frame_slots(fn),&f)!=GRCORE_OK) {r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;}
    uint64_t *p=slots(s,f); if(!p){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
    memset(p,0,frame_slots(fn)*sizeof(*p));
    p[H_FUNC]=r->entry;p[H_PC]=0;p[H_SP]=H_OPERANDS+fn->nlocals;p[H_NLOCALS]=fn->nlocals;p[H_CTRL_DEPTH]=0;p[H_INSTANCE]=(uintptr_t)r->instance;
    diagnostic_record_frame_entry(r,r->entry,p[H_SP],H_OPERANDS+fn->nlocals);
    for(uint32_t i=0;i<r->nargs && i<fn->nparams;i++)p[H_OPERANDS+i]=r->args[i];
    if(wasm_set_guest_frame_identity(s,f,r->m,r->entry,0)!=GRCORE_OK){
      (void)grcore_stack_pop(s);r->error=GLWASM_ERR_INTERNAL;
      return GRCORE_STEP_FINISHED;
    }
  }
  if (!r->jit_entry_attempted) {
    r->jit_entry_attempted = 1;
    if (!r->m->funcs[r->entry].imported) {
      uint32_t jit_result_count = 0;
      int entered = 0, returned = 0;
      GLWASM_Result jit_result = jit_try_enter(r->instance, r->entry,
          r->args, r->results, r->result_capacity, &jit_result_count,
          &entered, &returned);
      if (jit_result != GLWASM_OK) {
        r->error = jit_result;
        return GRCORE_STEP_FINISHED;
      }
      if (entered) {
        if (r->paused) return GRCORE_STEP_PAUSED;
        if (r->error != GLWASM_OK)
          return r->error == GLWASM_ERR_LIMIT ? GRCORE_STEP_UNWOUND
              : GRCORE_STEP_FINISHED;
        if (!returned) goto jit_entry_deoptimized;
        if (grcore_stack_frame_count(s) > r->stack_base) {
          (void)grcore_stack_pop(s);
        }
        if (grcore_stack_frame_count(s) <= r->stack_base) {
          r->result_count = jit_result_count;
          r->result = jit_result_count && r->results ? r->results[0] : 0;
          return GRCORE_STEP_FINISHED;
        }
      }
    }
  }
jit_entry_deoptimized:
  for (;;) {
    /* Whatever happened last (a call, a return, an exit from compiled
     * code), the frame on top says which instance runs now. */
    if (grcore_stack_frame_count(s) > r->stack_base) {
      uint64_t *top = slots(s, grcore_stack_top(s));
      if (top) run_bind(r, (GLWASM_Runtime *)(uintptr_t)top[H_INSTANCE]);
    }
    if (r->jit_pending) {
      uint32_t candidate = r->jit_pending_function;
      r->jit_pending = 0;
      GLWASM_Func *candidate_fn = &r->m->funcs[candidate];
      uint64_t inline_args[64];
      uint64_t *heap_args = NULL;
      uint64_t *candidate_args = inline_args;
      if (candidate_fn->nparams > sizeof(inline_args) /
          sizeof(inline_args[0])) {
#if SIZE_MAX < UINT64_MAX
        if ((size_t)candidate_fn->nparams > SIZE_MAX /
            sizeof(*candidate_args)) {
          r->error = GLWASM_ERR_LIMIT;
          return GRCORE_STEP_FINISHED;
        }
#endif
        heap_args = gcu_allocator_calloc(&r->m->alloc,
            candidate_fn->nparams, sizeof(*heap_args));
        if (!heap_args) {
          r->error = GLWASM_ERR_OOM;
          return GRCORE_STEP_FINISHED;
        }
        candidate_args = heap_args;
      }
      {
        uint64_t *candidate_slots = slots(s, grcore_stack_top(s));
        for (uint32_t i = 0; i < candidate_fn->nparams; ++i)
          candidate_args[i] = candidate_slots[H_OPERANDS + i];
        GLWASM_TypeDef *candidate_type =
            &r->m->types[candidate_fn->type_index];
        uint32_t candidate_capacity = candidate_type->nresults;
        uint64_t *candidate_results = candidate_capacity
            ? gcu_allocator_calloc(&r->m->alloc, candidate_capacity,
                sizeof(*candidate_results)) : NULL;
        if (candidate_capacity && !candidate_results) {
          gcu_allocator_free(&r->m->alloc, heap_args);
          r->error = GLWASM_ERR_OOM;
          return GRCORE_STEP_FINISHED;
        }
        uint32_t candidate_result_count = 0;
        int entered = 0, returned = 0;
        GLWASM_Result jit_result = jit_try_enter(r->instance, candidate,
            candidate_args, candidate_results, candidate_capacity,
            &candidate_result_count, &entered, &returned);
        gcu_allocator_free(&r->m->alloc, heap_args);
        if (jit_result != GLWASM_OK) {
          gcu_allocator_free(&r->m->alloc, candidate_results);
          r->error = jit_result;
          return GRCORE_STEP_FINISHED;
        }
        if (entered) {
          if (r->paused) {
            gcu_allocator_free(&r->m->alloc, candidate_results);
            return GRCORE_STEP_PAUSED;
          }
          if (r->error != GLWASM_OK) {
            gcu_allocator_free(&r->m->alloc, candidate_results);
            return r->error == GLWASM_ERR_LIMIT ? GRCORE_STEP_UNWOUND
                : GRCORE_STEP_FINISHED;
          }
          if (returned) {
            if (grcore_stack_frame_count(s) > r->stack_base)
              (void)grcore_stack_pop(s);
            if (grcore_stack_frame_count(s) <= r->stack_base) {
              r->result_count = candidate_result_count;
              r->result = candidate_result_count ? candidate_results[0] : 0;
              if (candidate_result_count && r->results)
                memcpy(r->results, candidate_results,
                    (size_t)candidate_result_count * sizeof(*r->results));
              gcu_allocator_free(&r->m->alloc, candidate_results);
              return GRCORE_STEP_FINISHED;
            }
            if (candidate_result_count) {
              uint64_t *caller = slots(s, grcore_stack_top(s));
              uint32_t caller_sp = (uint32_t)caller[H_SP];
              memcpy(&caller[caller_sp], candidate_results,
                  (size_t)candidate_result_count * sizeof(*candidate_results));
              caller_sp += candidate_result_count;
              caller[H_SP] = caller_sp;
            }
          }
        }
        gcu_allocator_free(&r->m->alloc, candidate_results);
      }
      /* A compiled return pops to the caller, which may belong to another
       * instance. */
      uint64_t *top = slots(s, grcore_stack_top(s));
      if (top) run_bind(r, (GLWASM_Runtime *)(uintptr_t)top[H_INSTANCE]);
    }
    f=grcore_stack_top(s); uint64_t *p=slots(s,f);
    uint32_t fi=(uint32_t)p[H_FUNC]; GLWASM_Func *fn=&r->m->funcs[fi];
    uint32_t pc=(uint32_t)p[H_PC], sp=(uint32_t)p[H_SP];
    diagnostic_record_sp(r,fi,pc,sp,H_OPERANDS+fn->nlocals);
    if(pc>=fn->nins){
      uint32_t nres=r->m->types[fn->type_index].nresults;
      uint32_t value_base=H_OPERANDS+fn->nlocals;
      if(sp<value_base||nres>sp-value_base){
        r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
      }
      uint32_t result_base=sp-nres;
      size_t result_bytes=0;
      if(!gcu_safe_mul_size((size_t)nres,sizeof(uint64_t),&result_bytes)){
        r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;
      }
      uint64_t *returned=nres?gcu_allocator_malloc(&r->m->alloc,
          result_bytes):NULL;
      if(nres&&!returned){r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
      if(nres)memcpy(returned,&p[result_base],
          result_bytes);
      grcore_stack_pop(s);
      if(grcore_stack_frame_count(s)<=r->stack_base){
        r->result_count=nres;
        r->result=nres?returned[0]:0;
        if(nres&&r->results)memcpy(r->results,returned,
            result_bytes);
        gcu_allocator_free(&r->m->alloc,returned);
        return GRCORE_STEP_FINISHED;
      }
      f=grcore_stack_top(s);p=slots(s,f);sp=(uint32_t)p[H_SP];
      if(nres){memcpy(&p[sp],returned,(size_t)nres*sizeof(*p));sp+=nres;}
      gcu_allocator_free(&r->m->alloc,returned);
      p[H_SP]=sp;continue;
    }
    GLWASM_TranslatedInstr *ti=&fn->translated->ins[pc]; GLWASM_Instr in=ti->source;
    r->diagnostic_function=fi;
    r->diagnostic_pc=pc;
    r->diagnostic_opcode=in.op;
    r->diagnostic_has_instruction=1;
    r->diagnostic_call_target=UINT32_MAX;
    r->diagnostic_call_sp=0;
    r->diagnostic_call_base=0;
    r->diagnostic_call_required=0;
    r->diagnostic_caller_params=0;
    r->diagnostic_caller_locals=0;
    r->diagnostic_callee_params=0;
    r->diagnostic_reason=0;
    int skip_charge=p[H_SKIP_CHARGE]!=0;p[H_SKIP_CHARGE]=0;
    p[H_PC]=pc+1; p[H_SP]=sp;
    if(!skip_charge&&glwasm_opcode_cost(in.op))grcore_context_charge_fuel(ctx,glwasm_opcode_cost(in.op));
    /* Every instruction boundary is a runtime-debug stop site. A resume skips
     * this same poll once, then executes the instruction without recharging. */
    if(!skip_charge) {
      GRCORE_Verdict v=grcore_stack_poll(ctx,
          wasm_function_identity(r->m,fi),fn->translated->offsets[pc]);
      if(v==GRCORE_VERDICT_PAUSE){p=slots(s,grcore_stack_top(s));p[H_PC]=pc;p[H_SKIP_CHARGE]=1;return GRCORE_STEP_PAUSED;}
      if(v==GRCORE_VERDICT_UNWIND){r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_UNWOUND;}
      f=grcore_stack_top(s);p=slots(s,f);sp=(uint32_t)p[H_SP];
    }
    switch(in.op){
      case 0x00:return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_UNREACHABLE);
      case 0x01: break;
      case 0x02: case 0x03: case 0x04: case 0x1f:{
        uint64_t condition=1;
        if(in.op==0x04)condition=p[--sp];
        uint32_t nparams=in.a<fn->nblocks?fn->blocks[in.a].nparams:0;uint32_t depth=(uint32_t)p[H_CTRL_DEPTH];
        size_t cb=control_base(fn)+2u*depth;
        if(depth>=fn->nins){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        p[cb]=pc;p[cb+1]=sp>=nparams?sp-nparams:sp;p[H_CTRL_DEPTH]=depth+1;
        if(in.op==0x04&&!condition){p[H_PC]=ti->branch_target;if(ti->branch_target==ti->alternate_target)p[H_CTRL_DEPTH]=depth;}
        break;
      }
      case 0x05:{uint32_t depth=(uint32_t)p[H_CTRL_DEPTH];if(depth){size_t cb=control_base(fn)+2u*(depth-1u);uint32_t op=(uint32_t)p[cb];uint32_t nres=block_results(fn,fn->ins[op].a);sp=(uint32_t)p[cb+1]+nres;p[H_CTRL_DEPTH]=depth-1;}p[H_PC]=ti->alternate_target;break;}
      case 0x0b:{uint32_t depth=(uint32_t)p[H_CTRL_DEPTH];if(depth){size_t cb=control_base(fn)+2u*(depth-1u);uint32_t op=(uint32_t)p[cb];uint32_t nres=block_results(fn,fn->ins[op].a);sp=(uint32_t)p[cb+1]+nres;p[H_CTRL_DEPTH]=depth-1;}break;}
      case 0x08: {
        if(in.a>=r->m->ntags||r->m->tags[in.a].type_index>=r->m->ntypes){
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        GLWASM_TypeDef *type=&r->m->types[r->m->tags[in.a].type_index];
        if(sp<H_OPERANDS+fn->nlocals+type->nparams){
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        uint32_t payload_base=sp-type->nparams;
        ExceptionValue *exception=new_exception(r,in.a,&p[payload_base]);
        if(!exception){if(r->error==GLWASM_OK)r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
        sp=payload_base;p[H_SP]=sp;
        r->pending_exception=exception;
        if(dispatch_exception(r,exception)){
          f=grcore_stack_top(s);p=slots(s,f);sp=(uint32_t)p[H_SP];
          break;
        }
        return finish_uncaught_exception(ctx,r,fi,pc,exception);
      }
      case 0x0a: {
        if(sp<=H_OPERANDS+fn->nlocals){
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        uint64_t reference=p[--sp];p[H_SP]=sp;
        if(!reference)return raise_trap(ctx,r,fi,pc,
            GLWASM_TRAP_INVALID_REFERENCE);
        ExceptionValue *exception=find_exception(r->instance,reference);
        if(!exception)return raise_trap(ctx,r,fi,pc,
            GLWASM_TRAP_INVALID_REFERENCE);
        r->pending_exception=exception;
        if(dispatch_exception(r,exception)){
          f=grcore_stack_top(s);p=slots(s,f);sp=(uint32_t)p[H_SP];
          break;
        }
        return finish_uncaught_exception(ctx,r,fi,pc,exception);
      }
      case 0x0f: p[H_PC]=fn->nins; break;
      case 0x10: case 0x12: case 0x11: case 0x13: {
        /* Resolve the callee to a store function: this instance's own, an
         * import (the host's or another instance's), or a table element of
         * any of those. A Wasm callee runs as its own instance's frame. */
        uint32_t callee=in.a;uint64_t indirect_index=0;
        r->diagnostic_call_target=callee;
        r->diagnostic_reason=1;
        r->diagnostic_call_sp=sp;
        r->diagnostic_call_base=H_OPERANDS+fn->nlocals;
        r->diagnostic_call_required=r->diagnostic_call_base;
        r->diagnostic_caller_params=fn->nparams;
        r->diagnostic_caller_locals=fn->nlocals;
        r->diagnostic_callee_params=0;
        const StoreFunc *target=NULL;
        if(in.op==0x11||in.op==0x13){
          if(in.b>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          if(in.a>=r->m->ntypes){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          size_t base=H_OPERANDS+fn->nlocals;
          size_t required=base+r->m->types[in.a].nparams+1u;
          if((size_t)sp<required){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          uint64_t ti_index=p[--sp];indirect_index=ti_index;
          TableLease lease;
          if(!table_acquire(r,in.b,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
          if(ti_index>=lease.view.length){table_release(&lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_UNDEFINED_ELEMENT);}
          uint64_t function_ref=lease.view.elements[ti_index];
          table_release(&lease);
          if(!function_ref)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_UNINITIALIZED_ELEMENT);
          target=funcref_target(r->store,function_ref);
          if(!target||!same_function_type(target->type,&r->m->types[in.a]))
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);
        } else {
          if(callee>=r->m->nfuncs){r->diagnostic_reason=2;r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          target=instance_func(r->instance,callee);
        }
        const GLWASM_TypeDef *callee_type=target->type;
        uint32_t np=callee_type->nparams;
        r->diagnostic_callee_params=np;
        r->diagnostic_call_required=r->diagnostic_call_base+np;
        if(sp < H_OPERANDS + fn->nlocals + np){r->diagnostic_reason=4;r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        const GCU_Allocator *call_alloc=r->store->alloc;
        uint64_t *argv=np?gcu_allocator_calloc(call_alloc,np,sizeof(*argv)):NULL;
        if(np&&!argv){r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
        for(uint32_t j=0;j<np;j++) { argv[np-1-j]=p[--sp]; }
        p[H_SP]=sp;
        if(!target->instance){
          uint32_t nres=callee_type->nresults;
          uint64_t *host_results=nres?gcu_allocator_calloc(call_alloc,
              nres,sizeof(*host_results)):NULL;
          if(nres&&!host_results){gcu_allocator_free(call_alloc,argv);
            r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
          /* A paused host call resumes only at the same call site. */
          GLWASM_Result called=call_host(r->instance,target,argv,host_results,
              r->pending_host&&r->pending_host_function==callee);
          if(called==GLWASM_ERR_PAUSED){
            /* The callback owns any continuation state in its `user` value.
             * Put the exact operands back so retrying after resume repeats
             * this call once, without charging the opcode twice. */
            for(uint32_t j=0;j<np;j++)p[sp++]=argv[j];
            if(in.op==0x11||in.op==0x13)p[sp++]=indirect_index;
            p[H_PC]=pc;p[H_SP]=sp;p[H_SKIP_CHARGE]=1;
            r->pending_host=1;r->pending_host_function=callee;
            gcu_allocator_free(call_alloc,host_results);
            gcu_allocator_free(call_alloc,argv);
            GRCORE_Verdict verdict=request_host_pause(ctx,r->m,fi,
                fn->translated->offsets[pc],1);
            return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
                GRCORE_STEP_UNWOUND;
          }
          r->pending_host=0;
          if(called==GLWASM_OK&&nres){memcpy(&p[sp],host_results,
              (size_t)nres*sizeof(*p));sp+=nres;p[H_SP]=sp;}
          gcu_allocator_free(call_alloc,host_results);
          gcu_allocator_free(call_alloc,argv);
          if(called!=GLWASM_OK){r->error=called;return GRCORE_STEP_FINISHED;}
          if(in.op==0x12||in.op==0x13)p[H_PC]=fn->nins;
          break;
        }
        GLWASM_Runtime *callee_instance=target->instance;
        callee=target->index;
        GLWASM_Func *cf=&callee_instance->module->funcs[callee];
        if(in.op==0x12||in.op==0x13)grcore_stack_pop(s);
        if(grcore_stack_push(s,r->engine,frame_slots(cf),&f)!=GRCORE_OK){r->diagnostic_reason=5;gcu_allocator_free(call_alloc,argv);r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;}
        p=slots(s,f);if(!p){r->diagnostic_reason=6;(void)grcore_stack_pop(s);gcu_allocator_free(call_alloc,argv);r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}memset(p,0,frame_slots(cf)*sizeof(*p));
        p[H_FUNC]=callee;p[H_PC]=0;p[H_SP]=H_OPERANDS+cf->nlocals;p[H_NLOCALS]=cf->nlocals;p[H_CTRL_DEPTH]=0;p[H_INSTANCE]=(uintptr_t)callee_instance;
        run_bind(r,callee_instance);
        diagnostic_record_frame_entry(r,callee,p[H_SP],H_OPERANDS+cf->nlocals);
        for(uint32_t j=0;j<np;j++) { p[H_OPERANDS+j]=argv[j]; }
        if(wasm_set_guest_frame_identity(s,f,r->m,callee,0)!=GRCORE_OK){
          r->diagnostic_reason=7;
          (void)grcore_stack_pop(s);gcu_allocator_free(call_alloc,argv);
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        gcu_allocator_free(call_alloc,argv);
        /* The common instruction epilogue writes this frame's SP. We have
         * switched `p` from the caller to the callee, so also switch the
         * local SP value before that writeback. */
        sp=(uint32_t)p[H_SP];
        fi=callee;fn=cf;pc=0;
        r->jit_pending_function=callee;
        r->jit_pending=1;
        break;
      }
      case 0x20:p[sp++]=p[H_OPERANDS+in.a];break;
      case 0x21:p[H_OPERANDS+in.a]=p[--sp];break;
      case 0x22:p[H_OPERANDS+in.a]=p[sp-1];break;
      case 0x23:{
        if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        const StoreGlobal *global=r->instance->globals[in.a];
        uint64_t value=global_read(global);
        if(!global_value_valid(r->store,global,value))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);
        p[sp++]=value;break;
      }
      case 0x24:{
        if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        StoreGlobal *global=r->instance->globals[in.a];
        uint64_t value=p[--sp];
        if(!global_value_valid(r->store,global,value))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);
        global_write(global,value);break;
      }
      case 0x25: case 0x26: {
        if(in.a>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        uint64_t value=0,index;
        if(in.op==0x26)value=p[--sp];
        index=p[--sp];
        TableLease lease;if(!table_acquire(r,in.a,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
        if(index>=lease.view.length){table_release(&lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);}
        if(in.op==0x25){value=lease.view.elements[index];if(!table_reference_valid(r,in.a,value)){table_release(&lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);}p[sp++]=value;}
        else {if(!table_reference_valid(r,in.a,value)){table_release(&lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);}lease.view.elements[index]=value;}
        table_release(&lease);
        break;
      }
      case 0x28: case 0x29: case 0x2a: case 0x2b:
      case 0x2c: case 0x2d: case 0x2e: case 0x2f:
      case 0x30: case 0x31: case 0x32: case 0x33:
      case 0x34: case 0x35: {
        size_t width = memory_access_size(in.op);
        uint64_t address = p[--sp], effective = 0;
        MemoryLease lease;
        if (!width || !memory_acquire(r,in.b,&lease))
          return raise_trap(ctx, r, fi, pc, GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        if (!memory_range(r, in.b, address, in.c, width, &effective)){
          memory_release(&lease);
          return raise_trap(ctx, r, fi, pc, GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        uint64_t value = load_le(lease.view.bytes + effective, width);
        memory_release(&lease);
        if ((in.op == 0x2c || in.op == 0x2e || in.op == 0x30 ||
             in.op == 0x32 || in.op == 0x34) && width < 8u) {
          value = sign_extend(value, (unsigned)(width * 8u));
        }
        p[sp++] = value;
        break;
      }
      case 0x36: case 0x37: case 0x38: case 0x39:
      case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: {
        size_t width = memory_access_size(in.op);
        uint64_t value = p[--sp], address = p[--sp], effective = 0;
        MemoryLease lease;
        if (!width || !memory_acquire(r,in.b,&lease))
          return raise_trap(ctx, r, fi, pc, GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        if (!memory_range(r, in.b, address, in.c, width, &effective)){
          memory_release(&lease);
          return raise_trap(ctx, r, fi, pc, GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
        }
        store_le(lease.view.bytes + effective, width, value);
        memory_release(&lease);
        break;
      }
      case 0x3f:
        if (in.a >= r->m->nmems) { r->error = GLWASM_ERR_INTERNAL; return GRCORE_STEP_FINISHED; }
        {MemoryLease lease;if(!memory_acquire(r,in.a,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}memory_release(&lease);}
        p[sp++] = run_memory(r, in.a)->pages;
        break;
      case 0x40: {
        if (in.a >= r->m->nmems) { r->error = GLWASM_ERR_INTERNAL; return GRCORE_STEP_FINISHED; }
        uint64_t delta = p[--sp], old = 0;
        if (memory_grow(r, in.a, delta, &old)) p[sp++] = old;
        else p[sp++] = r->m->mems[in.a].addr64 ? UINT64_MAX : UINT32_MAX;
        break;
      }
      case 0xd0:p[sp++]=0;break;
      case 0xd1:p[sp-1u]=p[sp-1u]==0u;break;
      case 0xd2:p[sp++]=instance_funcref(r->instance,in.a);break;
      case 0xfc08: case 0xfc09: case 0xfc0a: case 0xfc0b: {
        const uint32_t sub=(uint32_t)(in.op&0xffu);
        if(sub==9u) {
          if(in.a>=r->m->ndatas){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          if(r->instance->data_dropped)r->instance->data_dropped[in.a]=1;
          break;
        }
        if(!r->bulk_active) {
          uint64_t length=p[--sp],source=0,destination=0,value=0;
          if(sub==8u) { source=p[--sp];destination=p[--sp]; }
          else if(sub==10u) { source=p[--sp];destination=p[--sp]; }
          else { value=p[--sp]&0xffu;destination=p[--sp]; }
          r->bulk_func=fi;r->bulk_pc=pc;r->bulk_op=sub;
          r->bulk_dst=destination;r->bulk_src=source;r->bulk_len=length;
          r->bulk_done=0;r->bulk_value=value;r->bulk_active=1;p[H_SP]=sp;
        } else if(r->bulk_func!=fi||r->bulk_pc!=pc||r->bulk_op!=sub) {
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        {
          uint64_t checked=0;
          const uint32_t destination_memory=sub==8u?in.b:in.a;
          MemoryLease destination_lease;
          if(!memory_acquire(r,destination_memory,&destination_lease))
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
          int valid=memory_range(r,destination_memory,r->bulk_dst,0,r->bulk_len,&checked);
          memory_release(&destination_lease);
          if(!valid)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
          if(sub==8u) {
            if(in.a>=r->m->ndatas)
              return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
            uint64_t data_length=r->instance->data_dropped[in.a]?0:r->m->datas[in.a].nbytes;
            if(r->bulk_src>data_length||r->bulk_len>data_length-r->bulk_src)
              return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
          }
          if(sub==10u){MemoryLease source_lease;
            if(!memory_acquire(r,in.b,&source_lease))
              return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
            valid=memory_range(r,in.b,r->bulk_src,0,r->bulk_len,&checked);
            memory_release(&source_lease);
            if(!valid)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);}
        }
        while(r->bulk_done<r->bulk_len) {
          const uint64_t left=r->bulk_len-r->bulk_done;
          const size_t chunk=(size_t)(left>4096u?4096u:left);
          uint64_t destination=0,source=0;
          const uint64_t offset=(sub==10u&&r->bulk_dst>r->bulk_src&&
              r->bulk_dst-r->bulk_src<r->bulk_len)?r->bulk_len-r->bulk_done-chunk:r->bulk_done;
          const uint32_t destination_memory=sub==8u?in.b:in.a;
          MemoryLease destination_lease,source_lease;
          if(!memory_acquire(r,destination_memory,&destination_lease))
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);
          int valid=memory_range(r,destination_memory,r->bulk_dst,offset,chunk,&destination);
          uint8_t *dst_bytes=destination_lease.view.bytes;
          uint8_t *src_bytes=NULL;
          int source_acquired=0;
          if(sub==10u){
            if(in.b==destination_memory)src_bytes=destination_lease.view.bytes;
            else if(memory_acquire(r,in.b,&source_lease)){source_acquired=1;src_bytes=source_lease.view.bytes;}
            else valid=0;
            if(valid)valid=memory_range(r,in.b,r->bulk_src,offset,chunk,&source);
          }
          if(!valid){if(source_acquired)memory_release(&source_lease);memory_release(&destination_lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);}
          if(sub==8u) {
            if(in.a>=r->m->ndatas)
              {memory_release(&destination_lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);}
            uint64_t data_length=r->instance->data_dropped[in.a]?0:r->m->datas[in.a].nbytes;
            if(r->bulk_src>data_length||r->bulk_len>data_length-r->bulk_src)
              {memory_release(&destination_lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS);}
            source=r->bulk_src+offset;
            memcpy(dst_bytes+destination,
                r->m->datas[in.a].bytes+source,chunk);
          } else if(sub==10u) {
            memmove(dst_bytes+destination,src_bytes+source,chunk);
          } else memset(dst_bytes+destination,(int)r->bulk_value,chunk);
          if(source_acquired)memory_release(&source_lease);
          memory_release(&destination_lease);
          r->bulk_done+=chunk;
          if(r->bulk_done<r->bulk_len) {
            grcore_context_charge_fuel(ctx,chunk);
            GRCORE_Verdict verdict=grcore_stack_poll(ctx,
                wasm_function_identity(r->m,fi),
                fn->translated->offsets[pc]);
            if(verdict==GRCORE_VERDICT_PAUSE) {
              p=slots(s,grcore_stack_top(s));p[H_PC]=pc;p[H_SKIP_CHARGE]=1;
              p[H_SP]=sp;return GRCORE_STEP_PAUSED;
            }
            if(verdict==GRCORE_VERDICT_UNWIND) {
              r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_UNWOUND;
            }
            p=slots(s,grcore_stack_top(s));sp=(uint32_t)p[H_SP];
          }
        }
        r->bulk_active=0;
        break;
      }
      case 0xfc0c: case 0xfc0d: case 0xfc0e: case 0xfc0f:
      case 0xfc10: case 0xfc11: {
        const uint32_t sub=(uint32_t)(in.op&0xffu);
        if(sub==13u) {
          if(in.a>=r->m->nelems){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          elem_drop(r->instance,in.a);break;
        }
        if(sub==16u) {
          if(in.a>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          TableLease lease;if(!table_acquire(r,in.a,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
          p[sp++]=lease.view.length;table_release(&lease);break;
        }
        if(sub==15u) {
          if(in.a>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          uint64_t delta=p[--sp],value=p[--sp],old=0;
          if(!table_reference_valid(r,in.a,value))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);
          const int addr64=r->m->tables[in.a].addr64;
          if(table_object_grow(r->store,run_table(r,in.a),delta,value,&old))p[sp++]=old;
          else p[sp++]=addr64?UINT64_MAX:UINT32_MAX;
          break;
        }
        if(!r->bulk_active) {
          uint64_t length=p[--sp],source=0,destination=0,value=0;
          if(sub==12u||sub==14u){source=p[--sp];destination=p[--sp];}
          else {value=p[--sp];destination=p[--sp];}
          r->bulk_func=fi;r->bulk_pc=pc;r->bulk_op=sub;
          r->bulk_dst=destination;r->bulk_src=source;r->bulk_len=length;
          r->bulk_done=0;r->bulk_value=value;r->bulk_active=1;p[H_SP]=sp;
        } else if(r->bulk_func!=fi||r->bulk_pc!=pc||r->bulk_op!=sub) {
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        const uint32_t destination_table=sub==12u?in.b:in.a;
        if(destination_table>=r->m->ntables)
          return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        TableLease destination_lease;
        if(!table_acquire(r,destination_table,&destination_lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
        int destination_valid=r->bulk_dst<=destination_lease.view.length&&
            r->bulk_len<=destination_lease.view.length-r->bulk_dst;
        table_release(&destination_lease);
        if(!destination_valid)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        if(sub==12u) {
          if(in.a>=r->m->nelems)
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
          uint64_t element_length=r->instance->elem_counts[in.a];
          if(r->bulk_src>element_length||r->bulk_len>element_length-r->bulk_src)
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
        }
        if(sub==14u){if(in.b>=r->m->ntables)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);TableLease source_lease;if(!table_acquire(r,in.b,&source_lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}int source_valid=r->bulk_src<=source_lease.view.length&&r->bulk_len<=source_lease.view.length-r->bulk_src;table_release(&source_lease);if(!source_valid)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);}
        while(r->bulk_done<r->bulk_len) {
          uint64_t left=r->bulk_len-r->bulk_done;
          uint64_t count=left>1024u?1024u:left;
          uint64_t offset=(sub==14u&&destination_table==in.b&&
              r->bulk_dst>r->bulk_src&&r->bulk_dst-r->bulk_src<r->bulk_len)
              ?r->bulk_len-r->bulk_done-count:r->bulk_done;
          TableLease destination_lease,source_lease;
          if(!table_acquire(r,destination_table,&destination_lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
          uint64_t *destination_elements=destination_lease.view.elements;
          uint64_t *source_elements=NULL;int source_acquired=0;
          int valid=r->bulk_dst+offset<=destination_lease.view.length&&
              count<=destination_lease.view.length-(r->bulk_dst+offset);
          if(sub==14u){if(in.b==destination_table)source_elements=destination_elements;
            else if(table_acquire(r,in.b,&source_lease)){source_acquired=1;source_elements=source_lease.view.elements;}
            else valid=0;
            const uint64_t source_length=in.b==destination_table?destination_lease.view.length:source_lease.view.length;
            if(valid)valid=r->bulk_src+offset<=source_length&&count<=source_length-(r->bulk_src+offset);}
          if(!valid){if(source_acquired)table_release(&source_lease);table_release(&destination_lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);}
          if(sub==12u) {
            const uint64_t *values=r->instance->elem_values[in.a];
            for(uint64_t j=0;j<count;j++)
              destination_elements[r->bulk_dst+offset+j]=values[r->bulk_src+offset+j];
          } else if(sub==14u) {
            memmove(&destination_elements[r->bulk_dst+offset],
                &source_elements[r->bulk_src+offset],(size_t)count*sizeof(uint64_t));
          } else {
            if(!table_reference_valid(r,destination_table,r->bulk_value)) {
              if(source_acquired)table_release(&source_lease);
              table_release(&destination_lease);
              return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);
            }
            for(uint64_t j=0;j<count;j++)
              destination_elements[r->bulk_dst+offset+j]=r->bulk_value;
          }
          if(source_acquired)table_release(&source_lease);
          table_release(&destination_lease);
          r->bulk_done+=count;
          if(r->bulk_done<r->bulk_len) {
            grcore_context_charge_fuel(ctx,count);
            GRCORE_Verdict verdict=grcore_stack_poll(ctx,
                wasm_function_identity(r->m,fi),fn->translated->offsets[pc]);
            if(verdict==GRCORE_VERDICT_PAUSE) {
              p=slots(s,grcore_stack_top(s));p[H_PC]=pc;p[H_SKIP_CHARGE]=1;
              p[H_SP]=sp;return GRCORE_STEP_PAUSED;
            }
            if(verdict==GRCORE_VERDICT_UNWIND) {
              r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_UNWOUND;
            }
            p=slots(s,grcore_stack_top(s));sp=(uint32_t)p[H_SP];
          }
        }
        r->bulk_active=0;break;
      }
      case 0x41:p[sp++]=(uint32_t)in.c;break;
      case 0x42:p[sp++]=in.c;break;
      case 0x43:p[sp++]=in.a;break;
      case 0x44:p[sp++]=in.c;break;
      case 0x1a:if(sp>H_OPERANDS+fn->nlocals)sp--;break;
      case 0x1b: case 0x1c:{uint64_t c=p[--sp],b=p[--sp],a=p[--sp];p[sp++]=c?a:b;break;}
      case 0x0c: case 0x0d: case 0x0e:{uint32_t branch_depth=in.a;uint32_t branch_target=ti->branch_target;uint64_t cond=1;if(in.op==0x0d)cond=p[--sp];if(in.op==0x0e){uint32_t selector=(uint32_t)p[--sp];uint32_t choice=selector<in.b-1u?selector:in.b-1u;branch_depth=fn->u32s[in.a+choice];branch_target=fn->translated->branch_table_targets[ti->alternate_target+choice];}if(cond){uint32_t depth=branch_depth,cdepth=(uint32_t)p[H_CTRL_DEPTH];if(depth>=cdepth){p[H_CTRL_DEPTH]=0;p[H_PC]=fn->nins;}else{uint32_t ci=cdepth-1u-depth;size_t cb=control_base(fn)+2u*ci;uint32_t opener=(uint32_t)p[cb];uint32_t arity=branch_arity(fn,opener);uint32_t base=(uint32_t)p[cb+1];if(arity&&sp>=arity)memmove(&p[base],&p[sp-arity],(size_t)arity*sizeof(*p));sp=base+arity;p[H_CTRL_DEPTH]=fn->ins[opener].op==0x03?ci+1u:ci;p[H_PC]=branch_target;} } break;}
      default:
        {
          GRCORE_Step numeric_step;
          if (wasm_interpret_numeric(ctx, r, fi, pc, in, p, &sp,
              &numeric_step)) {
            if (r->error != GLWASM_OK || r->trap != GLWASM_TRAP_NONE)
              return numeric_step;
            break;
          }
        }
        if(in.op>=0x45 && in.op<=0x4f){uint32_t b=(uint32_t)p[--sp],a=0,v=0;if(in.op!=0x45)a=(uint32_t)p[--sp];switch(in.op){case 0x45:v=!b;break;case 0x46:v=a==b;break;case 0x47:v=a!=b;break;case 0x48:v=(int32_t)a<(int32_t)b;break;case 0x49:v=a<b;break;case 0x4a:v=(int32_t)a>(int32_t)b;break;case 0x4b:v=a>b;break;case 0x4c:v=(int32_t)a<=(int32_t)b;break;case 0x4d:v=a<=b;break;case 0x4e:v=(int32_t)a>=(int32_t)b;break;case 0x4f:v=a>=b;break;}p[sp++]=v;break;}
        if(in.op>=0x6a && in.op<=0x78){uint32_t b=(uint32_t)p[--sp],a=(uint32_t)p[--sp],v=0;switch(in.op){case 0x6a:v=a+b;break;case 0x6b:v=a-b;break;case 0x6c:v=a*b;break;case 0x71:v=a&b;break;case 0x72:v=a|b;break;case 0x73:v=a^b;break;case 0x74:v=a<<(b&31);break;case 0x75:v=arithmetic_shift_right32(a,b);break;case 0x76:v=a>>(b&31);break;case 0x6d:if(!b)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_DIVIDE_BY_ZERO);if(a==0x80000000u&&b==UINT32_MAX)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INTEGER_OVERFLOW);v=(uint32_t)((int32_t)a/(int32_t)b);break;case 0x6e:if(!b)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_DIVIDE_BY_ZERO);v=a/b;break;default:r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}p[sp++]=v;break;}
        if(in.op>=0x7c && in.op<=0x8a){uint64_t b=p[--sp],a=p[--sp],v=0;switch(in.op){case 0x7c:v=a+b;break;case 0x7d:v=a-b;break;case 0x7e:v=a*b;break;case 0x83:v=a&b;break;case 0x84:v=a|b;break;case 0x85:v=a^b;break;default:r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}p[sp++]=v;break;}
        r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;
    }
    diagnostic_record_sp(r,fi,pc,sp,H_OPERANDS+fn->nlocals);
    p[H_SP]=sp;
  }
}

static GRCORE_Step execute(GRCORE_Context *ctx, void *opaque) {
  Run *run = opaque;
  GRCORE_Step step = execute_impl(ctx, opaque);
  if (run) {
    run->diagnostic_step = (uint8_t)step;
    run->diagnostic_context_state = (uint8_t)grcore_context_state(ctx);
  }
  return step;
}

/* ------------------------------------------------------------------ */
/* The store                                                          */
/* ------------------------------------------------------------------ */

/* Each kind of store object lives in a growable array of pointers, so an
 * object's address never moves once handed out. */
#define STORE_ADD(kind, type, field, count, capacity)                       \
  static int store_add_##kind(GLWASM_Store *store, type *object,           \
      uint32_t *out_address) {                                             \
    if (store->count == store->capacity) {                                 \
      if (store->capacity >= UINT32_MAX / 2u) return 0;                    \
      uint32_t next = store->capacity ? store->capacity * 2u : 16u;        \
      type **grown = gcu_allocator_calloc(store->alloc, next,              \
          sizeof(*grown));                                                 \
      if (!grown) return 0;                                                \
      if (store->count)                                                    \
        memcpy(grown, store->field, store->count * sizeof(*grown));        \
      gcu_allocator_free(store->alloc, store->field);                      \
      store->field = grown;                                                \
      store->capacity = next;                                              \
    }                                                                      \
    store->field[store->count] = object;                                   \
    *out_address = store->count++;                                         \
    return 1;                                                              \
  }
STORE_ADD(func, StoreFunc, funcs, nfuncs, cfuncs)
STORE_ADD(table, StoreTable, tables, ntables, ctables)
STORE_ADD(memory, StoreMemory, memories, nmemories, cmemories)
STORE_ADD(global, StoreGlobal, globals, nglobals, cglobals)
STORE_ADD(tag, StoreTag, tags, ntags, ctags)
STORE_ADD(instance, GLWASM_Runtime, instances, ninstances, cinstances)
#undef STORE_ADD

static void free_store_func(GLWASM_Store *store, StoreFunc *func) {
  if (!func) return;
  gcu_allocator_free(store->alloc, func->host.parameter_types);
  gcu_allocator_free(store->alloc, func->host.result_types);
  gcu_allocator_free(store->alloc, func->host_type.params);
  gcu_allocator_free(store->alloc, func->host_type.results);
  gcu_allocator_free(store->alloc, func);
}

static void free_store_memory(GLWASM_Store *store, StoreMemory *memory) {
  if (!memory) return;
  if (!memory->hosted) gcu_allocator_free(store->guest_alloc, memory->bytes);
  gcu_allocator_free(store->alloc, memory);
}

static void free_store_table(GLWASM_Store *store, StoreTable *table) {
  if (!table) return;
  if (!table->hosted) gcu_allocator_free(store->guest_alloc, table->elements);
  gcu_allocator_free(store->alloc, table);
}

static void free_store_tag(GLWASM_Store *store, StoreTag *tag) {
  if (!tag) return;
  gcu_allocator_free(store->alloc, tag->params);
  gcu_allocator_free(store->alloc, tag);
}

/* Free an instance and what it owns that is not a store object: its index
 * arrays, element instances, and JIT state in every context it ran in. */
static void instance_free(GLWASM_Runtime *r) {
  if (!r) return;
  GLWASM_Store *store = r->store;
  GLWASM_Module *module = r->module;
  if (store->boundary_context && store->boundary_context != store->context) {
    clear_jit_context_state(module, store->boundary_context,
        &r->boundary_jit_slots, &r->boundary_jit_registered,
        &r->boundary_jit_reservation);
    release_debug_names(store->boundary_context, module->debug_id,
        store->boundary_engine);
  }
  clear_jit_context_state(module, store->context, &r->jit_slots,
      &r->jit_registered, &r->jit_reservation);
  gcu_allocator_free(store->alloc, r->funcs);
  gcu_allocator_free(store->alloc, r->tables);
  gcu_allocator_free(store->alloc, r->memories);
  gcu_allocator_free(store->alloc, r->globals);
  gcu_allocator_free(store->alloc, r->tags);
  gcu_allocator_free(store->alloc, r->data_dropped);
  if (r->elem_values)
    for (uint32_t i = 0; i < module->nelems; i++)
      gcu_allocator_free(store->alloc, r->elem_values[i]);
  gcu_allocator_free(store->alloc, r->elem_values);
  gcu_allocator_free(store->alloc, r->elem_counts);
  if (r->memory_debug_names) {
    for (uint32_t i = 0; i < module->nmems; i++)
      gcu_allocator_free(&module->alloc, r->memory_debug_names[i]);
    gcu_allocator_free(&module->alloc, r->memory_debug_names);
  }
  gcu_allocator_free(&module->alloc, r->test_observed_functions);
  for (size_t i = 0; i < r->jit_spill_count; ++i)
    gcu_allocator_free(&module->alloc, r->jit_spills[i]);
  gcu_allocator_free(&module->alloc, r->jit_spills);
  gcu_allocator_free(&module->alloc, r->jit_spill_capacities);
  gcu_allocator_free(&module->alloc, r->jit_deopt_cells);
  gcu_allocator_free(store->alloc, r);
  glwasm_module_destroy(module);
}

GLWASM_Result glwasm_store_create(const GLWASM_Policy *policy,
    GLWASM_Store **out_store) {
  if (!out_store) return GLWASM_ERR_INVALID;
  *out_store = NULL;
  const GCU_Allocator *alloc = gcu_allocator_default();
  GLWASM_Store *store = gcu_allocator_calloc(alloc, 1, sizeof(*store));
  if (!store) return GLWASM_ERR_OOM;
  store->alloc = alloc;
  if (policy) store->policy = *policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &store->policy);
  GRCORE_Result core = grcore_group_create(NULL, NULL, &store->group);
  if (core == GRCORE_OK) core = grcore_options_create(NULL, &store->options);
  if (core == GRCORE_OK)
    core = grcore_options_set_fuel(store->options, store->policy.default_fuel);
  if (core == GRCORE_OK)
    core = grcore_options_set_memory_bytes(store->options,
        store->policy.default_memory_bytes);
  if (core == GRCORE_OK)
    core = grcore_options_set_guest_depth(store->options,
        store->policy.default_guest_depth);
  if (core == GRCORE_OK)
    core = grcore_options_set_native_depth(store->options,
        store->policy.default_native_depth);
  if (core == GRCORE_OK)
    core = grcore_options_set_native_stack_bytes(store->options,
        store->policy.default_native_stack_bytes);
  if (core == GRCORE_OK)
    core = grcore_context_create(store->group, store->options,
        &store->context);
  if (core == GRCORE_OK)
    core = grcore_engine_register(store->context, &descriptor, &store->engine);
  if (core == GRCORE_OK) {
    store->guest_alloc = grcore_context_allocator(store->context);
    store->run = (Run){.store = store, .ctx = store->context,
        .engine = store->engine, .guest_alloc = store->guest_alloc,
        .last_jit_lower_pc = UINT32_MAX};
    core = grcore_context_register(store->context, &trap_key, &store->run);
  }
  if (core != GRCORE_OK) {
    glwasm_store_destroy(store);
    return map_core_result(core);
  }
  *out_store = store;
  return GLWASM_OK;
}

void glwasm_store_destroy(GLWASM_Store *store) {
  if (!store) return;
  free_exceptions(store);
  for (uint32_t i = 0; i < store->ninstances; i++)
    instance_free(store->instances[i]);
  for (uint32_t i = 0; i < store->nfuncs; i++)
    free_store_func(store, store->funcs[i]);
  for (uint32_t i = 0; i < store->nmemories; i++)
    free_store_memory(store, store->memories[i]);
  for (uint32_t i = 0; i < store->ntables; i++)
    free_store_table(store, store->tables[i]);
  for (uint32_t i = 0; i < store->nglobals; i++)
    gcu_allocator_free(store->alloc, store->globals[i]);
  for (uint32_t i = 0; i < store->ntags; i++)
    free_store_tag(store, store->tags[i]);
  if (store->context) grcore_context_destroy(store->context);
  if (store->group) grcore_group_destroy(store->group);
  if (store->options) grcore_options_destroy(store->options);
  gcu_allocator_free(store->alloc, store->instances);
  gcu_allocator_free(store->alloc, store->funcs);
  gcu_allocator_free(store->alloc, store->memories);
  gcu_allocator_free(store->alloc, store->tables);
  gcu_allocator_free(store->alloc, store->globals);
  gcu_allocator_free(store->alloc, store->tags);
  gcu_allocator_free(store->alloc, store->run.args);
  gcu_allocator_free(store->alloc, store);
}

GRCORE_Context *glwasm_store_context(const GLWASM_Store *store) {
  return store ? store->context : NULL;
}

GLWASM_Result glwasm_store_set_fuel(GLWASM_Store *store, uint64_t fuel) {
  if (!store) return GLWASM_ERR_INVALID;
  return map_core_result(grcore_context_set_fuel(store->context, fuel));
}

GLWASM_Store *glwasm_runtime_store(const GLWASM_Runtime *runtime) {
  return runtime ? runtime->store : NULL;
}

GRCORE_Context *glwasm_runtime_context(const GLWASM_Runtime *runtime) {
  return runtime ? runtime->store->context : NULL;
}

GLWASM_Trap glwasm_store_last_trap(const GLWASM_Store *store) {
  return store ? store->run.trap : GLWASM_TRAP_NONE;
}

/* ------------------------------------------------------------------ */
/* Instantiation                                                       */
/* ------------------------------------------------------------------ */

/* A constant expression, evaluated in `r` with the globals and functions
 * it has so far. Validation fixed the shape; this only computes. */
static int const_eval(const GLWASM_Runtime *r, const GLWASM_Func *expr,
    uint64_t *out) {
  uint64_t stack[64];
  size_t sp = 0;
  for (uint32_t i = 0; i < expr->nins; i++) {
    const GLWASM_Instr *x = &expr->ins[i];
    uint64_t a, b;
    switch (x->op) {
      case 0x0b:
        if (sp != 1) return 0;
        *out = stack[0];
        return 1;
      case 0x41: if (sp == 64) return 0; stack[sp++] = (uint32_t)x->c; break;
      case 0x42: if (sp == 64) return 0; stack[sp++] = x->c; break;
      case 0x43: if (sp == 64) return 0; stack[sp++] = x->a; break;
      case 0x44: if (sp == 64) return 0; stack[sp++] = x->c; break;
      case 0xd0: if (sp == 64) return 0; stack[sp++] = 0; break;
      case 0xd2:
        if (sp == 64 || x->a >= r->module->nfuncs) return 0;
        stack[sp++] = instance_funcref(r, x->a);
        break;
      case 0x23:
        if (sp == 64 || x->a >= r->module->nglobals || !r->globals[x->a])
          return 0;
        stack[sp++] = global_read(r->globals[x->a]);
        break;
      case 0x6a: case 0x6b: case 0x6c: case 0x7c: case 0x7d: case 0x7e:
        if (sp < 2) return 0;
        b = stack[--sp]; a = stack[--sp];
        switch (x->op) {
          case 0x6a: stack[sp++] = (uint32_t)((uint32_t)a + (uint32_t)b); break;
          case 0x6b: stack[sp++] = (uint32_t)((uint32_t)a - (uint32_t)b); break;
          case 0x6c: stack[sp++] = (uint32_t)((uint32_t)a * (uint32_t)b); break;
          case 0x7c: stack[sp++] = a + b; break;
          case 0x7d: stack[sp++] = a - b; break;
          default: stack[sp++] = a * b; break;
        }
        break;
      default:
        return 0;
    }
  }
  return 0;
}

static int types_equal(const GLWASM_TypeDef *a, const GLWASM_TypeDef *b) {
  return same_function_type(a, b);
}

/* Spec import matching for limits: the object's current size reaches the
 * import's minimum, and an import maximum needs an object maximum no
 * larger. */
static int limits_match(uint64_t size, uint8_t has_max, uint64_t max,
    uint64_t import_min, uint8_t import_has_max, uint64_t import_max) {
  if (size < import_min) return 0;
  if (!import_has_max) return 1;
  return has_max && max <= import_max;
}

static int host_types_copy(const GCU_Allocator *alloc,
    const GLWASM_HostValueType *from, uint32_t count,
    GLWASM_HostValueType **out) {
  *out = NULL;
  if (!count) return 1;
  if (!from) return 0;
  *out = gcu_allocator_calloc(alloc, count, sizeof(**out));
  if (!*out) return 0;
  memcpy(*out, from, count * sizeof(**out));
  return 1;
}

/* A host function's declared type matches the import's type exactly. */
static int host_signature_matches(const GLWASM_HostImport *binding,
    const GLWASM_TypeDef *type) {
  if (binding->parameter_count != type->nparams ||
      binding->result_count != type->nresults) return 0;
  if ((type->nparams && !binding->parameters) ||
      (type->nresults && !binding->results)) return 0;
  for (uint32_t i = 0; i < type->nparams; i++)
    if (!host_value_type(type->params[i]) ||
        binding->parameters[i] != host_value_type(type->params[i])) return 0;
  for (uint32_t i = 0; i < type->nresults; i++)
    if (!host_value_type(type->results[i]) ||
        binding->results[i] != host_value_type(type->results[i])) return 0;
  return 1;
}

/* Make the store function for a host function import, typed as declared. */
static StoreFunc *host_function_object(GLWASM_Store *store,
    const GLWASM_HostImport *binding, const GLWASM_TypeDef *type) {
  StoreFunc *f = gcu_allocator_calloc(store->alloc, 1, sizeof(*f));
  if (!f) return NULL;
  f->host.function = binding->function;
  f->host.resume = binding->resume;
  f->host.function_results = binding->function_results;
  f->host.resume_results = binding->resume_results;
  f->host.user = binding->user;
  f->host_type.nparams = type->nparams;
  f->host_type.nresults = type->nresults;
  int ok = host_types_copy(store->alloc, binding->parameters, type->nparams,
          &f->host.parameter_types) &&
      host_types_copy(store->alloc, binding->results, type->nresults,
          &f->host.result_types);
  if (ok && type->nparams) {
    f->host_type.params = gcu_allocator_calloc(store->alloc, type->nparams,
        sizeof(*f->host_type.params));
    ok = f->host_type.params != NULL;
    if (ok) memcpy(f->host_type.params, type->params,
        type->nparams * sizeof(*type->params));
  }
  if (ok && type->nresults) {
    f->host_type.results = gcu_allocator_calloc(store->alloc, type->nresults,
        sizeof(*f->host_type.results));
    ok = f->host_type.results != NULL;
    if (ok) memcpy(f->host_type.results, type->results,
        type->nresults * sizeof(*type->results));
  }
  if (!ok) { free_store_func(store, f); return NULL; }
  f->type = &f->host_type;
  return f;
}

/* Host tags with one identity and one payload type are one tag object. */
static StoreTag *host_tag_object(GLWASM_Store *store, uint64_t identity,
    const GLWASM_TypeDef *type, uint32_t *address, int *created) {
  *created = 0;
  for (uint32_t i = 0; i < store->ntags; i++) {
    StoreTag *tag = store->tags[i];
    if (tag->host_identity != identity || tag->nparams != type->nparams)
      continue;
    int same = 1;
    for (uint32_t j = 0; same && j < type->nparams; j++)
      same = same_value_type(tag->params[j], type->params[j]);
    if (same) { *address = i; return tag; }
  }
  StoreTag *tag = gcu_allocator_calloc(store->alloc, 1, sizeof(*tag));
  if (!tag) return NULL;
  tag->host_identity = identity;
  tag->nparams = type->nparams;
  if (type->nparams) {
    tag->params = gcu_allocator_calloc(store->alloc, type->nparams,
        sizeof(*tag->params));
    if (!tag->params) { free_store_tag(store, tag); return NULL; }
    memcpy(tag->params, type->params, type->nparams * sizeof(*type->params));
  }
  if (!store_add_tag(store, tag, address)) {
    free_store_tag(store, tag);
    return NULL;
  }
  *created = 1;
  return tag;
}

static const GLWASM_HostImport *find_binding(const GLWASM_Import *import,
    const GLWASM_HostImport *imports, size_t import_count) {
  for (size_t j = 0; j < import_count; j++)
    if (imports[j].module && imports[j].field &&
        strlen(imports[j].module) == import->module_name.length &&
        strlen(imports[j].field) == import->field.length &&
        !memcmp(imports[j].module, import->module_name.bytes,
            import->module_name.length) &&
        !memcmp(imports[j].field, import->field.bytes, import->field.length))
      return &imports[j];
  return NULL;
}

/* What a failed link reports. */
static GLWASM_Result link_failure(GLWASM_InstantiateError *error,
    const GLWASM_Import *import, GLWASM_LinkFailure reason) {
  if (error) {
    error->stage = GLWASM_INSTANTIATE_LINK;
    error->link.module = import ? import->module_name.bytes : NULL;
    error->link.field = import ? import->field.bytes : NULL;
    error->link.reason = reason;
  }
  return GLWASM_ERR_LINK;
}

/* Undo an instantiation that failed before it could have any effect: free
 * the instance and every store object made for it. */
static void instantiate_rollback(GLWASM_Store *store, GLWASM_Runtime *r,
    uint32_t funcs, uint32_t tables, uint32_t memories, uint32_t globals,
    uint32_t tags) {
  while (store->nfuncs > funcs) free_store_func(store, store->funcs[--store->nfuncs]);
  while (store->ntables > tables) free_store_table(store, store->tables[--store->ntables]);
  while (store->nmemories > memories)
    free_store_memory(store, store->memories[--store->nmemories]);
  while (store->nglobals > globals)
    gcu_allocator_free(store->alloc, store->globals[--store->nglobals]);
  while (store->ntags > tags) free_store_tag(store, store->tags[--store->ntags]);
  instance_free(r);
}

static void *instance_array(GLWASM_Store *store, uint32_t count,
    size_t size, int *ok) {
  if (!count) return NULL;
  void *array = gcu_allocator_calloc(store->alloc, count, size);
  if (!array) *ok = 0;
  return array;
}

/* Link one import to its object, or report why it does not link. */
static GLWASM_Result link_import(GLWASM_Store *store, GLWASM_Runtime *r,
    const GLWASM_Import *import, const GLWASM_HostImport *binding,
    GLWASM_InstantiateError *error) {
  GLWASM_Module *m = r->module;
  const uint8_t kind = import->kind;
  const int linked = binding->kind == GLWASM_HOST_IMPORT_EXTERN;
  if (linked) {
    const GLWASM_Extern *object = &binding->resource.linked;
    const uint32_t count = object->kind == GLWASM_EXTERN_FUNC ? store->nfuncs
        : object->kind == GLWASM_EXTERN_TABLE ? store->ntables
        : object->kind == GLWASM_EXTERN_MEMORY ? store->nmemories
        : object->kind == GLWASM_EXTERN_GLOBAL ? store->nglobals
        : object->kind == GLWASM_EXTERN_TAG ? store->ntags : 0;
    if (object->store != store || object->address >= count)
      return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
    if (object->kind != kind)
      return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
  }
  switch (kind) {
    case GLWASM_EXT_FUNC: {
      const GLWASM_TypeDef *type = &m->types[m->funcs[import->index].type_index];
      if (linked) {
        uint32_t address = binding->resource.linked.address;
        if (!types_equal(store->funcs[address]->type, type))
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        r->funcs[import->index] = address;
        return GLWASM_OK;
      }
      if (binding->kind != GLWASM_HOST_IMPORT_FUNCTION ||
          (!binding->function && !binding->function_results))
        return link_failure(error, import, binding->kind ==
            GLWASM_HOST_IMPORT_FUNCTION ? GLWASM_LINK_INVALID_BINDING
                                        : GLWASM_LINK_INCOMPATIBLE);
      if (!host_signature_matches(binding, type) ||
          (type->nresults > 1 && !binding->function_results))
        return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      StoreFunc *f = host_function_object(store, binding, type);
      uint32_t address;
      if (!f) return GLWASM_ERR_OOM;
      if (!store_add_func(store, f, &address)) {
        free_store_func(store, f);
        return GLWASM_ERR_OOM;
      }
      r->funcs[import->index] = address;
      return GLWASM_OK;
    }
    case GLWASM_EXT_TABLE: {
      const GLWASM_Table *decl = &m->tables[import->index];
      StoreTable *table;
      if (linked) {
        table = store->tables[binding->resource.linked.address];
      } else {
        const GLWASM_HostTableProvider *provider = binding->resource.table;
        if (binding->kind != GLWASM_HOST_IMPORT_TABLE)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (!provider || !provider->begin || !provider->end || !provider->grow)
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        if (provider->element_type != host_value_type(decl->elem))
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (decl->elem.heap != GLWASM_HT_FUNC &&
            decl->elem.heap != GLWASM_HT_NOFUNC && !provider->validate_token)
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        table = gcu_allocator_calloc(store->alloc, 1, sizeof(*table));
        if (!table) return GLWASM_ERR_OOM;
        table->hosted = 1;
        table->provider = *provider;
        table->elem = decl->elem;
        table->has_max = provider->has_maximum;
        table->max = provider->maximum;
        table->addr64 = provider->address64;
        uint32_t address;
        if (!store_add_table(store, table, &address)) {
          free_store_table(store, table);
          return GLWASM_ERR_OOM;
        }
      }
      TableLease lease;
      if (!table_lease(table, &lease))
        return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
      const uint64_t length = lease.view.length;
      table_release(&lease);
      if (!same_value_type(table->elem, decl->elem) ||
          table->addr64 != decl->addr64 ||
          !limits_match(length, table->has_max, table->max, decl->min,
              decl->has_max, decl->max))
        return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      r->tables[import->index] = table;
      return GLWASM_OK;
    }
    case GLWASM_EXT_MEM: {
      const GLWASM_Memory *decl = &m->mems[import->index];
      StoreMemory *memory;
      if (linked) {
        memory = store->memories[binding->resource.linked.address];
      } else {
        const GLWASM_HostMemoryProvider *provider = binding->resource.memory;
        if (binding->kind != GLWASM_HOST_IMPORT_MEMORY)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (!provider || !provider->begin || !provider->end || !provider->grow)
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        memory = gcu_allocator_calloc(store->alloc, 1, sizeof(*memory));
        if (!memory) return GLWASM_ERR_OOM;
        memory->hosted = 1;
        memory->provider = *provider;
        memory->has_max = provider->has_maximum;
        memory->max = provider->maximum_pages;
        memory->addr64 = provider->address64;
        uint32_t address;
        if (!store_add_memory(store, memory, &address)) {
          free_store_memory(store, memory);
          return GLWASM_ERR_OOM;
        }
      }
      MemoryLease lease;
      if (!memory_lease(memory, &lease))
        return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
      const uint64_t pages = memory->pages;
      memory_release(&lease);
      if (memory->addr64 != decl->addr64 ||
          !limits_match(pages, memory->has_max, memory->max, decl->min,
              decl->has_max, decl->max))
        return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      r->memories[import->index] = memory;
      return GLWASM_OK;
    }
    case GLWASM_EXT_GLOBAL: {
      const GLWASM_Global *decl = &m->globals[import->index];
      StoreGlobal *global;
      if (linked) {
        global = store->globals[binding->resource.linked.address];
        if (!same_value_type(global->type, decl->type) ||
            global->mut != decl->mut)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      } else {
        GLWASM_HostGlobal *host = binding->resource.global;
        if (binding->kind != GLWASM_HOST_IMPORT_GLOBAL)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (!host || !host->value)
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        if (host->type != host_value_type(decl->type) ||
            (host->mutable_ != 0) != (decl->mut != 0))
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (decl->type.kind == GLWASM_VK_REF &&
            decl->type.heap != GLWASM_HT_FUNC &&
            decl->type.heap != GLWASM_HT_NOFUNC && !host->validate_token)
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        global = gcu_allocator_calloc(store->alloc, 1, sizeof(*global));
        if (!global) return GLWASM_ERR_OOM;
        global->type = decl->type;
        global->mut = decl->mut;
        global->host = host;
        uint32_t address;
        if (!store_add_global(store, global, &address)) {
          gcu_allocator_free(store->alloc, global);
          return GLWASM_ERR_OOM;
        }
        if (!global_value_valid(store, global, global_read(global)))
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
      }
      r->globals[import->index] = global;
      return GLWASM_OK;
    }
    case GLWASM_EXT_TAG: {
      const GLWASM_Tag *decl = &m->tags[import->index];
      const GLWASM_TypeDef *type = &m->types[decl->type_index];
      StoreTag *tag;
      if (linked) {
        tag = store->tags[binding->resource.linked.address];
      } else {
        const GLWASM_HostTag *host = binding->resource.tag;
        if (binding->kind != GLWASM_HOST_IMPORT_TAG)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        if (!host || !host->identity ||
            (host->parameter_count && !host->parameters))
          return link_failure(error, import, GLWASM_LINK_INVALID_BINDING);
        if (host->parameter_count != type->nparams)
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        for (uint32_t i = 0; i < type->nparams; i++)
          if (host->parameters[i] != host_value_type(type->params[i]))
            return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
        uint32_t address;
        int created;
        tag = host_tag_object(store, host->identity, type, &address, &created);
        if (!tag) return GLWASM_ERR_OOM;
      }
      if (tag->nparams != type->nparams)
        return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      for (uint32_t i = 0; i < type->nparams; i++)
        if (!same_value_type(tag->params[i], type->params[i]))
          return link_failure(error, import, GLWASM_LINK_INCOMPATIBLE);
      r->tags[import->index] = tag;
      return GLWASM_OK;
    }
    default:
      return link_failure(error, import, GLWASM_LINK_UNSUPPORTED_KIND);
  }
}

/* table.init for an active segment at instantiation: bounds first, then
 * the copy, so an out-of-range segment writes nothing. */
static GLWASM_Trap apply_element_segment(GLWASM_Runtime *r, uint32_t index) {
  const GLWASM_Elem *e = &r->module->elems[index];
  uint64_t offset = 0;
  if (!const_eval(r, &e->offset, &offset)) return GLWASM_TRAP_UNREACHABLE;
  StoreTable *table = r->tables[e->table];
  if (!r->module->tables[e->table].addr64) offset = (uint32_t)offset;
  TableLease lease;
  if (!table_lease(table, &lease)) return GLWASM_TRAP_TABLE_OUT_OF_BOUNDS;
  const uint64_t count = r->elem_counts[index];
  if (offset > lease.view.length || count > lease.view.length - offset) {
    table_release(&lease);
    return GLWASM_TRAP_TABLE_OUT_OF_BOUNDS;
  }
  if (count)
    memcpy(lease.view.elements + offset, r->elem_values[index],
        (size_t)count * sizeof(uint64_t));
  table_release(&lease);
  return GLWASM_TRAP_NONE;
}

static GLWASM_Trap apply_data_segment(GLWASM_Runtime *r, uint32_t index) {
  const GLWASM_Data *d = &r->module->datas[index];
  uint64_t offset = 0;
  if (!const_eval(r, &d->offset, &offset)) return GLWASM_TRAP_UNREACHABLE;
  StoreMemory *memory = r->memories[d->mem];
  if (!memory->addr64) offset = (uint32_t)offset;
  MemoryLease lease;
  if (!memory_lease(memory, &lease)) return GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
  if (offset > lease.view.length_bytes ||
      d->nbytes > lease.view.length_bytes - offset) {
    memory_release(&lease);
    return GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS;
  }
  if (d->nbytes) memcpy(lease.view.bytes + offset, d->bytes, d->nbytes);
  memory_release(&lease);
  return GLWASM_TRAP_NONE;
}

GLWASM_Result glwasm_store_instantiate(GLWASM_Store *store,
    GLWASM_Module *m, const GLWASM_HostImport *imports, size_t import_count,
    int allow_start, GLWASM_InstantiateError *error,
    GLWASM_Runtime **out_runtime) {
  if (out_runtime) *out_runtime = NULL;
  if (error) memset(error, 0, sizeof(*error));
  if (!store || !m || !out_runtime || (import_count && !imports))
    return GLWASM_ERR_INVALID;
  if (store->run.active) return GLWASM_ERR_INVALID;
  if (m->has_start && !allow_start) {
    if (error) {
      error->stage = GLWASM_INSTANTIATE_LINK;
      error->link.reason = GLWASM_LINK_START_NOT_ALLOWED;
    }
    return GLWASM_ERR_LINK;
  }
  const uint32_t saved_funcs = store->nfuncs, saved_tables = store->ntables,
      saved_memories = store->nmemories, saved_globals = store->nglobals,
      saved_tags = store->ntags;
  GLWASM_Runtime *r = gcu_allocator_calloc(store->alloc, 1, sizeof(*r));
  if (!r) return GLWASM_ERR_OOM;
  r->store = store;
  r->module = m;
  glwasm_module_retain(m);
  r->boundary_token = atomic_fetch_add_explicit(&next_boundary_token, 1,
      memory_order_relaxed);
  if (!r->boundary_token)
    r->boundary_token = atomic_fetch_add_explicit(&next_boundary_token,
        1, memory_order_relaxed);
  GLWASM_Policy module_policy;
  if (m->has_policy) module_policy = m->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &module_policy);
  r->jit_mode = module_policy.max_jit_code_bytes ? GLWASM_TEST_JIT_AUTO
                                                 : GLWASM_TEST_JIT_INTERPRETER;
  int ok = 1;
  r->funcs = instance_array(store, m->nfuncs, sizeof(*r->funcs), &ok);
  r->tables = instance_array(store, m->ntables, sizeof(*r->tables), &ok);
  r->memories = instance_array(store, m->nmems, sizeof(*r->memories), &ok);
  r->globals = instance_array(store, m->nglobals, sizeof(*r->globals), &ok);
  r->tags = instance_array(store, m->ntags, sizeof(*r->tags), &ok);
  r->data_dropped = instance_array(store, m->ndatas, 1, &ok);
  r->elem_values = instance_array(store, m->nelems, sizeof(*r->elem_values), &ok);
  r->elem_counts = instance_array(store, m->nelems, sizeof(*r->elem_counts), &ok);
  GLWASM_Result result = ok ? GLWASM_OK : GLWASM_ERR_OOM;
  if (result == GLWASM_OK)
    result = create_jit_slots(m, store->context, &r->jit_slots);
  if (result == GLWASM_OK && m->nfuncs) {
    r->jit_registered = gcu_allocator_calloc(&m->alloc, m->nfuncs,
        sizeof(*r->jit_registered));
    if (!r->jit_registered) result = GLWASM_ERR_OOM;
  }
  if (result == GLWASM_OK)
    result = register_jit_runtime(r, store->context, &r->jit_reservation);
  if (result == GLWASM_OK)
    result = ensure_debug_names(store->context, m, store->engine);
  if (result != GLWASM_OK) {
    instantiate_rollback(store, r, saved_funcs, saved_tables, saved_memories,
        saved_globals, saved_tags);
    return result;
  }

  /* 1. Link every import, in order. */
  for (uint32_t i = 0; i < m->nimports; i++) {
    const GLWASM_Import *import = &m->imports[i];
    if (import->module_name.length >= 5u &&
        !memcmp(import->module_name.bytes, "tang.", 5u)) {
      int allowed = m->has_policy && m->policy.allow_tang_v1_imports &&
          import->module_name.length == 7u &&
          !memcmp(import->module_name.bytes, "tang.v1", 7u);
      if (!allowed) {
        instantiate_rollback(store, r, saved_funcs, saved_tables,
            saved_memories, saved_globals, saved_tags);
        return link_failure(error, import, GLWASM_LINK_UNRESOLVED);
      }
    }
    const GLWASM_HostImport *binding = find_binding(import, imports,
        import_count);
    if (!binding) {
      GLWASM_Result failed = link_failure(error, import,
          GLWASM_LINK_UNRESOLVED);
      instantiate_rollback(store, r, saved_funcs, saved_tables,
          saved_memories, saved_globals, saved_tags);
      return failed;
    }
    result = link_import(store, r, import, binding, error);
    if (result != GLWASM_OK) {
      instantiate_rollback(store, r, saved_funcs, saved_tables,
          saved_memories, saved_globals, saved_tags);
      return result;
    }
  }

  /* 2. Allocate what the module defines, in index order: functions,
   * tables, memories, globals (each initializer sees the globals before
   * it), tags, then each segment's element instance. */
  for (uint32_t i = 0; result == GLWASM_OK && i < m->nfuncs; i++) {
    if (m->funcs[i].imported) continue;
    StoreFunc *f = gcu_allocator_calloc(store->alloc, 1, sizeof(*f));
    uint32_t address;
    if (!f) { result = GLWASM_ERR_OOM; break; }
    f->instance = r;
    f->index = i;
    f->type = &m->types[m->funcs[i].type_index];
    if (!store_add_func(store, f, &address)) {
      free_store_func(store, f);
      result = GLWASM_ERR_OOM;
      break;
    }
    r->funcs[i] = address;
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < m->nmems; i++) {
    if (m->mems[i].imported) continue;
    StoreMemory *memory = gcu_allocator_calloc(store->alloc, 1,
        sizeof(*memory));
    uint32_t address;
    if (!memory) { result = GLWASM_ERR_OOM; break; }
    memory->has_max = m->mems[i].has_max;
    memory->max = m->mems[i].max;
    memory->addr64 = m->mems[i].addr64;
    if (!store_add_memory(store, memory, &address)) {
      free_store_memory(store, memory);
      result = GLWASM_ERR_OOM;
      break;
    }
    r->memories[i] = memory;
    if (!memory_object_grow(store, memory, m->mems[i].min,
        m->limits.memory_maximum, NULL))
      result = GLWASM_ERR_LIMIT;
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < m->nglobals; i++) {
    if (m->globals[i].imported) continue;
    StoreGlobal *global = gcu_allocator_calloc(store->alloc, 1,
        sizeof(*global));
    uint32_t address;
    if (!global) { result = GLWASM_ERR_OOM; break; }
    global->type = m->globals[i].type;
    global->mut = m->globals[i].mut;
    if (!const_eval(r, &m->globals[i].init, &global->value)) {
      gcu_allocator_free(store->alloc, global);
      result = GLWASM_ERR_INTERNAL;
      break;
    }
    if (!store_add_global(store, global, &address)) {
      gcu_allocator_free(store->alloc, global);
      result = GLWASM_ERR_OOM;
      break;
    }
    r->globals[i] = global;
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < m->ntables; i++) {
    if (m->tables[i].imported) continue;
    const GLWASM_Table *decl = &m->tables[i];
    StoreTable *table = gcu_allocator_calloc(store->alloc, 1, sizeof(*table));
    uint32_t address;
    uint64_t init = 0;
    if (!table) { result = GLWASM_ERR_OOM; break; }
    table->elem = decl->elem;
    table->has_max = decl->has_max;
    table->max = decl->max;
    table->addr64 = decl->addr64;
    if (!store_add_table(store, table, &address)) {
      free_store_table(store, table);
      result = GLWASM_ERR_OOM;
      break;
    }
    r->tables[i] = table;
    if (decl->has_init && !const_eval(r, &decl->init, &init)) {
      result = GLWASM_ERR_INTERNAL;
      break;
    }
    if (!table_object_grow(store, table, decl->min, init, NULL))
      result = GLWASM_ERR_LIMIT;
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < m->ntags; i++) {
    if (m->tags[i].imported) continue;
    const GLWASM_TypeDef *type = &m->types[m->tags[i].type_index];
    StoreTag *tag = gcu_allocator_calloc(store->alloc, 1, sizeof(*tag));
    uint32_t address;
    if (!tag) { result = GLWASM_ERR_OOM; break; }
    tag->nparams = type->nparams;
    if (type->nparams) {
      tag->params = gcu_allocator_calloc(store->alloc, type->nparams,
          sizeof(*tag->params));
      if (!tag->params) { free_store_tag(store, tag); result = GLWASM_ERR_OOM; break; }
      memcpy(tag->params, type->params, type->nparams * sizeof(*type->params));
    }
    if (!store_add_tag(store, tag, &address)) {
      free_store_tag(store, tag);
      result = GLWASM_ERR_OOM;
      break;
    }
    r->tags[i] = tag;
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < m->nelems; i++) {
    const GLWASM_Elem *e = &m->elems[i];
    const uint32_t count = e->use_exprs ? e->nexprs : e->nfuncs;
    if (!count) continue;
    r->elem_values[i] = gcu_allocator_calloc(store->alloc, count,
        sizeof(uint64_t));
    if (!r->elem_values[i]) { result = GLWASM_ERR_OOM; break; }
    r->elem_counts[i] = count;
    for (uint32_t j = 0; j < count; j++) {
      if (!e->use_exprs) r->elem_values[i][j] = instance_funcref(r, e->funcs[j]);
      else if (!const_eval(r, &e->exprs[j], &r->elem_values[i][j])) {
        result = GLWASM_ERR_INTERNAL;
        break;
      }
    }
  }
  uint32_t instance_address = 0;
  if (result == GLWASM_OK && !store_add_instance(store, r, &instance_address))
    result = GLWASM_ERR_OOM;
  if (result != GLWASM_OK) {
    if (error) error->stage = GLWASM_INSTANTIATE_ALLOCATE;
    instantiate_rollback(store, r, saved_funcs, saved_tables, saved_memories,
        saved_globals, saved_tags);
    return result;
  }

  /* 3. From here an instantiation has effects others can see, so the
   * instance stays in the store whatever happens. Active element segments
   * in order, then active data segments in order; each applied or
   * declarative segment is dropped. */
  for (uint32_t i = 0; i < m->nelems; i++) {
    const GLWASM_Elem *e = &m->elems[i];
    if (e->mode == GLWASM_ELEM_PASSIVE) continue;
    if (e->mode == GLWASM_ELEM_ACTIVE) {
      GLWASM_Trap trap = apply_element_segment(r, i);
      if (trap != GLWASM_TRAP_NONE) {
        if (error) { error->stage = GLWASM_INSTANTIATE_SEGMENTS; error->trap = trap; }
        store->run.trap = trap;
        return GLWASM_ERR_GUEST;
      }
    }
    elem_drop(r, i);
  }
  for (uint32_t i = 0; i < m->ndatas; i++) {
    if (m->datas[i].passive) continue;
    GLWASM_Trap trap = apply_data_segment(r, i);
    if (trap != GLWASM_TRAP_NONE) {
      if (error) { error->stage = GLWASM_INSTANTIATE_SEGMENTS; error->trap = trap; }
      store->run.trap = trap;
      return GLWASM_ERR_GUEST;
    }
    r->data_dropped[i] = 1;
  }

  /* 4. The start function. */
  if (m->has_start) {
    uint64_t results[1];
    uint32_t count = 0;
    GLWASM_Result started = glwasm_runtime_invoke_results(r, m->start, NULL,
        0, results, &count);
    if (started != GLWASM_OK) {
      if (error) {
        error->stage = GLWASM_INSTANTIATE_START;
        error->trap = store->run.trap;
      }
      return started;
    }
  }
  *out_runtime = r;
  return GLWASM_OK;
}

/* Test seam: one instance in a store of its own, made from the module's
 * policy, destroyed with the instance. */
GLWASM_Result glwasm_runtime_create(GLWASM_Module *module,
    GLWASM_Runtime **out) {
  if (!out) return GLWASM_ERR_INVALID;
  *out = NULL;
  if (!module) return GLWASM_ERR_INVALID;
  GLWASM_Policy policy;
  if (module->has_policy) policy = module->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy);
  GLWASM_Store *store = NULL;
  GLWASM_Result result = glwasm_store_create(&policy, &store);
  if (result != GLWASM_OK) return result;
  store->solo = 1;
  result = glwasm_store_instantiate(store, module, NULL, 0, 1, NULL, out);
  if (result != GLWASM_OK) glwasm_store_destroy(store);
  return result;
}

GLWASM_Result glwasm_runtime_create_with_imports(GLWASM_Module *module,
    const GLWASM_HostImport *imports, size_t import_count, int allow_start,
    GLWASM_LinkError *link_error, GLWASM_Runtime **out) {
  if (!out) return GLWASM_ERR_INVALID;
  *out = NULL;
  if (!module) return GLWASM_ERR_INVALID;
  GLWASM_Policy policy;
  if (module->has_policy) policy = module->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &policy);
  GLWASM_Store *store = NULL;
  GLWASM_Result result = glwasm_store_create(&policy, &store);
  if (result != GLWASM_OK) return result;
  store->solo = 1;
  GLWASM_InstantiateError error;
  result = glwasm_store_instantiate(store, module, imports, import_count,
      allow_start, &error, out);
  if (link_error) *link_error = error.link;
  if (result != GLWASM_OK) glwasm_store_destroy(store);
  return result;
}

void glwasm_runtime_destroy(GLWASM_Runtime *runtime) {
  if (runtime && runtime->store->solo) glwasm_store_destroy(runtime->store);
}

GLWASM_Result glwasm_runtime_register_boundary_engine(
    GLWASM_Runtime *runtime,const GRCORE_EngineDescriptor *descriptor,
    GRCORE_EngineId *out_engine){
  if(!runtime||!descriptor||!out_engine||runtime->store->run.active)
    return GLWASM_ERR_INVALID;
  GRCORE_Result result=grcore_engine_register(runtime->store->context,descriptor,
      out_engine);
  return map_core_result(result);
}
GLWASM_Result glwasm_runtime_bind_boundary_context(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId *out_wasm_engine){
  if(!runtime||!context||!out_wasm_engine||runtime->store->run.active||
      runtime->store->nested_active)return GLWASM_ERR_INVALID;
  GRCORE_EngineId engine=find_wasm_engine(context);
  if(!engine){
    GRCORE_Result result=grcore_engine_register(context,&descriptor,&engine);
    if(result!=GRCORE_OK)return map_core_result(result);
  }
  GLWASM_Result names=ensure_debug_names(context,runtime->module,engine);
  if(names!=GLWASM_OK)return names;
  /* A boundary run can call into any instance of the store: every one's
   * functions must be locatable in the adopted context. */
  for(uint32_t i=0;i<runtime->store->ninstances;i++){
    names=ensure_debug_names(context,runtime->store->instances[i]->module,engine);
    if(names!=GLWASM_OK)return names;
  }
  if(runtime->store->boundary_context==context){
    *out_wasm_engine=engine;
    return GLWASM_OK;
  }
  GRCORE_EntrySlot **new_slots=NULL;
  uint8_t *new_registered=NULL;
  GRCORE_DeoptReservation *new_reservation=NULL;
  if(context!=runtime->store->context){
    names=retain_debug_names(context,runtime->module,engine);
    if(names!=GLWASM_OK)return names;
    names=create_jit_slots(runtime->module,context,&new_slots);
    if(names!=GLWASM_OK){
      release_debug_names(context,runtime->module->debug_id,engine);
      return names;
    }
    new_registered=runtime->module->nfuncs?
        gcu_allocator_calloc(&runtime->module->alloc,
            runtime->module->nfuncs,sizeof(*new_registered)):NULL;
    if(runtime->module->nfuncs&&!new_registered){
      clear_jit_slots(runtime->module,context,new_slots);
      release_debug_names(context,runtime->module->debug_id,engine);
      return GLWASM_ERR_OOM;
    }
    names=register_jit_runtime(runtime,context,&new_reservation);
    if(names!=GLWASM_OK){
      clear_jit_slots(runtime->module,context,new_slots);
      gcu_allocator_free(&runtime->module->alloc,new_registered);
      release_debug_names(context,runtime->module->debug_id,engine);
      return names;
    }
  }
  if(runtime->store->boundary_context&&runtime->store->boundary_context!=runtime->store->context){
    clear_jit_context_state(runtime->module,runtime->store->boundary_context,
        &runtime->boundary_jit_slots,&runtime->boundary_jit_registered,
        &runtime->boundary_jit_reservation);
    release_debug_names(runtime->store->boundary_context,runtime->module->debug_id,
        runtime->store->boundary_engine);
  }
  runtime->store->boundary_context=context;runtime->store->boundary_engine=engine;
  runtime->boundary_jit_slots=new_slots;
  runtime->boundary_jit_registered=new_registered;
  runtime->boundary_jit_reservation=new_reservation;
  *out_wasm_engine=engine;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_unbind_boundary_context(GLWASM_Runtime *runtime) {
  if(!runtime||runtime->store->run.active||runtime->store->nested_active)
    return GLWASM_ERR_INVALID;
  if(runtime->store->boundary_context&&runtime->store->boundary_context!=runtime->store->context){
    clear_jit_context_state(runtime->module,runtime->store->boundary_context,
        &runtime->boundary_jit_slots,&runtime->boundary_jit_registered,
        &runtime->boundary_jit_reservation);
    release_debug_names(runtime->store->boundary_context,runtime->module->debug_id,
        runtime->store->boundary_engine);
  }
  runtime->store->boundary_context=NULL;
  runtime->store->boundary_engine=0;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_invoke_export(GLWASM_Runtime *runtime,
    const char *name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *result){
  if(!runtime||!name)return GLWASM_ERR_INVALID;
  size_t length=strlen(name);
  for(uint32_t i=0;i<runtime->module->nexports;i++){
    GLWASM_Export *exported=&runtime->module->exports[i];
    if(exported->kind==GLWASM_EXT_FUNC&&exported->name.length==length&&
        !memcmp(exported->name.bytes,name,length))
      return glwasm_runtime_invoke(runtime,exported->index,arguments,
          argument_count,result);
  }
  return GLWASM_ERR_INVALID;
}
GLWASM_Result glwasm_runtime_invoke_export_results(GLWASM_Runtime *runtime,
    const char *name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *results,uint32_t *result_count){
  if(!runtime||!name)return GLWASM_ERR_INVALID;
  size_t length=strlen(name);
  for(uint32_t i=0;i<runtime->module->nexports;i++){
    GLWASM_Export *exported=&runtime->module->exports[i];
    if(exported->kind==GLWASM_EXT_FUNC&&exported->name.length==length&&
        !memcmp(exported->name.bytes,name,length))
      return glwasm_runtime_invoke_results(runtime,exported->index,arguments,
          argument_count,results,result_count);
  }
  return GLWASM_ERR_INVALID;
}
GLWASM_Result glwasm_runtime_export(const GLWASM_Runtime *runtime,
    const char *name, size_t length, GLWASM_Extern *out_extern) {
  if (!runtime || (length && !name) || !out_extern) return GLWASM_ERR_INVALID;
  const GLWASM_Module *m = runtime->module;
  const GLWASM_Store *store = runtime->store;
  for (uint32_t i = 0; i < m->nexports; i++) {
    const GLWASM_Export *e = &m->exports[i];
    if (e->name.length != length || memcmp(e->name.bytes, name, length))
      continue;
    GLWASM_Extern object = {store, e->kind, 0};
    uint32_t count = 0;
    const void *wanted = NULL;
    switch (e->kind) {
      case GLWASM_EXT_FUNC: object.address = runtime->funcs[e->index];
        *out_extern = object; return GLWASM_OK;
      case GLWASM_EXT_TABLE: count = store->ntables;
        wanted = runtime->tables[e->index]; break;
      case GLWASM_EXT_MEM: count = store->nmemories;
        wanted = runtime->memories[e->index]; break;
      case GLWASM_EXT_GLOBAL: count = store->nglobals;
        wanted = runtime->globals[e->index]; break;
      case GLWASM_EXT_TAG: count = store->ntags;
        wanted = runtime->tags[e->index]; break;
      default: return GLWASM_ERR_INTERNAL;
    }
    /* Objects other than functions are found by identity: an export of an
     * import names the very object the import linked to. */
    for (uint32_t a = 0; a < count; a++) {
      const void *candidate = e->kind == GLWASM_EXT_TABLE
          ? (const void *)store->tables[a]
          : e->kind == GLWASM_EXT_MEM ? (const void *)store->memories[a]
          : e->kind == GLWASM_EXT_GLOBAL ? (const void *)store->globals[a]
          : (const void *)store->tags[a];
      if (candidate == wanted) {
        object.address = a;
        *out_extern = object;
        return GLWASM_OK;
      }
    }
    return GLWASM_ERR_INTERNAL;
  }
  return GLWASM_ERR_INVALID;
}

static int extern_of(const GLWASM_Store *store, GLWASM_Extern object,
    uint8_t kind) {
  if (!store || object.store != store || object.kind != kind) return 0;
  switch (kind) {
    case GLWASM_EXTERN_FUNC: return object.address < store->nfuncs;
    case GLWASM_EXTERN_TABLE: return object.address < store->ntables;
    case GLWASM_EXTERN_MEMORY: return object.address < store->nmemories;
    case GLWASM_EXTERN_GLOBAL: return object.address < store->nglobals;
    case GLWASM_EXTERN_TAG: return object.address < store->ntags;
    default: return 0;
  }
}

GLWASM_Result glwasm_store_global_get(const GLWASM_Store *store,
    GLWASM_Extern global, uint64_t *value) {
  if (!value || !extern_of(store, global, GLWASM_EXTERN_GLOBAL))
    return GLWASM_ERR_INVALID;
  *value = global_read(store->globals[global.address]);
  return GLWASM_OK;
}

GLWASM_Result glwasm_store_global_set(GLWASM_Store *store,
    GLWASM_Extern global, uint64_t value) {
  if (!extern_of(store, global, GLWASM_EXTERN_GLOBAL)) return GLWASM_ERR_INVALID;
  StoreGlobal *object = store->globals[global.address];
  if (!object->mut || !global_value_valid(store, object, value))
    return GLWASM_ERR_INVALID;
  if (object->type.kind == GLWASM_VK_I32 || object->type.kind == GLWASM_VK_F32)
    value = (uint32_t)value;
  global_write(object, value);
  return GLWASM_OK;
}

GLWASM_Result glwasm_store_memory_size(const GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t *bytes) {
  if (!bytes || !extern_of(store, memory, GLWASM_EXTERN_MEMORY))
    return GLWASM_ERR_INVALID;
  MemoryLease lease;
  if (!memory_lease(store->memories[memory.address], &lease))
    return GLWASM_ERR_INVALID;
  *bytes = lease.view.length_bytes;
  memory_release(&lease);
  return GLWASM_OK;
}

static GLWASM_Result memory_transfer(const GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t offset, void *out, const void *in,
    size_t size) {
  if (!extern_of(store, memory, GLWASM_EXTERN_MEMORY) || (size && !out && !in))
    return GLWASM_ERR_INVALID;
  MemoryLease lease;
  if (!memory_lease(store->memories[memory.address], &lease))
    return GLWASM_ERR_INVALID;
  if (offset > lease.view.length_bytes ||
      (uint64_t)size > lease.view.length_bytes - offset) {
    memory_release(&lease);
    return GLWASM_ERR_INVALID;
  }
  if (size) {
    if (out) memcpy(out, lease.view.bytes + offset, size);
    else memcpy(lease.view.bytes + offset, in, size);
  }
  memory_release(&lease);
  return GLWASM_OK;
}

GLWASM_Result glwasm_store_memory_read(const GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t offset, void *buffer, size_t size) {
  if (size && !buffer) return GLWASM_ERR_INVALID;
  return memory_transfer(store, memory, offset, buffer, NULL, size);
}

GLWASM_Result glwasm_store_memory_write(GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t offset, const void *buffer, size_t size) {
  if (size && !buffer) return GLWASM_ERR_INVALID;
  return memory_transfer(store, memory, offset, NULL, buffer, size);
}

GLWASM_Result glwasm_store_funcref(const GLWASM_Store *store,
    GLWASM_Extern function, uint64_t *token) {
  if (!token || !extern_of(store, function, GLWASM_EXTERN_FUNC))
    return GLWASM_ERR_INVALID;
  *token = (uint64_t)function.address + 1u;
  return GLWASM_OK;
}

static GLWASM_Result finish_nested(GLWASM_Runtime *runtime,GRCORE_Result core,
    GRCORE_Outcome outcome,uint64_t *results,uint32_t *result_count) {
  if(core==GRCORE_OK&&outcome==GRCORE_OUTCOME_PAUSED){
    runtime->store->run.paused=1;
    return GLWASM_ERR_PAUSED;
  }
  runtime->jit_test_pause_bulk_chunks=0;
  GRCORE_Stack *stack=grcore_context_stack(runtime->store->nested_context);
  while(grcore_stack_frame_count(stack)>runtime->store->nested_base)
    (void)grcore_stack_pop(stack);
  GRCORE_Result leave=grcore_boundary_leave(stack,runtime->store->nested_boundary);
  Run *run=&runtime->store->run;
  GLWASM_Result output=run->error!=GLWASM_OK?run->error:
      run->trap!=GLWASM_TRAP_NONE?GLWASM_ERR_GUEST:
      core!=GRCORE_OK?map_core_result(core):
      leave!=GRCORE_OK?map_core_result(leave):GLWASM_OK;
  if(output==GLWASM_OK){
    if(result_count)*result_count=run->result_count;
    if(results&&run->results&&results!=run->results&&run->result_count)
      memcpy(results,run->results,
        (size_t)run->result_count*sizeof(*results));
  }
  if(run->args)gcu_allocator_free(runtime->store->alloc,run->args);
  runtime->boundary_token=runtime->store->nested_saved_token;
  runtime->store->run=runtime->store->nested_saved_run;
  runtime->store->nested_active=0;runtime->store->nested_context=NULL;
  runtime->store->nested_caller=0;runtime->store->nested_instance=0;runtime->store->nested_base=0;
  runtime->store->nested_owns_run=0;
  memset(&runtime->store->nested_boundary,0,sizeof(runtime->store->nested_boundary));
  return output;
}

/* Start a Wasm entry on the caller's stack and retain the boundary activation
 * and guest frames when a runtime-core poll pauses. */
GLWASM_Result glwasm_runtime_invoke_nested_results(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    const char *export_name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *result,uint32_t *result_count){
  if(!runtime||!context||!caller||!instance_token||!export_name||!result_count||
      (argument_count&&!arguments)||runtime->store->run.active||runtime->store->nested_active)
    return GLWASM_ERR_INVALID;
  /* A paused context owns a saved continuation. Do not add a boundary or
   * guest frames to it: grcore_run would reject the state, and the direct
   * re-entry path below is reserved for an active host callback. */
  if(grcore_context_state(context)==GRCORE_CONTEXT_PAUSED)
    return GLWASM_ERR_INVALID;
  memset(&runtime->store->last_foreign_exception,0,
      sizeof(runtime->store->last_foreign_exception));
  uint32_t function=UINT32_MAX;
  size_t name_length=strlen(export_name);
  for(uint32_t i=0;i<runtime->module->nexports;i++){
    GLWASM_Export *item=&runtime->module->exports[i];
    if(item->kind==GLWASM_EXT_FUNC&&item->name.length==name_length&&
        !memcmp(item->name.bytes,export_name,name_length)){
      function=item->index;break;
    }
  }
  if(function==UINT32_MAX)return GLWASM_ERR_INVALID;
  GLWASM_Func *fn=&runtime->module->funcs[function];
  if(fn->type_index>=runtime->module->ntypes||
      runtime->module->types[fn->type_index].nparams!=argument_count)
    return GLWASM_ERR_INVALID;
  uint32_t result_arity=runtime->module->types[fn->type_index].nresults;
  uint32_t capacity=*result_count;
  if(capacity<result_arity||(result_arity&&!result)){
    *result_count=result_arity;return GLWASM_ERR_LIMIT;
  }
  GRCORE_EngineId wasm_engine=0;
  GLWASM_Result bound=glwasm_runtime_bind_boundary_context(runtime,context,
      &wasm_engine);
  if(bound!=GLWASM_OK)return bound;
  GRCORE_Result core=GRCORE_OK;
  GRCORE_Stack *stack=grcore_context_stack(context);
  size_t base=grcore_stack_frame_count(stack);
  GRCORE_BoundaryRef boundary={0};
  core=grcore_boundary_enter(stack,caller,wasm_engine,instance_token,true,
      &boundary);
  if(core!=GRCORE_OK)return map_core_result(core);
  runtime->store->nested_saved_run=runtime->store->run;
  runtime->store->nested_saved_token=runtime->boundary_token;
  runtime->store->nested_context=context;runtime->store->nested_caller=caller;
  runtime->store->nested_instance=instance_token;runtime->store->nested_base=base;
  runtime->store->nested_boundary=boundary;
  Run *run=&runtime->store->run;
  run->ctx=context;run->engine=wasm_engine;
  run->entry_instance=runtime;run_bind(run,runtime);
  run->entry=function;run->nargs=argument_count;run->stack_base=base;
  run->results=result;run->result_capacity=capacity;
  run->result_arity=result_arity;run->result_count=0;
  run->jit_entry_attempted=0;
  run->jit_pending=0;
  run->args=argument_count?gcu_allocator_calloc(runtime->store->alloc,
      argument_count,sizeof(*run->args)):NULL;
  run->result=0;run->error=GLWASM_OK;run->trap=GLWASM_TRAP_NONE;
  run->pending_host=0;run->active=1;run->paused=0;run->bulk_active=0;
  run->guest_alloc=runtime->store->guest_alloc;
  if(argument_count&&!run->args){runtime->store->run=runtime->store->nested_saved_run;
    runtime->store->nested_context=NULL;
    (void)grcore_boundary_leave(stack,boundary);return GLWASM_ERR_OOM;}
  if(argument_count)memcpy(run->args,arguments,(size_t)argument_count*sizeof(*arguments));
  runtime->boundary_token=instance_token;
  if(!fn->imported){
    GRCORE_FrameRef frame;
    core=grcore_stack_push(stack,wasm_engine,frame_slots(fn),&frame);
    if(core==GRCORE_OK){
      uint64_t *slots_ptr=slots(stack,frame);
      if(!slots_ptr)core=GRCORE_ERR_INVALID;
      else {memset(slots_ptr,0,frame_slots(fn)*sizeof(*slots_ptr));
        slots_ptr[H_FUNC]=function;slots_ptr[H_PC]=0;
        slots_ptr[H_INSTANCE]=(uintptr_t)runtime;
        slots_ptr[H_SP]=H_OPERANDS+fn->nlocals;
        slots_ptr[H_NLOCALS]=fn->nlocals;
        slots_ptr[H_CTRL_DEPTH]=0;
        diagnostic_record_frame_entry(&runtime->store->run,function,
            slots_ptr[H_SP],H_OPERANDS+fn->nlocals);
        for(uint32_t i=0;i<argument_count;i++)slots_ptr[H_OPERANDS+i]=arguments[i];
        core=wasm_set_guest_frame_identity(stack,frame,runtime->module,
            function,0);}
    }
  }
  if(core!=GRCORE_OK){runtime->store->run.error=map_core_result(core);
    return finish_nested(runtime,core,GRCORE_OUTCOME_FINISHED,result,
        result_count);}
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  core=grcore_run(context,execute,run,&outcome);
  if(core==GRCORE_ERR_INVALID){
    /* An embedding engine may already be running on this context. In that
     * case its entry owns the eventual pause; retain only the Wasm frames. */
    GRCORE_Step step=execute(context,run);
    core=GRCORE_OK;
    if(step==GRCORE_STEP_PAUSED)outcome=GRCORE_OUTCOME_PAUSED;
    else if(step==GRCORE_STEP_UNWOUND)
      core=run->error==GLWASM_ERR_LIMIT?GRCORE_ERR_LIMIT:GRCORE_ERR_GUEST;
  }else if(core==GRCORE_OK){
    runtime->store->nested_owns_run=1;
  }
  if(core==GRCORE_OK&&outcome==GRCORE_OUTCOME_PAUSED){
    runtime->store->nested_active=1;runtime->store->run.paused=1;
  }
  return finish_nested(runtime,core,outcome,result,result_count);
}

GLWASM_Result glwasm_runtime_last_foreign_exception(
    const GLWASM_Runtime *runtime,GLWASM_ForeignException *out_exception){
  if(!runtime||!out_exception)return GLWASM_ERR_INVALID;
  *out_exception=runtime->store->last_foreign_exception;
  return out_exception->engine&&out_exception->identity
      ?GLWASM_OK:GLWASM_ERR_INVALID;
}

GLWASM_Result glwasm_runtime_invoke_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    const char *export_name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *result){
  if(!runtime||!result||!export_name)return GLWASM_ERR_INVALID;
  uint32_t function=UINT32_MAX;
  size_t export_length=strlen(export_name);
  for(uint32_t i=0;i<runtime->module->nexports;i++){
    GLWASM_Export *item=&runtime->module->exports[i];
    if(item->kind==GLWASM_EXT_FUNC&&item->name.length==export_length&&
        !memcmp(item->name.bytes,export_name,export_length)){
      function=item->index;break;
    }
  }
  if(function==UINT32_MAX||runtime->module->funcs[function].type_index>=
      runtime->module->ntypes||runtime->module->types[
      runtime->module->funcs[function].type_index].nresults>1)
    return GLWASM_ERR_UNSUPPORTED;
  uint32_t capacity=1;*result=0;
  return glwasm_runtime_invoke_nested_results(runtime,context,caller,
      instance_token,export_name,arguments,argument_count,result,&capacity);
}

GLWASM_Result glwasm_runtime_resume_nested_results(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    uint64_t *result,uint32_t *result_count){
  if(!runtime||!context||!result_count||!runtime->store->nested_active||
      runtime->store->nested_context!=context||runtime->store->nested_caller!=caller||
      runtime->store->nested_instance!=instance_token)return GLWASM_ERR_INVALID;
  if(*result_count<runtime->store->run.result_arity||
      (runtime->store->run.result_arity&&!result)){
    *result_count=runtime->store->run.result_arity;return GLWASM_ERR_LIMIT;
  }
  runtime->store->run.result_capacity=*result_count;
  runtime->store->run.results=result;
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result core;
  if(runtime->store->nested_owns_run){
    core=grcore_resume(context,&outcome);
  }else{
    GRCORE_Step step=execute(context,&runtime->store->run);
    core=GRCORE_OK;
    if(step==GRCORE_STEP_PAUSED)outcome=GRCORE_OUTCOME_PAUSED;
    else if(step==GRCORE_STEP_UNWOUND)
      core=runtime->store->run.error==GLWASM_ERR_LIMIT?GRCORE_ERR_LIMIT:GRCORE_ERR_GUEST;
  }
  return finish_nested(runtime,core,outcome,result,result_count);
}
GLWASM_Result glwasm_runtime_resume_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    uint64_t *result){
  if(!runtime||!result||runtime->store->run.result_arity>1)
    return GLWASM_ERR_INVALID;
  uint32_t capacity=1;
  *result=0;
  return glwasm_runtime_resume_nested_results(runtime,context,caller,
      instance_token,result,&capacity);
}
/* Test seam: replace the store's context with one whose fuel, guest depth
 * and stack movement are those given. Every store-owned memory and table
 * moves to the new context's allocator; JIT state and debug names are
 * remade for it. Refused while running or with live exceptions. */
GLWASM_Result glwasm_runtime_set_test_limits(GLWASM_Runtime *runtime,
    uint64_t fuel, uint64_t depth, int always_move) {
  if (!runtime) return GLWASM_ERR_INVALID;
  GLWASM_Store *store = runtime->store;
  if (store->run.active || store->exceptions) return GLWASM_ERR_INVALID;
  GRCORE_Options *options = NULL;
  GRCORE_Context *context = NULL;
  GRCORE_EngineId engine = 0;
  GRCORE_Result cr = grcore_options_create(NULL, &options);
  if (cr == GRCORE_OK) cr = grcore_options_set_fuel(options, fuel);
  if (cr == GRCORE_OK) cr = grcore_options_set_memory_bytes(options,
      store->policy.default_memory_bytes);
  if (cr == GRCORE_OK) cr = grcore_options_set_guest_depth(options, depth);
  if (cr == GRCORE_OK) cr = grcore_options_set_native_depth(options,
      store->policy.default_native_depth);
  if (cr == GRCORE_OK) cr = grcore_options_set_native_stack_bytes(options,
      store->policy.default_native_stack_bytes);
  if (cr == GRCORE_OK) cr = grcore_context_create(store->group, options, &context);
  if (cr == GRCORE_OK) cr = grcore_engine_register(context, &descriptor, &engine);
  for (uint32_t i = 0; cr == GRCORE_OK && i < store->ninstances; i++) {
    GLWASM_Result names = ensure_debug_names(context,
        store->instances[i]->module, engine);
    if (names != GLWASM_OK)
      cr = names == GLWASM_ERR_OOM ? GRCORE_ERR_OOM : GRCORE_ERR_INVALID;
  }
  if (cr == GRCORE_OK) cr = grcore_context_register(context, &trap_key, &store->run);
  if (cr == GRCORE_OK)
    cr = grcore_stack_set_always_move(grcore_context_stack(context),
        always_move != 0);
  if (cr != GRCORE_OK) {
    if (context) grcore_context_destroy(context);
    if (options) grcore_options_destroy(options);
    return map_core_result(cr);
  }
  const GCU_Allocator *new_alloc = grcore_context_allocator(context);
  /* Copy every store-owned memory and table first; commit only when all
   * copies exist, so a failure leaves the store as it was. */
  uint8_t **memories = store->nmemories ? gcu_allocator_calloc(store->alloc,
      store->nmemories, sizeof(*memories)) : NULL;
  uint64_t **tables = store->ntables ? gcu_allocator_calloc(store->alloc,
      store->ntables, sizeof(*tables)) : NULL;
  GLWASM_Result result = (store->nmemories && !memories) ||
      (store->ntables && !tables) ? GLWASM_ERR_OOM : GLWASM_OK;
  for (uint32_t i = 0; result == GLWASM_OK && i < store->nmemories; i++) {
    StoreMemory *memory = store->memories[i];
    if (memory->hosted || !memory->length) continue;
    if (memory->length > SIZE_MAX) { result = GLWASM_ERR_LIMIT; break; }
    memories[i] = gcu_allocator_calloc(new_alloc, (size_t)memory->length, 1);
    if (!memories[i]) { result = GLWASM_ERR_OOM; break; }
    memcpy(memories[i], memory->bytes, (size_t)memory->length);
  }
  for (uint32_t i = 0; result == GLWASM_OK && i < store->ntables; i++) {
    StoreTable *table = store->tables[i];
    if (table->hosted || !table->length) continue;
    if (table->length > SIZE_MAX / sizeof(uint64_t)) { result = GLWASM_ERR_LIMIT; break; }
    tables[i] = gcu_allocator_calloc(new_alloc, (size_t)table->length,
        sizeof(uint64_t));
    if (!tables[i]) { result = GLWASM_ERR_OOM; break; }
    memcpy(tables[i], table->elements, (size_t)table->length * sizeof(uint64_t));
  }
  GRCORE_EntrySlot ***slots = store->ninstances ? gcu_allocator_calloc(
      store->alloc, store->ninstances, sizeof(*slots)) : NULL;
  GRCORE_DeoptReservation **reservations = store->ninstances ?
      gcu_allocator_calloc(store->alloc, store->ninstances,
          sizeof(*reservations)) : NULL;
  if (result == GLWASM_OK && store->ninstances && (!slots || !reservations))
    result = GLWASM_ERR_OOM;
  for (uint32_t i = 0; result == GLWASM_OK && i < store->ninstances; i++) {
    result = create_jit_slots(store->instances[i]->module, context, &slots[i]);
    if (result == GLWASM_OK)
      result = register_jit_runtime(store->instances[i], context,
          &reservations[i]);
  }
  if (result != GLWASM_OK) {
    for (uint32_t i = 0; i < store->nmemories && memories; i++)
      gcu_allocator_free(new_alloc, memories[i]);
    for (uint32_t i = 0; i < store->ntables && tables; i++)
      gcu_allocator_free(new_alloc, tables[i]);
    for (uint32_t i = 0; i < store->ninstances && slots; i++) {
      clear_jit_slots(store->instances[i]->module, context, slots[i]);
      if (reservations[i]) grcore_deopt_release(context, reservations[i]);
    }
    gcu_allocator_free(store->alloc, memories);
    gcu_allocator_free(store->alloc, tables);
    gcu_allocator_free(store->alloc, slots);
    gcu_allocator_free(store->alloc, reservations);
    grcore_context_destroy(context);
    grcore_options_destroy(options);
    return result;
  }
  for (uint32_t i = 0; i < store->nmemories; i++) {
    StoreMemory *memory = store->memories[i];
    if (memory->hosted) continue;
    gcu_allocator_free(store->guest_alloc, memory->bytes);
    memory->bytes = memories[i];
  }
  for (uint32_t i = 0; i < store->ntables; i++) {
    StoreTable *table = store->tables[i];
    if (table->hosted) continue;
    gcu_allocator_free(store->guest_alloc, table->elements);
    table->elements = tables[i];
  }
  for (uint32_t i = 0; i < store->ninstances; i++) {
    GLWASM_Runtime *r = store->instances[i];
    clear_jit_context_state(r->module, store->context, &r->jit_slots,
        &r->jit_registered, &r->jit_reservation);
    r->jit_slots = slots[i];
    r->jit_reservation = reservations[i];
    r->jit_registered = r->module->nfuncs ? gcu_allocator_calloc(
        &r->module->alloc, r->module->nfuncs, sizeof(*r->jit_registered))
        : NULL;
  }
  gcu_allocator_free(store->alloc, memories);
  gcu_allocator_free(store->alloc, tables);
  gcu_allocator_free(store->alloc, slots);
  gcu_allocator_free(store->alloc, reservations);
  grcore_context_destroy(store->context);
  if (store->options) grcore_options_destroy(store->options);
  store->context = context;
  store->options = options;
  store->engine = engine;
  store->guest_alloc = new_alloc;
  store->run.guest_alloc = new_alloc;
  store->run.ctx = context;
  store->run.engine = engine;
  for (uint32_t i = 0; i < store->ninstances; i++)
    if (store->instances[i]->module->nfuncs &&
        !store->instances[i]->jit_registered) return GLWASM_ERR_OOM;
  return GLWASM_OK;
}
static void discard_guest_frames(GLWASM_Runtime*r){GRCORE_Stack*s=grcore_context_stack(r->store->context);while(grcore_stack_frame_count(s))(void)grcore_stack_pop(s);}
static void free_run_args(GLWASM_Runtime*r){if(r->store->run.args){gcu_allocator_free(r->store->alloc,r->store->run.args);r->store->run.args=NULL;}}
/* Restored frames name the source store's instances; point each at the
 * destination instance with the same index. */
static GLWASM_Result rebind_restored_guest_frames(GLWASM_Store *store,
    const GLWASM_Snapshot *snapshot) {
  GRCORE_Stack *stack = grcore_context_stack(store->context);
  size_t count = grcore_stack_frame_count(stack);
  GRCORE_FrameRef frame = grcore_stack_top(stack);
  /* Validate the whole chain before changing any frame. */
  for (size_t i = 0; i < count; ++i) {
    uint64_t *frame_slots = slots(stack, frame);
    if (!frame_slots) return GLWASM_ERR_INTERNAL;
    uint32_t index = UINT32_MAX;
    for (uint32_t k = 0; k < snapshot->ninstances; k++)
      if (snapshot->instances[k] == (uintptr_t)frame_slots[H_INSTANCE]) index = k;
    if (index == UINT32_MAX ||
        frame_slots[H_FUNC] >= store->instances[index]->module->nfuncs)
      return GLWASM_ERR_INTERNAL;
    frame = grcore_stack_caller(stack, frame);
  }
  frame = grcore_stack_top(stack);
  for (size_t i = 0; i < count; ++i) {
    uint64_t *frame_slots = slots(stack, frame);
    for (uint32_t k = 0; k < snapshot->ninstances; k++)
      if (snapshot->instances[k] == (uintptr_t)frame_slots[H_INSTANCE]) {
        frame_slots[H_INSTANCE] = (uintptr_t)store->instances[k];
        break;
      }
    frame = grcore_stack_caller(stack, frame);
  }
  return GLWASM_OK;
}
static GLWASM_Result finish_run(GLWASM_Runtime*r,GRCORE_Result cr,
    GRCORE_Outcome outcome,uint64_t*out,uint32_t*result_count,int*paused){
  /* runtime-core writes outcome only on success. An unwind can leave a stale
   * PAUSED value in the caller's storage, but it is never resumable. */
  const int did_pause=cr==GRCORE_OK&&r->store->run.error==GLWASM_OK&&
      r->store->run.trap==GLWASM_TRAP_NONE&&outcome==GRCORE_OUTCOME_PAUSED;
  if(paused)*paused=did_pause;
  r->store->run.paused=did_pause;
  if(r->store->run.paused)return GLWASM_ERR_PAUSED;
  r->jit_test_pause_bulk_chunks=0;
  if(r->store->run.error!=GLWASM_OK){discard_guest_frames(r);r->store->run.active=0;
    r->store->run.bulk_active=0;free_run_args(r);return r->store->run.error;}
  if(r->store->run.trap!=GLWASM_TRAP_NONE){discard_guest_frames(r);r->store->run.active=0;
    r->store->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}
  if(cr==GRCORE_ERR_GUEST){discard_guest_frames(r);r->store->run.active=0;
    r->store->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}
  if(cr==GRCORE_ERR_LIMIT){discard_guest_frames(r);r->store->run.active=0;
    r->store->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_LIMIT;}
  if(cr!=GRCORE_OK){discard_guest_frames(r);r->store->run.active=0;
    r->store->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_INTERNAL;}
  if(result_count)*result_count=r->store->run.result_count;
  if(out&&r->store->run.results&&out!=r->store->run.results&&r->store->run.result_count)
    memcpy(out,r->store->run.results,(size_t)r->store->run.result_count*sizeof(*out));
  r->store->run.active=0;r->store->run.bulk_active=0;free_run_args(r);return GLWASM_OK;
}

static int snapshot_value_type_supported(GLWASM_ValType type){
  return type.kind!=GLWASM_VK_REF||type.heap==GLWASM_HT_FUNC||
      type.heap==GLWASM_HT_NOFUNC;
}
static int snapshot_module_supported(const GLWASM_Module*m){
  if(!m)return 0;
  for(uint32_t i=0;i<m->ntypes;i++){
    for(uint32_t j=0;j<m->types[i].nparams;j++)
      if(!snapshot_value_type_supported(m->types[i].params[j]))return 0;
    for(uint32_t j=0;j<m->types[i].nresults;j++)
      if(!snapshot_value_type_supported(m->types[i].results[j]))return 0;
  }
  for(uint32_t i=0;i<m->nfuncs;i++)for(uint32_t j=0;j<m->funcs[i].nlocals;j++)
    if(!snapshot_value_type_supported(m->funcs[i].locals[j]))return 0;
  for(uint32_t i=0;i<m->nglobals;i++)
    if(!snapshot_value_type_supported(m->globals[i].type))return 0;
  for(uint32_t i=0;i<m->ntables;i++)
    if(!snapshot_value_type_supported(m->tables[i].elem))return 0;
  return 1;
}

/* Only a store made entirely of Wasm objects can be captured: host objects
 * have no snapshot hooks of their own. */
static int snapshot_store_supported(const GLWASM_Store *store) {
  for (uint32_t i = 0; i < store->ninstances; i++)
    if (!snapshot_module_supported(store->instances[i]->module)) return 0;
  for (uint32_t i = 0; i < store->nfuncs; i++)
    if (!store->funcs[i]->instance) return 0;
  for (uint32_t i = 0; i < store->nmemories; i++)
    if (store->memories[i]->hosted) return 0;
  for (uint32_t i = 0; i < store->ntables; i++)
    if (store->tables[i]->hosted) return 0;
  for (uint32_t i = 0; i < store->nglobals; i++)
    if (store->globals[i]->host) return 0;
  for (uint32_t i = 0; i < store->ntags; i++)
    if (store->tags[i]->host_identity) return 0;
  return 1;
}

static void snapshot_free(GLWASM_Snapshot*s){
  if(!s)return;
  const GCU_Allocator *a=s->alloc;
  if(s->core)grcore_snapshot_release(s->core);
  if(s->memories)for(uint32_t i=0;i<s->nmemories;i++)gcu_allocator_free(a,s->memories[i]);
  if(s->tables)for(uint32_t i=0;i<s->ntables;i++)gcu_allocator_free(a,s->tables[i]);
  for(uint32_t i=0;i<s->ninstances;i++){
    if(s->data_dropped)gcu_allocator_free(a,s->data_dropped[i]);
    if(s->elem_counts)gcu_allocator_free(a,s->elem_counts[i]);
    if(s->modules&&s->modules[i])glwasm_module_destroy(s->modules[i]);
  }
  gcu_allocator_free(a,s->modules);gcu_allocator_free(a,s->instances);
  gcu_allocator_free(a,s->memories);gcu_allocator_free(a,s->memory_bytes);
  gcu_allocator_free(a,s->memory_pages);gcu_allocator_free(a,s->tables);
  gcu_allocator_free(a,s->table_sizes);gcu_allocator_free(a,s->globals);
  gcu_allocator_free(a,s->data_dropped);gcu_allocator_free(a,s->elem_counts);
  gcu_allocator_free(a,s->args);gcu_allocator_free(a,s);
}

#define SNAP_ARRAY(field,count,type) do{if((count)){s->field=gcu_allocator_calloc(a,(count),sizeof(type));if(!s->field){snapshot_free(s);return GLWASM_ERR_OOM;}}}while(0)

GLWASM_Result glwasm_runtime_snapshot(GLWASM_Runtime*r,GLWASM_Snapshot**out){
  if(!r||!out)return GLWASM_ERR_INVALID;
  GLWASM_Store *store=r->store;
  if(!store->run.active||!store->run.paused||store->run.pending_host||
      !snapshot_store_supported(store))return GLWASM_ERR_UNSUPPORTED;
  const GCU_Allocator *a=store->alloc;
  GLWASM_Snapshot*s=gcu_allocator_calloc(a,1,sizeof(*s));
  if(!s)return GLWASM_ERR_OOM;
  s->alloc=a;
  s->ninstances=store->ninstances;s->nfuncs=store->nfuncs;
  s->nmemories=store->nmemories;s->ntables=store->ntables;
  s->nglobals=store->nglobals;s->ntags=store->ntags;
  SNAP_ARRAY(modules,s->ninstances,GLWASM_Module*);
  SNAP_ARRAY(instances,s->ninstances,uintptr_t);
  SNAP_ARRAY(data_dropped,s->ninstances,uint8_t*);
  SNAP_ARRAY(elem_counts,s->ninstances,uint32_t*);
  s->entry_instance=UINT32_MAX;
  for(uint32_t i=0;i<s->ninstances;i++){
    GLWASM_Runtime *instance=store->instances[i];
    GLWASM_Module *m=instance->module;
    s->modules[i]=m;glwasm_module_retain(m);
    s->instances[i]=(uintptr_t)instance;
    if(instance==store->run.entry_instance)s->entry_instance=i;
    if(m->ndatas){s->data_dropped[i]=gcu_allocator_calloc(a,m->ndatas,1);
      if(!s->data_dropped[i]){snapshot_free(s);return GLWASM_ERR_OOM;}
      memcpy(s->data_dropped[i],instance->data_dropped,m->ndatas);}
    if(m->nelems){s->elem_counts[i]=gcu_allocator_calloc(a,m->nelems,sizeof(uint32_t));
      if(!s->elem_counts[i]){snapshot_free(s);return GLWASM_ERR_OOM;}
      memcpy(s->elem_counts[i],instance->elem_counts,m->nelems*sizeof(uint32_t));}
  }
  GRCORE_Result cr=grcore_context_snapshot(store->context,NULL,&s->core);
  if(cr!=GRCORE_OK){snapshot_free(s);return map_core_result(cr);}
  SNAP_ARRAY(globals,s->nglobals,uint64_t);
  for(uint32_t i=0;i<s->nglobals;i++)s->globals[i]=store->globals[i]->value;
  SNAP_ARRAY(memories,s->nmemories,uint8_t*);SNAP_ARRAY(memory_bytes,s->nmemories,uint64_t);
  SNAP_ARRAY(memory_pages,s->nmemories,uint64_t);
  for(uint32_t i=0;i<s->nmemories;i++){
    const StoreMemory *memory=store->memories[i];
    s->memory_bytes[i]=memory->length;s->memory_pages[i]=memory->pages;
    if(memory->length){
      if(memory->length>SIZE_MAX){snapshot_free(s);return GLWASM_ERR_LIMIT;}
      s->memories[i]=gcu_allocator_calloc(a,(size_t)memory->length,1);
      if(!s->memories[i]){snapshot_free(s);return GLWASM_ERR_OOM;}
      memcpy(s->memories[i],memory->bytes,(size_t)memory->length);
    }
  }
  SNAP_ARRAY(tables,s->ntables,uint64_t*);SNAP_ARRAY(table_sizes,s->ntables,uint64_t);
  for(uint32_t i=0;i<s->ntables;i++){
    const StoreTable *table=store->tables[i];
    s->table_sizes[i]=table->length;
    if(table->length){
      if(table->length>SIZE_MAX/sizeof(uint64_t)){snapshot_free(s);return GLWASM_ERR_LIMIT;}
      s->tables[i]=gcu_allocator_calloc(a,(size_t)table->length,sizeof(uint64_t));
      if(!s->tables[i]){snapshot_free(s);return GLWASM_ERR_OOM;}
      memcpy(s->tables[i],table->elements,(size_t)table->length*sizeof(uint64_t));
    }
  }
  const Run *run=&store->run;
  s->nargs=run->nargs;s->entry=run->entry;s->result=run->result;
  s->result_capacity=run->result_capacity;
  s->result_arity=run->result_arity;s->result_count=run->result_count;
  s->bulk_func=run->bulk_func;s->bulk_pc=run->bulk_pc;s->bulk_op=run->bulk_op;
  s->jit_entry_attempted=run->jit_entry_attempted;s->jit_pending=run->jit_pending;
  s->jit_pending_function=run->jit_pending_function;
  s->bulk_dst=run->bulk_dst;s->bulk_src=run->bulk_src;s->bulk_len=run->bulk_len;
  s->bulk_done=run->bulk_done;s->bulk_value=run->bulk_value;s->bulk_active=run->bulk_active;
  SNAP_ARRAY(args,s->nargs,uint64_t);
  if(s->nargs)memcpy(s->args,run->args,s->nargs*sizeof(uint64_t));
  *out=s;return GLWASM_OK;
}
#undef SNAP_ARRAY

/* The destination is `r`'s store; it must match the snapshot's shape. */
GLWASM_Result glwasm_runtime_restore(GLWASM_Runtime*r,const GLWASM_Snapshot*s){
  if(!r||!s||!s->core)return GLWASM_ERR_INVALID;
  GLWASM_Store *store=r->store;
  if(store->run.active||!snapshot_store_supported(store)||
      store->ninstances!=s->ninstances||store->nfuncs!=s->nfuncs||
      store->nmemories!=s->nmemories||store->ntables!=s->ntables||
      store->nglobals!=s->nglobals||store->ntags!=s->ntags)
    return GLWASM_ERR_INVALID;
  for(uint32_t i=0;i<s->ninstances;i++)
    if(store->instances[i]->module!=s->modules[i])return GLWASM_ERR_INVALID;
  const GCU_Allocator *g=store->guest_alloc;
  GLWASM_Result failure=GLWASM_ERR_OOM;
  uint8_t **mems=s->nmemories?gcu_allocator_calloc(store->alloc,s->nmemories,sizeof(*mems)):NULL;
  uint64_t **tabs=s->ntables?gcu_allocator_calloc(store->alloc,s->ntables,sizeof(*tabs)):NULL;
  uint64_t *args=s->nargs?gcu_allocator_calloc(store->alloc,s->nargs,sizeof(*args)):NULL;
  if((s->nmemories&&!mems)||(s->ntables&&!tabs)||(s->nargs&&!args))goto fail;
  for(uint32_t i=0;i<s->nmemories;i++)if(s->memory_bytes[i]){
    if(s->memory_bytes[i]>SIZE_MAX){failure=GLWASM_ERR_LIMIT;goto fail;}
    mems[i]=gcu_allocator_calloc(g,(size_t)s->memory_bytes[i],1);if(!mems[i])goto fail;
    memcpy(mems[i],s->memories[i],(size_t)s->memory_bytes[i]);}
  for(uint32_t i=0;i<s->ntables;i++)if(s->table_sizes[i]){
    if(s->table_sizes[i]>SIZE_MAX/sizeof(uint64_t)){failure=GLWASM_ERR_LIMIT;goto fail;}
    tabs[i]=gcu_allocator_calloc(g,(size_t)s->table_sizes[i],sizeof(uint64_t));if(!tabs[i])goto fail;
    memcpy(tabs[i],s->tables[i],(size_t)s->table_sizes[i]*sizeof(uint64_t));}
  if(s->nargs)memcpy(args,s->args,s->nargs*sizeof(*args));
  {GRCORE_RestoreEnv env=GRCORE_RESTORE_ENV_INIT(NULL,NULL,execute,&store->run,"wasm");
   GRCORE_Result cr=grcore_context_restore(store->context,s->core,&env);
   if(cr!=GRCORE_OK){failure=GLWASM_ERR_INVALID;goto fail;}}
  if(rebind_restored_guest_frames(store,s)!=GLWASM_OK){failure=GLWASM_ERR_INTERNAL;goto fail;}
  for(uint32_t i=0;i<s->nmemories;i++){StoreMemory *memory=store->memories[i];
    gcu_allocator_free(g,memory->bytes);memory->bytes=mems[i];
    memory->length=s->memory_bytes[i];memory->pages=s->memory_pages[i];}
  for(uint32_t i=0;i<s->ntables;i++){StoreTable *table=store->tables[i];
    gcu_allocator_free(g,table->elements);table->elements=tabs[i];table->length=s->table_sizes[i];}
  for(uint32_t i=0;i<s->nglobals;i++)store->globals[i]->value=s->globals[i];
  for(uint32_t i=0;i<s->ninstances;i++){
    GLWASM_Runtime *instance=store->instances[i];
    GLWASM_Module *m=instance->module;
    if(m->ndatas)memcpy(instance->data_dropped,s->data_dropped[i],m->ndatas);
    for(uint32_t e=0;e<m->nelems;e++)
      if(!s->elem_counts[i][e])elem_drop(instance,e);
  }
  gcu_allocator_free(store->alloc,mems);gcu_allocator_free(store->alloc,tabs);
  {Run *run=&store->run;
  gcu_allocator_free(store->alloc,run->args);
  run->args=args;run->nargs=s->nargs;run->entry=s->entry;
  run->result=s->result;run->results=NULL;
  run->result_capacity=s->result_capacity;
  run->result_arity=s->result_arity;run->result_count=s->result_count;
  /* The core snapshot stores guest frames, not the run's pointers: bind it
   * to the destination store before resume. */
  run->ctx=store->context;run->engine=store->engine;run->store=store;
  run->entry_instance=s->entry_instance<store->ninstances
      ?store->instances[s->entry_instance]:r;
  run_bind(run,run->entry_instance);
  run->guest_alloc=store->guest_alloc;run->stack_base=0;
  run->bulk_func=s->bulk_func;run->bulk_pc=s->bulk_pc;run->bulk_op=s->bulk_op;
  run->jit_entry_attempted=s->jit_entry_attempted;run->jit_pending=s->jit_pending;
  run->jit_pending_function=s->jit_pending_function;
  run->bulk_dst=s->bulk_dst;run->bulk_src=s->bulk_src;run->bulk_len=s->bulk_len;
  run->bulk_done=s->bulk_done;run->bulk_value=s->bulk_value;run->bulk_active=s->bulk_active;
  run->active=1;run->paused=1;run->pending_host=0;run->error=GLWASM_OK;run->trap=GLWASM_TRAP_NONE;}
  r->jit_test_pause_bulk_chunks=0;
  return GLWASM_OK;
fail:
  for(uint32_t i=0;i<s->nmemories&&mems;i++)gcu_allocator_free(g,mems[i]);
  for(uint32_t i=0;i<s->ntables&&tabs;i++)gcu_allocator_free(g,tabs[i]);
  gcu_allocator_free(store->alloc,mems);gcu_allocator_free(store->alloc,tabs);
  gcu_allocator_free(store->alloc,args);
  return failure;
}

void glwasm_snapshot_release(GLWASM_Snapshot*s){snapshot_free(s);}
GLWASM_Result glwasm_runtime_invoke_results(GLWASM_Runtime*r,uint32_t fi,
    const uint64_t*args,uint32_t nargs,uint64_t*out,uint32_t*result_count){
  if(!r||fi>=r->module->nfuncs||(nargs&&!args)||!result_count||
      r->store->run.active)return GLWASM_ERR_INVALID;
  GLWASM_Func*fn=&r->module->funcs[fi];
  if(fn->type_index>=r->module->ntypes||
      nargs!=r->module->types[fn->type_index].nparams)return GLWASM_ERR_INVALID;
  uint32_t result_arity=r->module->types[fn->type_index].nresults;
  uint32_t capacity=*result_count;
  if(capacity<result_arity||(result_arity&&!out)){
    *result_count=result_arity;return GLWASM_ERR_LIMIT;
  }
  Run*state=&r->store->run;
  state->args=nargs?gcu_allocator_calloc(r->store->alloc,nargs,
      sizeof(*state->args)):NULL;
  if(nargs&&!state->args)return GLWASM_ERR_OOM;
  state->ctx=r->store->context;state->engine=r->store->engine;
  state->store=r->store;state->entry_instance=r;run_bind(state,r);
  state->guest_alloc=r->store->guest_alloc;
  state->entry=fi;state->nargs=nargs;state->stack_base=0;state->result=0;
  state->results=out;state->result_capacity=capacity;
  state->result_arity=result_arity;state->result_count=0;
  state->error=GLWASM_OK;state->trap=GLWASM_TRAP_NONE;state->paused=0;
  state->diagnostic_function=UINT32_MAX;
  state->diagnostic_pc=0;
  state->diagnostic_call_target=UINT32_MAX;
  state->diagnostic_call_sp=0;
  state->diagnostic_call_base=0;
  state->diagnostic_call_required=0;
  state->diagnostic_caller_params=0;
  state->diagnostic_caller_locals=0;
  state->diagnostic_callee_params=0;
  state->diagnostic_frame_entry_function=UINT32_MAX;
  state->diagnostic_frame_entry_sp=0;
  state->diagnostic_frame_entry_base=0;
  state->diagnostic_first_bad_sp_function=UINT32_MAX;
  state->diagnostic_first_bad_sp_pc=UINT32_MAX;
  state->diagnostic_first_bad_sp=0;
  state->diagnostic_first_bad_base=0;
  state->diagnostic_opcode=UINT16_MAX;
  state->diagnostic_has_instruction=0;
  state->diagnostic_reason=0;
  state->jit_entry_attempted=0;state->jit_pending=0;state->active=1;
  for(uint32_t i=0;i<nargs;i++)state->args[i]=args[i];
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result cr=grcore_run(r->store->context,execute,state,&outcome);
  r->store->last_core_result = cr;
  r->store->last_outcome = outcome;
  return finish_run(r,cr,outcome,out,result_count,NULL);
}
GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime*r,uint32_t fi,
    const uint64_t*args,uint32_t nargs,uint64_t*out){
  if(!r||fi>=r->module->nfuncs||!out)return GLWASM_ERR_INVALID;
  uint32_t capacity=1;
  if(r->module->funcs[fi].type_index>=r->module->ntypes||
      r->module->types[r->module->funcs[fi].type_index].nresults>1)
    return GLWASM_ERR_UNSUPPORTED;
  *out=0;
  return glwasm_runtime_invoke_results(r,fi,args,nargs,out,&capacity);
}
GLWASM_Result glwasm_runtime_resume_results(GLWASM_Runtime*r,uint64_t*out,
    uint32_t*result_count,int*paused){
  if(!r||!result_count||!r->store->run.active||!r->store->run.paused)
    return GLWASM_ERR_INVALID;
  if(*result_count<r->store->run.result_arity||
      (r->store->run.result_arity&&!out)){
    *result_count=r->store->run.result_arity;return GLWASM_ERR_LIMIT;
  }
  r->store->run.result_capacity=*result_count;
  r->store->run.results=out;
  /* `paused` describes the previous run's outcome. Clear it before the
   * restored callback is re-entered so a JIT entry cannot mistake the old
   * pause for a new poll verdict. finish_run sets it again if this resume
   * pauses a second time. */
  r->store->run.paused=0;
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result cr=grcore_resume(r->store->context,&outcome);
  r->store->last_core_result=cr;
  r->store->last_outcome=outcome;
  return finish_run(r,cr,outcome,out,result_count,paused);
}
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime*r,uint64_t*out,int*paused){
  if(!r||!out||!r->store->run.active||!r->store->run.paused||r->store->run.result_arity>1)
    return GLWASM_ERR_INVALID;
  uint32_t capacity=1;
  *out=0;
  return glwasm_runtime_resume_results(r,out,&capacity,paused);
}
GLWASM_Result glwasm_runtime_pause_position(const GLWASM_Runtime*r,
    uint32_t*function,uint64_t*offset){
  if(!r||!function||!offset||!r->store->run.paused)return GLWASM_ERR_INVALID;
  GRCORE_PollIdentity identity;
  GRCORE_Result result=grcore_context_poll_identity(r->store->context,&identity);
  if(result!=GRCORE_OK||identity.function>UINT32_MAX)return GLWASM_ERR_INVALID;
  *function=(uint32_t)identity.function;*offset=identity.offset;return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime*r){if(!r||r->store->run.active)return GLWASM_ERR_INVALID;GRCORE_Port*port=NULL;if(grcore_context_port(r->store->context,&port)!=GRCORE_OK)return GLWASM_ERR_OOM;GRCORE_Result cr=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);grcore_port_release(port);return cr==GRCORE_OK?GLWASM_OK:GLWASM_ERR_INTERNAL;}
GLWASM_Result glwasm_store_test_pause_on_fuel(GLWASM_Store *store,
    int enabled) {
  if (!store || store->run.active) return GLWASM_ERR_INVALID;
  store->test_fuel_pauses = enabled ? 1u : 0u;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_test_pause_after_bulk_chunks(
    GLWASM_Runtime *runtime, uint32_t chunks) {
  if (!runtime || runtime->store->run.active) return GLWASM_ERR_INVALID;
  runtime->jit_test_pause_bulk_chunks = chunks;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_test_observed_functions(
    const GLWASM_Runtime *runtime, uint8_t *out, uint32_t capacity,
    uint32_t *required) {
  if (!runtime || !required) return GLWASM_ERR_INVALID;
  const uint32_t count = runtime->module->nfuncs;
  *required = count;
  if (capacity < count) return GLWASM_ERR_LIMIT;
  if (count && (!out || !runtime->test_observed_functions))
    return GLWASM_ERR_INVALID;
  if (count) memcpy(out, runtime->test_observed_functions, count);
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_test_reset_observed_functions(
    GLWASM_Runtime *runtime) {
  if (!runtime || runtime->store->run.active || runtime->store->nested_active)
    return GLWASM_ERR_INVALID;
  if (runtime->module->nfuncs && !runtime->test_observed_functions) {
    runtime->test_observed_functions = gcu_allocator_calloc(
        &runtime->module->alloc, runtime->module->nfuncs,
        sizeof(*runtime->test_observed_functions));
    if (!runtime->test_observed_functions) return GLWASM_ERR_OOM;
  }
  if (runtime->module->nfuncs)
    memset(runtime->test_observed_functions, 0, runtime->module->nfuncs);
  return GLWASM_OK;
}
GLWASM_Trap glwasm_runtime_last_trap(const GLWASM_Runtime*r){return r?r->store->run.trap:GLWASM_TRAP_NONE;}
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime*r){return r&&r->store->context?grcore_context_fuel_used(r->store->context):0;}
GLWASM_Result glwasm_runtime_test_set_jit_mode(GLWASM_Runtime *runtime,
    GLWASM_TestJitMode mode) {
  if (!runtime || runtime->store->run.active ||
      mode < GLWASM_TEST_JIT_INTERPRETER || mode > GLWASM_TEST_JIT_REQUIRE)
    return GLWASM_ERR_INVALID;
  runtime->jit_mode = mode;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_test_failure_info(GLWASM_Runtime *runtime,
    uint32_t function, GLWASM_TestRuntimeFailureInfo *out) {
  if (!runtime || !out || function >= runtime->module->nfuncs)
    return GLWASM_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  out->run_error = runtime->store->run.error;
  out->trap = runtime->store->run.trap;
  out->core_result = runtime->store->last_core_result;
  out->outcome = runtime->store->last_outcome;
  out->last_function = runtime->store->run.diagnostic_function;
  out->last_pc = runtime->store->run.diagnostic_pc;
  out->call_target = runtime->store->run.diagnostic_call_target;
  out->call_sp = runtime->store->run.diagnostic_call_sp;
  out->call_base = runtime->store->run.diagnostic_call_base;
  out->call_required = runtime->store->run.diagnostic_call_required;
  out->caller_params = runtime->store->run.diagnostic_caller_params;
  out->caller_locals = runtime->store->run.diagnostic_caller_locals;
  out->callee_params = runtime->store->run.diagnostic_callee_params;
  out->frame_entry_function = runtime->store->run.diagnostic_frame_entry_function;
  out->frame_entry_sp = runtime->store->run.diagnostic_frame_entry_sp;
  out->frame_entry_base = runtime->store->run.diagnostic_frame_entry_base;
  out->first_bad_sp_function = runtime->store->run.diagnostic_first_bad_sp_function;
  out->first_bad_sp_pc = runtime->store->run.diagnostic_first_bad_sp_pc;
  out->first_bad_sp = runtime->store->run.diagnostic_first_bad_sp;
  out->first_bad_base = runtime->store->run.diagnostic_first_bad_base;
  out->jit_lower_pc = runtime->store->run.last_jit_lower_pc;
  out->jit_lower_raw_result = runtime->store->run.last_jit_lower_raw_result;
  out->jit_lower_stack_count = runtime->store->run.last_jit_lower_stack_count;
  out->jit_lower_site = runtime->store->run.last_jit_lower_site;
  out->deopt_count = runtime->test_deopt_count;
  out->first_deopt = runtime->test_first_deopt;
  out->last_deopt = runtime->test_last_deopt;
  out->jit_lower_opcode = runtime->store->run.last_jit_lower_opcode;
  out->last_opcode = runtime->store->run.diagnostic_opcode;
  out->has_last_instruction = runtime->store->run.diagnostic_has_instruction;
  out->diagnostic_reason = runtime->store->run.diagnostic_reason;
  out->has_jit_cache = runtime->module->jit_cache != NULL;
  out->has_jit_pages = runtime->module->jit_pages != NULL;
  out->has_jit_slots = runtime->jit_slots != NULL;
  out->has_jit_registered = runtime->jit_registered != NULL;
  out->has_function_slot = runtime->jit_slots && runtime->jit_slots[function];
  out->has_function_registered = runtime->jit_registered &&
      runtime->jit_registered[function];
  out->context_matches_runtime = runtime->store->run.ctx == runtime->store->context;
  out->engine_matches_runtime = runtime->store->run.engine == runtime->store->engine;
  out->last_step = runtime->store->run.diagnostic_step;
  out->context_state_after_step = runtime->store->run.diagnostic_context_state;
  out->jit_compile_result = runtime->last_jit_compile_result;
  out->jit_compile_attempted = runtime->last_jit_compile_attempted;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_test_jit_stats(GLWASM_Runtime *runtime,
    uint32_t function, GLWASM_JitCacheStats *out) {
  if (!runtime || !out || function >= runtime->module->nfuncs)
    return GLWASM_ERR_INVALID;
  memset(out, 0, sizeof(*out));
  if (!runtime->module->jit_cache) return GLWASM_OK;
  return glwasm_jit_cache_stats(runtime->module->jit_cache, function, out)
      ? GLWASM_OK : GLWASM_ERR_INTERNAL;
}
GLWASM_Result glwasm_runtime_test_retire_jit_function(
    GLWASM_Runtime *runtime, uint32_t function) {
  if (!runtime || runtime->store->run.active || !runtime->module->jit_cache ||
      function >= runtime->module->nfuncs ||
      function < runtime->module->nimported_funcs)
    return GLWASM_ERR_INVALID;
  GLWASM_JitCacheStats stats;
  if (!glwasm_jit_cache_stats(runtime->module->jit_cache, function, &stats) ||
      stats.compile_successes == 0)
    return GLWASM_ERR_INVALID;
  for (unsigned i = 0; i < 64u; ++i)
    (void)glwasm_jit_cache_record_entry(runtime->module->jit_cache, function,
        GLWASM_JIT_EXIT_NAMED_UNSUPPORTED);
  if (!glwasm_jit_cache_stats(runtime->module->jit_cache, function, &stats) ||
      !stats.interpreter_only)
    return GLWASM_ERR_INTERNAL;
  jit_clear_runtime_entry(runtime, function);
  glwasm_jit_cache_finish_retirement(runtime->module->jit_cache, function);
  return GLWASM_OK;
}
uint64_t glwasm_runtime_test_jit_opcode_exit_count(GLWASM_Runtime *runtime,
    uint16_t opcode) {
  return runtime ? glwasm_jit_cache_opcode_exit_count(
      runtime->module->jit_cache, opcode) : 0;
}
