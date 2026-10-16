/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * Story 3 execution gate for the pinned WebAssembly test suite. Commands
 * outside the implemented interpreter subset remain pending, never passing.
 */
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/script.h>
#include <ghoti.io/lang-wasm/decode.h>
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

namespace {
std::string read_file(const fs::path & path) {
  std::ifstream input(path, std::ios::binary);
  if (!input)
    throw std::runtime_error("cannot read " + path.string());
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

std::string text_bytes(const GLWASM_Text * node) {
  size_t length = 0;
  const uint8_t * bytes = glwasm_text_bytes(node, &length);
  return bytes ? std::string(reinterpret_cast<const char *>(bytes), length)
               : std::string();
}

std::string source_form_at(const std::string & source, GLWASM_SourceLoc loc) {
  size_t start = 0;
  for (uint32_t line = 1; line < loc.line && start < source.size(); ++line) {
    const size_t next = source.find('\n', start);
    if (next == std::string::npos)
      return {};
    start = next + 1;
  }
  if (loc.column == 0 || start + loc.column - 1 > source.size())
    return {};
  start += loc.column - 1;
  const size_t annotated_start = source.rfind("((@", start);
  if (annotated_start != std::string::npos) {
    const size_t module = source.find("module", annotated_start);
    if (module != std::string::npos && module <= start + 8)
      start = annotated_start;
  }
  if (source[start] != '(' ||
      (start + 1 < source.size() && source[start + 1] == ';')) {
    std::vector<size_t> opens;
    size_t block_comment = 0;
    bool string = false, escape = false, line_comment = false;
    for (size_t i = 0; i < start; ++i) {
      const char c = source[i];
      if (line_comment) {
        if (c == '\n') line_comment = false;
        continue;
      }
      if (block_comment) {
        if (c == '(' && i + 1 < start && source[i + 1] == ';') {
          ++block_comment; ++i;
        }
        else if (c == ';' && i + 1 < start && source[i + 1] == ')') {
          --block_comment; ++i;
        }
        continue;
      }
      if (string) {
        if (escape) escape = false;
        else if (c == '\\') escape = true;
        else if (c == '"') string = false;
        continue;
      }
      if (c == ';' && i + 1 < start && source[i + 1] == ';') {
        line_comment = true; ++i; continue;
      }
      if (c == '(' && i + 1 < start && source[i + 1] == ';') {
        block_comment = 1; ++i; continue;
      }
      if (c == '"') string = true;
      else if (c == '(') opens.push_back(i);
      else if (c == ')' && !opens.empty()) opens.pop_back();
    }
    if (opens.empty()) return {};
    start = opens.back();
  }
  size_t depth = 0, block_comment = 0;
  bool string = false, escape = false, line_comment = false;
  for (size_t i = start; i < source.size(); ++i) {
    const char c = source[i];
    if (line_comment) {
      if (c == '\n')
        line_comment = false;
      continue;
    }
    if (block_comment) {
      if (c == '(' && i + 1 < source.size() && source[i + 1] == ';') {
        ++block_comment;
        ++i;
      }
      else if (c == ';' && i + 1 < source.size() && source[i + 1] == ')') {
        --block_comment;
        ++i;
      }
      continue;
    }
    if (string) {
      if (escape)
        escape = false;
      else if (c == '\\')
        escape = true;
      else if (c == '"')
        string = false;
      continue;
    }
    if (c == ';' && i + 1 < source.size() && source[i + 1] == ';') {
      line_comment = true;
      ++i;
      continue;
    }
    if (c == '(' && i + 1 < source.size() && source[i + 1] == ';') {
      block_comment = 1;
      ++i;
      continue;
    }
    if (c == '"') {
      string = true;
      continue;
    }
    if (c == '(')
      ++depth;
    else if (c == ')' && --depth == 0)
      return source.substr(start, i - start + 1);
  }
  return {};
}

const GLWASM_Text * semantic_child(const GLWASM_Text * node, size_t wanted) {
  size_t i, seen = 0;
  for (i = 0; node && i < glwasm_text_size(node); ++i) {
    const GLWASM_Text * child = glwasm_text_at(node, i);
    if (glwasm_text_kind(child) == GLWASM_TEXT_LIST &&
        glwasm_text_size(child) &&
        text_bytes(glwasm_text_at(child, 0)).rfind("@", 0) == 0)
      continue;
    if (seen++ == wanted)
      return child;
  }
  return nullptr;
}

bool atom_is(const GLWASM_Text * node, const char * value) {
  return glwasm_text_kind(node) == GLWASM_TEXT_ATOM &&
         text_bytes(node) == value;
}

bool parse_wast_number(const GLWASM_Text *node, unsigned width, uint64_t *out) {
  if (!node || !out) return false;
  const std::string text=text_bytes(node);
  const bool negative=!text.empty()&&text[0]=='-';
  const size_t sign=(!text.empty()&&(text[0]=='-'||text[0]=='+'))?1u:0u;
  const int base=text.compare(sign,2,"0x")==0||text.compare(sign,2,"0X")==0?16:10;
  const std::string digits=text.substr(sign+(base==16?2u:0u));
  try {
    size_t consumed=0;
    const uint64_t magnitude=std::stoull(digits,&consumed,base);
    if(consumed!=digits.size())return false;
    const uint64_t positive_limit=width==32?UINT32_MAX:UINT64_MAX;
    const uint64_t negative_limit=UINT64_C(1)<<(width-1u);
    if((negative&&magnitude>negative_limit)||(!negative&&magnitude>positive_limit))return false;
    const uint64_t bits=negative?UINT64_C(0)-magnitude:magnitude;
    *out=width==32?static_cast<uint32_t>(bits):bits;
    return true;
  }
  catch (...) { return false; }
}

template <typename Float>
bool parse_wast_float(const std::string &text, Float *out) {
  if (text.empty() || !out) return false;
  std::string normalized = text;
  if (normalized[0] == '+') normalized.erase(0, 1);
  if (normalized.empty()) return false;
  const size_t sign = normalized[0] == '-' ? 1u : 0u;
  auto format = std::chars_format::general;
  if (normalized.compare(sign, 2, "0x") == 0 ||
      normalized.compare(sign, 2, "0X") == 0) {
    normalized.erase(sign, 2);
    format = std::chars_format::hex;
  }
  Float value{};
  const char *begin = normalized.data();
  const char *end = begin + normalized.size();
  const auto parsed = std::from_chars(begin, end, value, format);
  if (parsed.ptr != end) return false;
  if (parsed.ec == std::errc::result_out_of_range) {
    long double wider{};
    const auto wide = std::from_chars(begin, end, wider, format);
    if (wide.ec == std::errc{} && wide.ptr == end) {
      *out = static_cast<Float>(wider);
      return true;
    }
    if (wide.ec != std::errc::result_out_of_range) return false;

    const bool hexadecimal = format == std::chars_format::hex;
    const size_t sign = normalized[0] == '-' ? 1u : 0u;
    const size_t exponent_at = normalized.find_first_of(
        hexadecimal ? "pP" : "eE", sign);
    const std::string mantissa = normalized.substr(
        sign, exponent_at == std::string::npos ? std::string::npos
                                               : exponent_at - sign);
    size_t digits_before_point = 0, first_nonzero = 0;
    bool after_point = false, found_nonzero = false;
    unsigned leading_digit = 0;
    for (char c : mantissa) {
      if (c == '.') { after_point = true; continue; }
      if (!after_point) ++digits_before_point;
      unsigned digit;
      if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
      else if (hexadecimal && c >= 'a' && c <= 'f')
        digit = 10u + static_cast<unsigned>(c - 'a');
      else if (hexadecimal && c >= 'A' && c <= 'F')
        digit = 10u + static_cast<unsigned>(c - 'A');
      else return false;
      if (!found_nonzero && digit != 0) {
        found_nonzero = true;
        leading_digit = digit;
      }
    }
    if (!found_nonzero) {
      *out = normalized[0] == '-' ? -Float(0) : Float(0);
      return true;
    }
    // Count significant-position digits directly; the bounded exponent is
    // sufficient because this path is already outside long-double range.
    size_t position = 0;
    for (char c : mantissa) {
      if (c == '.') continue;
      unsigned digit = (c >= '0' && c <= '9')
          ? static_cast<unsigned>(c - '0')
          : hexadecimal && c >= 'a' && c <= 'f'
              ? 10u + static_cast<unsigned>(c - 'a')
              : hexadecimal && c >= 'A' && c <= 'F'
                  ? 10u + static_cast<unsigned>(c - 'A') : 0u;
      if (digit != 0) { first_nonzero = position; break; }
      ++position;
    }
    long long explicit_exponent = 0;
    if (exponent_at != std::string::npos) {
      size_t i = exponent_at + 1u;
      bool exponent_negative = i < normalized.size() && normalized[i] == '-';
      if (i < normalized.size() && (normalized[i] == '-' || normalized[i] == '+')) ++i;
      for (; i < normalized.size(); ++i) {
        if (normalized[i] < '0' || normalized[i] > '9') return false;
        explicit_exponent = std::min(1000000000LL,
            explicit_exponent * 10 + (normalized[i] - '0'));
      }
      if (exponent_negative) explicit_exponent = -explicit_exponent;
    }
    const long long before = static_cast<long long>(std::min<size_t>(
        digits_before_point, 1000000000u));
    const long long first = static_cast<long long>(std::min<size_t>(
        first_nonzero, 1000000000u));
    long long order = explicit_exponent + before - first - 1;
    if (hexadecimal) {
      unsigned top_bit = 0;
      while (leading_digit >>= 1u) ++top_bit;
      order = explicit_exponent + 4 * (before - first - 1) + top_bit;
    }
    if (order < 0) *out = normalized[0] == '-' ? -Float(0) : Float(0);
    else *out = normalized[0] == '-'
        ? -std::numeric_limits<Float>::infinity()
        : std::numeric_limits<Float>::infinity();
    return true;
  }
  if (parsed.ec != std::errc{}) return false;
  *out = value;
  return true;
}

bool parse_wast_typed_value(const GLWASM_Text *node, const std::string &form, uint64_t *out) {
  if (form == "i32.const") return parse_wast_number(node,32,out);
  if (form == "i64.const") return parse_wast_number(node,64,out);
  if (!node || !out || (form != "f32.const" && form != "f64.const")) return false;
  std::string text = text_bytes(node);
  const bool negative = !text.empty() && text[0] == '-';
  const std::string magnitude = negative ? text.substr(1) : text;
  if (magnitude == "nan" || magnitude.rfind("nan:", 0) == 0) {
    const std::string suffix = magnitude == "nan" ? "canonical" : magnitude.substr(4);
    if (form == "f32.const") {
      uint32_t bits = negative ? UINT32_C(0xffc00000) : UINT32_C(0x7fc00000);
      if (suffix.rfind("0x", 0) == 0) { try { size_t used=0;const std::string digits=suffix.substr(2);const uint64_t payload=std::stoull(digits,&used,16);if(used!=digits.size()||payload==0||payload>UINT32_C(0x7fffff))return false; bits=(negative?UINT32_C(0x80000000):0)|UINT32_C(0x7f800000)|(uint32_t)payload; } catch (...) { return false; } }
      else if (suffix != "canonical" && suffix != "arithmetic") return false;
      else if (suffix == "arithmetic") bits |= 1u;
      *out=bits;
    } else {
      uint64_t bits = negative ? UINT64_C(0xfff8000000000000) : UINT64_C(0x7ff8000000000000);
      if (suffix.rfind("0x", 0) == 0) { try { size_t used=0;const std::string digits=suffix.substr(2);const uint64_t payload=std::stoull(digits,&used,16);if(used!=digits.size()||payload==0||payload>UINT64_C(0x000fffffffffffff))return false; bits=(negative?UINT64_C(0x8000000000000000):0)|UINT64_C(0x7ff0000000000000)|payload; } catch (...) { return false; } }
      else if (suffix != "canonical" && suffix != "arithmetic") return false;
      else if (suffix == "arithmetic") bits |= 1u;
      *out=bits;
    }
    return true;
  }
  if (form == "f32.const") { float value; if(!parse_wast_float(text,&value))return false;uint32_t bits;std::memcpy(&bits,&value,sizeof(bits));*out=bits; }
  else { double value; if(!parse_wast_float(text,&value))return false;std::memcpy(out,&value,sizeof(value)); }
  return true;
}
bool wast_float_matches(uint64_t actual, uint64_t expected, const std::string &expected_text, bool f32) {
  const bool negative=!expected_text.empty()&&expected_text[0]=='-';
  const std::string mag=negative?expected_text.substr(1):expected_text;
  if(mag.rfind("nan",0)!=0)return f32?(uint32_t)actual==(uint32_t)expected:actual==expected;
  const std::string suffix=mag=="nan"?"canonical":mag.substr(4);
  if(f32){const uint32_t a=(uint32_t)actual,e=(uint32_t)expected;if(suffix=="arithmetic")return (a&UINT32_C(0x7f800000))==UINT32_C(0x7f800000)&&(a&UINT32_C(0x00400000))!=0;return suffix=="canonical"?(a&UINT32_C(0x7fffffff))==(e&UINT32_C(0x7fffffff)):suffix.rfind("0x",0)==0?a==e:false;}
  const uint64_t a=actual,e=expected;if(suffix=="arithmetic")return (a&UINT64_C(0x7ff0000000000000))==UINT64_C(0x7ff0000000000000)&&(a&UINT64_C(0x0008000000000000))!=0;return suffix=="canonical"?(a&UINT64_C(0x7fffffffffffffff))==(e&UINT64_C(0x7fffffffffffffff)):suffix.rfind("0x",0)==0?a==e:false;
}
uint8_t value_kind_for_form(const std::string &form) {
  if (form == "i32.const") return GLWASM_VK_I32;
  if (form == "i64.const") return GLWASM_VK_I64;
  if (form == "f32.const") return GLWASM_VK_F32;
  if (form == "f64.const") return GLWASM_VK_F64;
  return GLWASM_VK_BOT;
}
bool trap_message_matches(GLWASM_RuntimeTrap trap,const std::string &expected) {
  const char * exact=nullptr;
  switch(trap){
    case GLWASM_TRAP_UNREACHABLE: exact="unreachable"; break;
    case GLWASM_TRAP_DIVIDE_BY_ZERO: exact="integer divide by zero"; break;
    case GLWASM_TRAP_INTEGER_OVERFLOW: exact="integer overflow"; break;
    case GLWASM_TRAP_TABLE_OUT_OF_BOUNDS:
      return expected=="undefined element"||expected=="out of bounds table access";
    case GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS:
      return expected=="out of bounds memory access";
    case GLWASM_TRAP_UNINITIALIZED_ELEMENT:
      if(expected=="uninitialized element")return true;
      if(expected.rfind("uninitialized element ",0)!=0)return false;
      { unsigned index=0;const std::string suffix=expected.substr(22);
        const auto parsed=std::from_chars(suffix.data(),suffix.data()+suffix.size(),index);
        return !suffix.empty()&&parsed.ec==std::errc{}&&parsed.ptr==suffix.data()+suffix.size(); }
    case GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH:
      return expected=="indirect call type mismatch";
    case GLWASM_TRAP_INVALID_REFERENCE: exact="invalid reference"; break;
    case GLWASM_TRAP_NONE: return false;
  }
  return exact&&expected==exact;
}

bool instruction_supported(uint16_t op) {
  if(op==0x00||op==0x01||(op>=0x02&&op<=0x05)||(op>=0x0b&&op<=0x0e)||
     (op>=0x0f&&op<=0x13)||op==0x1a||op==0x1b||
     (op>=0x20&&op<=0x24)||op==0x08||op==0x0a||op==0x1f||op==0x41||op==0x42||op==0x43||op==0x44||
     (op>=0x45&&op<=0x4f)||(op>=0x92&&op<=0x98)||(op>=0xa0&&op<=0xa6))return true;
  switch(op){case 0x6a:case 0x6b:case 0x6c:case 0x6d:case 0x6e:case 0x71:case 0x72:case 0x73:case 0x74:case 0x75:case 0x76:case 0x7c:case 0x7d:case 0x7e:case 0x83:case 0x84:case 0x85:return true;}
  return false;
}

bool supported_spectest_global(GLWASM_Module *module,uint32_t index){
  if(index>=module->nglobals||module->globals[index].mut)return false;
  const GLWASM_Global *global=&module->globals[index];
  for(uint32_t i=0;i<module->nimports;i++){
    const GLWASM_Import *import=&module->imports[i];
    if(import->kind!=GLWASM_EXT_GLOBAL||import->index!=index)continue;
    if(import->module_name.length!=8||memcmp(import->module_name.bytes,"spectest",8))return false;
    const char *field=nullptr;
    switch(global->type.kind){case GLWASM_VK_I32:field="global_i32";break;case GLWASM_VK_I64:field="global_i64";break;case GLWASM_VK_F32:field="global_f32";break;case GLWASM_VK_F64:field="global_f64";break;default:return false;}
    return import->field.length==10&&!memcmp(import->field.bytes,field,10);
  }
  return false;
}

bool function_supported(GLWASM_Module *m,uint32_t start) {
  std::vector<uint8_t> seen(m->nfuncs,0);
  std::vector<uint32_t> todo(1,start);
  while(!todo.empty()){
    uint32_t fi=todo.back();todo.pop_back();
    if(fi>=m->nfuncs||seen[fi])continue;
    seen[fi]=1;const GLWASM_Func*f=&m->funcs[fi];
    if(f->imported||f->type_index>=m->ntypes||m->types[f->type_index].nresults>1)return false;
    for(uint32_t i=0;i<f->nins;i++){
      const GLWASM_Instr*in=&f->ins[i];
      if(!instruction_supported(in->op))return false;
      if(in->op==0x23||in->op==0x24){
        if(in->a<m->nglobals&&m->globals[in->a].imported&&!supported_spectest_global(m,in->a))return false;
      }
      if(in->op==0x10||in->op==0x12){if(in->a>=m->nfuncs)return false;todo.push_back(in->a);}
      if(in->op==0x11||in->op==0x13){if(in->b>=m->ntables||m->tables[in->b].imported)return false;for(uint32_t e=0;e<m->nelems;e++){const GLWASM_Elem*el=&m->elems[e];if(el->mode!=GLWASM_ELEM_ACTIVE||el->table!=in->b)continue;if(el->use_exprs){for(uint32_t j=0;j<el->nexprs;j++)for(uint32_t k=0;k<el->exprs[j].nins;k++)if(el->exprs[j].ins[k].op==0xd2)todo.push_back(el->exprs[j].ins[k].a);}else for(uint32_t j=0;j<el->nfuncs;j++)todo.push_back(el->funcs[j]);}}
    }
  }
  return true;
}

int execute_return_assertion(GLWASM_Runtime *runtime, GLWASM_Module *module,
    const GLWASM_Command &command) {
  const GLWASM_Text *invoke=semantic_child(command.syntax,1);
  const GLWASM_Text *expected=semantic_child(command.syntax,2);
  if(!runtime||!module||!invoke||glwasm_text_kind(invoke)!=GLWASM_TEXT_LIST||glwasm_text_size(invoke)<2)return false;
  const std::string op=text_bytes(glwasm_text_at(invoke,0));
  if(op!="invoke")return false;
  const std::string name=text_bytes(glwasm_text_at(invoke,1));
  uint32_t function=UINT32_MAX;
  for(uint32_t i=0;i<module->nexports;i++){
    const GLWASM_Export *e=&module->exports[i];
    if(e->kind==GLWASM_EXT_FUNC && std::string(e->name.bytes,e->name.length)==name){function=e->index;break;}
  }
  if(function==UINT32_MAX)return false;
  if(!function_supported(module,function))return false;
  if(module->funcs[function].type_index>=module->ntypes)return false;
  const uint32_t result_count=module->types[module->funcs[function].type_index].nresults;
  if(!expected){
    if(result_count!=0)return false;
    std::vector<uint64_t> args(module->types[module->funcs[function].type_index].nparams);uint32_t nargs=0;
    for(size_t i=2;i<glwasm_text_size(invoke);i++){
      const GLWASM_Text *a=glwasm_text_at(invoke,i);
      if(glwasm_text_kind(a)!=GLWASM_TEXT_LIST||glwasm_text_size(a)<2)return false;
      const std::string kind=text_bytes(glwasm_text_at(a,0));
      if(nargs>=module->types[module->funcs[function].type_index].nparams)return false;
      if(kind.empty()||value_kind_for_form(kind)!=module->types[module->funcs[function].type_index].params[nargs].kind)return false;
      if(!parse_wast_typed_value(glwasm_text_at(a,1),kind,&args[nargs++]))return false;
    }
    if(nargs!=module->types[module->funcs[function].type_index].nparams)return false;
    uint64_t ignored=0;
    return glwasm_runtime_invoke(runtime,function,args.data(),nargs,&ignored)==GLWASM_OK?1:2;
  }
  if(result_count!=1)return false;
  std::vector<uint64_t> args(module->types[module->funcs[function].type_index].nparams);uint32_t nargs=0;
  for(size_t i=2;i<glwasm_text_size(invoke);i++){
    const GLWASM_Text *a=glwasm_text_at(invoke,i);
    if(glwasm_text_kind(a)!=GLWASM_TEXT_LIST||glwasm_text_size(a)<2)return false;
    const std::string kind=text_bytes(glwasm_text_at(a,0));
    if(nargs>=module->types[module->funcs[function].type_index].nparams)return false;
    if(value_kind_for_form(kind)!=module->types[module->funcs[function].type_index].params[nargs].kind)return false;
    if(!parse_wast_typed_value(glwasm_text_at(a,1),kind,&args[nargs++]))return false;
  }
  if(nargs!=module->types[module->funcs[function].type_index].nparams)return false;
  if(glwasm_text_kind(expected)!=GLWASM_TEXT_LIST||glwasm_text_size(expected)!=2)return false;
  const std::string kind=text_bytes(glwasm_text_at(expected,0));
  if(value_kind_for_form(kind)!=module->types[module->funcs[function].type_index].results[0].kind)return false;
  uint64_t expected_value=0,actual=0;
  if(!parse_wast_typed_value(glwasm_text_at(expected,1),kind,&expected_value))return false;
  if(glwasm_runtime_invoke(runtime,function,args.data(),nargs,&actual)!=GLWASM_OK)return 2;
  const bool matches=kind=="i32.const" ? (uint32_t)actual==(uint32_t)expected_value : kind=="f32.const" ? wast_float_matches(actual,expected_value,text_bytes(glwasm_text_at(expected,1)),true) : kind=="f64.const" ? wast_float_matches(actual,expected_value,text_bytes(glwasm_text_at(expected,1)),false) : actual==expected_value;
  if(!matches)std::cerr<<"assertion runtime mismatch export="<<name<<" actual="<<actual<<" expected="<<expected_value<<" function="<<function<<" funcs="<<module->nfuncs<<" exports="<<module->nexports<<'\n';
  return matches ? 1 : -1;
}

int execute_trap_assertion(GLWASM_Runtime * runtime, GLWASM_Module * module,
    const GLWASM_Command & command) {
  const GLWASM_Text * invoke = semantic_child(command.syntax, 1);
  const GLWASM_Text * expected = semantic_child(command.syntax, 2);
  if (!runtime || !module || !invoke ||
      glwasm_text_kind(invoke) != GLWASM_TEXT_LIST ||
      glwasm_text_size(invoke) < 2 || !expected)
    return 0;
  if (text_bytes(glwasm_text_at(invoke, 0)) != "invoke")
    return 0;
  const std::string name = text_bytes(glwasm_text_at(invoke, 1));
  uint32_t function = UINT32_MAX;
  for (uint32_t i = 0; i < module->nexports; ++i) {
    const GLWASM_Export * exported = &module->exports[i];
    if (exported->kind == GLWASM_EXT_FUNC &&
        std::string(exported->name.bytes, exported->name.length) == name) {
      function = exported->index;
      break;
    }
  }
  if (function == UINT32_MAX || !function_supported(module, function))
    return 0;
  const GLWASM_TypeDef * type = &module->types[module->funcs[function].type_index];
  std::vector<uint64_t> args(type->nparams);
  uint32_t nargs = 0;
  for (size_t i = 2; i < glwasm_text_size(invoke); ++i) {
    const GLWASM_Text * arg = glwasm_text_at(invoke, i);
    if (glwasm_text_kind(arg) != GLWASM_TEXT_LIST ||
        glwasm_text_size(arg) < 2 || nargs >= type->nparams)
      return 0;
    const std::string kind = text_bytes(glwasm_text_at(arg, 0));
    if (value_kind_for_form(kind) != type->params[nargs].kind ||
        !parse_wast_typed_value(glwasm_text_at(arg, 1), kind, &args[nargs]))
      return 0;
    ++nargs;
  }
  if (nargs != type->nparams) return 0;
  uint64_t ignored = 0;
  const GLWASM_Result result =
      glwasm_runtime_invoke(runtime, function, args.data(), nargs, &ignored);
  if (result != GLWASM_ERR_GUEST)
    return result == GLWASM_OK ? -1 : 2;
  const std::string expected_text = text_bytes(expected);
  return trap_message_matches(glwasm_runtime_last_trap(runtime),expected_text)
      ? 1 : -1;
}

int execute_exception_assertion(GLWASM_Runtime * runtime, GLWASM_Module * module,
    const GLWASM_Command & command) {
  const GLWASM_Text * invoke = semantic_child(command.syntax, 1);
  if (!runtime || !module || !invoke ||
      glwasm_text_kind(invoke) != GLWASM_TEXT_LIST ||
      glwasm_text_size(invoke) < 2 ||
      text_bytes(glwasm_text_at(invoke, 0)) != "invoke")
    return 0;
  const std::string name = text_bytes(glwasm_text_at(invoke, 1));
  uint32_t function = UINT32_MAX;
  for (uint32_t i = 0; i < module->nexports; ++i) {
    const GLWASM_Export * exported = &module->exports[i];
    if (exported->kind == GLWASM_EXT_FUNC &&
        std::string(exported->name.bytes, exported->name.length) == name) {
      function = exported->index;
      break;
    }
  }
  if (function == UINT32_MAX || !function_supported(module, function))
    return 0;
  const GLWASM_TypeDef * type = &module->types[module->funcs[function].type_index];
  std::vector<uint64_t> args(type->nparams);
  uint32_t nargs = 0;
  for (size_t i = 2; i < glwasm_text_size(invoke); ++i) {
    const GLWASM_Text * arg = glwasm_text_at(invoke, i);
    if (glwasm_text_kind(arg) != GLWASM_TEXT_LIST ||
        glwasm_text_size(arg) < 2 || nargs >= type->nparams)
      return 0;
    const std::string kind = text_bytes(glwasm_text_at(arg, 0));
    if (value_kind_for_form(kind) != type->params[nargs].kind ||
        !parse_wast_typed_value(glwasm_text_at(arg, 1), kind, &args[nargs]))
      return 0;
    ++nargs;
  }
  if (nargs != type->nparams) return 0;
  uint64_t ignored = 0;
  const GLWASM_Result result =
      glwasm_runtime_invoke(runtime, function, args.data(), nargs, &ignored);
  if (result != GLWASM_ERR_GUEST)
    return result == GLWASM_OK ? -1 : 2;
  return glwasm_runtime_last_trap(runtime) == GLWASM_TRAP_NONE ? 1 : -1;
}

int execute_invoke(GLWASM_Runtime *runtime,GLWASM_Module *module,const GLWASM_Text*invoke){
  if(!runtime||!module||!invoke||glwasm_text_kind(invoke)!=GLWASM_TEXT_LIST||glwasm_text_size(invoke)<2)return 0;
  const std::string name=text_bytes(glwasm_text_at(invoke,1));uint32_t function=UINT32_MAX;
  for(uint32_t i=0;i<module->nexports;i++){const GLWASM_Export*e=&module->exports[i];if(e->kind==GLWASM_EXT_FUNC&&std::string(e->name.bytes,e->name.length)==name){function=e->index;break;}}
  if(function==UINT32_MAX||!function_supported(module,function))return 0;
  const GLWASM_Func *fn=&module->funcs[function];const GLWASM_TypeDef *type=&module->types[fn->type_index];
  std::vector<uint64_t> args(type->nparams);uint64_t result;uint32_t nargs=0;
  for(size_t i=2;i<glwasm_text_size(invoke);i++){const GLWASM_Text*a=glwasm_text_at(invoke,i);if(glwasm_text_kind(a)!=GLWASM_TEXT_LIST||glwasm_text_size(a)<2||nargs>=type->nparams)return 0;const std::string kind=text_bytes(glwasm_text_at(a,0));if(value_kind_for_form(kind)!=type->params[nargs].kind||!parse_wast_typed_value(glwasm_text_at(a,1),kind,&args[nargs]))return 0;nargs++;}
  if(nargs!=type->nparams)return 0;
  return glwasm_runtime_invoke(runtime,function,args.data(),nargs,&result)==GLWASM_OK?1:-1;
}

bool check_binary_validation_assertion(const GLWASM_Command & command,
                                       GLWASM_Result expected) {
  const GLWASM_Text * module = semantic_child(command.syntax, 1);
  std::vector<uint8_t> binary;
  bool in_binary = false;
  GLWASM_Module * decoded = nullptr;
  GLWASM_Failure failure{};
  GLWASM_Limits limits{};
  GLWASM_Result result;
  if (!module || glwasm_text_kind(module) != GLWASM_TEXT_LIST)
    return false;
  for (size_t i = 1; i < glwasm_text_size(module); ++i) {
    const GLWASM_Text * part = glwasm_text_at(module, i);
    if (atom_is(part, "binary")) {
      in_binary = true;
      continue;
    }
    if (in_binary && glwasm_text_kind(part) == GLWASM_TEXT_STRING) {
      size_t n = 0;
      const uint8_t * bytes = glwasm_text_bytes(part, &n);
      binary.insert(binary.end(), bytes, bytes + n);
      continue;
    }
    if (in_binary)
      return false;
  }
  if (!in_binary)
    return false;
  limits.module_bytes = std::numeric_limits<uint64_t>::max();
  limits.types = limits.imports = limits.exports = limits.functions =
      limits.globals = limits.tables = limits.memories = limits.tags =
          limits.data_segments = limits.element_segments = limits.locals =
              limits.instructions = limits.nesting = limits.br_table =
                  limits.name_length = std::numeric_limits<uint32_t>::max();
  limits.table_size = limits.memory_maximum = limits.memory_initial =
      std::numeric_limits<uint64_t>::max();
  result = glwasm_module_decode(binary.data(), binary.size(), &limits, nullptr,
                                &failure, &decoded);
  if (decoded)
    glwasm_module_destroy(decoded);
  if (result != expected)
    std::cerr << "binary assertion mismatch at line " << command.location.line
              << ": expected " << expected << " got " << result
              << (failure.limit ? std::string(" limit=") + failure.limit
                                : std::string())
              << '\n';
  return result == expected;
}

std::string text_module_source(const std::string & source,
                              const GLWASM_Text * candidate) {
  if (!candidate || glwasm_text_kind(candidate) != GLWASM_TEXT_LIST)
    return {};
  const GLWASM_Text * mode = semantic_child(candidate, 1);
  if (atom_is(mode, "quote")) {
    std::string quoted;
    for (size_t i = 2; i < glwasm_text_size(candidate); ++i) {
      const GLWASM_Text * fragment = semantic_child(candidate, i);
      if (!fragment || glwasm_text_kind(fragment) != GLWASM_TEXT_STRING)
        return {};
      quoted += text_bytes(fragment);
    }
    if (quoted.rfind("(module", 0) != 0)
      quoted = "(module " + quoted + ")";
    return quoted;
  }
  return source_form_at(source, glwasm_text_location(candidate));
}

std::map<std::string, std::string> classifications(const fs::path & path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read classification file " +
                             path.string());
  std::map<std::string, std::string> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      throw std::runtime_error("malformed classification row: " + line);
    result.emplace(line.substr(0, tab), line.substr(tab + 1));
  }
  return result;
}

