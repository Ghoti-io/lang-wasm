/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/cutil/allocator.h>
#include "runtime.h"
#include <math.h>
#include <limits.h>
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
static GLWASM_Result map_core_result(GRCORE_Result result){switch(result){case GRCORE_OK:return GLWASM_OK;case GRCORE_ERR_UNSUPPORTED:return GLWASM_ERR_UNSUPPORTED;case GRCORE_ERR_LIMIT:return GLWASM_ERR_LIMIT;case GRCORE_ERR_OOM:return GLWASM_ERR_OOM;case GRCORE_ERR_INVALID:return GLWASM_ERR_INVALID;case GRCORE_ERR_GUEST:return GLWASM_ERR_GUEST;case GRCORE_ERR_INTERNAL:default:return GLWASM_ERR_INTERNAL;}}

static GRCORE_Location locate(const GRCORE_Context *c, uint64_t f, uint64_t o) {
  (void)c; (void)f;
  return (GRCORE_Location){"wasm", o <= INT_MAX ? (int)o : INT_MAX};
}
static const GRCORE_EngineDescriptor descriptor = GRCORE_ENGINE_DESCRIPTOR_INIT(
    "lang-wasm", NULL, locate, NULL, {NULL,NULL,NULL}, {0,0,0}, NULL,NULL,NULL,NULL);

typedef struct Run {
  GLWASM_Module *m; GRCORE_Context *ctx; GRCORE_EngineId engine;
  uint64_t *globals; uint32_t **tables; uint64_t *table_sizes;
  uint32_t entry; uint64_t *args; uint32_t nargs; uint64_t result;
  GLWASM_Result error; GLWASM_RuntimeTrap trap; int active; int paused;
} Run;

struct GLWASM_Runtime {
  GLWASM_Module *module; uint64_t *globals; uint32_t **tables;
  uint64_t *table_sizes; GRCORE_Group *group; GRCORE_Options *options;
  GRCORE_Context *context; GRCORE_EngineId engine; Run run;
};

static uint64_t * slots(GRCORE_Stack *s, GRCORE_FrameRef f) { return grcore_stack_slots(s,f); }

static void trap_poll(GRCORE_Context *context, void *value, GRCORE_PollCall *call) {
  (void)context;
  Run *r=(Run *)value;
  if(r && r->trap!=GLWASM_TRAP_NONE){
    (void)grcore_pollcall_set_unwind_result(call,GRCORE_ERR_GUEST);
    (void)grcore_pollcall_vote(call,GRCORE_VERDICT_UNWIND);
  }
}
static const GRCORE_Key trap_key=GRCORE_KEY_INIT("wasm-trap",
  GRCORE_CARDINALITY_ONE,GRCORE_PHASE_DECIDE,NULL,trap_poll,NULL,NULL,NULL);
static GRCORE_Step raise_trap(GRCORE_Context *ctx,Run *r,uint32_t fi,
    uint32_t pc,GLWASM_RuntimeTrap trap){
  r->trap=trap;
  const GLWASM_Func *fn=&r->m->funcs[fi];
  GRCORE_Verdict v=grcore_stack_poll(ctx,fi,fn->translated->offsets[pc]);
  return v==GRCORE_VERDICT_UNWIND?GRCORE_STEP_UNWOUND:GRCORE_STEP_FINISHED;
}

