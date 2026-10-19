/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */

#include <ghoti.io/lang-wasm/lang-wasm.h>
#include <ghoti.io/runtime-debug/dap.h>
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <ghoti.io/runtime-debug/transport.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/options.h>
#ifdef GLWASM_DEBUG_RELOCATE
#include <ghoti.io/runtime-heap/relocate.h>
#endif
#include <ghoti.io/runtime-heap/roots.h>

#include "module_internal.h"
#include "runtime/runtime.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <cstdlib>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::string dap_request(unsigned seq,const char *command,
    const std::string &arguments) {
  std::string body="{\"seq\":"+std::to_string(seq)+
      ",\"type\":\"request\",\"command\":\""+command+
      "\",\"arguments\":"+arguments+"}";
  return "Content-Length: "+std::to_string(body.size())+"\r\n\r\n"+body;
}

struct WasmSectionSpan {
  uint8_t id;
  size_t begin;
  size_t payload;
  size_t end;
};

bool wasm_u32(const std::vector<uint8_t> &bytes,size_t *cursor,uint32_t *value) {
  uint32_t result=0;
  for(unsigned shift=0;shift<35;shift+=7){
    if(*cursor>=bytes.size())return false;
    const uint8_t byte=bytes[(*cursor)++];
    if(shift==28&&(byte&0xf0u))return false;
    result|=(uint32_t)(byte&0x7fu)<<shift;
    if(!(byte&0x80u)){*value=result;return true;}
  }
  return false;
}

bool wasm_sections(const std::vector<uint8_t> &bytes,
    std::vector<WasmSectionSpan> *sections) {
  static const uint8_t header[]={0,0x61,0x73,0x6d,1,0,0,0};
  if(bytes.size()<sizeof(header)||
      std::memcmp(bytes.data(),header,sizeof(header))!=0)return false;
  sections->clear();
  size_t cursor=sizeof(header);
  while(cursor<bytes.size()){
    const size_t begin=cursor;
    const uint8_t id=bytes[cursor++];
    uint32_t payload_size=0;
    if(!wasm_u32(bytes,&cursor,&payload_size)||
        payload_size>bytes.size()-cursor)return false;
    const size_t payload=cursor;
    cursor+=(size_t)payload_size;
    sections->push_back({id,begin,payload,cursor});
  }
  return cursor==bytes.size();
}

size_t wasm_section_begin(const std::vector<uint8_t> &bytes,uint8_t id) {
  std::vector<WasmSectionSpan> sections;
  if(!wasm_sections(bytes,&sections))return SIZE_MAX;
  for(const WasmSectionSpan &section:sections)
    if(section.id==id)return section.begin;
  return SIZE_MAX;
}

std::vector<uint8_t> debug_module_bytes() {
  const uint8_t bytes[]={
    0x00,0x61,0x73,0x6d,0x01,0x00,0x00,0x00,
    0x01,0x07,0x01,0x60,0x02,0x7f,0x7f,0x01,0x7f,
    0x03,0x02,0x01,0x00,
    0x0a,0x09,0x01,0x07,0x00,0x20,0x00,0x20,0x01,0x6a,0x0b,
    0x00,0x23,0x04,0x6e,0x61,0x6d,0x65,
    0x00,0x05,0x04,0x64,0x65,0x6d,0x6f,
    0x01,0x06,0x01,0x00,0x03,0x73,0x75,0x6d,
    0x02,0x0d,0x01,0x00,0x02,0x00,0x03,0x6c,0x68,0x73,0x01,0x03,0x72,0x68,0x73
  };
  std::vector<uint8_t> module(std::begin(bytes),std::end(bytes));
  const uint8_t memories_globals[]={
    0x05,0x03,0x01,0x00,0x01,
    0x06,0x06,0x01,0x7f,0x00,0x41,0x07,0x0b
  };
  const size_t code_begin=wasm_section_begin(module,10);
  if(code_begin==SIZE_MAX)return {};
  module.insert(module.begin()+(std::ptrdiff_t)code_begin,
      std::begin(memories_globals),
      std::end(memories_globals));
  const uint8_t data[]={0x0b,0x09,0x01,0x00,0x41,0x00,0x0b,0x03,0x61,0x62,0x63};
  const size_t custom_begin=wasm_section_begin(module,0);
  if(custom_begin==SIZE_MAX)return {};
  module.insert(module.begin()+(std::ptrdiff_t)custom_begin,
      std::begin(data),std::end(data));
  return module;
}

struct DebugPollSnapshot {
  GRDBG_FrameIdentity identity{};
  std::vector<uint64_t> frame_functions;
  std::vector<uint64_t> frame_offsets;
  std::vector<GRCORE_EngineId> frame_engines;
  std::vector<std::string> frame_names;
  std::string function_name;
  std::vector<std::string> locals;
  std::vector<std::string> operands;
  std::vector<std::string> globals;
  std::vector<std::vector<uint8_t>> memories;
};

bool copy_scope(GRDBG_Debugger *debugger,size_t frame_index,size_t scope_index,
    std::vector<std::string> *out) {
  GRDBG_Scope scope{};
  if(grdbg_debugger_scope(debugger,frame_index,scope_index,&scope)!=GRDBG_OK)
    return false;
  out->clear();
  for(size_t i=0;i<scope.variable_count;i++){
    GRDBG_Variable variable{};
    if(grdbg_debugger_variable(debugger,frame_index,scope_index,i,&variable)!=
        GRDBG_OK||!variable.name||!variable.text)
      return false;
    out->push_back(std::string(variable.name)+"="+variable.text);
  }
  return true;
}

std::vector<std::string> dap_response_bodies(const std::string &transcript) {
  std::vector<std::string> bodies;
  size_t cursor=0;
  while(cursor<transcript.size()){
    const size_t separator=transcript.find("\r\n\r\n",cursor);
    if(separator==std::string::npos)break;
    const size_t header=transcript.find("Content-Length: ",cursor);
    if(header==std::string::npos||header>=separator)break;
    const size_t digits=header+std::strlen("Content-Length: ");
    const size_t digits_end=transcript.find("\r\n",digits);
    if(digits_end==std::string::npos||digits_end>separator)break;
    const size_t length=static_cast<size_t>(std::strtoull(
        transcript.substr(digits,digits_end-digits).c_str(),nullptr,10));
    const size_t body_start=separator+4;
    if(length>transcript.size()-body_start)break;
    bodies.push_back(transcript.substr(body_start,length));
    cursor=body_start+length;
  }
  return bodies;
}