std::map<std::string, std::map<uint32_t, std::string>>
assertion_skips(const fs::path & path) {
  std::ifstream input(path);
  if (!input)
    throw std::runtime_error("cannot read assertion skip file " +
                             path.string());
  std::map<std::string, std::map<uint32_t, std::string>> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#')
      continue;
    std::istringstream fields(line);
    std::string file, line_text, reason, extra;
    if (!std::getline(fields, file, '\t') ||
        !std::getline(fields, line_text, '\t') ||
        !std::getline(fields, reason, '\t') ||
        std::getline(fields, extra, '\t'))
      throw std::runtime_error("malformed assertion skip row: " + line);
    const unsigned long parsed = std::stoul(line_text);
    if (parsed == 0 || parsed > UINT32_MAX ||
        !result[file].emplace(static_cast<uint32_t>(parsed), reason).second)
      throw std::runtime_error("duplicate or invalid assertion skip row: " +
                               line);
  }
  return result;
}
} // namespace

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

TEST(WastAdapter, TrapArgumentsMustMatchFunctionTypes) {
  const std::string module_source = "(module (func (export \"boom\") (param i64) unreachable))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(module_source.data()), module_source.size(), nullptr, nullptr, &error, &module), GLWASM_OK) << error.message;
  GLWASM_Runtime * runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  auto run = [&](const std::string & source) -> int {
    GLWASM_Script * script = nullptr;
    if (glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()), source.size(), nullptr, &error, &script) != GLWASM_OK || !script) return -10;
    GLWASM_Command command{};
    if (glwasm_script_at(script, 0, &command) != GLWASM_OK) { glwasm_script_destroy(script); return -11; }
    const int result = execute_trap_assertion(runtime, module, command);
    glwasm_script_destroy(script);
    return result;
  };
  EXPECT_EQ(run("(assert_trap (invoke \"boom\" (i32.const 1)) \"unreachable\")"), 0);
  EXPECT_EQ(run("(assert_trap (invoke \"boom\" (i64.const 1)) \"unreachable\")"), 1);
  EXPECT_EQ(run("(assert_trap (invoke \"boom\" (i64.const 1)) \"unreachable extra\")"), -1);
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
      .kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={}};
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
  EXPECT_EQ(glwasm_runtime_invoke_export(fuel_runtime,"fuel",nullptr,0,&result),
      GLWASM_ERR_LIMIT);
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
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",nullptr,0,&result),
      GLWASM_ERR_LIMIT);
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
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&host_tag}},
      {.module="host.v1",.field="second",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&host_tag}}};
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
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&first_tag}},
      {.module="host.v1",.field="second",.function=nullptr,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_TAG,.resource={.tag=&second_tag}}};
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
      bulk_pause_memory_end,host_memory_grow,&memory};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="arm",.function=host_capture_bulk_context,
       .resume=nullptr,.user=&memory.pause,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={}},
      {.module="host.v1",.field="mem",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_MEMORY,
       .resource={.memory=&provider}}};
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
      host_table_grow,GLWASM_HOST_EXTERNREF,bulk_pause_reference_check,&table};
  const GLWASM_HostImport imports[]={
      {.module="host.v1",.field="arm",.function=host_capture_bulk_context,
       .resume=nullptr,.user=&table.pause,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={}},
      {.module="host.v1",.field="tab",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_TABLE,
       .resource={.table=&provider}}};
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
  GLWASM_HostMemoryProvider provider{host_memory_begin,host_memory_end,host_memory_grow,&memory};
  GLWASM_HostImport binding{.module="host.v1",.field="mem",.function=nullptr,
      .resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_MEMORY,.resource={.memory=&provider}};
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
      host_memory_grow,&oversized_memory};
  const GLWASM_HostImport memory_import={.module="host.v1",.field="mem",
      .function=nullptr,.resume=nullptr,.user=nullptr,
      .kind=GLWASM_HOST_IMPORT_MEMORY,.resource={.memory=&memory_provider}};
  GLWASM_Runtime *runtime=parse_and_link(
      "(module (import \"host.v1\" \"mem\" (memory 1 1)))",
      &memory_import,1,&module,&link,&status);
  EXPECT_EQ(status,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  EXPECT_EQ(oversized_memory.leases,0);

  HostTableFixture oversized_table{{0,0,0}};
  GLWASM_HostTableProvider table_provider{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_EXTERNREF,host_reference_check,&oversized_table};
  const GLWASM_HostImport table_import={.module="host.v1",.field="tab",
      .function=nullptr,.resume=nullptr,.user=nullptr,
      .kind=GLWASM_HOST_IMPORT_TABLE,.resource={.table=&table_provider}};
  runtime=parse_and_link(
      "(module (import \"host.v1\" \"tab\" (table 1 2 externref)))",
      &table_import,1,&module,&link,&status);
  EXPECT_EQ(status,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  EXPECT_EQ(oversized_table.leases,0);
}

TEST(WastAdapter, ImportedTableAndGlobalProvidersValidateTypesAndTokens) {
  HostTableFixture table{{0,0}};
  GLWASM_HostTableProvider table_provider{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_EXTERNREF,host_reference_check,&table};
  uint64_t global_value=7;
  GLWASM_HostGlobal global{GLWASM_HOST_I32,1,&global_value,nullptr,nullptr};
  const GLWASM_HostImport bindings[]={
      {.module="host.v1",.field="tab",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_TABLE,.resource={.table=&table_provider}},
      {.module="host.v1",.field="g",.function=nullptr,.resume=nullptr,.user=nullptr,
       .kind=GLWASM_HOST_IMPORT_GLOBAL,.resource={.global=&global}}};
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
  const uint64_t global_arg[]={42};
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"setglobal",global_arg,1,&value),GLWASM_OK);
  EXPECT_EQ(global_value,42u);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"getglobal",nullptr,0,&value),GLWASM_OK);
  EXPECT_EQ(value,42u);
  ASSERT_EQ(glwasm_runtime_request_test_pause(runtime),GLWASM_OK);
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"isnull",nullptr,0,&value),GLWASM_ERR_PAUSED);
  GLWASM_Snapshot *unsupported_snapshot=nullptr;
  EXPECT_EQ(glwasm_runtime_snapshot(runtime,&unsupported_snapshot),GLWASM_ERR_UNSUPPORTED);
  EXPECT_EQ(unsupported_snapshot,nullptr);
  glwasm_runtime_destroy(runtime);glwasm_module_destroy(module);

  GLWASM_HostGlobal wrong{GLWASM_HOST_I64,1,&global_value,nullptr,nullptr};
  const GLWASM_HostImport mismatch={.module="host.v1",.field="g",
      .function=nullptr,.resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_GLOBAL,
      .resource={.global=&wrong}};
  const std::string global_source="(module (import \"host.v1\" \"g\" (global (mut i32))))";
  runtime=parse_and_link(global_source,&mismatch,1,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);

  HostTableFixture wrong_table_state{{0,0}};
  GLWASM_HostTableProvider wrong_table{host_table_begin,host_table_end,
      host_table_grow,GLWASM_HOST_FUNCREF,nullptr,&wrong_table_state};
  const GLWASM_HostImport table_mismatch={.module="host.v1",.field="tab",
      .function=nullptr,.resume=nullptr,.user=nullptr,.kind=GLWASM_HOST_IMPORT_TABLE,
      .resource={.table=&wrong_table}};
  const std::string table_source="(module (import \"host.v1\" \"tab\" (table 2 externref)))";
  runtime=parse_and_link(table_source,&table_mismatch,1,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(runtime,nullptr);
  runtime=parse_and_link(table_source,nullptr,0,&module,&link,&result);
  EXPECT_EQ(result,GLWASM_ERR_LINK);EXPECT_EQ(link.reason,GLWASM_LINK_UNRESOLVED);
}

