/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/runtime-core/a/boundary.h>
#include <ghoti.io/cutil/allocator.h>
#include "runtime.h"
#include <math.h>
#include <limits.h>
#include <stdatomic.h>
#include <string.h>

enum { H_FUNC, H_PC, H_SP, H_NLOCALS, H_CTRL_DEPTH, H_SKIP_CHARGE, H_OPERANDS };
static size_t operand_capacity(const GLWASM_Func *f){return (size_t)f->nins+2u;}
static size_t frame_slots(const GLWASM_Func *f){return H_OPERANDS+f->nparams+f->nlocals+operand_capacity(f)+2u*f->nins+2u;}
static size_t control_base(const GLWASM_Func *f){return H_OPERANDS+f->nparams+f->nlocals+operand_capacity(f);}
static uint32_t block_results(const GLWASM_Func*f,uint32_t op){return op<f->nblocks?f->blocks[op].nresults:0;}
static uint32_t branch_arity(const GLWASM_Func*f,uint32_t opener){if(opener>=f->nins)return 0;uint32_t block=f->ins[opener].a;if(block>=f->nblocks)return 0;return f->ins[opener].op==0x03?f->blocks[block].nparams:f->blocks[block].nresults;}
static uint32_t arithmetic_shift_right32(uint32_t value,uint32_t count){count&=31u;if(!count)return value;uint32_t shifted=value>>count;return value&UINT32_C(0x80000000)?shifted|(UINT32_MAX<<(32u-count)):shifted;}
static int const_value(const GLWASM_Module*m,const GLWASM_Func*f,const uint64_t*g,uint64_t*out){uint64_t st[32];size_t sp=0;
#define CONST_PUSH(value) do { if(sp>=sizeof(st)/sizeof(st[0]))return 0; st[sp++]=(value); } while(0)
  for(uint32_t i=0;i<f->nins;i++){const GLWASM_Instr*x=&f->ins[i];if(x->op==0x0b){if(sp!=1)return 0;*out=st[0];return 1;}if(x->op==0x41)CONST_PUSH((uint32_t)x->c);else if(x->op==0x42)CONST_PUSH(x->c);else if(x->op==0x43)CONST_PUSH(x->a);else if(x->op==0x44)CONST_PUSH(x->c);else if(x->op==0x23&&x->a<m->nglobals)CONST_PUSH(g[x->a]);else if(sp>=2){uint64_t b=st[--sp],a=st[--sp],v=0;if(x->op==0x6a)v=(uint32_t)a+(uint32_t)b;else if(x->op==0x6b)v=(uint32_t)a-(uint32_t)b;else if(x->op==0x6c)v=(uint32_t)a*(uint32_t)b;else if(x->op==0x7c)v=a+b;else if(x->op==0x7d)v=a-b;else if(x->op==0x7e)v=a*b;else return 0;CONST_PUSH(v);}else return 0;}
#undef CONST_PUSH
  return 0;}
static int same_value_type(GLWASM_ValType a,GLWASM_ValType b){return a.kind==b.kind&&a.nullable==b.nullable&&a.heap==b.heap&&a.mut==b.mut&&a.index==b.index;}
static int same_function_type(const GLWASM_TypeDef*a,const GLWASM_TypeDef*b){if(a->nparams!=b->nparams||a->nresults!=b->nresults)return 0;for(uint32_t i=0;i<a->nparams;i++)if(!same_value_type(a->params[i],b->params[i]))return 0;for(uint32_t i=0;i<a->nresults;i++)if(!same_value_type(a->results[i],b->results[i]))return 0;return 1;}
static int element_ref(const GLWASM_Elem *e,uint32_t i,uint64_t *out){if(!e||!out)return 0;if(!e->use_exprs){if(i>=e->nfuncs)return 0;*out=(uint64_t)e->funcs[i]+1u;return 1;}if(i>=e->nexprs)return 0;const GLWASM_Func *expr=&e->exprs[i];int seen=0;uint64_t value=0;for(uint32_t k=0;k<expr->nins;k++){if(expr->ins[k].op==0xd2){if(seen)return 0;value=(uint64_t)expr->ins[k].a+1u;seen=1;}else if(expr->ins[k].op==0xd0){if(seen)return 0;value=0;seen=1;}else if(expr->ins[k].op!=0x0b||k+1u!=expr->nins)return 0;}if(!seen)return 0;*out=value;return 1;}
static GLWASM_Result map_core_result(GRCORE_Result result){switch(result){case GRCORE_OK:return GLWASM_OK;case GRCORE_ERR_UNSUPPORTED:return GLWASM_ERR_UNSUPPORTED;case GRCORE_ERR_LIMIT:return GLWASM_ERR_LIMIT;case GRCORE_ERR_OOM:return GLWASM_ERR_OOM;case GRCORE_ERR_INVALID:return GLWASM_ERR_INVALID;case GRCORE_ERR_GUEST:return GLWASM_ERR_GUEST;case GRCORE_ERR_INTERNAL:default:return GLWASM_ERR_INTERNAL;}}

