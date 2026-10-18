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
static int const_value(const GLWASM_Module*m,const GLWASM_Func*f,const uint64_t*g,uint64_t*out){uint64_t st[32];size_t sp=0;
#define CONST_PUSH(value) do { if(sp>=sizeof(st)/sizeof(st[0]))return 0; st[sp++]=(value); } while(0)
  for(uint32_t i=0;i<f->nins;i++){const GLWASM_Instr*x=&f->ins[i];if(x->op==0x0b){if(sp!=1)return 0;*out=st[0];return 1;}if(x->op==0x41)CONST_PUSH((uint32_t)x->c);else if(x->op==0x42)CONST_PUSH(x->c);else if(x->op==0x43)CONST_PUSH(x->a);else if(x->op==0x44)CONST_PUSH(x->c);else if(x->op==0x23&&x->a<m->nglobals)CONST_PUSH(g[x->a]);else if(sp>=2){uint64_t b=st[--sp],a=st[--sp],v=0;if(x->op==0x6a)v=(uint32_t)a+(uint32_t)b;else if(x->op==0x6b)v=(uint32_t)a-(uint32_t)b;else if(x->op==0x6c)v=(uint32_t)a*(uint32_t)b;else if(x->op==0x7c)v=a+b;else if(x->op==0x7d)v=a-b;else if(x->op==0x7e)v=a*b;else return 0;CONST_PUSH(v);}else return 0;}
#undef CONST_PUSH
  return 0;}
static int same_value_type(GLWASM_ValType a,GLWASM_ValType b){return a.kind==b.kind&&a.nullable==b.nullable&&a.heap==b.heap&&a.mut==b.mut&&a.index==b.index;}
static int same_function_type(const GLWASM_TypeDef*a,const GLWASM_TypeDef*b){if(a->nparams!=b->nparams||a->nresults!=b->nresults)return 0;for(uint32_t i=0;i<a->nparams;i++)if(!same_value_type(a->params[i],b->params[i]))return 0;for(uint32_t i=0;i<a->nresults;i++)if(!same_value_type(a->results[i],b->results[i]))return 0;return 1;}
static int element_ref(const GLWASM_Elem *e,uint32_t i,uint64_t *out){if(!e||!out)return 0;if(!e->use_exprs){if(i>=e->nfuncs)return 0;*out=(uint64_t)e->funcs[i]+1u;return 1;}if(i>=e->nexprs)return 0;const GLWASM_Func *expr=&e->exprs[i];int seen=0;uint64_t value=0;for(uint32_t k=0;k<expr->nins;k++){if(expr->ins[k].op==0xd2){if(seen)return 0;value=(uint64_t)expr->ins[k].a+1u;seen=1;}else if(expr->ins[k].op==0xd0){if(seen)return 0;value=0;seen=1;}else if(expr->ins[k].op!=0x0b||k+1u!=expr->nins)return 0;}if(!seen)return 0;*out=value;return 1;}
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

typedef struct HostBinding {
  GLWASM_HostFunction function;
  GLWASM_HostResumeFunction resume;
  GLWASM_HostFunctionResults function_results;
  GLWASM_HostResumeFunctionResults resume_results;
  void *user;
  GLWASM_HostValueType *parameter_types;
  GLWASM_HostValueType *result_types;
  uint8_t resource_kind;
  uint32_t resource_index;
  union {
    GLWASM_HostMemoryProvider memory;
    GLWASM_HostTableProvider table;
    GLWASM_HostGlobal *global;
    GLWASM_HostTag tag;
  } resource;
} HostBinding;

typedef struct ExceptionValue {
  struct ExceptionValue *next;
  const GCU_Allocator *allocator;
  uint64_t identity;
  uint32_t tag;
  uint8_t imported_tag;
  uint8_t reserved[3];
  uint64_t host_tag_identity;
  uint32_t payload_count;
  uint64_t payload[];
} ExceptionValue;

typedef struct Run {
  GLWASM_Module *m; GRCORE_Context *ctx; GRCORE_EngineId engine;
  GLWASM_Runtime *instance;
  uint64_t *globals; uint64_t **tables; uint64_t *table_sizes;
  uint8_t **memories; uint64_t *memory_bytes; uint64_t *memory_pages;
  HostBinding *host_resources;
  uint8_t *data_dropped;
  uint8_t *elements_dropped;
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
  GLWASM_Result error; GLWASM_RuntimeTrap trap; int active; int paused;
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

struct GLWASM_Runtime {
  GLWASM_Module *module; uint64_t *globals; uint64_t **tables;
  uint64_t *table_sizes; uint8_t **memories; uint64_t *memory_bytes;
  char **memory_debug_names;
  uint64_t *memory_pages; uint8_t *data_dropped; uint8_t *elements_dropped;
  HostBinding *host_functions;
  HostBinding *host_resources;
  const GCU_Allocator *guest_alloc;
  GRCORE_Group *group; GRCORE_Options *options;
  GRCORE_Context *context; GRCORE_EngineId engine; uintptr_t boundary_token; Run run;
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
  ExceptionValue *exceptions;
  GRCORE_Context *boundary_context;
  GRCORE_EngineId boundary_engine;
  Run nested_saved_run;
  uintptr_t nested_saved_token;
  GRCORE_Context *nested_context;
  GRCORE_EngineId nested_caller;
  uintptr_t nested_instance;
  size_t nested_base;
  GRCORE_BoundaryRef nested_boundary;
  int nested_active;
  int nested_owns_run;
  GLWASM_ForeignException last_foreign_exception;
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
  GRCORE_Result last_core_result;
  GRCORE_Outcome last_outcome;
};

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
  if(context==runtime->context){
    if(slots_out)*slots_out=runtime->jit_slots;
    if(registered_out)*registered_out=runtime->jit_registered;
    if(reservation_out)*reservation_out=runtime->jit_reservation;
    return 1;
  }
  if(context==runtime->boundary_context){
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
  if (!runtime || runtime->run.ctx!=context||
      !jit_state_for_context(runtime,context,NULL,NULL,&reservation)||
      !reservation) {
    return (uint32_t)GRCORE_ERR_INVALID;
  }
  size_t rebuilt = 0;
  size_t keep = cause == 2u ? 0u : SIZE_MAX;
  jit_record_active_nested_deopts(runtime, cause);
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
      !jit_state_for_context(runtime,runtime->run.ctx,&jit_slots,
          &jit_registered,NULL)||!jit_registered||
      !jit_registered[function]||!jit_slots||!jit_slots[function])return;
  GRCORE_Code *code = grcore_entry_slot_code(jit_slots[function]);
  const uintptr_t address = code
      ? (uintptr_t)grjit_code_address(glwasm_jit_artifact_code(code)) : 0;
  (void)grcore_entry_slot_clear(runtime->run.ctx,
      jit_slots[function]);
  if (address)
    (void)grcore_code_unregister(runtime->run.ctx, address);
  jit_registered[function] = 0;
}