TEST(WastAdapter, PublicHostAPIRequiresExplicitVersionedImportGrant) {
  const std::string source=
      "(module (import \"tang.v1\" \"sum\""
      " (func $sum (param i32 i32) (result i32)))"
      " (func (export \"run\") (param i32 i32) (result i32)"
      "   (local.get 0) (local.get 1) (call $sum)))";
  GLWASM_Module *module=nullptr;
  GLWASM_ParseError parse_error{};
  ASSERT_EQ(glwasm_wat_module_parse(
      reinterpret_cast<const uint8_t *>(source.data()),source.size(),
      nullptr,nullptr,&parse_error,&module),GLWASM_OK)
      << parse_error.message;
  GLWASM_Runtime *runtime=nullptr;
  GLWASM_LinkError link_error{};
  EXPECT_EQ(glwasm_runtime_create_with_imports(module,nullptr,0,1,
      &link_error,&runtime),GLWASM_ERR_LINK);
  EXPECT_EQ(link_error.reason,GLWASM_LINK_UNRESOLVED);
  EXPECT_EQ(link_error.module,std::string("tang.v1"));
  ASSERT_EQ(runtime,nullptr);

  const GLWASM_HostImport imports[]={
      {.module="tang.v1",.field="sum",.function=host_add,.resume=nullptr,
       .user=nullptr,.kind=GLWASM_HOST_IMPORT_FUNCTION,.resource={}}};
  ASSERT_EQ(glwasm_runtime_create_with_imports(module,imports,1,1,
      &link_error,&runtime),GLWASM_OK);
  const uint64_t arguments[]={40,2};
  uint64_t result=0;
  EXPECT_EQ(glwasm_runtime_invoke_export(runtime,"run",arguments,2,&result),
      GLWASM_OK);
  EXPECT_EQ(result,UINT64_C(42));
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, DecimalIntegerWithLeadingZeroIsNotOctal) {
  const std::string module_source =
      "(module (func (export \"id\") (param i32) (result i32) "
      "(local.get 0)))";
  GLWASM_Module * module = nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(module_source.data()),
      module_source.size(), nullptr, nullptr, &error, &module), GLWASM_OK)
      << (error.message ? error.message : "");
  GLWASM_Runtime * runtime = nullptr;
  ASSERT_EQ(glwasm_runtime_create(module, &runtime), GLWASM_OK);
  const std::string source =
      "(assert_return (invoke \"id\" (i32.const 08)) (i32.const 8))";
  GLWASM_Script * script = nullptr;
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
      source.size(), nullptr, &error, &script), GLWASM_OK);
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script, 0, &command), GLWASM_OK);
  EXPECT_EQ(execute_return_assertion(runtime, module, command), 1);
  glwasm_script_destroy(script);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, RejectsIntegerLiteralsOutsideWasmWidth) {
  auto parse_token = [](const std::string &text, uint64_t *value,
                        const char *form) {
    GLWASM_ParseError error{};
    GLWASM_Text *tree=nullptr;
    const std::string source="(\""+text+"\")";
    if(glwasm_text_parse(reinterpret_cast<const uint8_t *>(source.data()),
        source.size(),nullptr,&error,&tree)!=GLWASM_OK)return false;
    const GLWASM_Text *expression=glwasm_text_at(tree,0);
    const bool parsed=expression&&glwasm_text_kind(expression)==GLWASM_TEXT_LIST&&
        parse_wast_typed_value(glwasm_text_at(expression,0),form,value);
    glwasm_text_destroy(tree);
    return parsed;
  };
  uint64_t value=0;
  EXPECT_TRUE(parse_token("4294967295",&value,"i32.const"));
  EXPECT_EQ(value,UINT32_MAX);
  EXPECT_TRUE(parse_token("-2147483648",&value,"i32.const"));
  EXPECT_EQ(value,UINT32_C(0x80000000));
  EXPECT_FALSE(parse_token("4294967296",&value,"i32.const"));
  EXPECT_FALSE(parse_token("-2147483649",&value,"i32.const"));
  EXPECT_TRUE(parse_token("18446744073709551615",&value,"i64.const"));
  EXPECT_EQ(value,UINT64_MAX);
  EXPECT_TRUE(parse_token("-9223372036854775808",&value,"i64.const"));
  EXPECT_EQ(value,UINT64_C(0x8000000000000000));
  EXPECT_FALSE(parse_token("18446744073709551616",&value,"i64.const"));
}