static GRCORE_Location locate(const GRCORE_Context *c, uint64_t f, uint64_t o) {
  (void)c; (void)f;
  return (GRCORE_Location){"wasm", o <= INT_MAX ? (int)o : INT_MAX};
}
static const GRCORE_EngineDescriptor descriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "lang-wasm", NULL, locate, NULL, {NULL,NULL,NULL}, {0,0,0}, NULL,NULL,NULL,NULL);

typedef struct HostBinding {
  GLWASM_HostFunction function;
  GLWASM_HostResumeFunction resume;
  void *user;
  GLWASM_HostValueType *parameter_types;
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
  size_t stack_base;
  uint32_t pending_host_function; int pending_host;
  ExceptionValue *pending_exception;
  void *snapshot_restore;
  uint32_t bulk_func, bulk_pc, bulk_op;
  uint64_t bulk_dst, bulk_src, bulk_len, bulk_done, bulk_value;
  int bulk_active;
  GLWASM_Result error; GLWASM_RuntimeTrap trap; int active; int paused;
} Run;

struct GLWASM_Runtime {
  GLWASM_Module *module; uint64_t *globals; uint64_t **tables;
  uint64_t *table_sizes; uint8_t **memories; uint64_t *memory_bytes;
  uint64_t *memory_pages; uint8_t *data_dropped; uint8_t *elements_dropped;
  HostBinding *host_functions;
  HostBinding *host_resources;
  const GCU_Allocator *guest_alloc;
  GRCORE_Group *group; GRCORE_Options *options;
  GRCORE_Context *context; GRCORE_EngineId engine; uintptr_t boundary_token; Run run;
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
};

static atomic_uintptr_t next_boundary_token=1;

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
  uint64_t result;
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
    const uint64_t *arguments, uint64_t *result, int resume) {
  if(!runtime||function>=runtime->module->nfuncs||
      !runtime->host_functions||!runtime->host_functions[function].function)
    return GLWASM_ERR_LINK;
  GLWASM_Func *func=&runtime->module->funcs[function];
  if(func->type_index>=runtime->module->ntypes)return GLWASM_ERR_INTERNAL;
  GLWASM_TypeDef *type=&runtime->module->types[func->type_index];
  if(type->nresults>1)return GLWASM_ERR_UNSUPPORTED;
  GLWASM_HostCall call={.instance=runtime,
      .context=runtime->run.ctx?runtime->run.ctx:runtime->context,
      .engine=runtime->run.engine?runtime->run.engine:runtime->engine,
      .instance_token=runtime->boundary_token,
      .has_resume=runtime->host_functions[function].resume!=NULL,
      .types=runtime->host_functions[function].parameter_types,
      .arguments=arguments,.argument_count=type->nparams,
      .result_count=type->nresults,
      .result_type=type->nresults?host_value_type(type->results[0]):GLWASM_HOST_I32,
      .user=runtime->host_functions[function].user};
  if(type->nresults&&call.result_type==0)return GLWASM_ERR_UNSUPPORTED;
  GRCORE_Context *context=runtime->run.ctx?runtime->run.ctx:runtime->context;
  GRCORE_Stack *stack=grcore_context_stack(context);
  GRCORE_ActivationRef activation={0};
  GRCORE_Result cr=grcore_activation_enter(stack,GRCORE_ACTIVATION_NATIVE,
      runtime->run.engine?runtime->run.engine:runtime->engine,true,NULL,
      &activation);
  if(cr!=GRCORE_OK)return map_core_result(cr);
  uint64_t ignored=0;
  GLWASM_HostFunction initial=runtime->host_functions[function].function;
  GLWASM_HostResumeFunction continuation=runtime->host_functions[function].resume;
  GLWASM_Result result_code=resume
      ?(continuation?continuation(&call,type->nresults?result:&ignored):GLWASM_ERR_INVALID)
      :initial(&call,type->nresults?result:&ignored);
  cr=grcore_activation_leave(stack,activation);
  if(cr!=GRCORE_OK)return map_core_result(cr);
  /* A host callback that returns PAUSED is a resumable state-machine step.
   * Its C activation is closed here; execute() restores the Wasm call operands
   * and retries this callback when the context resumes. */
  if(result_code==GLWASM_ERR_PAUSED && !continuation)return GLWASM_ERR_INVALID;
  return result_code;
}

/* A callback's resumable return closes its native activation before yielding.
 * Turn that return into an ordinary runtime-core poll pause so the context and
 * saved Wasm frames share the usual paused-state invariants. */