std::string dap_json_scalar(const std::string &body,const char *key) {
  const std::string needle="\""+std::string(key)+"\"";
  size_t cursor=body.find(needle);
  if(cursor==std::string::npos)return {};
  cursor=body.find(':',cursor+needle.size());
  if(cursor==std::string::npos)return {};
  do{++cursor;}while(cursor<body.size()&&
      (body[cursor]==' '||body[cursor]=='\t'||body[cursor]=='\r'||
       body[cursor]=='\n'));
  if(cursor==body.size())return {};
  if(body[cursor]=='"'){
    const size_t end=body.find('"',cursor+1);
    return end==std::string::npos?std::string():
        body.substr(cursor+1,end-cursor-1);
  }
  size_t end=cursor;
  while(end<body.size()&&body[end]!=','&&body[end]!='}')++end;
  while(end>cursor&&(body[end-1]==' '||body[end-1]=='\t'||
      body[end-1]=='\r'||body[end-1]=='\n'))--end;
  return body.substr(cursor,end-cursor);
}

const std::string *dap_response_for(const std::vector<std::string> &bodies,
    unsigned request_seq) {
  const std::string wanted=std::to_string(request_seq);
  for(const std::string &body:bodies)
    if(dap_json_scalar(body,"request_seq")==wanted)return &body;
  return nullptr;
}

struct DebugMemoryProviderState {
  std::vector<uint8_t> bytes;
  unsigned leases=0;
  unsigned begins=0;
  unsigned ends=0;
  bool fail_begin=false;
};

GLWASM_Result debug_memory_begin(void *opaque,GLWASM_HostMemoryView *view) {
  auto *state=static_cast<DebugMemoryProviderState *>(opaque);
  if(!state||!view||state->leases)return GLWASM_ERR_INTERNAL;
  ++state->begins;
  if(state->fail_begin)return GLWASM_ERR_INTERNAL;
  ++state->leases;
  view->bytes=state->bytes.data();
  view->length_bytes=state->bytes.size();
  return GLWASM_OK;
}

void debug_memory_end(void *opaque,GLWASM_HostMemoryView *) {
  auto *state=static_cast<DebugMemoryProviderState *>(opaque);
  if(!state||!state->leases)return;
  --state->leases;
  ++state->ends;
}

GLWASM_Result debug_memory_grow(void *,uint64_t,uint64_t *) {
  return GLWASM_ERR_UNSUPPORTED;
}

GRCORE_EngineId wasm_engine_id(GRCORE_Context *context) {
  for(GRCORE_EngineId id=1;id<=grcore_engine_count(context);id++){
    const GRCORE_EngineDescriptor *descriptor=grcore_engine_descriptor(context,
        id);
    if(descriptor&&descriptor->name&&
        std::strcmp(descriptor->name,"lang-wasm")==0)
      return id;
  }
  return 0;
}