TEST(WastAdapter, SupportsLargeArityAndZeroResultAssertions) {
  std::string module_source="(module (func (export \"id\") (param ";
  std::string arguments;
  for(unsigned i=0;i<40;i++){
    module_source+="i32 ";
    if(i)arguments+=" ";
    arguments+="(i32.const "+std::to_string(i)+")";
  }
  module_source+=") (result i32) (local.get 0)))";
  GLWASM_Module * module=nullptr;
  GLWASM_ParseError error{};
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(module_source.data()),
      module_source.size(),nullptr,nullptr,&error,&module),GLWASM_OK)
      << (error.message?error.message:"");
  GLWASM_Runtime * runtime=nullptr;
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  std::string assertion="(assert_return (invoke \"id\" "+arguments+") (i32.const 0))";
  GLWASM_Script * script=nullptr;
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(assertion.data()),
      assertion.size(),nullptr,&error,&script),GLWASM_OK);
  GLWASM_Command command{};
  ASSERT_EQ(glwasm_script_at(script,0,&command),GLWASM_OK);
  EXPECT_EQ(execute_return_assertion(runtime,module,command),1);
  glwasm_script_destroy(script);
  const std::string standalone_invoke="(invoke \"id\" "+arguments+")";
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(standalone_invoke.data()),
      standalone_invoke.size(),nullptr,&error,&script),GLWASM_OK);
  ASSERT_EQ(glwasm_script_at(script,0,&command),GLWASM_OK);
  ASSERT_EQ(command.kind,GLWASM_COMMAND_INVOKE);
  EXPECT_EQ(execute_invoke(runtime,module,command.syntax),1);
  glwasm_script_destroy(script);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);

  const std::string empty_source="(module (func (export \"run\")))";
  ASSERT_EQ(glwasm_wat_module_parse(reinterpret_cast<const uint8_t *>(empty_source.data()),
      empty_source.size(),nullptr,nullptr,&error,&module),GLWASM_OK);
  ASSERT_EQ(glwasm_runtime_create(module,&runtime),GLWASM_OK);
  const std::string empty_assertion="(assert_return (invoke \"run\"))";
  ASSERT_EQ(glwasm_script_parse(reinterpret_cast<const uint8_t *>(empty_assertion.data()),
      empty_assertion.size(),nullptr,&error,&script),GLWASM_OK);
  ASSERT_EQ(glwasm_script_at(script,0,&command),GLWASM_OK);
  EXPECT_EQ(execute_return_assertion(runtime,module,command),1);
  glwasm_script_destroy(script);
  glwasm_runtime_destroy(runtime);
  glwasm_module_destroy(module);
}