static GRCORE_Verdict request_host_pause(GRCORE_Context *context,
    uint32_t function,uint64_t offset,int has_frame) {
  GRCORE_Port *port=NULL;
  if(grcore_context_port(context,&port)!=GRCORE_OK)
    return GRCORE_VERDICT_UNWIND;
  GRCORE_Result posted=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);
  grcore_port_release(port);
  if(posted!=GRCORE_OK)return GRCORE_VERDICT_UNWIND;
  return has_frame?grcore_stack_poll(context,function,offset):
      grcore_poll(context,(GRCORE_Location){"wasm",(int)offset});
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
static int table_reference_valid(const Run *r,uint32_t index,uint64_t token) {
  if(!token||index>=r->m->ntables)return 1;
  const GLWASM_ValType type=r->m->tables[index].elem;
  if(type.kind!=GLWASM_VK_REF||type.heap==GLWASM_HT_FUNC||type.heap==GLWASM_HT_NOFUNC)return 1;
  HostBinding *binding=resource_binding(r,GLWASM_HOST_IMPORT_TABLE,index);
  return binding&&binding->resource.table.validate_token&&
      binding->resource.table.validate_token(binding->resource.table.user,token)==GLWASM_OK;
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
      next > SIZE_MAX / page_size)
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
    if((uint64_t)(uintptr_t)exception==reference)return exception;
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
        size_t destination=H_OPERANDS+function->nparams+function->nlocals;
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
          slots_ptr[destination+caught_count]=(uint64_t)(uintptr_t)exception;
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

static GRCORE_Step raise_trap(GRCORE_Context *ctx,Run *r,uint32_t fi,
    uint32_t pc,GLWASM_RuntimeTrap trap){
  r->trap=trap;
  const GLWASM_Func *fn=&r->m->funcs[fi];
  GRCORE_Verdict v=grcore_stack_poll(ctx,fi,fn->translated->offsets[pc]);
  return v==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:GRCORE_STEP_FINISHED;
}

static GRCORE_Step finish_uncaught_exception(GRCORE_Context *ctx,Run *run,
    uint32_t function,uint32_t pc) {
  uint64_t offset=0;
  if(function<run->m->nfuncs&&run->m->funcs[function].translated&&
      pc<run->m->funcs[function].translated->offset_count)
    offset=run->m->funcs[function].translated->offsets[pc];
  GRCORE_Verdict verdict=grcore_poll(ctx,(GRCORE_Location){"wasm",
      offset<=INT_MAX?(int)offset:INT_MAX});
  return verdict==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:
      GRCORE_STEP_FINISHED;
}