bool run_debug_poll_sequence(GLWASM_Module *module,GLWASM_TestJitMode mode,
    std::vector<DebugPollSnapshot> *snapshots,uint64_t *result,
    uint32_t entry_function=0,bool all_function_breakpoints=false) {
  GLWASM_Runtime *runtime=nullptr;
  if(glwasm_runtime_create(module,&runtime)!=GLWASM_OK||!runtime){
    if(runtime)glwasm_runtime_destroy(runtime);
    return false;
  }
  bool rooted=false;
  GRHEAP_Heap *heap=nullptr;
  void *probe=nullptr;
  auto finish=[&](){
    if(rooted)(void)grheap_root_remove(heap,&probe);
    glwasm_runtime_destroy(runtime);
  };
  if(glwasm_runtime_test_set_jit_mode(runtime,mode)!=GLWASM_OK){
    finish();
    return false;
  }
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  if(!context){finish();return false;}
  if(!wasm_engine_id(context)){finish();return false;}

  /* A rooted probe makes every poll exercise the context's registered heap
   * root while the guest stack is forced to move.  The test never retains a
   * pointer into a guest frame across a resume. */
  GRHEAP_Options *options=nullptr;
#ifdef GLWASM_DEBUG_RELOCATE
  const bool relocate=grheap_relocation_available();
#endif
  if(grheap_options_create(nullptr,&options)!=GRHEAP_OK||!options||
      grheap_options_set_torture(options,true)!=GRHEAP_OK||
#ifdef GLWASM_DEBUG_RELOCATE
      (relocate&&grheap_options_set_relocate(options,true)!=GRHEAP_OK)||
#endif
      grheap_heap_create(context,options,&heap)!=GRHEAP_OK||!heap){
    grheap_options_destroy(options);
    finish();
    return false;
  }
  grheap_options_destroy(options);
  static const GRHEAP_Type probe_type=GRHEAP_TYPE_INIT(
      "wasm-debug-probe",sizeof(uint64_t),nullptr,nullptr,nullptr,nullptr,
      nullptr);
  if(grheap_alloc(heap,&probe_type,&probe)!=GRHEAP_OK||!probe){
    finish();
    return false;
  }
  *static_cast<uint64_t *>(probe)=UINT64_C(0x7761736d646267);
  if(grheap_root_add(heap,&probe)!=GRHEAP_OK){
    finish();
    return false;
  }
  rooted=true;

  GRCORE_Stack *stack=grcore_context_stack(context);
  if(!stack||grcore_stack_set_always_move(stack,true)!=GRCORE_OK){
    finish();
    return false;
  }
  GRDBG_Debugger *debugger=nullptr;
  if(grdbg_debugger_attach(context,nullptr,&debugger)!=GRDBG_OK||!debugger){
    finish();
    return false;
  }
  GLWASM_TranslatedFunc *translated=module->funcs[0].translated;
  if(!translated||translated->offset_count!=translated->nins+1u){
    finish();
    return false;
  }
  std::vector<GRCORE_PollIdentity> identities;
  size_t expected_polls=0;
  const uint32_t target_functions=all_function_breakpoints?module->nfuncs:1u;
  for(uint32_t f=0;f<target_functions;f++){
    GLWASM_TranslatedFunc *target=module->funcs[f].translated;
    if(!target||target->offset_count!=target->nins+1u){finish();return false;}
    expected_polls+=target->nins;
    for(uint32_t i=0;i<target->nins;i++)
      identities.push_back({((uint64_t)module->debug_id<<32)|f,
          target->offsets[i],wasm_engine_id(context)});
  }
  std::vector<uint64_t> breakpoint_ids(identities.size());
  if(grdbg_debugger_set_instruction_breakpoints(debugger,"demo",
      identities.data(),identities.size(),breakpoint_ids.data())!=GRDBG_OK){
    finish();
    return false;
  }

  const uint64_t stack_moves_before=grcore_stack_move_count(stack);
  uint64_t arguments[]={20,22};
  GLWASM_Result status=glwasm_runtime_invoke(runtime,entry_function,arguments,
      2,result);
#ifdef GLWASM_DEBUG_RELOCATE
  unsigned probe_moves=0;
#endif
  while(status==GLWASM_ERR_PAUSED){
#ifdef GLWASM_DEBUG_RELOCATE
    void *probe_before=probe;
#endif
    if(grheap_collect(heap)!=GRHEAP_OK||!probe||
        *static_cast<uint64_t *>(probe)!=UINT64_C(0x7761736d646267))
      break;
#ifdef GLWASM_DEBUG_RELOCATE
    if(probe!=probe_before)++probe_moves;
#endif
    GRDBG_FrameIdentity identity=GRDBG_FRAME_IDENTITY_INIT;
    if(grdbg_debugger_frame_identity(debugger,0,&identity)!=GRDBG_OK)
      break;
    DebugPollSnapshot snapshot{};
    snapshot.identity=identity;
    if(identity.function_name)snapshot.function_name=identity.function_name;
    snapshot.identity.function_name=nullptr;
    size_t total_frames=0,available_frames=0;
    if(grdbg_debugger_frames(debugger,&total_frames,&available_frames)!=
        GRDBG_OK||available_frames>total_frames)
      break;
    for(size_t frame_index=0;frame_index<available_frames;frame_index++){
      GRDBG_FrameIdentity frame_identity=GRDBG_FRAME_IDENTITY_INIT;
      if(grdbg_debugger_frame_identity(debugger,frame_index,&frame_identity)!=
          GRDBG_OK)break;
      snapshot.frame_functions.push_back(frame_identity.function);
      snapshot.frame_offsets.push_back(frame_identity.offset);
      snapshot.frame_engines.push_back(frame_identity.engine);
      snapshot.frame_names.emplace_back(frame_identity.function_name?
          frame_identity.function_name:"");
    }
    if(snapshot.frame_functions.size()!=available_frames)break;
    if(!copy_scope(debugger,0,0,&snapshot.locals)||
        !copy_scope(debugger,0,1,&snapshot.operands)||
        !copy_scope(debugger,0,2,&snapshot.globals))
      break;
    size_t memory_count=0;
    if(grdbg_debugger_memory_count(debugger,0,&memory_count)!=GRDBG_OK)
      break;
    bool memory_ok=true;
    for(size_t i=0;i<memory_count;i++){
      GRDBG_Memory memory{};
      if(grdbg_debugger_memory(debugger,0,i,&memory)!=GRDBG_OK||
          memory.size_bytes>SIZE_MAX){
        memory_ok=false;
        break;
      }
      snapshot.memories.emplace_back((size_t)memory.size_bytes);
      if(grdbg_debugger_read_memory(debugger,0,i,0,
          snapshot.memories.back().data(),snapshot.memories.back().size())!=
          GRDBG_OK){
        memory_ok=false;
        break;
      }
    }
    if(!memory_ok)break;
    snapshots->push_back(std::move(snapshot));
    if(grdbg_debugger_continue(debugger)!=GRDBG_OK)break;
    int paused=0;
    status=glwasm_runtime_resume(runtime,result,&paused);
    if(status==GLWASM_OK&&paused)status=GLWASM_ERR_INTERNAL;
  }
  const bool complete=status==GLWASM_OK&&
      snapshots->size()==expected_polls&&
      (!all_function_breakpoints||
          grcore_stack_move_count(stack)>stack_moves_before)&&
      probe&&
      *static_cast<uint64_t *>(probe)==UINT64_C(0x7761736d646267)&&
#ifdef GLWASM_DEBUG_RELOCATE
      (!relocate||probe_moves>0);
#else
      true;
#endif
  finish();
  return complete;
}

}  // namespace