TEST(WastAdapter, RejectsTrailingGarbageInNaNPayload) {
  GLWASM_ParseError error{};
  const std::string malformed="(\"nan:0x123xyz\")";
  GLWASM_Text * tree=nullptr;
  ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(malformed.data()),
      malformed.size(),nullptr,&error,&tree),GLWASM_OK)
      << (error.message?error.message:"");
  const GLWASM_Text * expression=glwasm_text_at(tree,0);
  ASSERT_EQ(glwasm_text_kind(expression),GLWASM_TEXT_LIST);
  const GLWASM_Text * token=glwasm_text_at(expression,0);
  uint64_t value=0;
  EXPECT_FALSE(parse_wast_typed_value(token,"f32.const",&value));
  glwasm_text_destroy(tree);
}

TEST(WastAdapter, ParsesDecimalAndHexFloatsWithWastGrammar) {
  GLWASM_ParseError error{};
  uint64_t value=0;
  for (const auto & [text, form, expected] :
       std::vector<std::tuple<std::string, std::string, uint64_t>>{
           {"1.5", "f32.const", UINT32_C(0x3fc00000)},
           {"0x1.8p+1", "f32.const", UINT32_C(0x40400000)},
           {"1e-50", "f32.const", UINT32_C(0)},
           {"1e39", "f32.const", UINT32_C(0x7f800000)},
           {"3.4028234663852886e38", "f32.const", UINT32_C(0x7f7fffff)},
           {"1.401298464324817e-45", "f32.const", UINT32_C(1)},
           {"0x1p-149", "f32.const", UINT32_C(1)},
           {"1e-400", "f64.const", UINT64_C(0)},
           {"1e400", "f64.const", UINT64_C(0x7ff0000000000000)},
           {"1.7976931348623157e308", "f64.const", UINT64_C(0x7fefffffffffffff)},
           {"4.9406564584124654e-324", "f64.const", UINT64_C(1)},
           {"0x1p-1074", "f64.const", UINT64_C(1)}}) {
    const std::string source="(\""+text+"\")";
    GLWASM_Text * tree=nullptr;
    ASSERT_EQ(glwasm_text_parse(reinterpret_cast<const uint8_t *>(source.data()),
        source.size(),nullptr,&error,&tree),GLWASM_OK);
    const GLWASM_Text * expression=glwasm_text_at(tree,0);
    ASSERT_EQ(glwasm_text_kind(expression),GLWASM_TEXT_LIST);
    ASSERT_TRUE(parse_wast_typed_value(glwasm_text_at(expression,0),form,&value));
    EXPECT_EQ(value,expected);
    glwasm_text_destroy(tree);
  }
}