static GRCORE_Step execute(GRCORE_Context *ctx, void *opaque) {
  Run *r=(Run *)opaque; GRCORE_Stack *s=grcore_context_stack(ctx); GRCORE_FrameRef f;
  if (grcore_stack_frame_count(s)<=r->stack_base) {
    GLWASM_Func *fn=&r->m->funcs[r->entry];
    if(fn->imported){
      uint64_t result=0;
      GLWASM_Result called=call_host(r->instance,r->entry,r->args,&result,
          r->pending_host&&r->pending_host_function==r->entry);
      if(called==GLWASM_ERR_PAUSED){
        r->pending_host=1;r->pending_host_function=r->entry;
        GRCORE_Verdict verdict=request_host_pause(ctx,r->entry,0,0);
        return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
            GRCORE_STEP_UNWOUND;
      }
      r->pending_host=0;
      if(called!=GLWASM_OK){r->error=called;return GRCORE_STEP_FINISHED;}
      r->result=result;return GRCORE_STEP_FINISHED;
    }
    if (grcore_stack_push(s,r->engine,frame_slots(fn),&f)!=GRCORE_OK) {r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;}
    uint64_t *p=slots(s,f); if(!p){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
    memset(p,0,frame_slots(fn)*sizeof(*p));
    p[H_FUNC]=r->entry;p[H_PC]=0;p[H_SP]=H_OPERANDS+fn->nparams+fn->nlocals;p[H_NLOCALS]=fn->nparams+fn->nlocals;p[H_CTRL_DEPTH]=0;
    for(uint32_t i=0;i<r->nargs && i<fn->nparams;i++)p[H_OPERANDS+i]=r->args[i];
  }
  for (;;) {
    f=grcore_stack_top(s); uint64_t *p=slots(s,f);
    uint32_t fi=(uint32_t)p[H_FUNC]; GLWASM_Func *fn=&r->m->funcs[fi];
    uint32_t pc=(uint32_t)p[H_PC], sp=(uint32_t)p[H_SP];
    if(pc>=fn->nins){
      uint32_t nres=r->m->types[fn->type_index].nresults;
      uint64_t value=nres?p[sp-1]:0;
      grcore_stack_pop(s);
      if(grcore_stack_frame_count(s)<=r->stack_base){r->result=value;return GRCORE_STEP_FINISHED;}
      f=grcore_stack_top(s);p=slots(s,f);sp=(uint32_t)p[H_SP];
      if(nres)p[sp++]=value;
      p[H_SP]=sp;continue;
    }
    GLWASM_TranslatedInstr *ti=&fn->translated->ins[pc]; GLWASM_Instr in=ti->source;
    int skip_charge=p[H_SKIP_CHARGE]!=0;p[H_SKIP_CHARGE]=0;
    p[H_PC]=pc+1; p[H_SP]=sp;
    if(!skip_charge&&glwasm_opcode_cost(in.op))grcore_context_charge_fuel(ctx,glwasm_opcode_cost(in.op));
    int backedge=pc>0 && fn->ins[pc-1u].op==0x03;
    if(in.op==0x03 || pc==0 || backedge) {
      GRCORE_Verdict v=grcore_stack_poll(ctx,fi,fn->translated->offsets[pc]);
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
        if(sp<H_OPERANDS+fn->nparams+fn->nlocals+type->nparams){
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
        return finish_uncaught_exception(ctx,r,fi,pc);
      }
      case 0x0a: {
        if(sp<=H_OPERANDS+fn->nparams+fn->nlocals){
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
        return finish_uncaught_exception(ctx,r,fi,pc);
      }
      case 0x0f: p[H_PC]=fn->nins; break;
      case 0x10: case 0x12: case 0x11: case 0x13: {
        uint32_t callee=in.a;uint64_t indirect_index=0;
        if(in.op==0x11||in.op==0x13){
          if(in.b>=r->m->ntables){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          if(in.a>=r->m->ntypes){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          size_t base=H_OPERANDS+fn->nparams+fn->nlocals;
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
        if(callee>=r->m->nfuncs){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        GLWASM_Func *cf=&r->m->funcs[callee];
        if(cf->type_index>=r->m->ntypes||r->m->types[cf->type_index].nresults>1){r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}
        uint32_t np=cf->nparams;
        if(sp < H_OPERANDS + fn->nparams + fn->nlocals + np){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        uint64_t *argv=np?gcu_allocator_calloc(&r->m->alloc,np,sizeof(*argv)):NULL;
        if(np&&!argv){r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
        for(uint32_t j=0;j<np;j++) { argv[np-1-j]=p[--sp]; }
        p[H_SP]=sp;
        if(cf->imported){
          uint64_t host_result=0;
          GLWASM_Result called=call_host(r->instance,callee,argv,&host_result,
              r->pending_host&&r->pending_host_function==callee);
          if(called==GLWASM_ERR_PAUSED){
            /* The callback owns any continuation state in its `user` value.
             * Put the exact operands back so retrying after resume repeats
             * this call once, without charging the opcode twice. */
            for(uint32_t j=0;j<np;j++)p[sp++]=argv[j];
            if(in.op==0x11||in.op==0x13)p[sp++]=indirect_index;
            p[H_PC]=pc;p[H_SP]=sp;p[H_SKIP_CHARGE]=1;
            r->pending_host=1;r->pending_host_function=callee;
            gcu_allocator_free(&r->m->alloc,argv);
            GRCORE_Verdict verdict=request_host_pause(ctx,fi,
                fn->translated->offsets[pc],1);
            return verdict==GRCORE_VERDICT_PAUSE?GRCORE_STEP_PAUSED:
                GRCORE_STEP_UNWOUND;
          }
          r->pending_host=0;
          gcu_allocator_free(&r->m->alloc,argv);
          if(called!=GLWASM_OK){r->error=called;return GRCORE_STEP_FINISHED;}
          if(r->m->types[cf->type_index].nresults)p[sp++]=host_result;
          break;
        }
        if(in.op==0x12||in.op==0x13)grcore_stack_pop(s);
        if(grcore_stack_push(s,r->engine,frame_slots(cf),&f)!=GRCORE_OK){gcu_allocator_free(&r->m->alloc,argv);r->error=GLWASM_ERR_LIMIT;return GRCORE_STEP_FINISHED;}
        p=slots(s,f);memset(p,0,frame_slots(cf)*sizeof(*p));
        p[H_FUNC]=callee;p[H_PC]=0;p[H_SP]=H_OPERANDS+cf->nparams+cf->nlocals;p[H_NLOCALS]=cf->nparams+cf->nlocals;p[H_CTRL_DEPTH]=0;
        for(uint32_t j=0;j<np;j++) { p[H_OPERANDS+j]=argv[j]; }
        gcu_allocator_free(&r->m->alloc,argv);
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
            GRCORE_Verdict verdict=grcore_stack_poll(ctx,fi,
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
            GRCORE_Verdict verdict=grcore_stack_poll(ctx,fi,fn->translated->offsets[pc]);
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
      case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97: case 0x98:{uint32_t ub=(uint32_t)p[--sp],ua=(uint32_t)p[--sp];float a,b,v;memcpy(&a,&ua,4);memcpy(&b,&ub,4);switch(in.op){case 0x92:v=a+b;break;case 0x93:v=a-b;break;case 0x94:v=a*b;break;case 0x95:v=a/b;break;case 0x96:v=(isnan(a)||isnan(b))?NAN:(a==0.0f&&b==0.0f?(signbit(a)||signbit(b)?-0.0f:0.0f):fminf(a,b));break;case 0x97:v=(isnan(a)||isnan(b))?NAN:(a==0.0f&&b==0.0f?(signbit(a)&&signbit(b)?-0.0f:0.0f):fmaxf(a,b));break;default:v=copysignf(a,b);break;}memcpy(&p[sp++],&v,4);break;}
      case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4: case 0xa5: case 0xa6:{uint64_t ub=p[--sp],ua=p[--sp];double a,b,v;memcpy(&a,&ua,8);memcpy(&b,&ub,8);switch(in.op){case 0xa0:v=a+b;break;case 0xa1:v=a-b;break;case 0xa2:v=a*b;break;case 0xa3:v=a/b;break;case 0xa4:v=(isnan(a)||isnan(b))?NAN:(a==0.0&&b==0.0?(signbit(a)||signbit(b)?-0.0:0.0):fmin(a,b));break;case 0xa5:v=(isnan(a)||isnan(b))?NAN:(a==0.0&&b==0.0?(signbit(a)&&signbit(b)?-0.0:0.0):fmax(a,b));break;default:v=copysign(a,b);break;}memcpy(&p[sp++],&v,8);break;}
      case 0x1a:if(sp>H_OPERANDS+fn->nparams+fn->nlocals)sp--;break;
      case 0x1b:{uint64_t c=p[--sp],b=p[--sp],a=p[--sp];p[sp++]=c?a:b;break;}
      case 0x0c: case 0x0d: case 0x0e:{uint32_t branch_depth=in.a;uint32_t branch_target=ti->branch_target;uint64_t cond=1;if(in.op==0x0d)cond=p[--sp];if(in.op==0x0e){uint32_t selector=(uint32_t)p[--sp];uint32_t choice=selector<in.b-1u?selector:in.b-1u;branch_depth=fn->u32s[in.a+choice];branch_target=fn->translated->branch_table_targets[ti->alternate_target+choice];}if(cond){uint32_t depth=branch_depth,cdepth=(uint32_t)p[H_CTRL_DEPTH];if(depth>=cdepth){p[H_CTRL_DEPTH]=0;p[H_PC]=fn->nins;}else{uint32_t ci=cdepth-1u-depth;size_t cb=control_base(fn)+2u*ci;uint32_t opener=(uint32_t)p[cb];uint32_t arity=branch_arity(fn,opener);uint32_t base=(uint32_t)p[cb+1];if(arity&&sp>=arity)memmove(&p[base],&p[sp-arity],(size_t)arity*sizeof(*p));sp=base+arity;p[H_CTRL_DEPTH]=fn->ins[opener].op==0x03?ci+1u:ci;p[H_PC]=branch_target;} } break;}
      default:
        if(in.op>=0x45 && in.op<=0x4f){uint32_t b=(uint32_t)p[--sp],a=0,v=0;if(in.op!=0x45)a=(uint32_t)p[--sp];switch(in.op){case 0x45:v=!b;break;case 0x46:v=a==b;break;case 0x47:v=a!=b;break;case 0x48:v=(int32_t)a<(int32_t)b;break;case 0x49:v=a<b;break;case 0x4a:v=(int32_t)a>(int32_t)b;break;case 0x4b:v=a>b;break;case 0x4c:v=(int32_t)a<=(int32_t)b;break;case 0x4d:v=a<=b;break;case 0x4e:v=(int32_t)a>=(int32_t)b;break;case 0x4f:v=a>=b;break;}p[sp++]=v;break;}
        if(in.op>=0x6a && in.op<=0x78){uint32_t b=(uint32_t)p[--sp],a=(uint32_t)p[--sp],v=0;switch(in.op){case 0x6a:v=a+b;break;case 0x6b:v=a-b;break;case 0x6c:v=a*b;break;case 0x71:v=a&b;break;case 0x72:v=a|b;break;case 0x73:v=a^b;break;case 0x74:v=a<<(b&31);break;case 0x75:v=arithmetic_shift_right32(a,b);break;case 0x76:v=a>>(b&31);break;case 0x6d:if(!b)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_DIVIDE_BY_ZERO);if(a==0x80000000u&&b==UINT32_MAX)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INTEGER_OVERFLOW);v=(uint32_t)((int32_t)a/(int32_t)b);break;case 0x6e:if(!b)return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_DIVIDE_BY_ZERO);v=a/b;break;default:r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}p[sp++]=v;break;}
        if(in.op>=0x7c && in.op<=0x8a){uint64_t b=p[--sp],a=p[--sp],v=0;switch(in.op){case 0x7c:v=a+b;break;case 0x7d:v=a-b;break;case 0x7e:v=a*b;break;case 0x83:v=a&b;break;case 0x84:v=a|b;break;case 0x85:v=a^b;break;default:r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}p[sp++]=v;break;}
        r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;
    }
    p[H_SP]=sp;
  }
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
          for(uint32_t j=0;j<m->nfuncs;j++)
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
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
          for(uint32_t j=0;j<m->nfuncs;j++)
            gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
          gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_OOM;
        }
        for(uint32_t j=0;j<type->nparams;j++)
          host_functions[import->index].parameter_types[j]=host_value_type(type->params[j]);
        host_functions[import->index].function=spectest_noop;
      }
      continue;
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
        for(uint32_t j=0;j<m->nfuncs;j++)gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
        gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_LINK;}
      resource->resource_kind=expected;resource->resource_index=import->index;
      continue;
    }
    if(!binding||binding->kind!=GLWASM_HOST_IMPORT_FUNCTION||!binding->function||import->index>=m->nfuncs){
      if(link_error){link_error->module=import->module_name.bytes;
        link_error->field=import->field.bytes;
        link_error->reason=binding?GLWASM_LINK_INVALID_BINDING:GLWASM_LINK_UNRESOLVED;}
      for(uint32_t j=0;j<m->nfuncs;j++)
        gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_LINK;
    }
    GLWASM_Func *func=&m->funcs[import->index];
    if(func->type_index>=m->ntypes||m->types[func->type_index].nresults>1){
      if(link_error){link_error->module=import->module_name.bytes;
        link_error->field=import->field.bytes;
        link_error->reason=GLWASM_LINK_INVALID_BINDING;}
      for(uint32_t j=0;j<m->nfuncs;j++)
        gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_LINK;
    }
    GLWASM_TypeDef *type=&m->types[func->type_index];
    host_functions[import->index].parameter_types=type->nparams?
        gcu_allocator_calloc(&m->alloc,type->nparams,
            sizeof(*host_functions[import->index].parameter_types)):NULL;
    if(type->nparams&&!host_functions[import->index].parameter_types){
      for(uint32_t j=0;j<m->nfuncs;j++)
        gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);
      gcu_allocator_free(&m->alloc,host_functions);
      return GLWASM_ERR_OOM;
    }
    for(uint32_t j=0;j<type->nparams;j++){
      GLWASM_HostValueType mapped=host_value_type(type->params[j]);
      if(!mapped){
        if(link_error){link_error->module=import->module_name.bytes;
          link_error->field=import->field.bytes;
          link_error->reason=GLWASM_LINK_INVALID_BINDING;}
        for(uint32_t k=0;k<m->nfuncs;k++)
          gcu_allocator_free(&m->alloc,host_functions[k].parameter_types);
        gcu_allocator_free(&m->alloc,host_functions);
        return GLWASM_ERR_LINK;
      }
      host_functions[import->index].parameter_types[j]=mapped;
    }
    host_functions[import->index].function=binding->function;
    host_functions[import->index].resume=binding->resume;
    host_functions[import->index].user=binding->user;
  }
  GLWASM_Runtime*r=gcu_allocator_calloc(&m->alloc,1,sizeof(*r));
  if(!r){for(uint32_t j=0;j<m->nfuncs;j++)gcu_allocator_free(&m->alloc,host_functions[j].parameter_types);gcu_allocator_free(&m->alloc,host_functions);return GLWASM_ERR_OOM;}
  r->module=m;
  r->boundary_token=atomic_fetch_add_explicit(&next_boundary_token,1,
      memory_order_relaxed);
  if(!r->boundary_token)r->boundary_token=atomic_fetch_add_explicit(
      &next_boundary_token,1,memory_order_relaxed);
  r->host_functions=host_functions;
  r->host_resources=host_resources;
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
  GRCORE_EngineId engine=0;
  GRCORE_Result result=grcore_engine_register(context,&descriptor,&engine);
  if(result!=GRCORE_OK)return map_core_result(result);
  runtime->boundary_context=context;runtime->boundary_engine=engine;
  *out_wasm_engine=engine;
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

static GLWASM_Result finish_nested(GLWASM_Runtime *runtime,GRCORE_Result core,
    GRCORE_Outcome outcome,uint64_t *result) {
  if(core==GRCORE_OK&&outcome==GRCORE_OUTCOME_PAUSED){
    runtime->run.paused=1;
    return GLWASM_ERR_PAUSED;
  }
  GRCORE_Stack *stack=grcore_context_stack(runtime->nested_context);
  while(grcore_stack_frame_count(stack)>runtime->nested_base)
    (void)grcore_stack_pop(stack);
  GRCORE_Result leave=grcore_boundary_leave(stack,runtime->nested_boundary);
  Run *run=&runtime->run;
  GLWASM_Result output=run->error!=GLWASM_OK?run->error:
      run->trap!=GLWASM_TRAP_NONE?GLWASM_ERR_GUEST:
      core!=GRCORE_OK?map_core_result(core):
      leave!=GRCORE_OK?map_core_result(leave):GLWASM_OK;
  if(output==GLWASM_OK)*result=run->result;
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
GLWASM_Result glwasm_runtime_invoke_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    const char *export_name,const uint64_t *arguments,uint32_t argument_count,
    uint64_t *result){
  if(!runtime||!context||!caller||!instance_token||!export_name||!result||
      (argument_count&&!arguments)||runtime->run.active||runtime->nested_active)
    return GLWASM_ERR_INVALID;
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
      runtime->module->types[fn->type_index].nparams!=argument_count||
      runtime->module->types[fn->type_index].nresults>1)
    return GLWASM_ERR_INVALID;
  GRCORE_EngineId wasm_engine=0;
  GRCORE_Result core=GRCORE_OK;
  if(runtime->boundary_context==context&&runtime->boundary_engine){
    wasm_engine=runtime->boundary_engine;
  }else{
    core=grcore_engine_register(context,&descriptor,&wasm_engine);
    if(core!=GRCORE_OK)return map_core_result(core);
    runtime->boundary_context=context;runtime->boundary_engine=wasm_engine;
  }
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
        slots_ptr[H_SP]=H_OPERANDS+fn->nparams+fn->nlocals;
        slots_ptr[H_NLOCALS]=fn->nparams+fn->nlocals;
        slots_ptr[H_CTRL_DEPTH]=0;
        for(uint32_t i=0;i<argument_count;i++)slots_ptr[H_OPERANDS+i]=arguments[i];}
    }
  }
  if(core!=GRCORE_OK){runtime->run.error=map_core_result(core);
    return finish_nested(runtime,core,GRCORE_OUTCOME_FINISHED,result);}
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
  return finish_nested(runtime,core,outcome,result);
}