TEST(WasmDebug, StopsAtNamedInstructionAndInspectsLocalsAndOperandStack) {
  std::vector<uint8_t> bytes=debug_module_bytes();
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode(bytes.data(),bytes.size(),nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRCORE_Stack *guest_stack=grcore_context_stack(context);
  ASSERT_NE(guest_stack,nullptr);
  ASSERT_EQ(grcore_stack_set_always_move(guest_stack,true),GRCORE_OK);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);

  const GRCORE_PollIdentity identity[]={
    {((uint64_t)module->debug_id<<32),module->funcs[0].translated->offsets[1],
        engine}};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"demo",
      identity,1,&breakpoint),GRDBG_OK);
  uint64_t arguments[]={20,22},result=0;
  ASSERT_EQ(glwasm_runtime_invoke(runtime,0,arguments,2,&result),
      GLWASM_ERR_PAUSED);

  GRDBG_Stop stop{};
  ASSERT_EQ(grdbg_debugger_stopped(debugger,&stop),GRDBG_OK);
  EXPECT_EQ(stop.reason,GRDBG_STOP_BREAKPOINT);
  GRDBG_Frame frame{};
  ASSERT_EQ(grdbg_debugger_frame(debugger,0,&frame),GRDBG_OK);
  EXPECT_STREQ(frame.file,"demo");
  EXPECT_EQ(frame.line,0);
  GRDBG_FrameIdentity frame_info=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&frame_info),GRDBG_OK);
  EXPECT_STREQ(frame_info.function_name,"sum");
  EXPECT_EQ(frame_info.function,((uint64_t)module->debug_id<<32));
  EXPECT_EQ(frame_info.offset,module->funcs[0].translated->offsets[1]);

  size_t scope_count=0;
  ASSERT_EQ(grdbg_debugger_scope_count(debugger,0,&scope_count),GRDBG_OK);
  ASSERT_EQ(scope_count,3u);
  GRDBG_Scope locals{},stack{};
  ASSERT_EQ(grdbg_debugger_scope(debugger,0,0,&locals),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_scope(debugger,0,1,&stack),GRDBG_OK);
  GRDBG_Scope globals{};
  ASSERT_EQ(grdbg_debugger_scope(debugger,0,2,&globals),GRDBG_OK);
  EXPECT_EQ(locals.variable_count,2u);
  EXPECT_EQ(stack.variable_count,1u);
  EXPECT_EQ(globals.variable_count,1u);
  GRDBG_Variable local{},operand{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,0,0,&local),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,1,0,&operand),GRDBG_OK);
  EXPECT_STREQ(local.name,"lhs");
  EXPECT_STREQ(operand.name,"stack[0]");
  GRDBG_Variable global{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,0,&global),GRDBG_OK);
  EXPECT_STREQ(global.name,"global[0]");

  size_t memories=0;
  ASSERT_EQ(grdbg_debugger_memory_count(debugger,0,&memories),GRDBG_OK);
  ASSERT_EQ(memories,1u);
  GRDBG_Memory memory{};
  ASSERT_EQ(grdbg_debugger_memory(debugger,0,0,&memory),GRDBG_OK);
  EXPECT_STREQ(memory.name,"memory[0]");
  EXPECT_EQ(memory.size_bytes,65536u);
  uint8_t contents[3]={0,0,0};
  ASSERT_EQ(grdbg_debugger_read_memory(debugger,0,0,0,contents,
      sizeof(contents)),GRDBG_OK);
  EXPECT_EQ(std::memcmp(contents,"abc",3),0);
  uint8_t untouched=0xa5;
  EXPECT_EQ(grdbg_debugger_read_memory(debugger,0,0,memory.size_bytes,&untouched,
      1),GRDBG_ERR_INVALID);
  EXPECT_EQ(untouched,0xa5);

  std::string dap_input;
  dap_input+=dap_request(1,"stackTrace","{\"threadId\":1}");
  GRDBG_FrameIdentity dap_identity=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&dap_identity),GRDBG_OK);
  const std::string canonical_reference="grdbg:"+
      std::to_string(dap_identity.engine)+":"+
      std::to_string(dap_identity.function)+":"+
      std::to_string(dap_identity.offset);
  dap_input+=dap_request(2,"setInstructionBreakpoints",
      "{\"breakpoints\":[{\"instructionReference\":\""+
      canonical_reference+"\"}]}");
  dap_input+=dap_request(3,"setInstructionBreakpoints",
      "{\"breakpoints\":[{\"instructionReference\":\"grdbg:"+
      std::to_string(dap_identity.function)+":"+
      std::to_string(dap_identity.offset)+"\"}]}");
  dap_input+=dap_request(4,"scopes","{\"frameId\":1}");
  dap_input+=dap_request(5,"variables","{\"variablesReference\":4}");
  dap_input+=dap_request(6,"readMemory",
      "{\"memoryReference\":\"grdbg-memory:1:0\",\"offset\":0,\"count\":3}");
  dap_input+=dap_request(7,"readMemory",
      "{\"memoryReference\":\"grdbg-memory:1:0\",\"offset\":65534,\"count\":3,\"allowPartial\":true}");
  dap_input+=dap_request(8,"readMemory",
      "{\"memoryReference\":\"grdbg-memory:1:0\",\"offset\":0,\"count\":700}");
  dap_input+=dap_request(9,"disconnect","{\"terminateDebuggee\":false}");
  GRDBG_Transport *transport=nullptr;
  ASSERT_EQ(grdbg_transport_create_memory(dap_input.data(),dap_input.size(),
      nullptr,&transport),GRDBG_OK);
  GRDBG_Dap *dap=nullptr;
  GRDBG_Limits dap_limits{};
  grdbg_limits_default(&dap_limits);
  dap_limits.max_message_bytes=1024;
  ASSERT_EQ(grdbg_dap_create(debugger,transport,&dap_limits,&dap),GRDBG_OK);
  GRDBG_ServeResult serve=GRDBG_SERVE_RESUME;
  ASSERT_EQ(grdbg_dap_serve(dap,&serve),GRDBG_OK);
  EXPECT_EQ(serve,GRDBG_SERVE_DETACH);
  const uint8_t *dap_output=nullptr;size_t dap_output_size=0;
  ASSERT_EQ(grdbg_transport_memory_output(transport,&dap_output,
      &dap_output_size),GRDBG_OK);
  const std::string transcript(reinterpret_cast<const char *>(dap_output),
      dap_output_size);
  const std::vector<std::string> responses=dap_response_bodies(transcript);
  const char *expected_commands[]={"stackTrace","setInstructionBreakpoints",
      "setInstructionBreakpoints","scopes","variables","readMemory"};
  for(unsigned seq=1;seq<=6;seq++){
    const std::string *response=dap_response_for(responses,seq);
    ASSERT_NE(response,nullptr);
    EXPECT_EQ(dap_json_scalar(*response,"command"),expected_commands[seq-1]);
    EXPECT_EQ(dap_json_scalar(*response,"success"),seq==3?"false":"true");
  }
  const std::string *canonical_response=dap_response_for(responses,2);
  ASSERT_NE(canonical_response,nullptr);
  EXPECT_EQ(dap_json_scalar(*canonical_response,"instructionReference"),
      canonical_reference);
  const std::string *legacy_response=dap_response_for(responses,3);
  ASSERT_NE(legacy_response,nullptr);
  EXPECT_NE(legacy_response->find(
      "instructionReference must include grdbg engine, function and offset"),
      std::string::npos);
  EXPECT_NE(transcript.find(canonical_reference),std::string::npos);
  const std::string *partial_response=dap_response_for(responses,7);
  ASSERT_NE(partial_response,nullptr);
  EXPECT_EQ(dap_json_scalar(*partial_response,"command"),"readMemory");
  EXPECT_EQ(dap_json_scalar(*partial_response,"success"),"true");
  EXPECT_EQ(dap_json_scalar(*partial_response,"unreadableBytes"),"1");
  const std::string *oversized_response=dap_response_for(responses,8);
  ASSERT_NE(oversized_response,nullptr);
  EXPECT_EQ(dap_json_scalar(*oversized_response,"command"),"readMemory");
  EXPECT_EQ(dap_json_scalar(*oversized_response,"success"),"false");
  EXPECT_NE(oversized_response->find(
      "memory read exceeds the encoded response limit"),std::string::npos);
  const std::string *disconnect_response=dap_response_for(responses,9);
  ASSERT_NE(disconnect_response,nullptr);
  EXPECT_EQ(dap_json_scalar(*disconnect_response,"command"),"disconnect");
  EXPECT_EQ(dap_json_scalar(*disconnect_response,"success"),"true");
  EXPECT_NE(transcript.find("\"name\":\"sum\""),std::string::npos);
  EXPECT_NE(transcript.find("grdbg-memory:1:0"),std::string::npos);
  EXPECT_NE(transcript.find("YWJj"),std::string::npos);
  grdbg_dap_destroy(dap);
  grdbg_transport_destroy(transport);

  ASSERT_EQ(grdbg_debugger_step(debugger,GRDBG_STEP_INSTRUCTION),GRDBG_OK);
  int paused=0;
  ASSERT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(paused);
  ASSERT_EQ(grdbg_debugger_frame(debugger,0,&frame),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&frame_info),GRDBG_OK);
  EXPECT_EQ(frame_info.offset,module->funcs[0].translated->offsets[2]);

  ASSERT_EQ(grdbg_debugger_continue(debugger),GRDBG_OK);
  ASSERT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_EQ(result,42u);

  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, DebugFixtureSectionsRemainWellFormed) {
  const std::vector<uint8_t> bytes=debug_module_bytes();
  std::vector<WasmSectionSpan> sections;
  ASSERT_TRUE(wasm_sections(bytes,&sections));
  const uint8_t expected_ids[]={1,3,5,6,10,11,0};
  ASSERT_EQ(sections.size(),std::size(expected_ids));
  for(size_t i=0;i<sections.size();i++)EXPECT_EQ(sections[i].id,expected_ids[i]);
  const WasmSectionSpan &name=sections.back();
  ASSERT_GE(name.end-name.payload,5u);
  ASSERT_EQ(bytes[name.payload],4u);
  EXPECT_EQ(std::memcmp(bytes.data()+name.payload+1,"name",4),0);
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode(bytes.data(),bytes.size(),nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  ASSERT_EQ(module->module_name.length,4u);
  EXPECT_EQ(std::memcmp(module->module_name.bytes,"demo",4),0);
  ASSERT_EQ(module->function_names_count,1u);
  ASSERT_EQ(module->function_names[0].length,3u);
  EXPECT_EQ(std::memcmp(module->function_names[0].bytes,"sum",3),0);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, UnnamedModuleShowsImportedAndDefinedGlobalsAndEveryMemory) {
  const char source[]="(module (import \"host.v1\" \"g\" (global $imported (mut i32)))"
      " (global $defined (mut i32) (i32.const 9)) (memory 1) (memory 2)"
      " (func $run (export \"run\") (result i32) (global.get $imported)))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  uint64_t imported_value=17;
  GLWASM_HostGlobal global{GLWASM_HOST_I32,1,&imported_value,nullptr,nullptr};
  GLWASM_HostImport import{};
  import.module="host.v1";
  import.field="g";
  import.kind=GLWASM_HOST_IMPORT_GLOBAL;
  import.resource.global=&global;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,&import,1,0,nullptr,
      &runtime),GLWASM_OK);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);
  uint32_t function=0;
  while(function<module->nexports&&
      (module->exports[function].kind!=GLWASM_EXT_FUNC||
       module->exports[function].name.length!=3||
       std::memcmp(module->exports[function].name.bytes,"run",3)!=0))
    ++function;
  ASSERT_LT(function,module->nexports);
  const uint32_t function_index=module->exports[function].index;
  const GRCORE_PollIdentity identity[]={{
      (uint64_t)module->debug_id<<32,
      module->funcs[function_index].translated->offsets[0],engine}};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"wasm",
      identity,1,&breakpoint),GRDBG_OK);
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),
      GLWASM_ERR_PAUSED);
  GRDBG_Frame frame{};
  ASSERT_EQ(grdbg_debugger_frame(debugger,0,&frame),GRDBG_OK);
  EXPECT_STREQ(frame.file,"wasm");
  EXPECT_EQ(frame.line,0);
  const std::string fallback="function["+
      std::to_string(function_index)+"]";
  GRDBG_FrameIdentity frame_identity=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&frame_identity),GRDBG_OK);
  EXPECT_STREQ(frame_identity.function_name,fallback.c_str());
  size_t scopes=0;
  ASSERT_EQ(grdbg_debugger_scope_count(debugger,0,&scopes),GRDBG_OK);
  ASSERT_EQ(scopes,3u);
  GRDBG_Scope globals{};
  ASSERT_EQ(grdbg_debugger_scope(debugger,0,2,&globals),GRDBG_OK);
  ASSERT_EQ(globals.variable_count,2u);
  GRDBG_Variable imported{},defined{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,0,&imported),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,1,&defined),GRDBG_OK);
  EXPECT_STREQ(imported.name,"global[0]");
  EXPECT_STREQ(defined.name,"global[1]");
  const std::string imported_before=imported.text;
  imported_value=29;
  GRDBG_Variable imported_after{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,0,&imported_after),GRDBG_OK);
  EXPECT_STREQ(imported_after.name,"global[0]");
  EXPECT_NE(imported_after.text,imported_before);
  EXPECT_EQ(imported_value,29u);
  size_t memory_count=0;
  ASSERT_EQ(grdbg_debugger_memory_count(debugger,0,&memory_count),GRDBG_OK);
  ASSERT_EQ(memory_count,2u);
  GRDBG_Memory memory0{},memory1{};
  ASSERT_EQ(grdbg_debugger_memory(debugger,0,0,&memory0),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_memory(debugger,0,1,&memory1),GRDBG_OK);
  EXPECT_STREQ(memory0.name,"memory[0]");
  EXPECT_STREQ(memory1.name,"memory[1]");
  EXPECT_EQ(memory0.size_bytes,65536u);
  EXPECT_EQ(memory1.size_bytes,131072u);
  ASSERT_EQ(grdbg_debugger_continue(debugger),GRDBG_OK);
  int paused=0;
  ASSERT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_FALSE(paused);
  EXPECT_EQ(result,29u);
  uint8_t byte=0;
  EXPECT_EQ(grdbg_debugger_read_memory(debugger,0,0,0,&byte,1),
      GRDBG_ERR_INVALID);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, CompiledFramesMatchInterpreterAtInstructionPolls) {
  std::vector<uint8_t> bytes=debug_module_bytes();
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode(bytes.data(),bytes.size(),nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  ASSERT_EQ(module->nfuncs,1u);
  ASSERT_NE(module->funcs[0].translated,nullptr);

  std::vector<DebugPollSnapshot> interpreted,compiled;
  uint64_t interpreter_result=0,compiled_result=0;
  const bool interpreter_complete=run_debug_poll_sequence(module,
      GLWASM_TEST_JIT_INTERPRETER,&interpreted,&interpreter_result);
  ASSERT_TRUE(interpreter_complete);
  const bool compiled_complete=run_debug_poll_sequence(module,
      GLWASM_TEST_JIT_REQUIRE,&compiled,&compiled_result);
  ASSERT_TRUE(compiled_complete);
  EXPECT_EQ(interpreter_result,42u);
  EXPECT_EQ(compiled_result,interpreter_result);
  ASSERT_EQ(compiled.size(),interpreted.size());
  for(size_t i=0;i<interpreted.size();i++){
    EXPECT_EQ(interpreted[i].identity.function,compiled[i].identity.function);
    EXPECT_EQ(interpreted[i].identity.offset,compiled[i].identity.offset);
    EXPECT_NE(interpreted[i].identity.engine,0u);
    EXPECT_NE(compiled[i].identity.engine,0u);
    EXPECT_EQ(interpreted[i].identity.offset,
        module->funcs[0].translated->offsets[i]);
    EXPECT_EQ(interpreted[i].function_name,compiled[i].function_name);
    EXPECT_EQ(interpreted[i].locals,compiled[i].locals);
    EXPECT_EQ(interpreted[i].operands,compiled[i].operands);
    EXPECT_EQ(interpreted[i].globals,compiled[i].globals);
    EXPECT_EQ(interpreted[i].memories,compiled[i].memories);
  }
  glwasm_module_destroy(module);
}

TEST(WasmDebug, NestedCompiledFrameIdentitiesMatchAtEveryInstructionPoll) {
  const char source[]="(module"
      " (func $callee (param i32 i32) (result i32)"
      "   local.get 0 local.get 1 i32.add)"
      " (func $caller (param i32 i32) (result i32)"
      "   local.get 0 local.get 1 call $callee))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  ASSERT_EQ(module->nfuncs,2u);

  std::vector<DebugPollSnapshot> interpreted,compiled;
  uint64_t interpreter_result=0,compiled_result=0;
  ASSERT_TRUE(run_debug_poll_sequence(module,GLWASM_TEST_JIT_INTERPRETER,
      &interpreted,&interpreter_result,1,true));
  ASSERT_TRUE(run_debug_poll_sequence(module,GLWASM_TEST_JIT_REQUIRE,
      &compiled,&compiled_result,1,true));
  EXPECT_EQ(interpreter_result,42u);
  EXPECT_EQ(compiled_result,interpreter_result);
  ASSERT_EQ(interpreted.size(),compiled.size());
  bool observed_nested_frame=false;
  for(size_t poll=0;poll<interpreted.size();poll++){
    const DebugPollSnapshot &expected=interpreted[poll];
    const DebugPollSnapshot &actual=compiled[poll];
    ASSERT_EQ(expected.frame_functions.size(),actual.frame_functions.size());
    ASSERT_EQ(expected.frame_offsets.size(),actual.frame_offsets.size());
    ASSERT_EQ(expected.frame_engines.size(),actual.frame_engines.size());
    ASSERT_EQ(expected.frame_names.size(),actual.frame_names.size());
    observed_nested_frame|=expected.frame_functions.size()>=2;
    for(size_t frame=0;frame<expected.frame_functions.size();frame++){
      EXPECT_EQ(expected.frame_functions[frame],actual.frame_functions[frame]);
      EXPECT_EQ(expected.frame_offsets[frame],actual.frame_offsets[frame]);
      EXPECT_NE(expected.frame_engines[frame],0u);
      EXPECT_NE(actual.frame_engines[frame],0u);
      EXPECT_EQ(expected.frame_names[frame],actual.frame_names[frame]);
    }
  }
  EXPECT_TRUE(observed_nested_frame);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, ImportedMutableGlobalInspectionObservesHostMutation) {
  const char source[]="(module (import \"host.v1\" \"g\""
      " (global $g (mut i32)))"
      " (func (export \"read\") (result i32) global.get $g))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  uint64_t host_value=17;
  GLWASM_HostGlobal global{GLWASM_HOST_I32,1,&host_value,nullptr,nullptr};
  GLWASM_HostImport import{};
  import.module="host.v1";import.field="g";
  import.kind=GLWASM_HOST_IMPORT_GLOBAL;import.resource.global=&global;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,&import,1,0,nullptr,
      &runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_PollIdentity identity={
      (uint64_t)module->debug_id<<32,
      module->funcs[0].translated->offsets[0],wasm_engine_id(context)};
  ASSERT_NE(identity.engine,0u);
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"wasm",
      &identity,1,&breakpoint),GRDBG_OK);
  uint64_t result=0;
  ASSERT_EQ(glwasm_runtime_invoke_export(runtime,"read",nullptr,0,&result),
      GLWASM_ERR_PAUSED);
  GRDBG_Variable before{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,0,&before),GRDBG_OK);
  const std::string old_value=before.text;
  host_value=29;
  GRDBG_Variable after{};
  ASSERT_EQ(grdbg_debugger_variable(debugger,0,2,0,&after),GRDBG_OK);
  EXPECT_NE(after.text,old_value);
  ASSERT_EQ(grdbg_debugger_continue(debugger),GRDBG_OK);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_EQ(result,29u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, TerminalOffsetIsNotAnExecutableInstructionBreakpoint) {
  std::vector<uint8_t> bytes=debug_module_bytes();
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode(bytes.data(),bytes.size(),nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);
  GLWASM_TranslatedFunc *translated=module->funcs[0].translated;
  ASSERT_NE(translated,nullptr);
  ASSERT_EQ(translated->offset_count,translated->nins+1u);
  const GRCORE_PollIdentity terminal={
      (uint64_t)module->debug_id<<32,translated->offsets[translated->nins],
      engine};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"demo",
      &terminal,1,&breakpoint),GRDBG_OK);
  const uint64_t arguments[]={20,22};
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime,0,arguments,2,&result),GLWASM_OK);
  EXPECT_EQ(result,42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, EqualOffsetsInDifferentFunctionsDoNotAlias) {
  const char source[]="(module"
      " (func (export \"first\") (result i32) (i32.const 1))"
      " (func (export \"second\") (result i32) (i32.const 2)))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  ASSERT_EQ(module->nfuncs,2u);
  ASSERT_NE(module->funcs[0].translated,nullptr);
  ASSERT_NE(module->funcs[1].translated,nullptr);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);
  const uint64_t function0=(uint64_t)module->debug_id<<32;
  const uint64_t function1=function0|1u;
  const uint64_t shared_offset=module->funcs[1].translated->offsets[0];
  EXPECT_EQ(grcore_engine_check_instruction_for(context,engine,function1,
      shared_offset),GRCORE_INSTRUCTION_VALID);
  EXPECT_EQ(grcore_engine_check_instruction_for(context,engine,function0,
      shared_offset),GRCORE_INSTRUCTION_INVALID);
  const GRCORE_PollIdentity breakpoint_identity={function0,shared_offset,
      engine};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"wasm",
      &breakpoint_identity,1,&breakpoint),GRDBG_OK);
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime,1,nullptr,0,&result),GLWASM_OK);
  EXPECT_EQ(result,2u);
  EXPECT_EQ(function0|1u,function1);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, DapRejectsMemoryReferenceFromThePreviousStop) {
  std::vector<uint8_t> bytes=debug_module_bytes();
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_module_decode(bytes.data(),bytes.size(),nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);
  GLWASM_TranslatedFunc *translated=module->funcs[0].translated;
  ASSERT_NE(translated,nullptr);
  ASSERT_GT(translated->nins,1u);
  const uint64_t function=(uint64_t)module->debug_id<<32;
  const GRCORE_PollIdentity first={function,translated->offsets[0],engine};
  const GRCORE_PollIdentity second={function,translated->offsets[1],engine};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"demo",
      &first,1,&breakpoint),GRDBG_OK);
  std::string input;
  input+=dap_request(1,"scopes","{\"frameId\":1}");
  input+=dap_request(2,"variables","{\"variablesReference\":4}");
  const std::string next_instruction="grdbg:"+std::to_string(engine)+":"+
      std::to_string(second.function)+":"+std::to_string(second.offset);
  input+=dap_request(3,"setInstructionBreakpoints",
      "{\"breakpoints\":[{\"instructionReference\":\""+
      next_instruction+"\"}]}");
  input+=dap_request(4,"continue","{}");
  input+=dap_request(5,"readMemory",
      "{\"memoryReference\":\"grdbg-memory:1:0\",\"offset\":0,\"count\":1}");
  input+=dap_request(6,"disconnect","{\"terminateDebuggee\":false}");
  GRDBG_Transport *transport=nullptr;
  ASSERT_EQ(grdbg_transport_create_memory(input.data(),input.size(),nullptr,
      &transport),GRDBG_OK);
  GRDBG_Dap *dap=nullptr;
  ASSERT_EQ(grdbg_dap_create(debugger,transport,nullptr,&dap),GRDBG_OK);
  uint64_t arguments[]={20,22},result=0;
  EXPECT_EQ(glwasm_runtime_invoke(runtime,0,arguments,2,&result),
      GLWASM_ERR_PAUSED);
  GRDBG_ServeResult serve=GRDBG_SERVE_DETACH;
  ASSERT_EQ(grdbg_dap_serve(dap,&serve),GRDBG_OK);
  ASSERT_EQ(serve,GRDBG_SERVE_RESUME);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(paused);
  ASSERT_EQ(grdbg_dap_serve(dap,&serve),GRDBG_OK);
  EXPECT_EQ(serve,GRDBG_SERVE_DETACH);
  const uint8_t *output=nullptr;
  size_t output_size=0;
  ASSERT_EQ(grdbg_transport_memory_output(transport,&output,&output_size),
      GRDBG_OK);
  const std::string transcript(reinterpret_cast<const char *>(output),
      output_size);
  const auto responses=dap_response_bodies(transcript);
  const std::string *memory_variables=dap_response_for(responses,2);
  ASSERT_NE(memory_variables,nullptr);
  EXPECT_NE(memory_variables->find("grdbg-memory:1:0"),std::string::npos);
  const std::string *stale=dap_response_for(responses,5);
  ASSERT_NE(stale,nullptr);
  EXPECT_EQ(dap_json_scalar(*stale,"command"),"readMemory");
  EXPECT_EQ(dap_json_scalar(*stale,"success"),"false");
  EXPECT_NE(stale->find("memoryReference is invalid or stale"),
      std::string::npos);
  grdbg_dap_destroy(dap);
  grdbg_transport_destroy(transport);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, FunctionStepInOutAndOverFollowGuestFrames) {
  const char source[]="(module"
      " (func $callee (result i32) (i32.const 41))"
      " (func $caller (export \"run\") (result i32)"
      " (call $callee) (i32.const 1) (i32.add)))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  ASSERT_EQ(module->nfuncs,2u);
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  ASSERT_EQ(grdbg_debugger_request_pause(debugger),GRDBG_OK);
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),
      GLWASM_ERR_PAUSED);
  ASSERT_EQ(grdbg_debugger_step(debugger,GRDBG_STEP_IN),GRDBG_OK);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(paused);
  GRDBG_FrameIdentity identity=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&identity),GRDBG_OK);
  EXPECT_EQ((uint32_t)identity.function,0u);
  size_t frames=0;
  size_t available=0;
  ASSERT_EQ(grdbg_debugger_frames(debugger,&frames,&available),GRDBG_OK);
  EXPECT_EQ(frames,2u);

  ASSERT_EQ(grdbg_debugger_step(debugger,GRDBG_STEP_OUT),GRDBG_OK);
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(paused);
  identity=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&identity),GRDBG_OK);
  EXPECT_EQ((uint32_t)identity.function,1u);
  ASSERT_EQ(grdbg_debugger_frames(debugger,&frames,&available),GRDBG_OK);
  EXPECT_EQ(frames,1u);

  ASSERT_EQ(grdbg_debugger_step(debugger,GRDBG_STEP_OVER_INSTRUCTION),
      GRDBG_OK);
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_ERR_PAUSED);
  EXPECT_TRUE(paused);
  identity=GRDBG_FRAME_IDENTITY_INIT;
  ASSERT_EQ(grdbg_debugger_frame_identity(debugger,0,&identity),GRDBG_OK);
  EXPECT_EQ((uint32_t)identity.function,1u);
  ASSERT_EQ(grdbg_debugger_continue(debugger),GRDBG_OK);
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_FALSE(paused);
  EXPECT_EQ(result,42u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WasmDebug, ImportedMemoryInspectionBalancesEveryLease) {
  const char source[]="(module (import \"host.v1\" \"memory\" (memory 1))"
      " (func (export \"read\") (result i32)"
      " (i32.load (i32.const 0))))";
  GLWASM_Module *module=nullptr;
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(source),
      sizeof(source)-1,nullptr,nullptr,
      nullptr,&module),GLWASM_OK);
  ASSERT_NE(module,nullptr);
  DebugMemoryProviderState state;
  state.bytes.resize(65536);
  state.bytes[0]=0x78;state.bytes[1]=0x56;state.bytes[2]=0x34;state.bytes[3]=0x12;
  GLWASM_HostMemoryProvider provider{debug_memory_begin,debug_memory_end,
      debug_memory_grow,&state,0,0,0};
  GLWASM_HostImport import{};
  import.module="host.v1";
  import.field="memory";
  import.kind=GLWASM_HOST_IMPORT_MEMORY;
  import.resource.memory=&provider;
  GLWASM_Runtime *runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,&import,1,0,nullptr,
      &runtime),GLWASM_OK);
  ASSERT_NE(runtime,nullptr);
  GRCORE_Context *context=glwasm_runtime_context(runtime);
  ASSERT_NE(context,nullptr);
  GRDBG_Debugger *debugger=nullptr;
  ASSERT_EQ(grdbg_debugger_attach(context,nullptr,&debugger),GRDBG_OK);
  const GRCORE_EngineId engine=wasm_engine_id(context);
  ASSERT_NE(engine,0u);
  ASSERT_EQ(module->nfuncs,1u);
  const GRCORE_PollIdentity identity={
      (uint64_t)module->debug_id<<32,module->funcs[0].translated->offsets[0],
      engine};
  uint64_t breakpoint=0;
  ASSERT_EQ(grdbg_debugger_set_instruction_breakpoints(debugger,"wasm",
      &identity,1,&breakpoint),GRDBG_OK);
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"read",nullptr,0,&result),
      GLWASM_ERR_PAUSED);
  EXPECT_EQ(state.leases,0u);

  size_t count=0;
  ASSERT_EQ(grdbg_debugger_memory_count(debugger,0,&count),GRDBG_OK);
  ASSERT_EQ(count,1u);
  GRDBG_Memory memory{};
  ASSERT_EQ(grdbg_debugger_memory(debugger,0,0,&memory),GRDBG_OK);
  EXPECT_EQ(memory.size_bytes,65536u);
  EXPECT_EQ(state.leases,0u);
  uint8_t word[4]={};
  ASSERT_EQ(grdbg_debugger_read_memory(debugger,0,0,0,word,sizeof(word)),
      GRDBG_OK);
  EXPECT_EQ(word[0],0x78u);EXPECT_EQ(word[1],0x56u);
  EXPECT_EQ(word[2],0x34u);EXPECT_EQ(word[3],0x12u);
  EXPECT_EQ(state.leases,0u);
  EXPECT_EQ(state.begins,state.ends);

  uint8_t unchanged[2]={0xa5,0x5a};
  EXPECT_EQ(grdbg_debugger_read_memory(debugger,0,0,65535,unchanged,
      sizeof(unchanged)),GRDBG_ERR_INVALID);
  EXPECT_EQ(unchanged[0],0xa5u);EXPECT_EQ(unchanged[1],0x5au);
  EXPECT_EQ(state.leases,0u);
  EXPECT_EQ(state.begins,state.ends);

  state.fail_begin=true;
  const unsigned ends_before=state.ends;
  EXPECT_EQ(grdbg_debugger_read_memory(debugger,0,0,0,word,sizeof(word)),
      GRDBG_ERR_INVALID);
  EXPECT_EQ(state.leases,0u);
  EXPECT_EQ(state.ends,ends_before);

  state.fail_begin=false;
  ASSERT_EQ(grdbg_debugger_continue(debugger),GRDBG_OK);
  int paused=0;
  EXPECT_EQ(glwasm_runtime_resume(runtime,&result,&paused),GLWASM_OK);
  EXPECT_FALSE(paused);
  EXPECT_EQ(state.leases,0u);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

int main(int argc,char **argv) {
  ::testing::InitGoogleTest(&argc,argv);
  return RUN_ALL_TESTS();
}