TEST(WastCorpus, EveryPinnedFileParsesAndEveryCommandIsRecognized) {
  const char * root_env = std::getenv("GLWASM_WAST_CORPUS_ROOT");
  const char * classification_env = std::getenv("GLWASM_WAST_CLASSIFICATION");
  const char * skips_env = std::getenv("GLWASM_WAST_ASSERTION_SKIPS");
  const char * baseline_env = std::getenv("GLWASM_WAST_EXECUTION_BASELINE");
  const char * spec_commit_env = std::getenv("GLWASM_WAST_SPEC_COMMIT");
  ASSERT_NE(root_env, nullptr) << "Makefile must set GLWASM_WAST_CORPUS_ROOT";
  ASSERT_NE(classification_env, nullptr)
      << "Makefile must set GLWASM_WAST_CLASSIFICATION";
  ASSERT_NE(skips_env, nullptr)
      << "Makefile must set GLWASM_WAST_ASSERTION_SKIPS";
  ASSERT_NE(baseline_env, nullptr)
      << "Makefile must set GLWASM_WAST_EXECUTION_BASELINE";
  ASSERT_NE(spec_commit_env, nullptr)
      << "Makefile must set GLWASM_WAST_SPEC_COMMIT";
  std::ifstream baseline_input(baseline_env);
  std::string baseline_commit, baseline_extra;
  size_t expected_execution_assertions=0, expected_deferred_invokes=0;
  baseline_input.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
  ASSERT_TRUE(baseline_input >> baseline_commit >> expected_execution_assertions
                              >> expected_deferred_invokes)
      << "execution baseline must contain its spec commit and both counts";
  EXPECT_FALSE(baseline_input >> baseline_extra)
      << "execution baseline must contain exactly one data row";
  EXPECT_EQ(baseline_commit,spec_commit_env)
      << "execution baseline must match the fetched pinned spec corpus";
  const fs::path root(root_env);
  GLWASM_Limits wat_limits{};
  ASSERT_EQ(glwasm_limits_default(&wat_limits), GLWASM_OK);
  wat_limits.memory_maximum = wat_limits.memory_initial =
      std::numeric_limits<uint64_t>::max();
  wat_limits.table_size = std::numeric_limits<uint64_t>::max();
  const auto classes = classifications(classification_env);
  const auto per_assertion = assertion_skips(skips_env);
  size_t parsed_files = 0, run_files = 0, pending = 0, commands = 0;
  size_t binary_validation_checked = 0, binary_validation_passed = 0;
  size_t text_malformed_checked = 0, text_malformed_passed = 0;
  size_t text_invalid_checked = 0, text_invalid_passed = 0;
  size_t wat_modules_checked = 0, wat_modules_passed = 0,
         deferred_wat_modules = 0;
  size_t wat_modules_in_scope = 0, wat_in_scope_passed = 0;
  size_t execution_checked = 0, execution_passed = 0, deferred_invokes=0;
  size_t deferred_runtime_unsupported=0,deferred_runtime_init_traps=0;
  std::vector<std::string> wat_module_failures;
  std::map<std::string, size_t> skips;
  std::map<std::string, size_t> assertion_skip_counts;
  std::set<std::pair<std::string, uint32_t>> consumed_assertion_skips;
  std::vector<std::string> failures;
  std::vector<fs::path> files;
  for (const auto & entry : fs::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".wast")
      files.push_back(entry.path());
  }
  std::sort(files.begin(), files.end());

  for (const fs::path & path : files) {
    const std::string relative =
        "test/" + fs::relative(path, root).generic_string();
    const auto classification = classes.find(relative);
    if (classification == classes.end()) {
      failures.push_back(relative + ": missing classification");
      continue;
    }
    if (classification->second == "run")
      ++run_files;
    else if (classification->second.rfind("skip(", 0) == 0 &&
             classification->second.back() == ')')
      ++skips[classification->second.substr(5,
                                            classification->second.size() - 6)];
    else {
      failures.push_back(relative + ": invalid classification " +
                         classification->second);
      continue;
    }

    const std::string source = read_file(path);
    GLWASM_ParseError error{};
    GLWASM_Script * script = nullptr;
    const auto * bytes = reinterpret_cast<const uint8_t *>(source.data());
    GLWASM_Result result =
        glwasm_script_parse(bytes, source.size(), nullptr, &error, &script);
    // The three non-core custom-annotation files contain runner extensions
    // outside CAP-2. They remain syntax-checked, but are not fed to the strict
    // core command parser.
    GLWASM_Text * syntax = nullptr;
    const bool non_core = classification->second == "skip(non-core)";
    if (result != GLWASM_OK && non_core) {
      result =
          glwasm_text_parse(bytes, source.size(), nullptr, &error, &syntax);
    }
    if (result != GLWASM_OK) {
      failures.push_back(relative + ":" + std::to_string(error.location.line) +
                         ":" + std::to_string(error.location.column) + ": " +
                         (error.message ? error.message : "parse failed"));
      continue;
    }
    ++parsed_files;
    if (script) {
      GLWASM_Module *active_module=nullptr;
      GLWASM_Runtime *active_runtime=nullptr;
      size_t executed_assertions=0;
      size_t executed_assertions_passed=0;
      for (size_t i = 0; i < glwasm_script_size(script); ++i) {
        GLWASM_Command command{};
        ASSERT_EQ(glwasm_script_at(script, i, &command), GLWASM_OK);
        ++commands;
        if (command.kind == GLWASM_COMMAND_MODULE_TEXT) {
          const auto file_skips = per_assertion.find(relative);
          const auto line_skip = file_skips == per_assertion.end()
              ? std::map<uint32_t, std::string>::const_iterator{}
              : file_skips->second.find(command.location.line);
          if (file_skips != per_assertion.end() &&
              line_skip != file_skips->second.end()) {
            const std::string & reason = line_skip->second;
            if (reason.rfind("skip(", 0) != 0 || reason.back() != ')') {
              failures.push_back(relative + ":" +
                  std::to_string(command.location.line) +
                  ": invalid deferred module skip reason");
            }
            else {
              ++deferred_wat_modules;
              ++assertion_skip_counts[reason.substr(5, reason.size() - 6)];
              consumed_assertion_skips.emplace(relative, command.location.line);
            }
            continue;
          }
          const bool in_scope = classification->second == "run";
          const std::string form = source_form_at(source, command.location);
          if (form.empty()) {
            const std::string message =
                relative + ":" + std::to_string(command.location.line) +
                ": could not recover module source form";
            wat_module_failures.push_back(in_scope ? "in-scope " + message
                : classification->second + " " + message);
            if (in_scope)
              failures.push_back("in-scope WAT: " + message);
          }
          else {
            GLWASM_ParseError wat_error{};
            GLWASM_Module * lowered = nullptr;
            ++wat_modules_checked;
            if (in_scope)
              ++wat_modules_in_scope;
            const GLWASM_Result wat_result = glwasm_wat_module_parse(
                reinterpret_cast<const uint8_t *>(form.data()), form.size(),
                &wat_limits, nullptr, &wat_error, &lowered);
            if (wat_result == GLWASM_OK) {
              if(active_runtime){glwasm_runtime_destroy(active_runtime);active_runtime=nullptr;}
              if(active_module){glwasm_module_destroy(active_module);active_module=nullptr;}
              ++wat_modules_passed;
              if (in_scope)
                ++wat_in_scope_passed;
              if(in_scope && lowered){
                active_module=lowered;
                const GLWASM_Result runtime_result=glwasm_runtime_create(active_module,&active_runtime);
                if(runtime_result!=GLWASM_OK){
                  if(runtime_result==GLWASM_ERR_UNSUPPORTED)++deferred_runtime_unsupported;
                  else if(runtime_result==GLWASM_ERR_VALIDATE||runtime_result==GLWASM_ERR_GUEST)++deferred_runtime_init_traps;
                  else failures.push_back(relative+":"+std::to_string(command.location.line)+": runtime creation failed with result "+std::to_string(runtime_result));
                  glwasm_module_destroy(active_module);active_module=nullptr;active_runtime=nullptr;
                }
              }
              else if(lowered)glwasm_module_destroy(lowered);
            }
            else {
              if(lowered)glwasm_module_destroy(lowered);
              const std::string message =
                  relative + ":" + std::to_string(command.location.line) +
                  ":wat-" + std::to_string(wat_error.location.line) + ":" +
                  std::to_string(wat_error.location.column) + ": " +
                  (wat_error.message ? wat_error.message
                                     : "WAT module lowering failed") +
                  " [result " + std::to_string(wat_result) + "]";
              wat_module_failures.push_back(in_scope ? "in-scope " + message
                  : classification->second + " " + message);
              if (in_scope)
                failures.push_back("in-scope WAT: " + message);
            }
          }
        }
        if(command.kind==GLWASM_COMMAND_INVOKE){
          if(active_runtime){
            const int invoke_result=execute_invoke(active_runtime,active_module,command.syntax);
            if(invoke_result<0)failures.push_back(relative+":"+std::to_string(command.location.line)+": in-scope invoke trapped or failed");
            else if(invoke_result==0)++deferred_invokes;
            if(invoke_result<=0){
            glwasm_runtime_destroy(active_runtime);active_runtime=nullptr;
            }
          } else if(classification->second=="run") ++deferred_invokes;
        }
        if (command.kind >= GLWASM_COMMAND_ASSERT_RETURN) {
          const std::string & file_class = classification->second;
          const auto file_skips = per_assertion.find(relative);
          const auto line_skip =
              file_skips == per_assertion.end()
                  ? std::map<uint32_t, std::string>::const_iterator{}
                  : file_skips->second.find(command.location.line);
          if (file_class.rfind("skip(", 0) == 0 && file_class.back() == ')') {
            ++assertion_skip_counts[file_class.substr(
                5, file_class.size() - 6)];
          }
          else if (file_skips != per_assertion.end() &&
              line_skip != file_skips->second.end()) {
            ++assertion_skip_counts[line_skip->second.substr(
                5, line_skip->second.size() - 6)];
            consumed_assertion_skips.emplace(relative, command.location.line);
          }
          else if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED ||
                   command.kind == GLWASM_COMMAND_ASSERT_INVALID) {
            const GLWASM_Result expected =
                command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                    ? GLWASM_ERR_CORRUPT
                    : GLWASM_ERR_VALIDATE;
            const bool checked =
                check_binary_validation_assertion(command, expected);
            // Text forms require the text grammar/lowering work in Story 2;
            // binary forms exercise the existing decode+validate pipeline.
            if (checked) {
              ++binary_validation_checked;
              ++binary_validation_passed;
            }
            else if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED ||
                     command.kind == GLWASM_COMMAND_ASSERT_INVALID) {
              const GLWASM_Text * candidate =
                  semantic_child(command.syntax, 1);
              bool has_binary = false;
              for (size_t j = 0; candidate &&
                                 j < glwasm_text_size(candidate); ++j)
                has_binary |= atom_is(glwasm_text_at(candidate, j), "binary");
              if (has_binary) {
                failures.push_back(relative + ":" +
                                   std::to_string(command.location.line) +
                                   ": binary validation assertion produced an unexpected result");
              }
              else {
                const std::string form = text_module_source(source, candidate);
                GLWASM_ParseError text_error{};
                GLWASM_Module * lowered = nullptr;
                const bool quoted = atom_is(semantic_child(candidate, 1), "quote");
                const GLWASM_Result text_result = form.empty()
                    ? GLWASM_ERR_INVALID
                    : (quoted ? glwasm_wat_module_parse_verbatim
                              : glwasm_wat_module_parse)(
                          reinterpret_cast<const uint8_t *>(form.data()),
                          form.size(), &wat_limits, nullptr, &text_error,
                          &lowered);
                if (lowered)
                  glwasm_module_destroy(lowered);
                const bool expected_text_result =
                    command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                        ? text_result == GLWASM_ERR_CORRUPT
                        : text_result == GLWASM_ERR_VALIDATE;
                if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED)
                  ++text_malformed_checked;
                else
                  ++text_invalid_checked;
                if (expected_text_result) {
                  if (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED)
                    ++text_malformed_passed;
                  else
                    ++text_invalid_passed;
                }
                else {
                  failures.push_back(
                      relative + ":" + std::to_string(command.location.line) +
                      ": text assertion expected " +
                      (command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
                           ? "malformed"
                           : "invalid") +
                      ", got result " + std::to_string(text_result) +
                      (text_error.message ? ": " +
                           std::string(text_error.message)
                                         : std::string()));
                }
              }
            }
          }
          else if(command.kind==GLWASM_COMMAND_ASSERT_RETURN){
            const int execution=execute_return_assertion(active_runtime,active_module,command);
            if(execution==1){++executed_assertions;++executed_assertions_passed;}
            else if(execution<0){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": executed assert_return mismatch");}
            else if(execution==2){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": assert_return invocation trapped or failed");}
            else ++pending;
          }
          else if(command.kind==GLWASM_COMMAND_ASSERT_TRAP){
            const int execution=execute_trap_assertion(active_runtime,active_module,command);
            if(execution==1){++executed_assertions;++executed_assertions_passed;}
            else if(execution<0){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": executed assert_trap mismatch");}
            else if(execution==2){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": assert_trap invocation failed without a Wasm trap");}
            else ++pending;
          }
          else if(command.kind==GLWASM_COMMAND_ASSERT_EXCEPTION){
            const int execution=execute_exception_assertion(active_runtime,active_module,command);
            if(execution==1){++executed_assertions;++executed_assertions_passed;}
            else if(execution<0){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": executed assert_exception mismatch");}
            else if(execution==2){++executed_assertions;failures.push_back(relative+":"+std::to_string(command.location.line)+": assert_exception invocation failed without a guest exception");}
            else ++pending;
          }
          else ++pending;
        }
      }
      execution_checked += executed_assertions;
      execution_passed += executed_assertions_passed;
      if(active_runtime)glwasm_runtime_destroy(active_runtime);
      if(active_module)glwasm_module_destroy(active_module);
      glwasm_script_destroy(script);
    }
    else {
      for (size_t i = 0; i < glwasm_text_size(syntax); ++i) {
        const GLWASM_Text * command = glwasm_text_at(syntax, i);
        ++commands;
        if (glwasm_text_kind(command) == GLWASM_TEXT_LIST &&
            glwasm_text_size(command) != 0 &&
            text_bytes(glwasm_text_at(command, 0)).rfind("assert_", 0) == 0)
          ++pending;
      }
      glwasm_text_destroy(syntax);
    }
  }

  std::cout << "wg-3.0 WAST parse gate: parsed=" << parsed_files << "/"
            << files.size() << " commands=" << commands
            << " run-files=" << run_files
            << " skip-files=" << (files.size() - run_files)
            << " binary-validation-checked=" << binary_validation_checked
            << " binary-validation-passed=" << binary_validation_passed
            << " text-malformed-status=" << text_malformed_passed << "/"
            << text_malformed_checked
            << " text-invalid-status=" << text_invalid_passed << "/"
            << text_invalid_checked
            << " wat-modules-lowered=" << wat_modules_passed << "/"
            << wat_modules_checked
            << " in-scope-wat-modules-lowered=" << wat_in_scope_passed << "/"
            << wat_modules_in_scope
            << " deferred-wat-modules=" << deferred_wat_modules
            << " deferred-runtime-unsupported=" << deferred_runtime_unsupported
            << " deferred-runtime-init-traps=" << deferred_runtime_init_traps
            << " assertions-pending=" << pending << '\n';
  for (const auto & [reason, count] : skips)
    std::cout << "  skip(" << reason << ")=" << count << '\n';
  for (const auto & [reason, count] : assertion_skip_counts)
    std::cout << "  assertion-skip(" << reason << ")=" << count << '\n';
  for (const auto & failure : wat_module_failures)
    std::cout << "  wat-module-failure " << failure << '\n';
  size_t expected_assertion_skips = 0;
  for (const auto & [file, lines] : per_assertion) {
    (void)file;
    expected_assertion_skips += lines.size();
  }
  EXPECT_EQ(consumed_assertion_skips.size(), expected_assertion_skips)
      << "each assertion skip must match a source-located assertion command";
  std::cout << "execution assertions executed=" << execution_passed << "/"
            << execution_checked << " pending=" << pending
            << " deferred-invokes=" << deferred_invokes << '\n';
  EXPECT_EQ(execution_checked, expected_execution_assertions)
      << "pinned supported execution coverage changed; review the supported set explicitly";
  EXPECT_EQ(deferred_invokes, expected_deferred_invokes)
      << "pinned deferred standalone invokes changed; review the boundary explicitly";
  EXPECT_EQ(execution_passed, execution_checked)
      << "every executed supported assertion must pass";
  for (const auto & failure : failures)
    ADD_FAILURE() << failure;
  EXPECT_EQ(parsed_files, files.size()) << "all pinned WAST files must parse";
  EXPECT_EQ(classes.size(), files.size())
      << "classification and fetched file sets must agree";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