GLWASM_Result glwasm_runtime_resume_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context,GRCORE_EngineId caller,uintptr_t instance_token,
    uint64_t *result){
  if(!runtime||!context||!result||!runtime->nested_active||
      runtime->nested_context!=context||runtime->nested_caller!=caller||
      runtime->nested_instance!=instance_token)return GLWASM_ERR_INVALID;
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
  return finish_nested(runtime,core,outcome,result);
}
void glwasm_runtime_destroy(GLWASM_Runtime *r) {
  if (!r) return;
  free_exceptions(r);
  if (r->memories && r->guest_alloc)
    for (uint32_t i = 0; i < r->module->nmems; i++)
      gcu_allocator_free(r->guest_alloc, r->memories[i]);
  if (r->tables && r->guest_alloc)
    for (uint32_t i = 0; i < r->module->ntables; i++)
      gcu_allocator_free(r->guest_alloc, r->tables[i]);
  if(r->host_functions)
    for(uint32_t i=0;i<r->module->nfuncs;i++)
      gcu_allocator_free(&r->module->alloc,r->host_functions[i].parameter_types);
  if (r->context) grcore_context_destroy(r->context);
  if (r->group) grcore_group_destroy(r->group);
  if (r->options) grcore_options_destroy(r->options);
  gcu_allocator_free(&r->module->alloc, r->tables);
  gcu_allocator_free(&r->module->alloc, r->host_functions);
  gcu_allocator_free(&r->module->alloc, r->table_sizes);
  gcu_allocator_free(&r->module->alloc, r->memories);
  gcu_allocator_free(&r->module->alloc, r->memory_bytes);
  gcu_allocator_free(&r->module->alloc, r->memory_pages);
  gcu_allocator_free(&r->module->alloc, r->data_dropped);
  gcu_allocator_free(&r->module->alloc, r->elements_dropped);
  gcu_allocator_free(&r->module->alloc, r->globals);
  gcu_allocator_free(&r->module->alloc, r->run.args);
  gcu_allocator_free(&r->module->alloc, r);
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
  if (r->context) grcore_context_destroy(r->context);
  if (r->options) grcore_options_destroy(r->options);
  r->context = context;
  r->options = options;
  r->engine = engine;
  r->guest_alloc = new_alloc;
  r->run.guest_alloc = new_alloc;
  r->run.ctx = context;
  r->run.engine = engine;
  return GLWASM_OK;
}
static void discard_guest_frames(GLWASM_Runtime*r){GRCORE_Stack*s=grcore_context_stack(r->context);while(grcore_stack_frame_count(s))(void)grcore_stack_pop(s);}
static void free_run_args(GLWASM_Runtime*r){if(r->run.args){gcu_allocator_free(&r->module->alloc,r->run.args);r->run.args=NULL;}}
static GLWASM_Result finish_run(GLWASM_Runtime*r,GRCORE_Result cr,GRCORE_Outcome outcome,uint64_t*out,int*paused){if(paused)*paused=outcome==GRCORE_OUTCOME_PAUSED;r->run.paused=outcome==GRCORE_OUTCOME_PAUSED;if(r->run.paused)return GLWASM_ERR_PAUSED;if(r->run.error!=GLWASM_OK){discard_guest_frames(r);r->run.active=0;r->run.bulk_active=0;free_run_args(r);return r->run.error;}if(r->run.trap!=GLWASM_TRAP_NONE){discard_guest_frames(r);r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}if(cr==GRCORE_ERR_GUEST){discard_guest_frames(r);r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_GUEST;}if(cr==GRCORE_ERR_LIMIT){discard_guest_frames(r);r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_LIMIT;}if(cr!=GRCORE_OK){discard_guest_frames(r);r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_ERR_INTERNAL;}if(out)*out=r->run.result;r->run.active=0;r->run.bulk_active=0;free_run_args(r);return GLWASM_OK;}

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
  }
}