static GRCORE_Step execute(GRCORE_Context *ctx, void *opaque) {
  Run *r=(Run *)opaque; GRCORE_Stack *s=grcore_context_stack(ctx); GRCORE_FrameRef f;
  if (!grcore_stack_frame_count(s)) {
    GLWASM_Func *fn=&r->m->funcs[r->entry];
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
      if(!grcore_stack_frame_count(s)){r->result=value;return GRCORE_STEP_FINISHED;}
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
      case 0x02: case 0x03: case 0x04:{
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
      case 0x0f: p[H_PC]=fn->nins; break;
      case 0x10: case 0x12: case 0x11: case 0x13: {
        uint32_t callee=in.a;
        if(in.op==0x11||in.op==0x13){
          if(in.b>=r->m->ntables||r->m->tables[in.b].imported){r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}
          if(in.a>=r->m->ntypes){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          size_t base=H_OPERANDS+fn->nparams+fn->nlocals;
          size_t required=base+r->m->types[in.a].nparams+1u;
          if((size_t)sp<required){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
          uint32_t ti_index=(uint32_t)p[--sp];
          if(in.b>=r->m->ntables||ti_index>=r->table_sizes[in.b])return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_TABLE_OUT_OF_BOUNDS);
          if(!r->tables[in.b][ti_index])return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_UNINITIALIZED_ELEMENT);
          callee=r->tables[in.b][ti_index]-1u;
          if(callee>=r->m->nfuncs||r->m->funcs[callee].type_index>=r->m->ntypes||in.a>=r->m->ntypes||!same_function_type(&r->m->types[r->m->funcs[callee].type_index],&r->m->types[in.a]))return raise_trap(ctx,r,fi,pc,GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH);
        }
        if(callee>=r->m->nfuncs){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        GLWASM_Func *cf=&r->m->funcs[callee]; if(cf->imported){r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}
        if(cf->type_index>=r->m->ntypes||r->m->types[cf->type_index].nresults>1){r->error=GLWASM_ERR_UNSUPPORTED;return GRCORE_STEP_FINISHED;}
        uint32_t np=cf->nparams;
        if(sp < H_OPERANDS + fn->nparams + fn->nlocals + np){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}
        uint64_t *argv=np?gcu_allocator_calloc(&r->m->alloc,np,sizeof(*argv)):NULL;
        if(np&&!argv){r->error=GLWASM_ERR_OOM;return GRCORE_STEP_FINISHED;}
        for(uint32_t j=0;j<np;j++) { argv[np-1-j]=p[--sp]; }
        p[H_SP]=sp;
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
      case 0x23:if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}p[sp++]=r->globals[in.a];break;
      case 0x24:if(in.a>=r->m->nglobals){r->error=GLWASM_ERR_INTERNAL;return GRCORE_STEP_FINISHED;}r->globals[in.a]=p[--sp];break;
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

GLWASM_Result glwasm_runtime_create(GLWASM_Module*m,GLWASM_Runtime**out){
  if(!out)return GLWASM_ERR_INVALID;
  *out=NULL;
  if(!m)return GLWASM_ERR_INVALID;
  GLWASM_Runtime*r=gcu_allocator_calloc(&m->alloc,1,sizeof(*r));
  if(!r)return GLWASM_ERR_OOM;
  r->module=m;
  if(m->nglobals){
    r->globals=gcu_allocator_calloc(&m->alloc,m->nglobals,sizeof(*r->globals));
    if(!r->globals){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    for(uint32_t i=0;i<m->nglobals;i++){
      GLWASM_Global*gl=&m->globals[i];
      if(gl->imported){
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
  if(m->ntables){
    r->tables=gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*r->tables));
    r->table_sizes=gcu_allocator_calloc(&m->alloc,m->ntables,sizeof(*r->table_sizes));
    if(!r->tables||!r->table_sizes){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    for(uint32_t i=0;i<m->ntables;i++){
      if(m->tables[i].min>SIZE_MAX/sizeof(uint32_t)){glwasm_runtime_destroy(r);return GLWASM_ERR_LIMIT;}
      r->table_sizes[i]=m->tables[i].min;
      r->tables[i]=gcu_allocator_calloc(&m->alloc,(size_t)m->tables[i].min,sizeof(uint32_t));
      if(!r->tables[i]){glwasm_runtime_destroy(r);return GLWASM_ERR_OOM;}
    }
    for(uint32_t i=0;i<m->nelems;i++){
      GLWASM_Elem*e=&m->elems[i];
      if(e->mode!=GLWASM_ELEM_ACTIVE||e->table>=m->ntables||!e->offset.nins)continue;
      uint64_t offset=0;if(!const_value(m,&e->offset,r->globals,&offset)){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
      const uint32_t count=e->use_exprs?e->nexprs:e->nfuncs;
      if(offset>r->table_sizes[e->table]||count>r->table_sizes[e->table]-offset){glwasm_runtime_destroy(r);return GLWASM_ERR_VALIDATE;}
      for(uint32_t j=0;j<count;j++){
        uint32_t ref=0;
        if(e->use_exprs){
          const GLWASM_Func *expr=&e->exprs[j];
          int recognized=0;
          for(uint32_t k=0;k<expr->nins;k++){
            if(expr->ins[k].op==0xd2){if(recognized){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}ref=expr->ins[k].a+1u;recognized=1;}
            else if(expr->ins[k].op==0xd0){if(recognized){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}ref=0;recognized=1;}
            else if(expr->ins[k].op==0x0b){if(k+1u!=expr->nins){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}}
            else {glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
          }
          if(!recognized){glwasm_runtime_destroy(r);return GLWASM_ERR_UNSUPPORTED;}
        }else ref=e->funcs[j]+1u;
        r->tables[e->table][offset+j]=ref;
      }
    }
  }
  GRCORE_Result core_result=grcore_group_create(NULL,NULL,&r->group);
  if(core_result==GRCORE_OK)core_result=grcore_options_create(NULL,&r->options);
  if(core_result==GRCORE_OK)core_result=grcore_context_create(r->group,r->options,&r->context);
  if(core_result==GRCORE_OK)core_result=grcore_engine_register(r->context,&descriptor,&r->engine);
  if(core_result!=GRCORE_OK){GLWASM_Result result=map_core_result(core_result);glwasm_runtime_destroy(r);return result;}
  r->run=(Run){.m=m,.ctx=r->context,.engine=r->engine,.globals=r->globals,
    .tables=r->tables,.table_sizes=r->table_sizes};
  core_result=grcore_context_register(r->context,&trap_key,&r->run);
  if(core_result!=GRCORE_OK){GLWASM_Result result=map_core_result(core_result);glwasm_runtime_destroy(r);return result;}
  if(m->has_start){uint64_t ignored=0;GLWASM_Result started=glwasm_runtime_invoke(r,m->start,NULL,0,&ignored);if(started!=GLWASM_OK){glwasm_runtime_destroy(r);return started;}}
  *out=r;return GLWASM_OK;
}
void glwasm_runtime_destroy(GLWASM_Runtime*r){if(r){if(r->context)grcore_context_destroy(r->context);if(r->group)grcore_group_destroy(r->group);if(r->options)grcore_options_destroy(r->options);if(r->tables)for(uint32_t i=0;i<r->module->ntables;i++)gcu_allocator_free(&r->module->alloc,r->tables[i]);gcu_allocator_free(&r->module->alloc,r->tables);gcu_allocator_free(&r->module->alloc,r->table_sizes);gcu_allocator_free(&r->module->alloc,r->globals);gcu_allocator_free(&r->module->alloc,r->run.args);gcu_allocator_free(&r->module->alloc,r);}}
GLWASM_Result glwasm_runtime_set_test_limits(GLWASM_Runtime*r,uint64_t fuel,uint64_t depth,int always_move){if(!r||r->run.active)return GLWASM_ERR_INVALID;GRCORE_Options*options=NULL;GRCORE_Context*context=NULL;GRCORE_EngineId engine=0;GRCORE_Result cr=grcore_options_create(NULL,&options);if(cr==GRCORE_OK)cr=grcore_options_set_fuel(options,fuel);if(cr==GRCORE_OK)cr=grcore_options_set_guest_depth(options,depth);if(cr==GRCORE_OK)cr=grcore_context_create(r->group,options,&context);if(cr==GRCORE_OK)cr=grcore_engine_register(context,&descriptor,&engine);if(cr==GRCORE_OK)cr=grcore_context_register(context,&trap_key,&r->run);if(cr==GRCORE_OK)cr=grcore_stack_set_always_move(grcore_context_stack(context),always_move!=0);if(cr!=GRCORE_OK){if(context)grcore_context_destroy(context);if(options)grcore_options_destroy(options);return map_core_result(cr);}if(r->context)grcore_context_destroy(r->context);if(r->options)grcore_options_destroy(r->options);r->context=context;r->options=options;r->engine=engine;r->run.ctx=context;r->run.engine=engine;return GLWASM_OK;}
static void discard_guest_frames(GLWASM_Runtime*r){GRCORE_Stack*s=grcore_context_stack(r->context);while(grcore_stack_frame_count(s))(void)grcore_stack_pop(s);}
static void free_run_args(GLWASM_Runtime*r){if(r->run.args){gcu_allocator_free(&r->module->alloc,r->run.args);r->run.args=NULL;}}
static GLWASM_Result finish_run(GLWASM_Runtime*r,GRCORE_Result cr,GRCORE_Outcome outcome,uint64_t*out,int*paused){if(paused)*paused=outcome==GRCORE_OUTCOME_PAUSED;r->run.paused=outcome==GRCORE_OUTCOME_PAUSED;if(r->run.paused)return GLWASM_ERR_PAUSED;if(r->run.error!=GLWASM_OK){discard_guest_frames(r);r->run.active=0;free_run_args(r);return r->run.error;}if(r->run.trap!=GLWASM_TRAP_NONE){discard_guest_frames(r);r->run.active=0;free_run_args(r);return GLWASM_ERR_GUEST;}if(cr==GRCORE_ERR_GUEST){discard_guest_frames(r);r->run.active=0;free_run_args(r);return GLWASM_ERR_GUEST;}if(cr==GRCORE_ERR_LIMIT){discard_guest_frames(r);r->run.active=0;free_run_args(r);return GLWASM_ERR_LIMIT;}if(cr!=GRCORE_OK){discard_guest_frames(r);r->run.active=0;free_run_args(r);return GLWASM_ERR_INTERNAL;}if(out)*out=r->run.result;r->run.active=0;free_run_args(r);return GLWASM_OK;}
GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime*r,uint32_t fi,const uint64_t*args,uint32_t nargs,uint64_t*out){if(!r||fi>=r->module->nfuncs||!out||(nargs&&!args)||r->run.active)return GLWASM_ERR_INVALID;GLWASM_Func*fn=&r->module->funcs[fi];if(fn->imported)return GLWASM_ERR_UNSUPPORTED;if(fn->type_index>=r->module->ntypes||nargs!=r->module->types[fn->type_index].nparams)return GLWASM_ERR_INVALID;if(r->module->types[fn->type_index].nresults>1)return GLWASM_ERR_UNSUPPORTED;Run*state=&r->run;state->args=nargs?gcu_allocator_calloc(&r->module->alloc,nargs,sizeof(*state->args)):NULL;if(nargs&&!state->args)return GLWASM_ERR_OOM;state->entry=fi;state->nargs=nargs;state->result=0;state->error=GLWASM_OK;state->trap=GLWASM_TRAP_NONE;state->paused=0;state->active=1;for(uint32_t i=0;i<nargs;i++)state->args[i]=args[i];GRCORE_Outcome outcome;GRCORE_Result cr=grcore_run(r->context,execute,state,&outcome);return finish_run(r,cr,outcome,out,NULL);}
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime*r,uint64_t*out,int*paused){if(!r||!out||!r->run.active||!r->run.paused)return GLWASM_ERR_INVALID;GRCORE_Outcome outcome;GRCORE_Result cr=grcore_resume(r->context,&outcome);return finish_run(r,cr,outcome,out,paused);}
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime*r){if(!r||r->run.active)return GLWASM_ERR_INVALID;GRCORE_Port*port=NULL;if(grcore_context_port(r->context,&port)!=GRCORE_OK)return GLWASM_ERR_OOM;GRCORE_Result cr=grcore_port_post(port,GRCORE_REQUEST_INTERRUPT);grcore_port_release(port);return cr==GRCORE_OK?GLWASM_OK:GLWASM_ERR_INTERNAL;}
GLWASM_RuntimeTrap glwasm_runtime_last_trap(const GLWASM_Runtime*r){return r?r->run.trap:GLWASM_TRAP_NONE;}
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime*r){return r&&r->context?grcore_context_fuel_used(r->context):0;}