static void jit_record_active_nested_deopts(GLWASM_Runtime *runtime,
    uint64_t cause) {
  if (!runtime || runtime->jit_call_depth == 0) return;
  GLWASM_JitExit propagated_exit = runtime->run.paused || cause == 1u
      ? GLWASM_JIT_EXIT_PAUSE
      : cause == 2u ? GLWASM_JIT_EXIT_LIMIT_UNWIND
      : GLWASM_JIT_EXIT_GUARD_DEOPT;
  GRCORE_Stack *stack = grcore_context_stack(runtime->run.ctx);
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
  if (!runtime || runtime->run.ctx!=context||active_jit_runtime!=runtime||
      !decode_jit_function(runtime, callee, &function) ||
      !jit_state_for_context(runtime, context, &jit_slots, NULL, NULL)) {
    if (runtime && runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GLWASM_Func *func = &runtime->module->funcs[function];
  GLWASM_TypeDef *type = &runtime->module->types[func->type_index];
  if (arg_count != type->nparams || result_count != type->nresults ||
      (arg_count && !args)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  if (grcore_context_depth(context, GRCORE_DEPTH_GUEST) >=
      grcore_context_guest_depth(context)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  if (runtime->jit_call_depth == SIZE_MAX ||
      !ensure_jit_spill(runtime, function, runtime->jit_call_depth + 1u)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  GRCORE_Code *callee_handle = jit_slots[function]
      ? grcore_entry_slot_code(jit_slots[function]) : NULL;
  size_t deopt_cells = jit_code_max_deopt_cells(callee_handle
      ? glwasm_jit_artifact_code(callee_handle) : NULL);
  if (deopt_cells == SIZE_MAX) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  /* Compiled frames are rebuilt as a chain, so reserve each callee's full
   * conversion maximum before its guest frame or any guest effect exists. */
  GLWASM_Result reserved = jit_extend_deopt_reservation(runtime, context,
      deopt_cells);
  if (reserved != GLWASM_OK) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = reserved;
    return 1u;
  }
  size_t callee_depth = runtime->jit_call_depth + 1u;
  runtime->jit_deopt_cells[callee_depth] = deopt_cells;
  GRCORE_Stack *stack = grcore_context_stack(context);
  GRCORE_FrameRef frame;
  if (grcore_stack_push(stack, runtime->run.engine, frame_slots(func), &frame) !=
      GRCORE_OK) {
    runtime->jit_deopt_cells[callee_depth] = 0;
    jit_retract_deopt_reservation(runtime, context, deopt_cells);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  uint64_t *slots_ptr = slots(stack, frame);
  if (!slots_ptr) {
    (void)grcore_stack_pop(stack);
    runtime->jit_deopt_cells[callee_depth] = 0;
    jit_retract_deopt_reservation(runtime, context, deopt_cells);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  memset(slots_ptr, 0, frame_slots(func) * sizeof(*slots_ptr));
  slots_ptr[H_FUNC] = function;
  slots_ptr[H_PC] = 0;
  slots_ptr[H_SP] = H_OPERANDS + func->nlocals;
  slots_ptr[H_NLOCALS] = func->nlocals;
  diagnostic_record_frame_entry(&runtime->run,function,slots_ptr[H_SP],
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
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  ++runtime->jit_call_depth;
  return 0u;
}

static void wasm_jit_pop(void *opaque_context) {
  GRCORE_Context *context = opaque_context;
  GLWASM_Runtime *runtime=active_jit_runtime;
  if (!runtime || runtime->run.ctx!=context||runtime->jit_call_depth == 0) {
    if (runtime) runtime->run.error = GLWASM_ERR_INTERNAL;
    return;
  }
  GRCORE_Stack *stack = grcore_context_stack(context);
  uint64_t *frame_slots_ptr = slots(stack, grcore_stack_top(stack));
  if (!frame_slots_ptr) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return;
  }
  jit_record_nested_exit(runtime, (uint32_t)frame_slots_ptr[H_FUNC],
      GLWASM_JIT_EXIT_RETURN);
  if (!grcore_stack_frame_count(stack) ||
      grcore_stack_pop(stack) != GRCORE_OK) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
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
      runtime->run.ctx != context || !runtime->run.pending_exception ||
      runtime->jit_call_depth == 0 ||
      (uint32_t)(callee >> 32) != runtime->module->debug_id) {
    if (runtime) runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GRCORE_Stack *stack = grcore_context_stack(context);
  uint64_t *frame_slots_ptr = slots(stack, grcore_stack_top(stack));
  if (!frame_slots_ptr || frame_slots_ptr[H_FUNC] >= runtime->module->nfuncs ||
      frame_slots_ptr[H_INSTANCE] != (uintptr_t)runtime) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  const uint32_t function = (uint32_t)frame_slots_ptr[H_FUNC];
  /* A tail call may replace the callee named at the original call site.
   * The top guest frame remains the one returned by this native edge. */
  jit_record_nested_exit(runtime, function,
      GLWASM_JIT_EXIT_WASM_EXCEPTION);
  if (grcore_stack_pop(stack) != GRCORE_OK) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
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
  if (!runtime||runtime->run.ctx!=context||active_jit_runtime!=runtime||
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
      runtime->run.error = installed;
    return 1u;
  }
  /* Retirement can clear a target's per-context entry while compiled callers
   * still exist. Exit to the interpreter before generated call code loads that
   * empty slot. */
  if (!jit_registered[target] || !jit_slots[target] ||
      jit_slots[target]->entry <= GRCORE_ENTRY_REFUSED) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_UNSUPPORTED;
    return 1u;
  }
  if (!ensure_jit_spill(runtime, target, runtime->jit_call_depth)) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  GRCORE_Code *target_handle = grcore_entry_slot_code(jit_slots[target]);
  size_t target_deopt_cells = jit_code_max_deopt_cells(target_handle
      ? glwasm_jit_artifact_code(target_handle) : NULL);
  if (target_deopt_cells == SIZE_MAX) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  /* Tail transfer replaces this frame, but reserve the target's full maximum
   * additively before mutating the stack. Rebuild needs the sum of every active
   * frame's converting locations; the old allowance is retracted on commit. */
  size_t added_deopt_cells = target_deopt_cells;
  size_t caller_slots_count = frame_slots(caller);
  if (caller_slots_count > SIZE_MAX / sizeof(uint64_t)) {
    runtime->run.error = GLWASM_ERR_LIMIT;
    return 1u;
  }
  uint64_t *saved = gcu_allocator_calloc(&runtime->module->alloc,
      caller_slots_count, sizeof(*saved));
  if (!saved) {
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = GLWASM_ERR_OOM;
    return 1u;
  }
  memcpy(saved, caller_slots, caller_slots_count * sizeof(*saved));
  GRCORE_PollIdentity caller_identity = {0};
  if (grcore_stack_identity(stack, caller_ref, &caller_identity) != GRCORE_OK) {
    gcu_allocator_free(&runtime->module->alloc, saved);
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GLWASM_Result deopt_reserved = jit_extend_deopt_reservation(runtime,
      context, added_deopt_cells);
  if (deopt_reserved != GLWASM_OK) {
    gcu_allocator_free(&runtime->module->alloc, saved);
    if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = deopt_reserved;
    return 1u;
  }
  if (grcore_stack_pop(stack) != GRCORE_OK) {
    jit_retract_deopt_reservation(runtime, context, added_deopt_cells);
    gcu_allocator_free(&runtime->module->alloc, saved);
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 1u;
  }
  GRCORE_FrameRef target_ref;
  GRCORE_Result pushed = grcore_stack_push(stack, runtime->run.engine,
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
      diagnostic_record_frame_entry(&runtime->run,target,
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
    GRCORE_Result restored = grcore_stack_push(stack, runtime->run.engine,
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
    if (restored != GRCORE_OK) runtime->run.error = GLWASM_ERR_INTERNAL;
    else if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
      runtime->run.error = map_core_result(pushed);
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
  if(!runtime||runtime->run.ctx!=context||
      !decode_jit_function(runtime,callee,&function)||
      !jit_state_for_context(runtime,context,&jit_slots,NULL,NULL))return 1u;
  GLWASM_Result result = jit_install_for_runtime(runtime, function);
  if (result == GLWASM_OK && jit_slots[function]->entry >
      GRCORE_ENTRY_REFUSED) return 0u;
  if (result == GLWASM_OK && runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE) {
    GLWASM_JitCacheStats stats;
    runtime->run.error = glwasm_jit_cache_stats(runtime->module->jit_cache,
        function, &stats) &&
        stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT]
        ? GLWASM_ERR_LIMIT : GLWASM_ERR_UNSUPPORTED;
  }
  if (runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE)
    if (result != GLWASM_OK) runtime->run.error = result;
  return 1u;
}

static GLWASM_Result jit_install_for_runtime(GLWASM_Runtime *runtime,
    uint32_t function_index) {
  GLWASM_Module *module = runtime->module;
  GRCORE_Context *context=runtime->run.ctx;
  GRCORE_EngineId engine=runtime->run.engine;
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
    runtime->run.last_jit_lower_pc = UINT32_MAX;
    runtime->run.last_jit_lower_raw_result = UINT32_MAX;
    runtime->run.last_jit_lower_opcode = UINT16_MAX;
    runtime->run.last_jit_lower_stack_count = UINT32_MAX;
    runtime->run.last_jit_lower_site = UINT32_MAX;
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
        &jit_code,
        &unsupported_opcode, &runtime->run.last_jit_lower_pc,
        &runtime->run.last_jit_lower_raw_result,
        &runtime->run.last_jit_lower_opcode,
        &runtime->run.last_jit_lower_stack_count,
        &runtime->run.last_jit_lower_site);
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
  GRCORE_EntrySlot **jit_slots=NULL;
  if (!jit_state_for_context(runtime,runtime->run.ctx,&jit_slots,NULL,NULL)||
      (runtime->run.ctx==runtime->context&&
       runtime->run.engine!=runtime->engine)||
      (runtime->run.ctx==runtime->boundary_context&&
       runtime->run.engine!=runtime->boundary_engine)) {
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
  GRCORE_Stack *stack = grcore_context_stack(runtime->run.ctx);
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
      runtime->run.ctx, root_deopt_cells);
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
      runtime->run.engine, false, NULL, &activation);
  if (cr != GRCORE_OK) {
    jit_release_compiled_reservations(runtime, runtime->run.ctx);
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? map_core_result(cr) : GLWASM_OK;
  }
  size_t words = grjit_code_out_words(code);
  if (!words || words > SIZE_MAX / sizeof(uint64_t)) {
    (void)grcore_activation_leave(stack, activation);
    jit_release_compiled_reservations(runtime, runtime->run.ctx);
    grcore_code_release(handle);
    return GLWASM_ERR_INTERNAL;
  }
  uint64_t *out = gcu_allocator_calloc(&runtime->module->alloc, words,
      sizeof(*out));
  if (!out) {
    (void)grcore_activation_leave(stack, activation);
    jit_release_compiled_reservations(runtime, runtime->run.ctx);
    grcore_code_release(handle);
    return runtime->jit_mode == GLWASM_TEST_JIT_REQUIRE
        ? GLWASM_ERR_OOM : GLWASM_OK;
  }
  GLWASM_Runtime *previous_jit_runtime=active_jit_runtime;
  active_jit_runtime=runtime;
  uint32_t exit = grjit_code_call(code, runtime->run.ctx, runtime,
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
  jit_release_compiled_reservations(runtime, runtime->run.ctx);
  if(exit==GRJIT_EXIT_REBUILD_FAILED){
    /* A failed rebuild leaves compiled frames on the guest stack. The chain
     * has returned permanently, so discard frames created inside this JIT
     * activation before leaving it. finish_run/finish_nested will then unwind
     * the caller's remaining guest frames while preserving run.error. */
    GRCORE_Result unwind_result=grcore_unwind_to_activation(stack,activation,
        NULL);
    if(unwind_result!=GRCORE_OK && runtime->run.error==GLWASM_OK)
      runtime->run.error=GLWASM_ERR_INTERNAL;
  } else if (exit == GRJIT_EXIT_TRAP && runtime->run.error != GLWASM_OK) {
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
    jit_release_compiled_reservations(runtime, runtime->run.ctx);
    gcu_allocator_free(&runtime->module->alloc, out);
    grcore_code_release(handle);
    if (runtime->run.error == GLWASM_OK)
      runtime->run.error = GLWASM_ERR_INTERNAL;
    return runtime->run.error;
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
    if (runtime->run.paused || cause == 1u)
      cache_exit = GLWASM_JIT_EXIT_PAUSE;
    else if (cause == 2u) cache_exit = GLWASM_JIT_EXIT_LIMIT_UNWIND;
    else if (!had_nested_compiled_frames &&
        runtime->jit_exit_opcode != UINT16_MAX &&
        runtime->jit_exit_function == root_exit_function) {
      cache_exit = runtime->jit_exit_trap_reconstruction
          ? GLWASM_JIT_EXIT_TRAP_RECONSTRUCTION
          : GLWASM_JIT_EXIT_NAMED_UNSUPPORTED;
    } else cache_exit = GLWASM_JIT_EXIT_GUARD_DEOPT;
    if (cause == 1u) runtime->run.paused = 1;
    else if (cause == 2u) runtime->run.error = GLWASM_ERR_LIMIT;
  } else if (exit == GRJIT_EXIT_REBUILD_FAILED) {
    /* A host callback can complete with a guest-visible failure after its
     * compiled caller has already become impossible to rebuild. Preserve that
     * authoritative callback status; the failed chain is discarded by
     * finish_run. With no status to preserve, a failed rebuild is internal. */
    if (runtime->run.error == GLWASM_OK)
      runtime->run.error = GLWASM_ERR_INTERNAL;
  } else if (exit == GRJIT_EXIT_TRAP) {
    if (runtime->run.error == GLWASM_OK) {
      runtime->run.error = GLWASM_ERR_GUEST;
      cache_exit = GLWASM_JIT_EXIT_WASM_TRAP;
    } else {
      cache_exit = GLWASM_JIT_EXIT_HOST_CALL;
    }
  } else if (exit == GRJIT_EXIT_EXCEPTION) {
    ExceptionValue *exception = runtime->run.pending_exception;
    if (!exception) {
      runtime->run.error = GLWASM_ERR_INTERNAL;
      cache_exit = GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE;
    } else {
      uint32_t origin_pc = 0;
      uint64_t *origin = slots(stack, grcore_stack_top(stack));
      if (origin && origin[H_FUNC] == root_exit_function &&
          origin[H_PC] <= module->funcs[root_exit_function].nins)
        origin_pc = (uint32_t)origin[H_PC];
      if (!dispatch_exception(&runtime->run, exception))
        (void)finish_uncaught_exception(runtime->run.ctx, &runtime->run,
            root_exit_function, origin_pc, exception);
      cache_exit = GLWASM_JIT_EXIT_WASM_EXCEPTION;
    }
  } else {
    runtime->run.error = GLWASM_ERR_INTERNAL;
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
  GRCORE_Context *context = runtime->run.ctx
      ? runtime->run.ctx : runtime->context;
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

struct GLWASM_Snapshot {
  GLWASM_Module *module;
  GRCORE_Snapshot *core;
  uint64_t *globals;
  uint8_t **memories;
  uint64_t *memory_bytes,*memory_pages;
  uint64_t **tables;
  uint64_t *table_sizes;
  uint8_t *data_dropped,*elements_dropped;
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

static void free_exceptions(GLWASM_Runtime *runtime) {
  ExceptionValue *exception=runtime?runtime->exceptions:NULL;
  while(exception) {
    ExceptionValue *next=exception->next;
    gcu_allocator_free(exception->allocator,exception);
    exception=next;
  }
  if(runtime)runtime->exceptions=NULL;
  if(runtime)runtime->run.pending_exception=NULL;
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

/* Legacy WAST execution exposes the spec's print-only spectest functions. */
static GLWASM_Result spectest_noop(const GLWASM_HostCall *call,
    uint64_t *result) {
  (void)call;
  if (result) *result = 0;
  return GLWASM_OK;
}

static GLWASM_Result call_host(GLWASM_Runtime *runtime, uint32_t function,
    const uint64_t *arguments, uint64_t *results, int resume) {
  if(!runtime||function>=runtime->module->nfuncs||
      !runtime->host_functions||(!runtime->host_functions[function].function&&
       !runtime->host_functions[function].function_results))
    return GLWASM_ERR_LINK;
  GLWASM_Func *func=&runtime->module->funcs[function];
  if(func->type_index>=runtime->module->ntypes)return GLWASM_ERR_INTERNAL;
  GLWASM_TypeDef *type=&runtime->module->types[func->type_index];
  HostBinding *binding=&runtime->host_functions[function];
  if(type->nresults>1&&!binding->function_results)return GLWASM_ERR_UNSUPPORTED;
  GLWASM_HostCall call={.instance=runtime,
      .context=runtime->run.ctx?runtime->run.ctx:runtime->context,
      .engine=runtime->run.engine?runtime->run.engine:runtime->engine,
      .instance_token=runtime->boundary_token,
      .has_resume=binding->resume!=NULL||binding->resume_results!=NULL,
      .types=runtime->host_functions[function].parameter_types,
      .result_types=runtime->host_functions[function].result_types,
      .arguments=arguments,.argument_count=type->nparams,
      .result_count=type->nresults,
      .result_type=type->nresults==1?host_value_type(type->results[0]):GLWASM_HOST_I32,
      .user=runtime->host_functions[function].user};
  if(type->nresults==1&&call.result_type==0)return GLWASM_ERR_UNSUPPORTED;
  uint64_t ignored=0;
  uint64_t *output=type->nresults?results:&ignored;
  if(type->nresults&&!output)return GLWASM_ERR_INVALID;
  GRCORE_Context *context=runtime->run.ctx?runtime->run.ctx:runtime->context;
  GRCORE_Stack *stack=grcore_context_stack(context);
  GRCORE_ActivationRef activation={0};
  GRCORE_Result cr=grcore_activation_enter(stack,GRCORE_ACTIVATION_NATIVE,
      runtime->run.engine?runtime->run.engine:runtime->engine,true,NULL,
      &activation);
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
  Run *run = &runtime->run;
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
  int resume = run->pending_host &&
      run->pending_host_function == instruction.a;
  GLWASM_Result called = call_host(runtime, instruction.a, arguments,
      result_values, resume);
  if (called == GLWASM_OK) {
    run->pending_host = 0;
    return 0u;
  }
  if (called == GLWASM_ERR_PAUSED) {
    run->pending_host = 1;
    run->pending_host_function = instruction.a;
    GRCORE_Verdict verdict = request_host_pause(runtime->run.ctx,
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

static int memory_range(const Run *r, uint32_t index, uint64_t address,
    uint64_t offset, uint64_t width, uint64_t *out) {
  if (index >= r->m->nmems || offset > UINT64_MAX - address) return 0;
  uint64_t effective = address + offset;
  if (effective > r->memory_bytes[index] ||
      (uint64_t)width > r->memory_bytes[index] - effective) return 0;
  *out = effective;
  return 1;
}

typedef struct MemoryLease {
  GLWASM_HostMemoryView view;
  GLWASM_HostMemoryProvider *provider;
} MemoryLease;

static HostBinding *resource_binding(const Run *r, uint8_t kind,
    uint32_t index) {
  if(!r||!r->host_resources)return NULL;
  for(uint32_t i=0;i<r->m->nimports;i++){
    const GLWASM_Import *im=&r->m->imports[i];
    uint8_t expected=im->kind==GLWASM_EXT_MEM?GLWASM_HOST_IMPORT_MEMORY:
        im->kind==GLWASM_EXT_TABLE?GLWASM_HOST_IMPORT_TABLE:
        im->kind==GLWASM_EXT_GLOBAL?GLWASM_HOST_IMPORT_GLOBAL:
        im->kind==GLWASM_EXT_TAG?GLWASM_HOST_IMPORT_TAG:GLWASM_HOST_IMPORT_FUNCTION;
    if(expected==kind&&im->index==index&&
        r->host_resources[i].resource_kind==kind&&
        r->host_resources[i].resource_index==index)return &r->host_resources[i];
  }
  return NULL;
}

static int memory_acquire(Run *r,uint32_t index,MemoryLease *lease) {
  if(!r||!lease||index>=r->m->nmems)return 0;
  memset(lease,0,sizeof(*lease));
  HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_MEMORY,index);
  if(binding){
    lease->provider=&binding->resource.memory;
    if(lease->provider->begin(lease->provider->user,&lease->view)!=GLWASM_OK)return 0;
    const GLWASM_Memory *decl=&r->m->mems[index];
    uint64_t pages=lease->view.length_bytes/65536u;
    if((lease->view.length_bytes&&!lease->view.bytes)||lease->view.length_bytes%65536u||
        lease->view.length_bytes>r->m->limits.memory_maximum||
        pages<decl->min||(decl->has_max&&pages>decl->max)){
      lease->provider->end(lease->provider->user,&lease->view);memset(lease,0,sizeof(*lease));return 0;}
    r->memory_bytes[index]=lease->view.length_bytes;
    r->memory_pages[index]=lease->view.length_bytes/65536u;
  }else{
    lease->view.bytes=r->memories[index];lease->view.length_bytes=r->memory_bytes[index];
  }
  return 1;
}

static void memory_release(MemoryLease *lease) {
  if(lease&&lease->provider)
    lease->provider->end(lease->provider->user,&lease->view);
  if(lease)memset(lease,0,sizeof(*lease));
}

static int memory_grow(Run *run, uint32_t index, uint64_t delta,
    uint64_t *old_pages);

static uint64_t wasm_jit_memory_access(void *environment, uint64_t function,
    uint64_t pc, uint64_t address, uint64_t value) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || function >= runtime->module->nfuncs ||
      pc >= runtime->module->funcs[function].nins) return 0;
  runtime->jit_exit_trap_reconstruction = 0;
  GLWASM_Func *func = &runtime->module->funcs[function];
  const GLWASM_Instr instruction = func->translated->ins[pc].source;
  runtime->jit_helper_status = 1u;
  Run *run = &runtime->run;
  if (instruction.op == 0x40u) {
    if (instruction.a >= run->m->nmems ||
        run->m->mems[instruction.a].imported)
      return 0;
    uint64_t old_pages = 0;
    int grew = memory_grow(run, instruction.a, address, &old_pages);
    runtime->jit_helper_status = 0u;
    return grew ? old_pages :
        (run->m->mems[instruction.a].addr64 ? UINT64_MAX : UINT32_MAX);
  }
  const size_t width = memory_access_size(instruction.op);
  MemoryLease lease;
  uint64_t effective = 0;
  /* An imported provider's begin/end callbacks may be observable. Avoid
   * probing them before a guard that would replay the instruction. */
  if (instruction.b >= run->m->nmems || run->m->mems[instruction.b].imported)
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

/* One interpreter-sized memory.copy/fill chunk. The compiled caller polls
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
  if ((instruction.op != 0xfc0au && instruction.op != 0xfc0bu) ||
      instruction.a >= runtime->module->nmems ||
      (sub == 10u && instruction.b >= runtime->module->nmems)) return 1u;
  size_t capacity = glwasm_operand_capacity(func);
  if (argument_base > capacity || capacity - argument_base < 3u) return 1u;
  uint32_t destination_memory = instruction.a;
  uint32_t source_memory = instruction.b;
  if (runtime->module->mems[destination_memory].imported ||
      (sub == 10u && runtime->module->mems[source_memory].imported))
    return 1u;
  Run *run = &runtime->run;
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
    run->bulk_src = sub == 10u ? second : 0;
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
    if (sub == 10u) {
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

static uint32_t wasm_jit_helper_succeeded(void *environment) {
  GLWASM_Runtime *runtime = environment;
  /* GRJIT guards deopt on zero, so invert the helper's zero-on-success status. */
  return runtime && runtime->jit_helper_status == 0u;
}

static uint64_t wasm_jit_global_get(void *environment, uint64_t index) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || index >= runtime->module->nglobals) {
    if (runtime) runtime->jit_helper_status = 1u;
    return 0;
  }
  Run *run = &runtime->run;
  const GLWASM_Global *global = &runtime->module->globals[index];
  if (global->type.kind == GLWASM_VK_REF &&
      global->type.heap != GLWASM_HT_FUNC &&
      global->type.heap != GLWASM_HT_NOFUNC) {
    runtime->jit_helper_status = 1u;
    return 0;
  }
  HostBinding *binding = resource_binding(run,
      GLWASM_HOST_IMPORT_GLOBAL, (uint32_t)index);
  if (binding) {
    if (!binding->resource.global || !binding->resource.global->value) {
      runtime->jit_helper_status = 1u;
      return 0;
    }
    runtime->globals[index] = *binding->resource.global->value;
  }
  if (!global_reference_valid(run, (uint32_t)index, runtime->globals[index])) {
    runtime->jit_exit_trap_reconstruction = 1;
    runtime->jit_helper_status = 1u;
    return 0;
  }
  runtime->jit_helper_status = 0u;
  return runtime->globals[index];
}

static uint32_t wasm_jit_global_set(void *environment, uint64_t index,
    uint64_t value) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || index >= runtime->module->nglobals) {
    if (runtime) runtime->jit_helper_status = 1u;
    return 1u;
  }
  const GLWASM_Global *global = &runtime->module->globals[index];
  if (!global->mut || (global->type.kind == GLWASM_VK_REF &&
      global->type.heap != GLWASM_HT_FUNC &&
      global->type.heap != GLWASM_HT_NOFUNC)) {
    runtime->jit_helper_status = 1u;
    return 1u;
  }
  Run *run = &runtime->run;
  if (!global_reference_valid(run, (uint32_t)index, value)) {
    runtime->jit_exit_trap_reconstruction = 1;
    runtime->jit_helper_status = 1u;
    return 1u;
  }
  HostBinding *binding = resource_binding(run,
      GLWASM_HOST_IMPORT_GLOBAL, (uint32_t)index);
  if (binding) {
    if (!binding->resource.global || !binding->resource.global->value ||
        !binding->resource.global->mutable_) {
      runtime->jit_helper_status = 1u;
      return 1u;
    }
  }
  runtime->globals[index] = value;
  if (binding) {
    *binding->resource.global->value = value;
  }
  runtime->jit_helper_status = 0u;
  return 0u;
}

static uintptr_t wasm_jit_slot_entry(void *environment, uint64_t callee) {
  GLWASM_Runtime *runtime = environment;
  uint32_t function = 0;
  GRCORE_EntrySlot **jit_slots=NULL;
  if (!runtime||!jit_state_for_context(runtime,runtime->run.ctx,&jit_slots,
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
    GLWASM_Global *decl=&runtime->module->globals[index];
    if(decl->imported){
      Run inspection={.m=runtime->module,
          .host_resources=runtime->host_resources};
      HostBinding *binding=resource_binding(&inspection,
          GLWASM_HOST_IMPORT_GLOBAL,(uint32_t)index);
      GLWASM_HostGlobal *host=binding?binding->resource.global:NULL;
      if(!host||!host->value||host->type!=host_value_type(decl->type)||
          (host->mutable_!=0)!=(decl->mut!=0))
        return GRCORE_ERR_INVALID;
      value=*host->value;
      if(!global_reference_valid(&inspection,(uint32_t)index,value))
        return GRCORE_ERR_INVALID;
    }else{
      value=runtime->globals[index];
    }
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
  Run view={.m=runtime->module,.instance=runtime,.host_resources=runtime->host_resources,
    .memories=runtime->memories,.memory_bytes=runtime->memory_bytes,
    .memory_pages=runtime->memory_pages};
  MemoryLease lease;
  if(!memory_acquire(&view,(uint32_t)index,&lease))return GRCORE_ERR_INVALID;
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
  Run view={.m=runtime->module,.instance=runtime,.host_resources=runtime->host_resources,
    .memories=runtime->memories,.memory_bytes=runtime->memory_bytes,
    .memory_pages=runtime->memory_pages};
  MemoryLease lease;
  if(!memory_acquire(&view,(uint32_t)index,&lease))return GRCORE_ERR_INVALID;
  if(offset>SIZE_MAX||offset>lease.view.length_bytes||
      (uint64_t)size>lease.view.length_bytes-offset){
    memory_release(&lease);return GRCORE_ERR_INVALID;
  }
  if(size)memcpy(buffer,lease.view.bytes+(size_t)offset,size);
  memory_release(&lease);
  return GRCORE_OK;
}

typedef struct TableLease {
  GLWASM_HostTableView view;
  GLWASM_HostTableProvider *provider;
} TableLease;
static int table_acquire(Run *r,uint32_t index,TableLease *lease) {
  if(!r||!lease||index>=r->m->ntables)return 0;
  memset(lease,0,sizeof(*lease));
  HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_TABLE,index);
  if(binding){
    lease->provider=&binding->resource.table;
    if(lease->provider->begin(lease->provider->user,&lease->view)!=GLWASM_OK)return 0;
    const GLWASM_Table *decl=&r->m->tables[index];
    if((lease->view.length&&!lease->view.elements)||lease->view.length<decl->min||
        (decl->has_max&&lease->view.length>decl->max)){
      lease->provider->end(lease->provider->user,&lease->view);memset(lease,0,sizeof(*lease));return 0;}
    r->table_sizes[index]=lease->view.length;
  }else{
    lease->view.elements=r->tables[index];lease->view.length=r->table_sizes[index];
  }
  return 1;
}
static void table_release(TableLease *lease) {
  if(lease&&lease->provider)lease->provider->end(lease->provider->user,&lease->view);
  if(lease)memset(lease,0,sizeof(*lease));
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
  Run *run = &runtime->run;
  if (instruction.b >= runtime->module->ntables ||
      runtime->module->tables[instruction.b].imported) return 0;
  TableLease lease;
  if (!table_acquire(run, instruction.b, &lease)) return 0;
  uint32_t target = 0;
  int valid = 0;
  if (table_index < lease.view.length) {
    uint64_t reference = lease.view.elements[table_index];
    if (reference != 0 && reference <= runtime->module->nfuncs) {
      uint32_t candidate = (uint32_t)(reference - 1u);
      if (candidate < runtime->module->nfuncs &&
          runtime->module->funcs[candidate].type_index <
              runtime->module->ntypes &&
          same_function_type(&runtime->module->types[instruction.a],
              &runtime->module->types[
                  runtime->module->funcs[candidate].type_index])) {
        target = candidate;
        valid = 1;
      }
    }
  }
  table_release(&lease);
  if (valid) runtime->jit_helper_status = 0u;
  else runtime->jit_exit_trap_reconstruction = 1;
  return target;
}

static int table_reference_valid(const Run *r,uint32_t index,uint64_t token) {
  if(!token||index>=r->m->ntables)return 1;
  const GLWASM_ValType type=r->m->tables[index].elem;
  if(type.kind!=GLWASM_VK_REF||type.heap==GLWASM_HT_FUNC||type.heap==GLWASM_HT_NOFUNC)return 1;
  HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_TABLE,index);
  return binding&&binding->resource.table.validate_token&&
      binding->resource.table.validate_token(binding->resource.table.user,token)==GLWASM_OK;
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
  if ((instruction.op != 0xfc0eu && instruction.op != 0xfc11u) ||
      instruction.a >= runtime->module->ntables ||
      (sub == 14u && instruction.b >= runtime->module->ntables)) return 1u;
  size_t capacity = glwasm_operand_capacity(func);
  if (argument_base > capacity || capacity - argument_base < 3u) return 1u;
  uint64_t *spill = runtime->jit_spills[runtime->jit_call_depth];
  uint64_t *operands = spill + func->nlocals;
  uint64_t destination = operands[argument_base];
  uint64_t source = sub == 14u ? operands[argument_base + 1u] : 0;
  uint64_t length = operands[argument_base + 2u];
  /* Table32 operands are stored through i32 native spills, which may leave
   * upper bits from an earlier i64 value in the same reusable slot. */
  if (!runtime->module->tables[instruction.a].addr64) {
    destination = (uint32_t)destination;
    length = (uint32_t)length;
  }
  if (sub == 14u && !runtime->module->tables[instruction.b].addr64)
    source = (uint32_t)source;
  if (length > 1024u || (sub == 14u &&
      !same_value_type(runtime->module->tables[instruction.a].elem,
          runtime->module->tables[instruction.b].elem))) return 1u;
  uint64_t value = sub == 17u ? operands[argument_base + 1u] : 0;
  Run *run = &runtime->run;
  /* Acquiring imported table views is itself an embedder callback. Leave
   * those operations to the interpreter so a failed guard cannot replay it. */
  if (runtime->module->tables[instruction.a].imported ||
      (sub == 14u && runtime->module->tables[instruction.b].imported))
    return 1u;
  if (sub == 17u && !table_reference_valid(run, instruction.a, value))
    return 1u;
  GLWASM_Table *dst_decl = &runtime->module->tables[instruction.a];
  if (dst_decl->elem.heap == GLWASM_HT_FUNC ||
      dst_decl->elem.heap == GLWASM_HT_NOFUNC) {
    if (sub == 17u && value > runtime->module->nfuncs) return 1u;
  } else return 1u;
  TableLease dst_lease, src_lease;
  if (!table_acquire(run, instruction.a, &dst_lease)) return 1u;
  int distinct_source = sub == 14u && instruction.a != instruction.b;
  if (distinct_source && !table_acquire(run, instruction.b, &src_lease)) {
    table_release(&dst_lease);
    return 1u;
  }
  const uint64_t dst_length = dst_lease.view.length;
  const uint64_t src_length = sub == 14u
      ? (distinct_source ? src_lease.view.length : dst_length) : 0;
  int valid = destination <= dst_length && length <= dst_length - destination;
  if (sub == 14u)
    valid = valid && source <= src_length && length <= src_length - source;
  if (!valid) runtime->jit_exit_trap_reconstruction = 1;
  if (valid && length) {
    uint64_t *dst = dst_lease.view.elements + (size_t)destination;
    if (sub == 14u) {
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
  if(!token||index>=r->m->nglobals)return 1;
  const GLWASM_ValType type=r->m->globals[index].type;
  if(type.kind!=GLWASM_VK_REF||type.heap==GLWASM_HT_FUNC||type.heap==GLWASM_HT_NOFUNC)return 1;
  for(uint32_t i=0;i<r->m->nimports;i++){
    if(r->m->imports[i].kind!=GLWASM_EXT_GLOBAL||r->m->imports[i].index!=index)continue;
    HostBinding *binding=&r->host_resources[i];
    return binding->resource.global&&binding->resource.global->validate_token&&
        binding->resource.global->validate_token(binding->resource.global->user,token)==GLWASM_OK;
  }
  return 0;
}

static int memory_grow(Run *r, uint32_t index, uint64_t delta,
    uint64_t *old_pages) {
  const uint64_t page_size = UINT64_C(65536);
  if (index >= r->m->nmems) return 0;
  MemoryLease lease;
  if(!memory_acquire(r,index,&lease))return 0;
  uint64_t old = r->memory_pages[index];
  memory_release(&lease);
  if (old_pages) *old_pages = old;
  if (delta > UINT64_MAX - old) return 0;
  uint64_t next = old + delta;
  GLWASM_Memory *decl = &r->m->mems[index];
  const uint64_t address_limit = decl->addr64 ? (UINT64_C(1) << 48u) : 65536u;
  if ((decl->has_max && next > decl->max) || next > address_limit ||
      next > SIZE_MAX / page_size ||
      next > r->m->limits.memory_maximum / page_size)
    return 0;
  HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_MEMORY,index);
  if(binding){
    uint64_t provider_old=0;
    if(binding->resource.memory.grow(binding->resource.memory.user,delta,
        &provider_old)!=GLWASM_OK||provider_old!=old)return 0;
    if(!memory_acquire(r,index,&lease))return 0;
    int success=r->memory_pages[index]==next;
    memory_release(&lease);
    return success;
  }
  uint64_t bytes = next * page_size;
  if (bytes == r->memory_bytes[index]) return 1;
  uint8_t *grown = gcu_allocator_calloc(r->guest_alloc, (size_t)bytes, 1);
  if (!grown) return 0;
  if (r->memory_bytes[index])
    memcpy(grown, r->memories[index], (size_t)r->memory_bytes[index]);
  gcu_allocator_free(r->guest_alloc, r->memories[index]);
  r->memories[index] = grown;
  r->memory_bytes[index] = bytes;
  r->memory_pages[index] = next;
  return 1;
}

static GLWASM_Result initialize_memories(GLWASM_Module *m, uint64_t *globals,
    uint8_t **memories, uint64_t *memory_bytes, uint64_t *memory_pages,
    uint8_t *data_dropped, const GCU_Allocator *allocator,
    HostBinding *host_resources) {
  Run view={.m=m,.memories=memories,.memory_bytes=memory_bytes,
    .memory_pages=memory_pages,.host_resources=host_resources,
    .guest_alloc=allocator};
  for(uint32_t i=0;i<m->nmems;i++) {
    if(m->mems[i].imported){
      MemoryLease lease;
      if(!memory_acquire(&view,i,&lease))return GLWASM_ERR_LINK;
      uint64_t pages=view.memory_pages[i];GLWASM_Memory *decl=&m->mems[i];
      int valid=pages>=decl->min&&(!decl->has_max||pages<=decl->max);
      memory_release(&lease);if(!valid)return GLWASM_ERR_LINK;
    }else{
      uint64_t ignored=0;
      if(!memory_grow(&view,i,m->mems[i].min,&ignored)) return GLWASM_ERR_LIMIT;
    }
  }
  for(uint32_t i=0;i<m->ndatas;i++) {
    GLWASM_Data *d=&m->datas[i];
    if(d->passive) continue;
    uint64_t offset=0,end=0;
    MemoryLease lease;
    if(d->mem>=m->nmems||!const_value(m,&d->offset,globals,&offset)||
        !memory_acquire(&view,d->mem,&lease))return GLWASM_ERR_VALIDATE;
    int in_range=memory_range(&view,d->mem,offset,0,d->nbytes,&end);
    if(in_range&&d->nbytes)memcpy(lease.view.bytes+end,d->bytes,d->nbytes);
    memory_release(&lease);
    if(!in_range)
      return GLWASM_ERR_VALIDATE;
    if(data_dropped)data_dropped[i]=1;
  }
  return GLWASM_OK;
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
    if((fuel_limit!=UINT64_MAX&&
        grcore_context_fuel_remaining(context)==0)||
        (memory_limit!=UINT64_MAX&&memory_used>memory_limit)){
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
  for(ExceptionValue *exception=runtime->exceptions;exception;
      exception=exception->next)
    if(exception->identity==reference)return exception;
  return NULL;
}

static int exception_tag_matches(const Run *run,const ExceptionValue *exception,
    uint32_t tag_index) {
  if(!run||!exception||tag_index>=run->m->ntags)return 0;
  const GLWASM_Tag *tag=&run->m->tags[tag_index];
  if(tag->imported!=exception->imported_tag)return 0;
  if(!tag->imported)return tag_index==exception->tag;
  HostBinding *binding=resource_binding(run,GLWASM_HOST_IMPORT_TAG,tag_index);
  if(!binding||binding->resource.tag.identity==0||
      binding->resource.tag.identity!=exception->host_tag_identity||
      exception->tag>=run->m->ntags)return 0;
  const GLWASM_Tag *thrown_tag=&run->m->tags[exception->tag];
  if(thrown_tag->type_index>=run->m->ntypes||tag->type_index>=run->m->ntypes)
    return 0;
  const GLWASM_TypeDef *thrown_type=&run->m->types[thrown_tag->type_index];
  const GLWASM_TypeDef *caught_type=&run->m->types[tag->type_index];
  if(thrown_type->nparams!=caught_type->nparams)return 0;
  for(uint32_t i=0;i<thrown_type->nparams;i++){
    const GLWASM_ValType *a=&thrown_type->params[i];
    const GLWASM_ValType *b=&caught_type->params[i];
    if(a->kind!=b->kind||a->nullable!=b->nullable||a->heap!=b->heap||
        a->index!=b->index||a->mut!=b->mut)return 0;
  }
  return 1;
}

static ExceptionValue *new_exception(Run *run,uint32_t tag_index,
    const uint64_t *payload) {
  if(!run||!run->instance||tag_index>=run->m->ntags)return NULL;
  const GLWASM_Tag *tag=&run->m->tags[tag_index];
  if(tag->type_index>=run->m->ntypes)return NULL;
  const GLWASM_TypeDef *type=&run->m->types[tag->type_index];
  uint64_t bytes64=(uint64_t)sizeof(ExceptionValue)+
      (uint64_t)type->nparams*sizeof(uint64_t);
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
  exception->tag=tag_index;
  exception->imported_tag=tag->imported;
  exception->payload_count=type->nparams;
  if(tag->imported){
    HostBinding *binding=resource_binding(run,GLWASM_HOST_IMPORT_TAG,tag_index);
    if(!binding||!binding->resource.tag.identity){
      gcu_allocator_free(exception->allocator,exception);
      return NULL;
    }
    exception->host_tag_identity=binding->resource.tag.identity;
  }
  if(type->nparams)memcpy(exception->payload,payload,
      (size_t)type->nparams*sizeof(uint64_t));
  exception->next=run->instance->exceptions;
  run->instance->exceptions=exception;
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
  run->pending_exception=NULL;
  run->error=GLWASM_ERR_GUEST;
  return 0;
}

static int wasm_jit_exception_access(GLWASM_Runtime *runtime,
    uint64_t function, uint64_t *spill) {
  if (!runtime || runtime != active_jit_runtime ||
      runtime->run.ctx == NULL || function >= runtime->module->nfuncs ||
      runtime->jit_call_depth >= runtime->jit_spill_count || !spill ||
      runtime->jit_spills[runtime->jit_call_depth] != spill)
    return 0;
  uint64_t *frame = slots(grcore_context_stack(runtime->run.ctx),
      grcore_stack_top(grcore_context_stack(runtime->run.ctx)));
  return frame && frame[H_FUNC] == function &&
      frame[H_INSTANCE] == (uintptr_t)runtime;
}

static uint32_t wasm_jit_raise_tag(void *environment, uint64_t function,
    uint64_t tag, uint64_t *spill, uint64_t payload_base) {
  GLWASM_Runtime *runtime = environment;
  if (!wasm_jit_exception_access(runtime, function, spill) ||
      tag >= runtime->module->ntags ||
      runtime->module->tags[tag].type_index >= runtime->module->ntypes) {
    if (runtime) runtime->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  const GLWASM_TypeDef *type = &runtime->module->types[
      runtime->module->tags[tag].type_index];
  size_t capacity = runtime->jit_spill_capacities[runtime->jit_call_depth];
  if (payload_base > capacity || type->nparams > capacity - payload_base) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  ExceptionValue *exception = new_exception(&runtime->run, (uint32_t)tag,
      spill + payload_base);
  if (!exception) return 0u;
  for (uint32_t i = 0; i < type->nparams; ++i)
    if (type->params[i].kind == GLWASM_VK_I32 ||
        type->params[i].kind == GLWASM_VK_F32)
      exception->payload[i] &= UINT32_MAX;
  runtime->run.pending_exception = exception;
  return 1u;
}

static uint32_t wasm_jit_raise_ref(void *environment, uint64_t identity) {
  GLWASM_Runtime *runtime = environment;
  if (!runtime || runtime != active_jit_runtime || !runtime->run.ctx)
    return 0u;
  ExceptionValue *exception = find_exception(runtime, identity);
  if (!exception) {
    runtime->run.trap = GLWASM_TRAP_INVALID_REFERENCE;
    return 0u;
  }
  runtime->run.pending_exception = exception;
  return 1u;
}

static uint32_t wasm_jit_take_exception(void *environment, uint64_t function,
    uint64_t clause_index, uint64_t *spill, uint64_t base, uint64_t arity) {
  GLWASM_Runtime *runtime = environment;
  if (!wasm_jit_exception_access(runtime, function, spill)) {
    if (runtime) runtime->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  GLWASM_Func *func = &runtime->module->funcs[function];
  ExceptionValue *exception = runtime->run.pending_exception;
  if (!exception || clause_index >= func->ncatches ||
      base > glwasm_operand_capacity(func) ||
      arity > glwasm_operand_capacity(func) - base ||
      func->nlocals > runtime->jit_spill_capacities[runtime->jit_call_depth] ||
      base + arity > runtime->jit_spill_capacities[
          runtime->jit_call_depth] - func->nlocals) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  GLWASM_Catch *clause = &func->catches[clause_index];
  if (!clause->resolved ||
      (clause->kind < 2u && !exception_tag_matches(&runtime->run,
          exception, clause->tag)))
    return 0u;
  const uint32_t caught_count = clause->kind < 2u
      ? exception->payload_count : 0u;
  const uint32_t output_count = caught_count +
      (clause->kind == 1u || clause->kind == 3u);
  if (output_count != arity) {
    runtime->run.error = GLWASM_ERR_INTERNAL;
    return 0u;
  }
  uint64_t *destination = spill + func->nlocals + base;
  if (caught_count)
    memcpy(destination, exception->payload,
        (size_t)caught_count * sizeof(*destination));
  if (clause->kind == 1u || clause->kind == 3u)
    destination[caught_count] = exception->identity;
  runtime->run.pending_exception = NULL;
  return 1u;
}

static GRCORE_Step raise_trap(GRCORE_Context *ctx,Run *r,uint32_t fi,
    uint32_t pc,GLWASM_RuntimeTrap trap){
  r->trap=trap;
  const GLWASM_Func *fn=&r->m->funcs[fi];
  GRCORE_Verdict v=grcore_stack_poll(ctx,wasm_function_identity(r->m,fi),
      fn->translated->offsets[pc]);
  return v==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:GRCORE_STEP_FINISHED;
}

static int wasm_float_to_integer(double input, unsigned width, int is_signed,
    uint64_t *out) {
  if (__builtin_isnan(input)) return 0;
  if (width == 32u) {
    if (is_signed) {
      if (input < -2147483648.0 || input >= 2147483648.0) return 0;
      *out = (uint32_t)(int32_t)trunc(input);
    } else {
      if (input < 0.0 || input >= 4294967296.0) return 0;
      *out = (uint32_t)trunc(input);
    }
  } else if (is_signed) {
    const double edge = 9223372036854775808.0;
    if (input < -edge || input >= edge) return 0;
    int64_t value = (int64_t)trunc(input);
    memcpy(out, &value, sizeof(value));
  } else {
    const double edge = 18446744073709551616.0;
    if (input < 0.0 || input >= edge) return 0;
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
        case 0x98: z = copysignf(x,y); goto store_f32;
        default: NUMERIC_FAIL();
      }
      stack[(*sp)++] = value; return 1;
store_f32:
      memcpy(&value, &z, sizeof(z)); stack[(*sp)++] = (uint32_t)value;
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
      case 0xa6: z = copysign(x,y); goto store_f64;
      default: NUMERIC_FAIL();
    }
    stack[(*sp)++] = value; return 1;
store_f64:
    memcpy(&value, &z, sizeof(z)); stack[(*sp)++] = value; return 1;
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
        int src64 = op == 0xb4u || op == 0xb5u || op == 0xb9u || op == 0xbau;
        int unsig = op == 0xb3u || op == 0xb5u || op == 0xb8u || op == 0xbau;
        double number = src64 ? (unsig ? (double)a : (double)wasm_i64(a))
            : (unsig ? (double)(uint32_t)a : (double)wasm_i32((uint32_t)a));
        if (op <= 0xb5u) { float out = (float)number; uint32_t bits; memcpy(&bits,&out,4); value=bits; }
        else { double out = number; memcpy(&value,&out,8); }
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
            NUMERIC_TRAP(GLWASM_TRAP_INTEGER_OVERFLOW);
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
  if(runtime&&exception&&runtime->nested_boundary.id){
    GRCORE_Result stored=grcore_boundary_set_foreign_exception(
        grcore_context_stack(ctx),runtime->nested_boundary,run->engine,
        exception->identity);
    if(stored!=GRCORE_OK){
      run->error=map_core_result(stored);
      return GRCORE_STEP_UNWOUND;
    }
    runtime->last_foreign_exception.engine=run->engine;
    runtime->last_foreign_exception.identity=exception->identity;
  }
  return GRCORE_STEP_FINISHED;
}

static GRCORE_Step execute_impl(GRCORE_Context *ctx, void *opaque) {
  Run *r=(Run *)opaque; GRCORE_Stack *s=grcore_context_stack(ctx); GRCORE_FrameRef f;
  if (grcore_stack_frame_count(s)<=r->stack_base) {
    GLWASM_Func *fn=&r->m->funcs[r->entry];
    if(fn->imported){
      uint64_t *results=r->results;
      GLWASM_Result called=call_host(r->instance,r->entry,r->args,results,
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
        uint32_t callee=in.a;uint64_t indirect_index=0;
        r->diagnostic_call_target=callee;
        r->diagnostic_reason=1;
        r->diagnostic_call_sp=sp;
        r->diagnostic_call_base=H_OPERANDS+fn->nlocals;
        r->diagnostic_call_required=r->diagnostic_call_base;
        r->diagnostic_caller_params=fn->nparams;
        r->diagnostic_caller_locals=fn->nlocals;
        r->diagnostic_callee_params=0;
        if(in.op==0x11||in.op==0x13){
          if(in.b>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          if(in.a>=r->m->ntypes){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          size_t base=H_OPERANDS+fn->nlocals;
          size_t required=base+r->m->types[in.a].nparams+1u;
          if((size_t)sp<required){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          uint64_t ti_index=p[--sp];indirect_index=ti_index;
          TableLease lease;
          if(!table_acquire(r,in.b,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}
          if(ti_index>=lease.view.length){table_release(&lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);}
          uint64_t function_ref=lease.view.elements[ti_index];
          table_release(&lease);
          if(!function_ref)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_UNINITIALIZED_ELEMENT);
          if(function_ref>(uint64_t)r->m->nfuncs)
            return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);
          callee=(uint32_t)(function_ref-1u);
          if(callee>=r->m->nfuncs||r->m->funcs[callee].type_index>=r->m->ntypes||in.a>=r->m->ntypes||!same_function_type(&r->m->types[r->m->funcs[callee].type_index],&r->m->types[in.a]))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);
        }
        if(callee>=r->m->nfuncs){r->diagnostic_reason=2;r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        GLWASM_Func *cf=&r->m->funcs[callee];
        if(cf->type_index>=r->m->ntypes){r->diagnostic_reason=3;r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        uint32_t np=cf->nparams;
        r->diagnostic_callee_params=np;
        r->diagnostic_call_required=r->diagnostic_call_base+np;
        if(sp < H_OPERANDS + fn->nlocals + np){r->diagnostic_reason=4;r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        uint64_t *argv=np?gcu_allocator_calloc(&r->m->alloc,np,sizeof(*argv)):NULL;
        if(np&&!argv){r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
        for(uint32_t j=0;j<np;j++) { argv[np-1-j]=p[--sp]; }
        p[H_SP]=sp;
        if(cf->imported){
          uint32_t nres=r->m->types[cf->type_index].nresults;
          uint64_t *host_results=nres?gcu_allocator_calloc(&r->m->alloc,
              nres,sizeof(*host_results)):NULL;
          if(nres&&!host_results){gcu_allocator_free(&r->m->alloc,argv);
            r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
          GLWASM_Result called=call_host(r->instance,callee,argv,host_results,
              r->pending_host&&r->pending_host_function==callee);
          if(called==GLWASM_ERR_PAUSED){
            /* The callback owns any continuation state in its `user` value.
             * Put the exact operands back so retrying after resume repeats
             * this call once, without charging the opcode twice. */
            for(uint32_t j=0;j<np;j++)p[sp++]=argv[j];
            if(in.op==0x11||in.op==0x13)p[sp++]=indirect_index;
            p[H_PC]=pc;p[H_SP]=sp;p[H_SKIP_CHARGE]=1;
            r->pending_host=1;r->pending_host_function=callee;
            gcu_allocator_free(&r->m->alloc,host_results);
            gcu_allocator_free(&r->m->alloc,argv);
            GRCORE_Verdict verdict=request_host_pause(ctx,r->m,fi,
                fn->translated->offsets[pc],1);
            return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
                GRCORE_STEP_UNWOUND;
          }
          r->pending_host=0;
          if(called==GLWASM_OK&&nres){memcpy(&p[sp],host_results,
              (size_t)nres*sizeof(*p));sp+=nres;p[H_SP]=sp;}
          gcu_allocator_free(&r->m->alloc,host_results);
          gcu_allocator_free(&r->m->alloc,argv);
          if(called!=GLWASM_OK){r->error=called;return GRCORE_STEP_FINISHED;}
          if(in.op==0x12||in.op==0x13)p[H_PC]=fn->nins;
          break;
        }
        if(in.op==0x12||in.op==0x13)grcore_stack_pop(s);
        if(grcore_stack_push(s,r->engine,frame_slots(cf),&f)!=GRCORE_OK){r->diagnostic_reason=5;gcu_allocator_free(&r->m->alloc,argv);r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;}
        p=slots(s,f);if(!p){r->diagnostic_reason=6;(void)grcore_stack_pop(s);gcu_allocator_free(&r->m->alloc,argv);r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}memset(p,0,frame_slots(cf)*sizeof(*p));
        p[H_FUNC]=callee;p[H_PC]=0;p[H_SP]=H_OPERANDS+cf->nlocals;p[H_NLOCALS]=cf->nlocals;p[H_CTRL_DEPTH]=0;p[H_INSTANCE]=(uintptr_t)r->instance;
        diagnostic_record_frame_entry(r,callee,p[H_SP],H_OPERANDS+cf->nlocals);
        for(uint32_t j=0;j<np;j++) { p[H_OPERANDS+j]=argv[j]; }
        if(wasm_set_guest_frame_identity(s,f,r->m,callee,0)!=GRCORE_OK){
          r->diagnostic_reason=7;
          (void)grcore_stack_pop(s);gcu_allocator_free(&r->m->alloc,argv);
          r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
        }
        gcu_allocator_free(&r->m->alloc,argv);
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
      case 0x23:if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}{HostBinding*b=resource_binding(r,GLWASM_HOST_IMPORT_GLOBAL,in.a);if(b)r->globals[in.a]=*b->resource.global->value;if(!global_reference_valid(r,in.a,r->globals[in.a]))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);p[sp++]=r->globals[in.a];}break;
      case 0x24:if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}r->globals[in.a]=p[--sp];if(!global_reference_valid(r,in.a,r->globals[in.a]))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INVALID_REFERENCE);{HostBinding*b=resource_binding(r,GLWASM_HOST_IMPORT_GLOBAL,in.a);if(b)*b->resource.global->value=r->globals[in.a];}break;
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
        p[sp++] = r->memory_pages[in.a];
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
      case 0xd2:p[sp++]=(uint64_t)in.a+1u;break;
      case 0xfc08: case 0xfc09: case 0xfc0a: case 0xfc0b: {
        const uint32_t sub=(uint32_t)(in.op&0xffu);
        if(sub==9u) {
          if(in.a>=r->m->ndatas){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          if(r->data_dropped)r->data_dropped[in.a]=1;
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
            uint64_t data_length=r->data_dropped[in.a]?0:r->m->datas[in.a].nbytes;
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
            uint64_t data_length=r->data_dropped[in.a]?0:r->m->datas[in.a].nbytes;
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
          r->elements_dropped[in.a]=1;break;
        }
        if(sub==16u) {
          if(in.a>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          TableLease lease;if(!table_acquire(r,in.a,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}table_release(&lease);
          p[sp++]=r->table_sizes[in.a];break;
        }
        if(sub==15u) {
          if(in.a>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          uint64_t delta=p[--sp],value=p[--sp];
          TableLease lease;if(!table_acquire(r,in.a,&lease)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}uint64_t old=lease.view.length;table_release(&lease);uint64_t next=old;
          GLWASM_Table *table=&r->m->tables[in.a];
          if(delta<=UINT64_MAX-old)next=old+delta;else next=UINT64_MAX;
          uint64_t address_limit=table->addr64?UINT64_MAX:(UINT64_C(1)<<32u);
          if((table->has_max&&next>table->max)||next>address_limit||
              next>SIZE_MAX/sizeof(uint64_t))
            p[sp++]=table->addr64?UINT64_MAX:UINT32_MAX;
          else if(next==old)p[sp++]=old;
          else if(table->imported){
            HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_TABLE,in.a);
            uint64_t provider_old=0;
            if(!binding||binding->resource.table.grow(binding->resource.table.user,
                delta,value,&provider_old)!=GLWASM_OK||provider_old!=old)
              p[sp++]=table->addr64?UINT64_MAX:UINT32_MAX;
            else {TableLease updated;if(!table_acquire(r,in.a,&updated)){r->error=GLWASM_ERR_LINK;return GRCORE_STEP_FINISHED;}int ok=updated.view.length==next;table_release(&updated);p[sp++]=ok?old:(table->addr64?UINT64_MAX:UINT32_MAX);}
          }
          else {
            uint64_t *grown=gcu_allocator_calloc(r->guest_alloc,(size_t)next,sizeof(*grown));
            if(!grown)p[sp++]=table->addr64?UINT64_MAX:UINT32_MAX;
            else {if(old)memcpy(grown,r->tables[in.a],(size_t)old*sizeof(*grown));
              for(uint64_t j=old;j<next;j++)grown[j]=value;
              gcu_allocator_free(r->guest_alloc,r->tables[in.a]);r->tables[in.a]=grown;
              r->table_sizes[in.a]=next;p[sp++]=old;}
          }
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
          uint64_t element_length=r->elements_dropped[in.a]?0:
              (r->m->elems[in.a].use_exprs?r->m->elems[in.a].nexprs:r->m->elems[in.a].nfuncs);
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
            if(valid)valid=r->bulk_src+offset<=r->table_sizes[in.b]&&count<=r->table_sizes[in.b]-(r->bulk_src+offset);}
          if(!valid){if(source_acquired)table_release(&source_lease);table_release(&destination_lease);return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);}
          if(sub==12u) {
            GLWASM_Elem *e=&r->m->elems[in.a];
            for(uint64_t j=0;j<count;j++) {
          uint64_t ref=0;
              if(!element_ref(e,(uint32_t)(r->bulk_src+offset+j),&ref)) {
                table_release(&destination_lease);r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;
              }
              destination_elements[r->bulk_dst+offset+j]=ref;
            }
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
      case 0x1b:{uint64_t c=p[--sp],b=p[--sp],a=p[--sp];p[sp++]=c?a:b;break;}
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

static GLWASM_Result runtime_create_impl(GLWASM_Module*m,
    const GLWASM_HostImport *imports,size_t import_count,int allow_start,
    int legacy_spectest,GLWASM_LinkError *link_error,GLWASM_Runtime**out){
  if(!out)return GLWASM_ERR_INVALID;
  *out=NULL;
  if(!m)return GLWASM_ERR_INVALID;
  if(link_error)memset(link_error,0,sizeof(*link_error));
  if(m->has_start&&!allow_start){
    if(link_error)link_error->reason=GLWASM_LINK_START_NOT_ALLOWED;
    return GLWASM_ERR_LINK;
  }
  uint64_t binding_count=(uint64_t)m->nfuncs+m->nimports;
  if(binding_count>(uint64_t)SIZE_MAX)return GLWASM_ERR_LIMIT;
  size_t host_binding_count=(size_t)binding_count;
  HostBinding *host_functions=host_binding_count?
      gcu_allocator_calloc(&m->alloc,host_binding_count,sizeof(*host_functions)):NULL;
  if(host_binding_count&&!host_functions)return GLWASM_ERR_OOM;
  HostBinding *host_resources=host_functions?host_functions+m->nfuncs:NULL;
  for(uint32_t i=0;i<m->nimports;i++){
    GLWASM_Import *import=&m->imports[i];
    if(legacy_spectest){
      if(import->module_name.length!=8||
          memcmp(import->module_name.bytes,"spectest",8)){
        if(import->kind!=GLWASM_EXT_FUNC){
          for(uint32_t j=0;j<m->nfuncs;j++){
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[j].result_types);
          }
          gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_UNSUPPORTED;
        }
        continue;
      }
      if(import->kind==GLWASM_EXT_FUNC && import->index<m->nfuncs &&
          import->module_name.length==8 &&
          !memcmp(import->module_name.bytes,"spectest",8)){
        GLWASM_Func *func=&m->funcs[import->index];
        if(func->type_index>=m->ntypes){
          gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_INTERNAL;
        }
        GLWASM_TypeDef *type=&m->types[func->type_index];
        host_functions[import->index].parameter_types=type->nparams?
            gcu_allocator_calloc(&m->alloc,type->nparams,
                sizeof(*host_functions[import->index].parameter_types)):NULL;
        if(type->nparams&&!host_functions[import->index].parameter_types){
          for(uint32_t j=0;j<m->nfuncs;j++){
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[j].result_types);
          }
          gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_OOM;
        }
        for(uint32_t j=0;j<type->nparams;j++)
          host_functions[import->index].parameter_types[j]=host_value_type(type->params[j]);
        host_functions[import->index].function=spectest_noop;
      }
      continue;
    }
    if (import->module_name.length >= 5u &&
        !memcmp(import->module_name.bytes, "tang.", 5u)) {
      int allowed = m->has_policy && m->policy.allow_tang_v1_imports &&
          import->module_name.length == 7u &&
          !memcmp(import->module_name.bytes, "tang.v1", 7u);
      if (!allowed) {
        if (link_error) {
          link_error->module = import->module_name.bytes;
          link_error->field = import->field.bytes;
          link_error->reason = GLWASM_LINK_UNRESOLVED;
        }
        for (uint32_t j = 0; j < m->nfuncs; ++j) {
          gcu_allocator_free(&m->alloc, host_functions[j].parameter_types);
          gcu_allocator_free(&m->alloc, host_functions[j].result_types);
        }
        gcu_allocator_free(&m->alloc, host_functions);
        return GLWASM_ERR_LINK;
      }
    }
    const GLWASM_HostImport *binding=NULL;
    for(size_t j=0;j<import_count;j++){
      if(imports[j].module&&imports[j].field&&
          strlen(imports[j].module)==import->module_name.length&&
          strlen(imports[j].field)==import->field.length&&
          !memcmp(imports[j].module,import->module_name.bytes,import->module_name.length)&&
          !memcmp(imports[j].field,import->field.bytes,import->field.length)){
        binding=&imports[j];break;
      }
    }
    if(import->kind!=GLWASM_EXT_FUNC){
      uint8_t expected=import->kind==GLWASM_EXT_MEM?GLWASM_HOST_IMPORT_MEMORY:
          import->kind==GLWASM_EXT_TABLE?GLWASM_HOST_IMPORT_TABLE:
          import->kind==GLWASM_EXT_GLOBAL?GLWASM_HOST_IMPORT_GLOBAL:
          GLWASM_HOST_IMPORT_TAG;
      int valid=binding&&binding->kind==expected;
      HostBinding *resource=host_resources?&host_resources[i]:NULL;
      if(valid&&import->kind==GLWASM_EXT_MEM){
        valid=binding->resource.memory&&binding->resource.memory->begin&&
            binding->resource.memory->end&&binding->resource.memory->grow&&
            import->index<m->nmems;
        if(valid){GLWASM_HostMemoryView view={0};
          GLWASM_Result acquired=binding->resource.memory->begin(
              binding->resource.memory->user,&view);
          valid=acquired==GLWASM_OK&&(view.length_bytes==0||view.bytes)&&
              view.length_bytes%65536u==0;
          if(valid){uint64_t pages=view.length_bytes/65536u;GLWASM_Memory *decl=&m->mems[import->index];
            valid=pages>=decl->min&&(!decl->has_max||pages<=decl->max);}
          if(acquired==GLWASM_OK)binding->resource.memory->end(
              binding->resource.memory->user,&view);}
        if(valid)resource->resource.memory=*binding->resource.memory;
      }else if(valid&&import->kind==GLWASM_EXT_TABLE){
        valid=binding->resource.table&&binding->resource.table->begin&&
            binding->resource.table->end&&binding->resource.table->grow&&
            import->index<m->ntables;
        if(valid)valid=binding->resource.table->element_type==
            host_value_type(m->tables[import->index].elem);
        if(valid&&m->tables[import->index].elem.kind==GLWASM_VK_REF&&
            m->tables[import->index].elem.heap!=GLWASM_HT_FUNC&&
            m->tables[import->index].elem.heap!=GLWASM_HT_NOFUNC)
          valid=binding->resource.table->validate_token!=NULL;
        if(valid){GLWASM_HostTableView view={0};
          GLWASM_Result acquired=binding->resource.table->begin(
              binding->resource.table->user,&view);
          valid=acquired==GLWASM_OK&&(view.length==0||view.elements);
          if(valid){GLWASM_Table *decl=&m->tables[import->index];
            valid=view.length>=decl->min&&(!decl->has_max||view.length<=decl->max);}
          if(acquired==GLWASM_OK)binding->resource.table->end(
              binding->resource.table->user,&view);}
        if(valid)resource->resource.table=*binding->resource.table;
      }else if(valid&&import->kind==GLWASM_EXT_GLOBAL){
        valid=binding->resource.global&&binding->resource.global->value&&import->index<m->nglobals;
        if(valid){GLWASM_Global *decl=&m->globals[import->index];
          valid=binding->resource.global->type==host_value_type(decl->type)&&
              (binding->resource.global->mutable_!=0)==(decl->mut!=0);}
        if(valid&&m->globals[import->index].type.kind==GLWASM_VK_REF&&
            m->globals[import->index].type.heap!=GLWASM_HT_FUNC&&
            m->globals[import->index].type.heap!=GLWASM_HT_NOFUNC)
          valid=binding->resource.global->validate_token!=NULL;
        if(valid)resource->resource.global=binding->resource.global;
      }else if(valid&&import->kind==GLWASM_EXT_TAG){
        valid=binding->resource.tag&&binding->resource.tag->identity!=0&&
            import->index<m->ntags;
        if(valid){GLWASM_Tag *decl=&m->tags[import->index];
          if(decl->type_index>=m->ntypes)valid=0;
          else {GLWASM_TypeDef *type=&m->types[decl->type_index];
            valid=binding->resource.tag->parameter_count==type->nparams&&
                (!type->nparams||binding->resource.tag->parameters!=NULL);
            for(uint32_t j=0;valid&&j<type->nparams;j++)
              valid=binding->resource.tag->parameters[j]==host_value_type(type->params[j]);
            if(valid)resource->resource.tag=*binding->resource.tag;}}
      }
      if(!valid){if(link_error){link_error->module=import->module_name.bytes;
          link_error->field=import->field.bytes;link_error->reason=!binding?
              GLWASM_LINK_UNRESOLVED:GLWASM_LINK_INVALID_BINDING;}
        for(uint32_t j=0;j<m->nfuncs;j++){
          gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
          gcu_allocator_free(&m->alloc,host_functions[j].result_types);
        }
        gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_LINK;}
      resource->resource_kind=expected;resource->resource_index=import->index;
      continue;
    }
    if(!binding||binding->kind!=GLWASM_HOST_IMPORT_FUNCTION||
        (!binding->function&&!binding->function_results)||import->index>=m->nfuncs){
      if(link_error){link_error->module=import->module_name.bytes;
        link_error->field=import->field.bytes;
        link_error->reason=binding?GLWASM_LINK_INVALID_BINDING:GLWASM_LINK_UNRESOLVED;}
      for(uint32_t j=0;j<m->nfuncs;j++){
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[j].result_types);
          }
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_LINK;
    }
    GLWASM_Func *func=&m->funcs[import->index];
    if(func->type_index>=m->ntypes||
        (m->types[func->type_index].nresults>1&&!binding->function_results)){
      if(link_error){link_error->module=import->module_name.bytes;
        link_error->field=import->field.bytes;
        link_error->reason=GLWASM_LINK_INVALID_BINDING;}
      for(uint32_t j=0;j<m->nfuncs;j++){
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[j].result_types);
          }
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_LINK;
    }
    GLWASM_TypeDef *type=&m->types[func->type_index];
    host_functions[import->index].function_results=binding->function_results;
    host_functions[import->index].resume_results=binding->resume_results;
    host_functions[import->index].parameter_types=type->nparams?
        gcu_allocator_calloc(&m->alloc,type->nparams,
            sizeof(*host_functions[import->index].parameter_types)):NULL;
    if(type->nparams&&!host_functions[import->index].parameter_types){
      for(uint32_t j=0;j<m->nfuncs;j++){
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[j].result_types);
          }
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_OOM;
    }
    for(uint32_t j=0;j<type->nparams;j++){
      GLWASM_HostValueType mapped=host_value_type(type->params[j]);
      if(!mapped){
        if(link_error){link_error->module=import->module_name.bytes;
          link_error->field=import->field.bytes;
          link_error->reason=GLWASM_LINK_INVALID_BINDING;}
        for(uint32_t k=0;k<m->nfuncs;k++){
            gcu_allocator_free(&m->alloc,host_functions[k].parameter_types);
            gcu_allocator_free(&m->alloc,host_functions[k].result_types);
          }
        gcu_allocator_free(&m->alloc,host_functions);
        return GLWASM_ERR_LINK;
      }
      host_functions[import->index].parameter_types[j]=mapped;
    }
    host_functions[import->index].result_types=type->nresults?
        gcu_allocator_calloc(&m->alloc,type->nresults,
            sizeof(*host_functions[import->index].result_types)):NULL;
    if(type->nresults&&!host_functions[import->index].result_types){
      for(uint32_t j=0;j<m->nfuncs;j++){
        gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
        gcu_allocator_free(&m->alloc,host_functions[j].result_types);
      }
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_OOM;
    }
    for(uint32_t j=0;j<type->nresults;j++){
      GLWASM_HostValueType mapped=host_value_type(type->results[j]);
      if(!mapped){
        if(link_error){link_error->module=import->module_name.bytes;
          link_error->field=import->field.bytes;
          link_error->reason=GLWASM_LINK_INVALID_BINDING;}
        for(uint32_t k=0;k<m->nfuncs;k++){
          gcu_allocator_free(&m->alloc,host_functions[k].parameter_types);
          gcu_allocator_free(&m->alloc,host_functions[k].result_types);
        }
        gcu_allocator_free(&m->alloc,host_functions);
        return GLWASM_ERR_LINK;
      }
      host_functions[import->index].result_types[j]=mapped;
    }
    host_functions[import->index].function=binding->function;
    host_functions[import->index].resume=binding->resume;
    host_functions[import->index].user=binding->user;
  }
  GLWASM_Runtime*r=gcu_allocator_calloc(&m->alloc,1,sizeof(*r));
  if(!r){for(uint32_t j=0;j<m->nfuncs;j++){
    gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
    gcu_allocator_free(&m->alloc,host_functions[j].result_types);
  }gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_OOM;}
  r->module=m;
  glwasm_module_retain(m);
  r->host_functions = host_functions;
  r->host_resources = host_resources;
  r->boundary_token=atomic_fetch_add_explicit(&next_boundary_token,1,
      memory_order_relaxed);
  r->run.last_jit_lower_pc=UINT32_MAX;
  if(!r->boundary_token)r->boundary_token=atomic_fetch_add_explicit(
      &next_boundary_token,1,memory_order_relaxed);
  r->host_functions=host_functions;
  r->host_resources=host_resources;
  GLWASM_Policy initial_policy;
  if (m->has_policy) initial_policy = m->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED, &initial_policy);
  r->jit_mode = initial_policy.max_jit_code_bytes
      ? GLWASM_TEST_JIT_AUTO : GLWASM_TEST_JIT_INTERPRETER;
  if(m->nglobals){
    r->globals=gcu_allocator_calloc(&m->alloc,m->nglobals,sizeof(*r->globals));
    if(!r->globals){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    for(uint32_t i=0;i<m->nglobals;i++){
      GLWASM_Global*gl=&m->globals[i];
      if(gl->imported){
        HostBinding *host_global=NULL;
        for(uint32_t j=0;j<m->nimports;j++)
          if(m->imports[j].kind==GLWASM_EXT_GLOBAL&&m->imports[j].index==i&&
              host_resources&&host_resources[j].resource_kind==GLWASM_HOST_IMPORT_GLOBAL)
            host_global=&host_resources[j];
        if(host_global){r->globals[i]=*host_global->resource.global->value;if(!global_reference_valid(&(Run){.m=m,.host_resources=host_resources},i,r->globals[i])){glwasm_runtime_destroy(r);return GLWASM_ERR_LINK;}continue;}
        int found=0;
        for(uint32_t j=0;j<m->nimports;j++){
          GLWASM_Import*im=&m->imports[j];
          if(im->kind!=GLWASM_EXT_GLOBAL||im->index!=i)continue;
          found=1;
          if(im->module_name.length!=8||memcmp(im->module_name.bytes,"spectest",8)){
            glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;
          }
          if(gl->mut){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
          if(im->field.length==10&&!memcmp(im->field.bytes,"global_i32",10)&&gl->type.kind==GLWASM_VK_I32)r->globals[i]=666;
          else if(im->field.length==10&&!memcmp(im->field.bytes,"global_i64",10)&&gl->type.kind==GLWASM_VK_I64)r->globals[i]=666;
          else if(im->field.length==10&&!memcmp(im->field.bytes,"global_f32",10)&&gl->type.kind==GLWASM_VK_F32){float value=666.6f;uint32_t bits;memcpy(&bits,&value,sizeof(bits));r->globals[i]=bits;}
          else if(im->field.length==10&&!memcmp(im->field.bytes,"global_f64",10)&&gl->type.kind==GLWASM_VK_F64){double value=666.6;memcpy(&r->globals[i],&value,sizeof(value));}
          else {glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
        }
        if(!found){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
      }else if(!const_value(m,&gl->init,r->globals,&r->globals[i])){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
    }
  }
  GRCORE_Result core_result=grcore_group_create(NULL,NULL,&r->group);
  if(core_result==GRCORE_OK)core_result=grcore_options_create(NULL,&r->options);
  GLWASM_Policy runtime_policy;
  if(m->has_policy) runtime_policy=m->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED,&runtime_policy);
  if(core_result==GRCORE_OK)core_result=grcore_options_set_fuel(r->options,
      runtime_policy.default_fuel);
  if(core_result==GRCORE_OK)core_result=grcore_options_set_memory_bytes(
      r->options,runtime_policy.default_memory_bytes);
  if(core_result==GRCORE_OK)core_result=grcore_options_set_guest_depth(
      r->options,runtime_policy.default_guest_depth);
  if(core_result==GRCORE_OK)core_result=grcore_options_set_native_depth(
      r->options,runtime_policy.default_native_depth);
  if(core_result==GRCORE_OK)core_result=grcore_context_create(r->group,r->options,&r->context);
  if(core_result==GRCORE_OK)core_result=grcore_engine_register(r->context,&descriptor,&r->engine);
  if(core_result!=GRCORE_OK){GLWASM_Result result=map_core_result(core_result);glwasm_runtime_destroy(r);return result;}
  {
    GLWASM_Result slots_result = create_jit_slots(m, r->context,
        &r->jit_slots);
    if (slots_result != GLWASM_OK) {
      glwasm_runtime_destroy(r);
      return slots_result;
    }
    r->jit_registered = m->nfuncs
        ? gcu_allocator_calloc(&m->alloc, m->nfuncs, sizeof(*r->jit_registered))
        : NULL;
    if (m->nfuncs && !r->jit_registered) {
      glwasm_runtime_destroy(r);
      return GLWASM_ERR_OOM;
    }
    GLWASM_Result jit_context = register_jit_runtime(r, r->context,
        &r->jit_reservation);
    if (jit_context != GLWASM_OK) {
      glwasm_runtime_destroy(r);
      return jit_context;
    }
  }
  r->guest_alloc=grcore_context_allocator(r->context);
  if(m->ntables){
    r->tables=gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*r->tables));
    r->table_sizes=gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*r->table_sizes));
    if(!r->tables||!r->table_sizes){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    Run table_view={.m=m,.tables=r->tables,.table_sizes=r->table_sizes,
        .host_resources=r->host_resources,.guest_alloc=r->guest_alloc};
    for(uint32_t i=0;i<m->ntables;i++){
      if(m->tables[i].imported){
        int spectest=0;
        if(legacy_spectest)for(uint32_t j=0;j<m->nimports;j++)
          if(m->imports[j].kind==GLWASM_EXT_TABLE&&m->imports[j].index==i&&
              m->imports[j].module_name.length==8&&
              !memcmp(m->imports[j].module_name.bytes,"spectest",8))spectest=1;
        if(spectest){
          r->table_sizes[i]=m->tables[i].min;
          if(r->table_sizes[i]>SIZE_MAX/sizeof(uint64_t)){glwasm_runtime_destroy(r);return GLWASM_ERR_LIMIT;}
          if(r->table_sizes[i])r->tables[i]=gcu_allocator_calloc(r->guest_alloc,
              (size_t)r->table_sizes[i],sizeof(uint64_t));
          if(r->table_sizes[i]&&!r->tables[i]){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
          continue;
        }
        TableLease lease;if(!table_acquire(&table_view,i,&lease)){glwasm_runtime_destroy(r);return GLWASM_ERR_LINK;}
        int valid=lease.view.length>=m->tables[i].min&&(!m->tables[i].has_max||lease.view.length<=m->tables[i].max);
        table_release(&lease);if(!valid){glwasm_runtime_destroy(r);return GLWASM_ERR_LINK;}
      }else{
        if(m->tables[i].min>SIZE_MAX/sizeof(uint64_t)){glwasm_runtime_destroy(r);return GLWASM_ERR_LIMIT;}
        r->table_sizes[i]=m->tables[i].min;
        r->tables[i]=gcu_allocator_calloc(r->guest_alloc,(size_t)m->tables[i].min,sizeof(uint64_t));
        if(!r->tables[i]){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
      }
    }
    for(uint32_t i=0;i<m->nelems;i++){
      GLWASM_Elem*e=&m->elems[i];
      if(e->mode!=GLWASM_ELEM_ACTIVE||e->table>=m->ntables||!e->offset.nins)continue;
      uint64_t offset=0;if(!const_value(m,&e->offset,r->globals,&offset)){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
      const uint32_t count=e->use_exprs?e->nexprs:e->nfuncs;
      TableLease lease;if(!table_acquire(&table_view,e->table,&lease)){glwasm_runtime_destroy(r);return GLWASM_ERR_LINK;}
      if(offset>r->table_sizes[e->table]||count>r->table_sizes[e->table]-offset){table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_VALIDATE;}
      for(uint32_t j=0;j<count;j++){
        uint64_t ref=0;
        if(e->use_exprs){
          const GLWASM_Func *expr=&e->exprs[j];
          int recognized=0;
          for(uint32_t k=0;k<expr->nins;k++){
            if(expr->ins[k].op==0xd2){if(recognized){table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}ref=expr->ins[k].a+1u;recognized=1;}
            else if(expr->ins[k].op==0xd0){if(recognized){table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}ref=0;recognized=1;}
            else if(expr->ins[k].op==0x0b){if(k+1u!=expr->nins){table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}}
            else {table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
          }
          if(!recognized){table_release(&lease);glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
        }else ref=e->funcs[j]+1u;
        lease.view.elements[offset+j]=ref;
      }
      table_release(&lease);
    }
  }
  if(m->nelems){
    r->elements_dropped=gcu_allocator_calloc(&m->alloc,m->nelems,1);
    if(!r->elements_dropped){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    for(uint32_t i=0;i<m->nelems;i++)
      r->elements_dropped[i]=(uint8_t)(m->elems[i].mode!=GLWASM_ELEM_PASSIVE);
  }
  if (m->nmems) {
    r->memories=gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*r->memories));
    r->memory_bytes=gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*r->memory_bytes));
    r->memory_pages=gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*r->memory_pages));
    if(!r->memories||!r->memory_bytes||!r->memory_pages){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    if(legacy_spectest){
      Run memory_view={.m=m,.memories=r->memories,
          .memory_bytes=r->memory_bytes,.memory_pages=r->memory_pages,
          .host_resources=r->host_resources,.guest_alloc=r->guest_alloc};
      for(uint32_t i=0;i<m->nmems;i++)if(m->mems[i].imported){
        int spectest=0;
        for(uint32_t j=0;j<m->nimports;j++)
          if(m->imports[j].kind==GLWASM_EXT_MEM&&m->imports[j].index==i&&
              m->imports[j].module_name.length==8&&
              !memcmp(m->imports[j].module_name.bytes,"spectest",8))spectest=1;
        if(spectest){uint64_t old=0;if(!memory_grow(&memory_view,i,m->mems[i].min,&old)){
          glwasm_runtime_destroy(r);return GLWASM_ERR_LIMIT;}}
      }
    }
    GLWASM_Result mr=initialize_memories(m,r->globals,r->memories,
        r->memory_bytes,r->memory_pages,NULL,r->guest_alloc,r->host_resources);
    if(mr!=GLWASM_OK){glwasm_runtime_destroy(r);return mr;}
  }
  if(m->ndatas){r->data_dropped=gcu_allocator_calloc(&m->alloc,m->ndatas,1);if(!r->data_dropped){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}for(uint32_t i=0;i<m->ndatas;i++)if(!m->datas[i].passive)r->data_dropped[i]=1;}
  r->run=(Run){.m=m,.ctx=r->context,.engine=r->engine,.instance=r,.globals=r->globals,
    .tables=r->tables,.table_sizes=r->table_sizes,.memories=r->memories,
    .memory_bytes=r->memory_bytes,.memory_pages=r->memory_pages,
    .host_resources=r->host_resources,
    .data_dropped=r->data_dropped,.elements_dropped=r->elements_dropped,
    .guest_alloc=r->guest_alloc};
  core_result=grcore_context_register(r->context,&trap_key,&r->run);
  if(core_result!=GRCORE_OK){GLWASM_Result result=map_core_result(core_result);glwasm_runtime_destroy(r);return result;}
  GLWASM_Result names_result=ensure_debug_names(r->context,m,r->engine);
  if(names_result!=GLWASM_OK){glwasm_runtime_destroy(r);return names_result;}
  if(m->has_start){uint64_t ignored=0;GLWASM_Result started=glwasm_runtime_invoke(r,m->start,NULL,0,&ignored);if(started!=GLWASM_OK){glwasm_runtime_destroy(r);return started;}}
  *out=r;return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_create(GLWASM_Module *module,GLWASM_Runtime **out){
  return runtime_create_impl(module,NULL,0,1,1,NULL,out);
}
GLWASM_Result glwasm_runtime_create_with_imports(GLWASM_Module *module,
    const GLWASM_HostImport *imports,size_t import_count,int allow_start,
    GLWASM_LinkError *link_error,GLWASM_Runtime **out){
  if(!module||!out||(import_count&&!imports))return GLWASM_ERR_INVALID;
  return runtime_create_impl(module,imports,import_count,allow_start!=0,0,
      link_error,out);
}
GLWASM_Result glwasm_runtime_register_boundary_engine(
    GLWASM_Runtime *runtime,const GRCORE_EngineDescriptor *descriptor,
    GRCORE_EngineId *out_engine){
  if(!runtime||!descriptor||!out_engine||runtime->run.active)
    return GLWASM_ERR_INVALID;
  GRCORE_Result result=grcore_engine_register(runtime->context,descriptor,
      out_engine);
  return map_core_result(result);
}
GLWASM_Result glwasm_runtime_bind_boundary_context(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId *out_wasm_engine){
  if(!runtime||!context||!out_wasm_engine||runtime->run.active||
      runtime->nested_active)return GLWASM_ERR_INVALID;
  GRCORE_EngineId engine=find_wasm_engine(context);
  if(!engine){
    GRCORE_Result result=grcore_engine_register(context,&descriptor,&engine);
    if(result!=GRCORE_OK)return map_core_result(result);
  }
  GLWASM_Result names=ensure_debug_names(context,runtime->module,engine);
  if(names!=GLWASM_OK)return names;
  if(runtime->boundary_context==context){
    *out_wasm_engine=engine;
    return GLWASM_OK;
  }
  GRCORE_EntrySlot **new_slots=NULL;
  uint8_t *new_registered=NULL;
  GRCORE_DeoptReservation *new_reservation=NULL;
  if(context!=runtime->context){
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
  if(runtime->boundary_context&&runtime->boundary_context!=runtime->context){
    clear_jit_context_state(runtime->module,runtime->boundary_context,
        &runtime->boundary_jit_slots,&runtime->boundary_jit_registered,
        &runtime->boundary_jit_reservation);
    release_debug_names(runtime->boundary_context,runtime->module->debug_id,
        runtime->boundary_engine);
  }
  runtime->boundary_context=context;runtime->boundary_engine=engine;
  runtime->boundary_jit_slots=new_slots;
  runtime->boundary_jit_registered=new_registered;
  runtime->boundary_jit_reservation=new_reservation;
  *out_wasm_engine=engine;
  return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_unbind_boundary_context(GLWASM_Runtime *runtime) {
  if(!runtime||runtime->run.active||runtime->nested_active)
    return GLWASM_ERR_INVALID;
  if(runtime->boundary_context&&runtime->boundary_context!=runtime->context){
    clear_jit_context_state(runtime->module,runtime->boundary_context,
        &runtime->boundary_jit_slots,&runtime->boundary_jit_registered,
        &runtime->boundary_jit_reservation);
    release_debug_names(runtime->boundary_context,runtime->module->debug_id,
        runtime->boundary_engine);
  }
  runtime->boundary_context=NULL;
  runtime->boundary_engine=0;
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

static GLWASM_Result finish_nested(GLWASM_Runtime *runtime,GRCORE_Result core,
    GRCORE_Outcome outcome,uint64_t *results,uint32_t *result_count) {
  if(core==GRCORE_OK&&outcome==GRCORE_OUTCOME_PAUSED){
    runtime->run.paused=1;
    return GLWASM_ERR_PAUSED;
  }
  runtime->jit_test_pause_bulk_chunks=0;
  GRCORE_Stack *stack=grcore_context_stack(runtime->nested_context);
  while(grcore_stack_frame_count(stack)>runtime->nested_base)
    (void)grcore_stack_pop(stack);
  GRCORE_Result leave=grcore_boundary_leave(stack,runtime->nested_boundary);
  Run *run=&runtime->run;
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
  if(run->args)gcu_allocator_free(&runtime->module->alloc,run->args);
  runtime->boundary_token=runtime->nested_saved_token;
  runtime->run=runtime->nested_saved_run;
  runtime->nested_active=0;runtime->nested_context=NULL;
  runtime->nested_caller=0;runtime->nested_instance=0;runtime->nested_base=0;
  runtime->nested_owns_run=0;
  memset(&runtime->nested_boundary,0,sizeof(runtime->nested_boundary));
  return output;
}

/* Start a Wasm entry on the caller's stack and retain the boundary activation
 * and guest frames when a runtime-core poll pauses. */
GLWASM_Result glwasm_runtime_invoke_nested_results(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    const char *export_name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *result,uint32_t *result_count){
  if(!runtime||!context||!caller||!instance_token||!export_name||!result_count||
      (argument_count&&!arguments)||runtime->run.active||runtime->nested_active)
    return GLWASM_ERR_INVALID;
  /* A paused context owns a saved continuation. Do not add a boundary or
   * guest frames to it: grcore_run would reject the state, and the direct
   * re-entry path below is reserved for an active host callback. */
  if(grcore_context_state(context)==GRCORE_CONTEXT_PAUSED)
    return GLWASM_ERR_INVALID;
  memset(&runtime->last_foreign_exception,0,
      sizeof(runtime->last_foreign_exception));
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
  runtime->nested_saved_run=runtime->run;
  runtime->nested_saved_token=runtime->boundary_token;
  runtime->nested_context=context;runtime->nested_caller=caller;
  runtime->nested_instance=instance_token;runtime->nested_base=base;
  runtime->nested_boundary=boundary;
  Run *run=&runtime->run;
  run->ctx=context;run->engine=wasm_engine;run->instance=runtime;
  run->entry=function;run->nargs=argument_count;run->stack_base=base;
  run->results=result;run->result_capacity=capacity;
  run->result_arity=result_arity;run->result_count=0;
  run->jit_entry_attempted=0;
  run->jit_pending=0;
  run->args=argument_count?gcu_allocator_calloc(&runtime->module->alloc,
      argument_count,sizeof(*run->args)):NULL;
  run->result=0;run->error=GLWASM_OK;run->trap=GLWASM_TRAP_NONE;
  run->pending_host=0;run->active=1;run->paused=0;run->bulk_active=0;
  run->m=runtime->module;run->globals=runtime->globals;run->tables=runtime->tables;
  run->table_sizes=runtime->table_sizes;run->memories=runtime->memories;
  run->memory_bytes=runtime->memory_bytes;run->memory_pages=runtime->memory_pages;
  run->host_resources=runtime->host_resources;run->data_dropped=runtime->data_dropped;
  run->elements_dropped=runtime->elements_dropped;run->guest_alloc=runtime->guest_alloc;
  if(argument_count&&!run->args){runtime->run=runtime->nested_saved_run;
    runtime->nested_context=NULL;
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
        diagnostic_record_frame_entry(&runtime->run,function,
            slots_ptr[H_SP],H_OPERANDS+fn->nlocals);
        for(uint32_t i=0;i<argument_count;i++)slots_ptr[H_OPERANDS+i]=arguments[i];
        core=wasm_set_guest_frame_identity(stack,frame,runtime->module,
            function,0);}
    }
  }
  if(core!=GRCORE_OK){runtime->run.error=map_core_result(core);
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
    runtime->nested_owns_run=1;
  }
  if(core==GRCORE_OK&&outcome==GRCORE_OUTCOME_PAUSED){
    runtime->nested_active=1;runtime->run.paused=1;
  }
  return finish_nested(runtime,core,outcome,result,result_count);
}

GLWASM_Result glwasm_runtime_last_foreign_exception(
    const GLWASM_Runtime *runtime,GLWASM_ForeignException *out_exception){
  if(!runtime||!out_exception)return GLWASM_ERR_INVALID;
  *out_exception=runtime->last_foreign_exception;
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
  if(!runtime||!context||!result_count||!runtime->nested_active||
      runtime->nested_context!=context||runtime->nested_caller!=caller||
      runtime->nested_instance!=instance_token)return GLWASM_ERR_INVALID;
  if(*result_count<runtime->run.result_arity||
      (runtime->run.result_arity&&!result)){
    *result_count=runtime->run.result_arity;return GLWASM_ERR_LIMIT;
  }
  runtime->run.result_capacity=*result_count;
  runtime->run.results=result;
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result core;
  if(runtime->nested_owns_run){
    core=grcore_resume(context,&outcome);
  }else{
    GRCORE_Step step=execute(context,&runtime->run);
    core=GRCORE_OK;
    if(step==GRCORE_STEP_PAUSED)outcome=GRCORE_OUTCOME_PAUSED;
    else if(step==GRCORE_STEP_UNWOUND)
      core=runtime->run.error==GLWASM_ERR_LIMIT?GRCORE_ERR_LIMIT:GRCORE_ERR_GUEST;
  }
  return finish_nested(runtime,core,outcome,result,result_count);
}
GLWASM_Result glwasm_runtime_resume_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    uint64_t *result){
  if(!runtime||!result||runtime->run.result_arity>1)
    return GLWASM_ERR_INVALID;
  uint32_t capacity=1;
  *result=0;
  return glwasm_runtime_resume_nested_results(runtime,context,caller,
      instance_token,result,&capacity);
}
void glwasm_runtime_destroy(GLWASM_Runtime *r) {
  if (!r) return;
  if(r->boundary_context&&r->boundary_context!=r->context){
    clear_jit_context_state(r->module,r->boundary_context,
        &r->boundary_jit_slots,&r->boundary_jit_registered,
        &r->boundary_jit_reservation);
    release_debug_names(r->boundary_context,r->module->debug_id,
        r->boundary_engine);
  }
  free_exceptions(r);
  if (r->memories && r->guest_alloc)
    for (uint32_t i = 0; i < r->module->nmems; i++)
      gcu_allocator_free(r->guest_alloc, r->memories[i]);
  if (r->tables && r->guest_alloc)
    for (uint32_t i = 0; i < r->module->ntables; i++)
      gcu_allocator_free(r->guest_alloc, r->tables[i]);
  if(r->host_functions)
    for(uint32_t i=0;i<r->module->nfuncs;i++){
      gcu_allocator_free(&r->module->alloc,r->host_functions[i].parameter_types);
      gcu_allocator_free(&r->module->alloc,r->host_functions[i].result_types);
    }
  if (r->context) clear_jit_slots(r->module, r->context, r->jit_slots);
  else gcu_allocator_free(&r->module->alloc, r->jit_slots);
  r->jit_slots = NULL;
  gcu_allocator_free(&r->module->alloc, r->jit_registered);
  r->jit_registered = NULL;
  if (r->context && r->jit_reservation)
    grcore_deopt_release(r->context, r->jit_reservation);
  r->jit_reservation = NULL;
  if (r->context) grcore_context_destroy(r->context);
  if (r->group) grcore_group_destroy(r->group);
  if (r->options) grcore_options_destroy(r->options);
  gcu_allocator_free(&r->module->alloc, r->tables);
  gcu_allocator_free(&r->module->alloc, r->host_functions);
  gcu_allocator_free(&r->module->alloc, r->table_sizes);
  gcu_allocator_free(&r->module->alloc, r->memories);
  gcu_allocator_free(&r->module->alloc, r->memory_bytes);
  gcu_allocator_free(&r->module->alloc, r->memory_pages);
  if(r->memory_debug_names){
    for(uint32_t i=0;i<r->module->nmems;i++)
      gcu_allocator_free(&r->module->alloc,r->memory_debug_names[i]);
    gcu_allocator_free(&r->module->alloc,r->memory_debug_names);
  }
  gcu_allocator_free(&r->module->alloc, r->data_dropped);
  gcu_allocator_free(&r->module->alloc, r->elements_dropped);
  gcu_allocator_free(&r->module->alloc, r->globals);
  gcu_allocator_free(&r->module->alloc, r->test_observed_functions);
  for (size_t i = 0; i < r->jit_spill_count; ++i)
    gcu_allocator_free(&r->module->alloc, r->jit_spills[i]);
  gcu_allocator_free(&r->module->alloc, r->jit_spills);
  gcu_allocator_free(&r->module->alloc, r->jit_spill_capacities);
  gcu_allocator_free(&r->module->alloc, r->jit_deopt_cells);
  GLWASM_Module *module = r->module;
  gcu_allocator_free(&module->alloc, r->run.args);
  gcu_allocator_free(&module->alloc, r);
  glwasm_module_destroy(module);
}
GRCORE_Context *glwasm_runtime_context(const GLWASM_Runtime *runtime) {
  return runtime?runtime->context:NULL;
}
GLWASM_Result glwasm_runtime_set_test_limits(GLWASM_Runtime *r,
    uint64_t fuel, uint64_t depth, int always_move) {
  if (!r || r->run.active || r->exceptions) return GLWASM_ERR_INVALID;
  GRCORE_Options *options = NULL;
  GRCORE_Context *context = NULL;
  GRCORE_EngineId engine = 0;
  GRCORE_Result cr = grcore_options_create(NULL, &options);
  if (cr == GRCORE_OK) cr = grcore_options_set_fuel(options, fuel);
  GLWASM_Policy runtime_policy;
  if(r->module->has_policy)runtime_policy=r->module->policy;
  else glwasm_policy_preset(GLWASM_POLICY_UNTRUSTED,&runtime_policy);
  if(cr==GRCORE_OK)cr=grcore_options_set_memory_bytes(options,
      runtime_policy.default_memory_bytes);
  if (cr == GRCORE_OK) cr = grcore_options_set_guest_depth(options, depth);
  if (cr == GRCORE_OK) cr = grcore_context_create(r->group, options, &context);
  if (cr == GRCORE_OK) cr = grcore_engine_register(context, &descriptor, &engine);
  if (cr == GRCORE_OK) {
    GLWASM_Result names=ensure_debug_names(context,r->module,engine);
    if(names!=GLWASM_OK)cr=names==GLWASM_ERR_OOM?GRCORE_ERR_OOM:GRCORE_ERR_INVALID;
  }
  if (cr == GRCORE_OK) cr = grcore_context_register(context, &trap_key, &r->run);
  if (cr == GRCORE_OK)
    cr = grcore_stack_set_always_move(grcore_context_stack(context), always_move != 0);
  if (cr != GRCORE_OK) {
    if (context) grcore_context_destroy(context);
    if (options) grcore_options_destroy(options);
    return map_core_result(cr);
  }

  const GCU_Allocator *new_alloc = grcore_context_allocator(context);
  uint8_t **new_memories = r->module->nmems ?
      gcu_allocator_calloc(&r->module->alloc, r->module->nmems,
          sizeof(*new_memories)) : NULL;
  if (r->module->nmems && !new_memories) {
    grcore_context_destroy(context);
    grcore_options_destroy(options);
    return GLWASM_ERR_OOM;
  }
  for (uint32_t i = 0; i < r->module->nmems; i++) {
    if(r->module->mems[i].imported){new_memories[i]=NULL;continue;}
    uint64_t bytes = r->memory_bytes[i];
    if (bytes > SIZE_MAX) {
      for (uint32_t j = 0; j < i; j++) gcu_allocator_free(new_alloc, new_memories[j]);
      gcu_allocator_free(&r->module->alloc, new_memories);
      grcore_context_destroy(context);
      grcore_options_destroy(options);
      return GLWASM_ERR_LIMIT;
    }
    new_memories[i] = bytes ? gcu_allocator_calloc(new_alloc, (size_t)bytes, 1) : NULL;
    if (bytes && !new_memories[i]) {
      for (uint32_t j = 0; j < i; j++) gcu_allocator_free(new_alloc, new_memories[j]);
      gcu_allocator_free(&r->module->alloc, new_memories);
      grcore_context_destroy(context);
      grcore_options_destroy(options);
      return GLWASM_ERR_OOM;
    }
    if (bytes) memcpy(new_memories[i], r->memories[i], (size_t)bytes);
  }

  uint64_t **new_tables = r->module->ntables ?
      gcu_allocator_calloc(&r->module->alloc, r->module->ntables,
          sizeof(*new_tables)) : NULL;
  if (r->module->ntables && !new_tables) {
    for (uint32_t i = 0; i < r->module->nmems; i++)
      gcu_allocator_free(new_alloc, new_memories[i]);
    gcu_allocator_free(&r->module->alloc, new_memories);
    grcore_context_destroy(context);
    grcore_options_destroy(options);
    return GLWASM_ERR_OOM;
  }
  for (uint32_t i = 0; i < r->module->ntables; i++) {
    if(r->module->tables[i].imported){new_tables[i]=NULL;continue;}
    uint64_t entries = r->table_sizes[i];
    if (entries > SIZE_MAX / sizeof(*new_tables[i])) {
      for (uint32_t j = 0; j < i; j++) gcu_allocator_free(new_alloc, new_tables[j]);
      for (uint32_t j = 0; j < r->module->nmems; j++)
        gcu_allocator_free(new_alloc, new_memories[j]);
      gcu_allocator_free(&r->module->alloc, new_tables);
      gcu_allocator_free(&r->module->alloc, new_memories);
      grcore_context_destroy(context);
      grcore_options_destroy(options);
      return GLWASM_ERR_LIMIT;
    }
    new_tables[i] = entries ?
        gcu_allocator_calloc(new_alloc, (size_t)entries, sizeof(*new_tables[i])) : NULL;
    if (entries && !new_tables[i]) {
      for (uint32_t j = 0; j < i; j++) gcu_allocator_free(new_alloc, new_tables[j]);
      for (uint32_t j = 0; j < r->module->nmems; j++)
        gcu_allocator_free(new_alloc, new_memories[j]);
      gcu_allocator_free(&r->module->alloc, new_tables);
      gcu_allocator_free(&r->module->alloc, new_memories);
      grcore_context_destroy(context);
      grcore_options_destroy(options);
      return GLWASM_ERR_OOM;
    }
    if (entries) memcpy(new_tables[i], r->tables[i],
        (size_t)entries * sizeof(*new_tables[i]));
  }

  GRCORE_EntrySlot **new_jit_slots = NULL;
  GLWASM_Result slot_result = create_jit_slots(r->module, context,
      &new_jit_slots);
  if (slot_result != GLWASM_OK) {
    for (uint32_t i = 0; i < r->module->ntables; ++i)
      gcu_allocator_free(new_alloc, new_tables[i]);
    for (uint32_t i = 0; i < r->module->nmems; ++i)
      gcu_allocator_free(new_alloc, new_memories[i]);
    gcu_allocator_free(&r->module->alloc, new_tables);
    gcu_allocator_free(&r->module->alloc, new_memories);
    grcore_context_destroy(context);
    grcore_options_destroy(options);
    return slot_result;
  }
  GRCORE_DeoptReservation *new_jit_reservation = NULL;
  GLWASM_Result jit_context = register_jit_runtime(r, context,
      &new_jit_reservation);
  if (jit_context != GLWASM_OK) {
    clear_jit_slots(r->module, context, new_jit_slots);
    for (uint32_t i = 0; i < r->module->ntables; ++i)
      gcu_allocator_free(new_alloc, new_tables[i]);
    for (uint32_t i = 0; i < r->module->nmems; ++i)
      gcu_allocator_free(new_alloc, new_memories[i]);
    gcu_allocator_free(&r->module->alloc, new_tables);
    gcu_allocator_free(&r->module->alloc, new_memories);
    grcore_context_destroy(context);
    grcore_options_destroy(options);
    return jit_context;
  }

  const GCU_Allocator *old_alloc = r->guest_alloc;
  for (uint32_t i = 0; i < r->module->nmems; i++) {
    gcu_allocator_free(old_alloc, r->memories[i]);
    r->memories[i] = new_memories[i];
  }
  for (uint32_t i = 0; i < r->module->ntables; i++) {
    gcu_allocator_free(old_alloc, r->tables[i]);
    r->tables[i] = new_tables[i];
  }
  gcu_allocator_free(&r->module->alloc, new_tables);
  gcu_allocator_free(&r->module->alloc, new_memories);
  if (r->context) {
    clear_jit_slots(r->module, r->context, r->jit_slots);
    if (r->jit_reservation)
      grcore_deopt_release(r->context, r->jit_reservation);
    grcore_context_destroy(r->context);
  }
  if (r->options) grcore_options_destroy(r->options);
  r->context = context;
  r->options = options;
  r->engine = engine;
  r->jit_slots = new_jit_slots;
  memset(r->jit_registered, 0,
      (size_t)r->module->nfuncs * sizeof(*r->jit_registered));
  r->jit_reservation = new_jit_reservation;
  r->guest_alloc = new_alloc;
  r->run.guest_alloc = new_alloc;
  r->run.ctx = context;
  r->run.engine = engine;
  return GLWASM_OK;
}
static void discard_guest_frames(GLWASM_Runtime*r){GRCORE_Stack*s=grcore_context_stack(r->context);while(grcore_stack_frame_count(s))(void)grcore_stack_pop(s);}
static void free_run_args(GLWASM_Runtime*r){if(r->run.args){gcu_allocator_free(&r->module->alloc,r->run.args);r->run.args=NULL;}}
static GLWASM_Result rebind_restored_guest_frames(GLWASM_Runtime *runtime) {
  GRCORE_Stack *stack = grcore_context_stack(runtime->context);
  size_t count = grcore_stack_frame_count(stack);
  GRCORE_FrameRef frame = grcore_stack_top(stack);
  /* Validate the whole chain before changing any frame. A Wasm snapshot may
   * be restored into another instance of the same module, so frame headers
   * must never retain the source instance's address. */
  for (size_t i = 0; i < count; ++i) {
    uint64_t *frame_slots = slots(stack, frame);
    if (!frame_slots || frame_slots[H_FUNC] >= runtime->module->nfuncs)
      return GLWASM_ERR_INTERNAL;
    frame = grcore_stack_caller(stack, frame);
  }
  frame = grcore_stack_top(stack);
  for (size_t i = 0; i < count; ++i) {
    uint64_t *frame_slots = slots(stack, frame);
    /* The first pass guarantees these are valid until this parked context is
     * resumed; no frame mutation or collection runs between the passes. */
    frame_slots[H_INSTANCE] = (uintptr_t)runtime;
    frame = grcore_stack_caller(stack, frame);
  }
  return GLWASM_OK;
}
static GLWASM_Result finish_run(GLWASM_Runtime*r,GRCORE_Result cr,
    GRCORE_Outcome outcome,uint64_t*out,uint32_t*result_count,int*paused){
  /* runtime-core writes outcome only on success. An unwind can leave a stale
   * PAUSED value in the caller's storage, but it is never resumable. */
  const int did_pause=cr==GRCORE_OK&&r->run.error==GLWASM_OK&&
      r->run.trap==GLWASM_TRAP_NONE&&outcome==GRCORE_OUTCOME_PAUSED;
  if(paused)*paused=did_pause;
  r->run.paused=did_pause;
  if(r->run.paused)return GLWASM_ERR_PAUSED;
  r->jit_test_pause_bulk_chunks=0;
  if(r->run.error!=GLWASM_OK){discard_guest_frames(r);r->run.active=0;
    r->run.bulk_active=0;free_run_args(r);return r->run.error;}
  if(r->run.trap!=GLWASM_TRAP_NONE){discard_guest_frames(r);r->run.active=0;
    r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}
  if(cr==GRCORE_ERR_GUEST){discard_guest_frames(r);r->run.active=0;
    r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}
  if(cr==GRCORE_ERR_LIMIT){discard_guest_frames(r);r->run.active=0;
    r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_LIMIT;}
  if(cr!=GRCORE_OK){discard_guest_frames(r);r->run.active=0;
    r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_INTERNAL;}
  if(result_count)*result_count=r->run.result_count;
  if(out&&r->run.results&&out!=r->run.results&&r->run.result_count)
    memcpy(out,r->run.results,(size_t)r->run.result_count*sizeof(*out));
  r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_OK;
}

static int snapshot_value_type_supported(GLWASM_ValType type){
  return type.kind!=GLWASM_VK_REF||type.heap==GLWASM_HT_FUNC||
      type.heap==GLWASM_HT_NOFUNC;
}
static int snapshot_module_supported(const GLWASM_Module*m){
  if(!m||m->nimports)return 0;
  for(uint32_t i=0;i<m->ntypes;i++){
    for(uint32_t j=0;j<m->types[i].nparams;j++)
      if(!snapshot_value_type_supported(m->types[i].params[j]))return 0;
    for(uint32_t j=0;j<m->types[i].nresults;j++)
      if(!snapshot_value_type_supported(m->types[i].results[j]))return 0;
  }
  for(uint32_t i=0;i<m->nfuncs;i++)for(uint32_t j=0;j<m->funcs[i].nlocals;j++)
    if(!snapshot_value_type_supported(m->funcs[i].locals[j]))return 0;
  for(uint32_t i=0;i<m->nglobals;i++)
    if(m->globals[i].imported||!snapshot_value_type_supported(m->globals[i].type))return 0;
  for(uint32_t i=0;i<m->nmems;i++)if(m->mems[i].imported)return 0;
  for(uint32_t i=0;i<m->ntables;i++)if(m->tables[i].imported||
      !snapshot_value_type_supported(m->tables[i].elem))return 0;
  return 1;
}

static void snapshot_free(GLWASM_Snapshot*s){
  if(!s)return;
  GLWASM_Module*m=s->module;
  if(s->core)grcore_snapshot_release(s->core);
  if(m){
    if(s->memories)for(uint32_t i=0;i<m->nmems;i++)gcu_allocator_free(&m->alloc,s->memories[i]);
    if(s->tables)for(uint32_t i=0;i<m->ntables;i++)gcu_allocator_free(&m->alloc,s->tables[i]);
    gcu_allocator_free(&m->alloc,s->memories);gcu_allocator_free(&m->alloc,s->memory_bytes);
    gcu_allocator_free(&m->alloc,s->memory_pages);gcu_allocator_free(&m->alloc,s->tables);
    gcu_allocator_free(&m->alloc,s->table_sizes);gcu_allocator_free(&m->alloc,s->globals);
    gcu_allocator_free(&m->alloc,s->data_dropped);gcu_allocator_free(&m->alloc,s->elements_dropped);
    gcu_allocator_free(&m->alloc,s->args);gcu_allocator_free(&m->alloc,s);
    glwasm_module_destroy(m);
  }
}

GLWASM_Result glwasm_runtime_snapshot(GLWASM_Runtime*r,GLWASM_Snapshot**out){
  if(!r||!out||!r->run.active||!r->run.paused||r->run.pending_host||
      !snapshot_module_supported(r->module))return GLWASM_ERR_UNSUPPORTED;
  GLWASM_Module*m=r->module;GLWASM_Snapshot*s=gcu_allocator_calloc(&m->alloc,1,sizeof(*s));
  if(!s)return GLWASM_ERR_OOM;
  s->module=m;
  glwasm_module_retain(m);
  GRCORE_Result cr=grcore_context_snapshot(r->context,NULL,&s->core);
  if(cr!=GRCORE_OK){snapshot_free(s);return map_core_result(cr);}
#define SNAP_ALLOC(field,count,type) do{if((count)){s->field=gcu_allocator_calloc(&m->alloc,(count),sizeof(type));if(!s->field){snapshot_free(s);return GLWASM_ERR_OOM;}}}while(0)
  SNAP_ALLOC(globals,m->nglobals,uint64_t);if(m->nglobals)memcpy(s->globals,r->globals,m->nglobals*sizeof(uint64_t));
  SNAP_ALLOC(memories,m->nmems,uint8_t*);SNAP_ALLOC(memory_bytes,m->nmems,uint64_t);SNAP_ALLOC(memory_pages,m->nmems,uint64_t);
  for(uint32_t i=0;i<m->nmems;i++){s->memory_bytes[i]=r->memory_bytes[i];s->memory_pages[i]=r->memory_pages[i];if(s->memory_bytes[i]){s->memories[i]=gcu_allocator_calloc(&m->alloc,(size_t)s->memory_bytes[i],1);if(!s->memories[i]){snapshot_free(s);return GLWASM_ERR_OOM;}memcpy(s->memories[i],r->memories[i],(size_t)s->memory_bytes[i]);}}
  SNAP_ALLOC(tables,m->ntables,uint64_t*);SNAP_ALLOC(table_sizes,m->ntables,uint64_t);
  for(uint32_t i=0;i<m->ntables;i++){s->table_sizes[i]=r->table_sizes[i];if(s->table_sizes[i]){if(s->table_sizes[i]>SIZE_MAX/sizeof(uint64_t)){snapshot_free(s);return GLWASM_ERR_LIMIT;}s->tables[i]=gcu_allocator_calloc(&m->alloc,(size_t)s->table_sizes[i],sizeof(uint64_t));if(!s->tables[i]){snapshot_free(s);return GLWASM_ERR_OOM;}memcpy(s->tables[i],r->tables[i],(size_t)s->table_sizes[i]*sizeof(uint64_t));}}
  SNAP_ALLOC(data_dropped,m->ndatas,uint8_t);if(m->ndatas)memcpy(s->data_dropped,r->data_dropped,m->ndatas);
  SNAP_ALLOC(elements_dropped,m->nelems,uint8_t);if(m->nelems)memcpy(s->elements_dropped,r->elements_dropped,m->nelems);
  s->nargs=r->run.nargs;s->entry=r->run.entry;s->result=r->run.result;
  s->result_capacity=r->run.result_capacity;
  s->result_arity=r->run.result_arity;s->result_count=r->run.result_count;
  s->bulk_func=r->run.bulk_func;s->bulk_pc=r->run.bulk_pc;s->bulk_op=r->run.bulk_op;
  s->jit_entry_attempted=r->run.jit_entry_attempted;s->jit_pending=r->run.jit_pending;s->jit_pending_function=r->run.jit_pending_function;
  s->bulk_dst=r->run.bulk_dst;s->bulk_src=r->run.bulk_src;s->bulk_len=r->run.bulk_len;s->bulk_done=r->run.bulk_done;s->bulk_value=r->run.bulk_value;s->bulk_active=r->run.bulk_active;
  SNAP_ALLOC(args,s->nargs,uint64_t);if(s->nargs)memcpy(s->args,r->run.args,s->nargs*sizeof(uint64_t));
#undef SNAP_ALLOC
  *out=s;return GLWASM_OK;
}

GLWASM_Result glwasm_runtime_restore(GLWASM_Runtime*r,const GLWASM_Snapshot*s){
  if(!r||!s||r->module!=s->module||!s->core||r->run.active||!snapshot_module_supported(r->module))return GLWASM_ERR_INVALID;
  GLWASM_Result restore_error=GLWASM_ERR_OOM;
  GLWASM_Module*m=r->module;const GCU_Allocator*a=r->guest_alloc;
  uint8_t**mems=m->nmems?gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*mems)):NULL;
  uint64_t**tabs=m->ntables?gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*tabs)):NULL;
  uint64_t*globals=m->nglobals?gcu_allocator_calloc(&m->alloc,m->nglobals,sizeof(*globals)):NULL;
  uint8_t*dd=m->ndatas?gcu_allocator_calloc(&m->alloc,m->ndatas,1):NULL;
  uint8_t*ed=m->nelems?gcu_allocator_calloc(&m->alloc,m->nelems,1):NULL;
  uint64_t*args=s->nargs?gcu_allocator_calloc(&m->alloc,s->nargs,sizeof(*args)):NULL;
  uint64_t*mb=m->nmems?gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*mb)):NULL;
  uint64_t*mp=m->nmems?gcu_allocator_calloc(&m->alloc,m->nmems,sizeof(*mp)):NULL;
  uint64_t*ts=m->ntables?gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*ts)):NULL;
  if((m->nmems&&(!mems||!mb||!mp))||(m->ntables&&(!tabs||!ts))||(m->nglobals&&!globals)||(m->ndatas&&!dd)||(m->nelems&&!ed)||(s->nargs&&!args))goto oom;
  for(uint32_t i=0;i<m->nmems;i++){mb[i]=s->memory_bytes[i];mp[i]=s->memory_pages[i];if(mb[i]){if(mb[i]>SIZE_MAX)goto limit;mems[i]=gcu_allocator_calloc(a,(size_t)mb[i],1);if(!mems[i])goto oom;memcpy(mems[i],s->memories[i],(size_t)mb[i]);}}
  for(uint32_t i=0;i<m->ntables;i++){ts[i]=s->table_sizes[i];if(ts[i]){if(ts[i]>SIZE_MAX/sizeof(uint64_t))goto limit;tabs[i]=gcu_allocator_calloc(a,(size_t)ts[i],sizeof(uint64_t));if(!tabs[i])goto oom;memcpy(tabs[i],s->tables[i],(size_t)ts[i]*sizeof(uint64_t));}}
  if(m->nglobals)memcpy(globals,s->globals,m->nglobals*sizeof(*globals));
  if(m->ndatas)memcpy(dd,s->data_dropped,m->ndatas);
  if(m->nelems)memcpy(ed,s->elements_dropped,m->nelems);
  if(s->nargs)memcpy(args,s->args,s->nargs*sizeof(*args));
  {GRCORE_RestoreEnv env=GRCORE_RESTORE_ENV_INIT(NULL,NULL,execute,&r->run,"wasm");GRCORE_Result cr=grcore_context_restore(r->context,s->core,&env);if(cr!=GRCORE_OK)goto invalid_core;}
  if (rebind_restored_guest_frames(r) != GLWASM_OK) {
    restore_error = GLWASM_ERR_INTERNAL;
    goto cleanup_error;
  }
  for(uint32_t i=0;i<m->nmems;i++){gcu_allocator_free(a,r->memories[i]);r->memories[i]=mems[i];mems[i]=NULL;r->memory_bytes[i]=mb[i];r->memory_pages[i]=mp[i];}
  for(uint32_t i=0;i<m->ntables;i++){gcu_allocator_free(a,r->tables[i]);r->tables[i]=tabs[i];tabs[i]=NULL;r->table_sizes[i]=ts[i];}
  if(m->nglobals)memcpy(r->globals,globals,m->nglobals*sizeof(*globals));
  if(m->ndatas)memcpy(r->data_dropped,dd,m->ndatas);
  if(m->nelems)memcpy(r->elements_dropped,ed,m->nelems);
  r->run.args=args;args=NULL;r->run.nargs=s->nargs;r->run.entry=s->entry;
  r->run.result=s->result;r->run.results=NULL;
  r->run.result_capacity=s->result_capacity;
  r->run.result_arity=s->result_arity;r->run.result_count=s->result_count;
  /* The core snapshot stores guest frames, not the host-side Run aliases.
   * Rebind every borrowed pointer to the destination instance before resume. */
  r->run.ctx=r->context;r->run.engine=r->engine;r->run.instance=r;
  r->run.m=m;r->run.globals=r->globals;r->run.tables=r->tables;
  r->run.table_sizes=r->table_sizes;r->run.memories=r->memories;
  r->run.memory_bytes=r->memory_bytes;r->run.memory_pages=r->memory_pages;
  r->run.host_resources=r->host_resources;
  r->run.data_dropped=r->data_dropped;
  r->run.elements_dropped=r->elements_dropped;
  r->run.guest_alloc=r->guest_alloc;r->run.stack_base=0;
  r->run.bulk_func=s->bulk_func;r->run.bulk_pc=s->bulk_pc;r->run.bulk_op=s->bulk_op;
  r->run.jit_entry_attempted=s->jit_entry_attempted;r->run.jit_pending=s->jit_pending;r->run.jit_pending_function=s->jit_pending_function;
  r->run.bulk_dst=s->bulk_dst;r->run.bulk_src=s->bulk_src;r->run.bulk_len=s->bulk_len;r->run.bulk_done=s->bulk_done;r->run.bulk_value=s->bulk_value;r->run.bulk_active=s->bulk_active;r->run.active=1;r->run.paused=1;r->run.pending_host=0;r->run.error=GLWASM_OK;r->run.trap=GLWASM_TRAP_NONE;
  r->jit_test_pause_bulk_chunks=0;
  gcu_allocator_free(&m->alloc,mems);gcu_allocator_free(&m->alloc,tabs);gcu_allocator_free(&m->alloc,globals);gcu_allocator_free(&m->alloc,dd);gcu_allocator_free(&m->alloc,ed);gcu_allocator_free(&m->alloc,args);gcu_allocator_free(&m->alloc,mb);gcu_allocator_free(&m->alloc,mp);gcu_allocator_free(&m->alloc,ts);return GLWASM_OK;
invalid_core:
  for(uint32_t i=0;i<m->nmems;i++)gcu_allocator_free(a,mems?mems[i]:NULL);
  for(uint32_t i=0;i<m->ntables;i++)gcu_allocator_free(a,tabs?tabs[i]:NULL);
  gcu_allocator_free(&m->alloc,mems);gcu_allocator_free(&m->alloc,tabs);gcu_allocator_free(&m->alloc,globals);gcu_allocator_free(&m->alloc,dd);gcu_allocator_free(&m->alloc,ed);gcu_allocator_free(&m->alloc,args);gcu_allocator_free(&m->alloc,mb);gcu_allocator_free(&m->alloc,mp);gcu_allocator_free(&m->alloc,ts);return GLWASM_ERR_INVALID;
limit:
  restore_error=GLWASM_ERR_LIMIT;goto cleanup_error;
oom:
  restore_error=GLWASM_ERR_OOM;goto cleanup_error;
cleanup_error:
  for(uint32_t i=0;i<m->nmems;i++)gcu_allocator_free(a,mems?mems[i]:NULL);
  for(uint32_t i=0;i<m->ntables;i++)gcu_allocator_free(a,tabs?tabs[i]:NULL);
  gcu_allocator_free(&m->alloc,mems);gcu_allocator_free(&m->alloc,tabs);gcu_allocator_free(&m->alloc,globals);gcu_allocator_free(&m->alloc,dd);gcu_allocator_free(&m->alloc,ed);gcu_allocator_free(&m->alloc,args);gcu_allocator_free(&m->alloc,mb);gcu_allocator_free(&m->alloc,mp);gcu_allocator_free(&m->alloc,ts);return restore_error;
}

void glwasm_snapshot_release(GLWASM_Snapshot*s){snapshot_free(s);}
GLWASM_Result glwasm_runtime_invoke_results(GLWASM_Runtime*r,uint32_t fi,
    const uint64_t*args,uint32_t nargs,uint64_t*out,uint32_t*result_count){
  if(!r||fi>=r->module->nfuncs||(nargs&&!args)||!result_count||
      r->run.active)return GLWASM_ERR_INVALID;
  GLWASM_Func*fn=&r->module->funcs[fi];
  if(fn->type_index>=r->module->ntypes||
      nargs!=r->module->types[fn->type_index].nparams)return GLWASM_ERR_INVALID;
  uint32_t result_arity=r->module->types[fn->type_index].nresults;
  uint32_t capacity=*result_count;
  if(capacity<result_arity||(result_arity&&!out)){
    *result_count=result_arity;return GLWASM_ERR_LIMIT;
  }
  Run*state=&r->run;
  state->args=nargs?gcu_allocator_calloc(&r->module->alloc,nargs,
      sizeof(*state->args)):NULL;
  if(nargs&&!state->args)return GLWASM_ERR_OOM;
  state->ctx=r->context;state->engine=r->engine;state->instance=r;
  state->m=r->module;state->globals=r->globals;state->tables=r->tables;
  state->table_sizes=r->table_sizes;state->memories=r->memories;
  state->memory_bytes=r->memory_bytes;state->memory_pages=r->memory_pages;
  state->host_resources=r->host_resources;state->data_dropped=r->data_dropped;
  state->elements_dropped=r->elements_dropped;state->guest_alloc=r->guest_alloc;
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
  GRCORE_Result cr=grcore_run(r->context,execute,state,&outcome);
  r->last_core_result = cr;
  r->last_outcome = outcome;
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
  if(!r||!result_count||!r->run.active||!r->run.paused)
    return GLWASM_ERR_INVALID;
  if(*result_count<r->run.result_arity||
      (r->run.result_arity&&!out)){
    *result_count=r->run.result_arity;return GLWASM_ERR_LIMIT;
  }
  r->run.result_capacity=*result_count;
  r->run.results=out;
  /* `paused` describes the previous run's outcome. Clear it before the
   * restored callback is re-entered so a JIT entry cannot mistake the old
   * pause for a new poll verdict. finish_run sets it again if this resume
   * pauses a second time. */
  r->run.paused=0;
  GRCORE_Outcome outcome=GRCORE_OUTCOME_FINISHED;
  GRCORE_Result cr=grcore_resume(r->context,&outcome);
  r->last_core_result=cr;
  r->last_outcome=outcome;
  return finish_run(r,cr,outcome,out,result_count,paused);
}
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime*r,uint64_t*out,int*paused){
  if(!r||!out||!r->run.active||!r->run.paused||r->run.result_arity>1)
    return GLWASM_ERR_INVALID;
  uint32_t capacity=1;
  *out=0;
  return glwasm_runtime_resume_results(r,out,&capacity,paused);
}
GLWASM_Result glwasm_runtime_pause_position(const GLWASM_Runtime*r,
    uint32_t*function,uint64_t*offset){
  if(!r||!function||!offset||!r->run.paused)return GLWASM_ERR_INVALID;
  GRCORE_PollIdentity identity;
  GRCORE_Result result=grcore_context_poll_identity(r->context,&identity);
  if(result!=GRCORE_OK||identity.function>UINT32_MAX)return GLWASM_ERR_INVALID;
  *function=(uint32_t)identity.function;*offset=identity.offset;return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime*r){if(!r||r->run.active)return GLWASM_ERR_INVALID;GRCORE_Port*port=NULL;if(grcore_context_port(r->context,&port)!=GRCORE_OK)return GLWASM_ERR_OOM;GRCORE_Result cr=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);grcore_port_release(port);return cr==GRCORE_OK?GLWASM_OK:GLWASM_ERR_INTERNAL;}
GLWASM_Result glwasm_runtime_test_pause_after_bulk_chunks(
    GLWASM_Runtime *runtime, uint32_t chunks) {
  if (!runtime || runtime->run.active) return GLWASM_ERR_INVALID;
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
  if (!runtime || runtime->run.active || runtime->nested_active)
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
GLWASM_RuntimeTrap glwasm_runtime_last_trap(const GLWASM_Runtime*r){return r?r->run.trap:GLWASM_TRAP_NONE;}
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime*r){return r&&r->context?grcore_context_fuel_used(r->context):0;}
GLWASM_Result glwasm_runtime_test_set_jit_mode(GLWASM_Runtime *runtime,
    GLWASM_TestJitMode mode) {
  if (!runtime || runtime->run.active ||
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
  out->run_error = runtime->run.error;
  out->trap = runtime->run.trap;
  out->core_result = runtime->last_core_result;
  out->outcome = runtime->last_outcome;
  out->last_function = runtime->run.diagnostic_function;
  out->last_pc = runtime->run.diagnostic_pc;
  out->call_target = runtime->run.diagnostic_call_target;
  out->call_sp = runtime->run.diagnostic_call_sp;
  out->call_base = runtime->run.diagnostic_call_base;
  out->call_required = runtime->run.diagnostic_call_required;
  out->caller_params = runtime->run.diagnostic_caller_params;
  out->caller_locals = runtime->run.diagnostic_caller_locals;
  out->callee_params = runtime->run.diagnostic_callee_params;
  out->frame_entry_function = runtime->run.diagnostic_frame_entry_function;
  out->frame_entry_sp = runtime->run.diagnostic_frame_entry_sp;
  out->frame_entry_base = runtime->run.diagnostic_frame_entry_base;
  out->first_bad_sp_function = runtime->run.diagnostic_first_bad_sp_function;
  out->first_bad_sp_pc = runtime->run.diagnostic_first_bad_sp_pc;
  out->first_bad_sp = runtime->run.diagnostic_first_bad_sp;
  out->first_bad_base = runtime->run.diagnostic_first_bad_base;
  out->jit_lower_pc = runtime->run.last_jit_lower_pc;
  out->jit_lower_raw_result = runtime->run.last_jit_lower_raw_result;
  out->jit_lower_stack_count = runtime->run.last_jit_lower_stack_count;
  out->jit_lower_site = runtime->run.last_jit_lower_site;
  out->deopt_count = runtime->test_deopt_count;
  out->first_deopt = runtime->test_first_deopt;
  out->last_deopt = runtime->test_last_deopt;
  out->jit_lower_opcode = runtime->run.last_jit_lower_opcode;
  out->last_opcode = runtime->run.diagnostic_opcode;
  out->has_last_instruction = runtime->run.diagnostic_has_instruction;
  out->diagnostic_reason = runtime->run.diagnostic_reason;
  out->has_jit_cache = runtime->module->jit_cache != NULL;
  out->has_jit_pages = runtime->module->jit_pages != NULL;
  out->has_jit_slots = runtime->jit_slots != NULL;
  out->has_jit_registered = runtime->jit_registered != NULL;
  out->has_function_slot = runtime->jit_slots && runtime->jit_slots[function];
  out->has_function_registered = runtime->jit_registered &&
      runtime->jit_registered[function];
  out->context_matches_runtime = runtime->run.ctx == runtime->context;
  out->engine_matches_runtime = runtime->run.engine == runtime->engine;
  out->last_step = runtime->run.diagnostic_step;
  out->context_state_after_step = runtime->run.diagnostic_context_state;
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
  if (!runtime || runtime->run.active || !runtime->module->jit_cache ||
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