GLWASM_Result glwasm_runtime_snapshot(GLWASM_Runtime*r,GLWASM_Snapshot**out){
  if(!r||!out||!r->run.active||!r->run.paused||r->run.pending_host||
      !snapshot_module_supported(r->module))return GLWASM_ERR_UNSUPPORTED;
  GLWASM_Module*m=r->module;GLWASM_Snapshot*s=gcu_allocator_calloc(&m->alloc,1,sizeof(*s));
  if(!s)return GLWASM_ERR_OOM;
  s->module=m;
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
  s->nargs=r->run.nargs;s->entry=r->run.entry;s->result=r->run.result;s->bulk_func=r->run.bulk_func;s->bulk_pc=r->run.bulk_pc;s->bulk_op=r->run.bulk_op;
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
  for(uint32_t i=0;i<m->nmems;i++){gcu_allocator_free(a,r->memories[i]);r->memories[i]=mems[i];mems[i]=NULL;r->memory_bytes[i]=mb[i];r->memory_pages[i]=mp[i];}
  for(uint32_t i=0;i<m->ntables;i++){gcu_allocator_free(a,r->tables[i]);r->tables[i]=tabs[i];tabs[i]=NULL;r->table_sizes[i]=ts[i];}
  if(m->nglobals)memcpy(r->globals,globals,m->nglobals*sizeof(*globals));
  if(m->ndatas)memcpy(r->data_dropped,dd,m->ndatas);
  if(m->nelems)memcpy(r->elements_dropped,ed,m->nelems);
  r->run.args=args;args=NULL;r->run.nargs=s->nargs;r->run.entry=s->entry;r->run.result=s->result;r->run.bulk_func=s->bulk_func;r->run.bulk_pc=s->bulk_pc;r->run.bulk_op=s->bulk_op;
  r->run.bulk_dst=s->bulk_dst;r->run.bulk_src=s->bulk_src;r->run.bulk_len=s->bulk_len;r->run.bulk_done=s->bulk_done;r->run.bulk_value=s->bulk_value;r->run.bulk_active=s->bulk_active;r->run.active=1;r->run.paused=1;r->run.pending_host=0;r->run.error=GLWASM_OK;r->run.trap=GLWASM_TRAP_NONE;
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
GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime*r,uint32_t fi,const uint64_t*args,uint32_t nargs,uint64_t*out){if(!r||fi>=r->module->nfuncs||!out||(nargs&&!args)||r->run.active)return GLWASM_ERR_INVALID;GLWASM_Func*fn=&r->module->funcs[fi];if(fn->type_index>=r->module->ntypes||nargs!=r->module->types[fn->type_index].nparams)return GLWASM_ERR_INVALID;if(r->module->types[fn->type_index].nresults>1)return GLWASM_ERR_UNSUPPORTED;Run*state=&r->run;state->args=nargs?gcu_allocator_calloc(&r->module->alloc,nargs,sizeof(*state->args)):NULL;if(nargs&&!state->args)return GLWASM_ERR_OOM;state->entry=fi;state->nargs=nargs;state->stack_base=0;state->result=0;state->error=GLWASM_OK;state->trap=GLWASM_TRAP_NONE;state->paused=0;state->active=1;for(uint32_t i=0;i<nargs;i++)state->args[i]=args[i];GRCORE_Outcome outcome;GRCORE_Result cr=grcore_run(r->context,execute,state,&outcome);return finish_run(r,cr,outcome,out,NULL);}
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime*r,uint64_t*out,int*paused){if(!r||!out||!r->run.active||!r->run.paused)return GLWASM_ERR_INVALID;GRCORE_Outcome outcome;GRCORE_Result cr=grcore_resume(r->context,&outcome);return finish_run(r,cr,outcome,out,paused);}
GLWASM_Result glwasm_runtime_pause_position(const GLWASM_Runtime*r,
    uint32_t*function,uint64_t*offset){
  if(!r||!function||!offset||!r->run.paused)return GLWASM_ERR_INVALID;
  GRCORE_PollIdentity identity;
  GRCORE_Result result=grcore_context_poll_identity(r->context,&identity);
  if(result!=GRCORE_OK||identity.function>UINT32_MAX)return GLWASM_ERR_INVALID;
  *function=(uint32_t)identity.function;*offset=identity.offset;return GLWASM_OK;
}
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime*r){if(!r||r->run.active)return GLWASM_ERR_INVALID;GRCORE_Port*port=NULL;if(grcore_context_port(r->context,&port)!=GRCORE_OK)return GLWASM_ERR_OOM;GRCORE_Result cr=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);grcore_port_release(port);return cr==GRCORE_OK?GLWASM_OK:GLWASM_ERR_INTERNAL;}
GLWASM_RuntimeTrap glwasm_runtime_last_trap(const GLWASM_Runtime*r){return r?r->run.trap:GLWASM_TRAP_NONE;}
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime*r){return r&&r->context?grcore_context_fuel_used(r->context):0;}
