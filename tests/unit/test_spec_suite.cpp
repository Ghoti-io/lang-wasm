/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 *
 * The pinned wg-3.0 spec suite, executed. Every command of every in-scope
 * file runs in the interpreter tier and again in the JIT tier, each with its
 * own state. A command passes, fails, or is skipped for a named deferred
 * feature; nothing is left pending. The gate fails on any failure, on any
 * command the runner cannot execute, and on any JIT refusal or exit that is
 * not a named rule (tests/data-wast-jit-rules.tsv).
 *
 * The frame differential runs the same corpus again, with every invocation
 * stopped once inside and its whole stack compared across tiers (FrameDiff).
 */
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/host.h>
#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/lang-wasm/script.h>
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <ghoti.io/runtime-heap/heap.h>
#include <ghoti.io/runtime-heap/options.h>
#ifdef GLWASM_SPEC_RELOCATE
#include <ghoti.io/runtime-heap/relocate.h>
#endif
#include <ghoti.io/runtime-heap/roots.h>
#include "module_internal.h"
#include "runtime/runtime.h"
#include "wast_values.h"
#include <gtest/gtest.h>
#if defined(__linux__) && defined(__has_include)
#if __has_include(<valgrind/valgrind.h>)
#include <valgrind/valgrind.h>
#define GLWASM_TEST_HAS_VALGRIND_CLIENT 1
#endif
#endif

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using wast::atom_is;
using wast::semantic_child;
using wast::semantic_size;
using wast::text_bytes;

namespace {

/* The guest call depth the suite runs with: deep enough for every
 * recursive test, finite so assert_exhaustion terminates. */
constexpr uint64_t kGuestDepth = 2048;

std::string read_file(const fs::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  std::ostringstream out;
  out << input.rdbuf();
  return out.str();
}

/* The complete source text of the form that starts at `loc`. The WAT
 * lowering reads module text, so a module command is re-read from source. */
std::string source_form_at(const std::string &source, GLWASM_SourceLoc loc) {
  size_t start = loc.offset;
  if (start >= source.size()) return {};
  if (source[start] != '(') {
    size_t open = source.rfind('(', start);
    if (open == std::string::npos) return {};
    start = open;
  }
  size_t depth = 0, block_comment = 0;
  bool string = false, escape = false, line_comment = false;
  for (size_t i = start; i < source.size(); ++i) {
    const char c = source[i];
    const char next = i + 1 < source.size() ? source[i + 1] : '\0';
    if (line_comment) { if (c == '\n' || c == '\r') line_comment = false; continue; }
    if (block_comment) {
      if (c == '(' && next == ';') { ++block_comment; ++i; }
      else if (c == ';' && next == ')') { --block_comment; ++i; }
      continue;
    }
    if (string) {
      if (escape) escape = false;
      else if (c == '\\') escape = true;
      else if (c == '"') string = false;
      continue;
    }
    if (c == ';' && next == ';') { line_comment = true; ++i; continue; }
    if (c == '(' && next == ';') { block_comment = 1; ++i; continue; }
    if (c == '"') { string = true; continue; }
    if (c == '(') ++depth;
    else if (c == ')' && --depth == 0)
      return source.substr(start, i - start + 1);
  }
  return {};
}

/* ------------------------------------------------------------------ */
/* Classification inputs                                               */
/* ------------------------------------------------------------------ */

std::map<std::string, std::string> read_classifications(const fs::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  std::map<std::string, std::string> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos)
      throw std::runtime_error("malformed classification row: " + line);
    if (!result.emplace(line.substr(0, tab), line.substr(tab + 1)).second)
      throw std::runtime_error("duplicate classification row: " + line);
  }
  return result;
}

/* file -> source line -> reason, for commands inside run files that use a
 * deferred feature. A skipped module also skips the commands that use it. */
std::map<std::string, std::map<uint32_t, std::string>>
read_line_skips(const fs::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  std::map<std::string, std::map<uint32_t, std::string>> result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string file, number, reason, extra;
    if (!std::getline(fields, file, '\t') ||
        !std::getline(fields, number, '\t') ||
        !std::getline(fields, reason, '\t') ||
        std::getline(fields, extra, '\t') ||
        reason.rfind("skip(", 0) != 0 || reason.back() != ')')
      throw std::runtime_error("malformed skip row: " + line);
    const unsigned long parsed = std::stoul(number);
    if (parsed == 0 || parsed > UINT32_MAX ||
        !result[file].emplace(static_cast<uint32_t>(parsed),
            reason.substr(5, reason.size() - 6)).second)
      throw std::runtime_error("duplicate or invalid skip row: " + line);
  }
  return result;
}

/* opcode -> rule name, for every compile refusal and named exit the JIT is
 * allowed to take, and the rule, if any, that allows retirement by exit
 * rate. Anything else the JIT tier records is a failure. */
struct JitRules {
  std::map<uint16_t, std::string> refusals;
  std::map<uint16_t, std::string> exits;
  std::string retirement;
};

JitRules read_jit_rules(const fs::path &path) {
  std::ifstream input(path);
  if (!input) throw std::runtime_error("cannot read " + path.string());
  JitRules rules;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string kind, opcode, rule, extra;
    if (!std::getline(fields, kind, '\t') ||
        !std::getline(fields, opcode, '\t') ||
        !std::getline(fields, rule, '\t') ||
        std::getline(fields, extra, '\t') || rule.empty())
      throw std::runtime_error("malformed JIT rule row: " + line);
    /* Retirement has no opcode of its own: it follows from exits, each of
     * which is checked against its own row. */
    if (kind == "retirement") {
      if (opcode != "-" || !rules.retirement.empty())
        throw std::runtime_error("malformed JIT retirement row: " + line);
      rules.retirement = rule;
      continue;
    }
    const unsigned long value = std::stoul(opcode, nullptr, 16);
    if (value > UINT16_MAX)
      throw std::runtime_error("JIT rule opcode out of range: " + line);
    auto &table = kind == "refusal" ? rules.refusals
        : kind == "exit" ? rules.exits
        : throw std::runtime_error("unknown JIT rule kind: " + line);
    if (!table.emplace(static_cast<uint16_t>(value), rule).second)
      throw std::runtime_error("duplicate JIT rule: " + line);
  }
  return rules;
}

/* ------------------------------------------------------------------ */
/* Values                                                              */
/* ------------------------------------------------------------------ */

/* Externref values the script names as (ref.extern N) are host tokens; the
 * null reference is token zero, so N travels as N + 1. */
uint64_t extern_token(uint64_t n) { return n + 1u; }

struct ExpectedValue {
  enum class Kind { Bits32, Bits64, Nan32, Nan64, Null, NonNullFunc,
                    Extern, AnyExtern, AnyRef } kind = Kind::Bits32;
  uint64_t bits = 0;
  wast::NanPattern nan = wast::NanPattern::None;
};

/* One result: a single expectation, or (either ...) of alternatives. */
using ExpectedResult = std::vector<ExpectedValue>;

bool parse_argument(const GLWASM_Text *node, const GLWASM_ValType &type,
    uint64_t *out, std::string *why) {
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_LIST ||
      glwasm_text_size(node) < 1) {
    *why = "argument is not a constant form";
    return false;
  }
  const std::string form = text_bytes(glwasm_text_at(node, 0));
  const GLWASM_Text *operand = glwasm_text_at(node, 1);
  const std::string text = text_bytes(operand);
  switch (type.kind) {
    case GLWASM_VK_I32:
      if (form == "i32.const" && wast::parse_integer(text, 32, out)) return true;
      break;
    case GLWASM_VK_I64:
      if (form == "i64.const" && wast::parse_integer(text, 64, out)) return true;
      break;
    case GLWASM_VK_F32:
      if (form == "f32.const" && wast::parse_float_bits(text, true, out, nullptr))
        return true;
      break;
    case GLWASM_VK_F64:
      if (form == "f64.const" && wast::parse_float_bits(text, false, out, nullptr))
        return true;
      break;
    case GLWASM_VK_REF:
      if (form == "ref.null") { *out = 0; return true; }
      if (form == "ref.extern" && (type.heap == GLWASM_HT_EXTERN ||
          type.heap == GLWASM_HT_NOEXTERN)) {
        uint64_t n = 0;
        if (wast::parse_integer(text, 64, &n)) { *out = extern_token(n); return true; }
      }
      break;
    default:
      break;
  }
  *why = "argument " + form + " " + text + " does not fit its parameter type";
  return false;
}

bool parse_expected_value(const GLWASM_Text *node, ExpectedValue *out,
    std::string *why) {
  using Kind = ExpectedValue::Kind;
  if (!node || glwasm_text_kind(node) != GLWASM_TEXT_LIST ||
      glwasm_text_size(node) < 1) {
    *why = "result is not a constant form";
    return false;
  }
  const std::string form = text_bytes(glwasm_text_at(node, 0));
  const std::string text = text_bytes(glwasm_text_at(node, 1));
  if (form == "i32.const") {
    out->kind = Kind::Bits32;
    if (wast::parse_integer(text, 32, &out->bits)) return true;
  } else if (form == "i64.const") {
    out->kind = Kind::Bits64;
    if (wast::parse_integer(text, 64, &out->bits)) return true;
  } else if (form == "f32.const" || form == "f64.const") {
    const bool f32 = form == "f32.const";
    if (wast::parse_float_bits(text, f32, &out->bits, &out->nan)) {
      out->kind = out->nan != wast::NanPattern::None
          ? (f32 ? Kind::Nan32 : Kind::Nan64)
          : (f32 ? Kind::Bits32 : Kind::Bits64);
      return true;
    }
  } else if (form == "ref.null") {
    out->kind = Kind::Null;
    return true;
  } else if (form == "ref.func") {
    out->kind = Kind::NonNullFunc;
    return true;
  } else if (form == "ref.extern") {
    if (glwasm_text_size(node) == 1) { out->kind = Kind::AnyExtern; return true; }
    uint64_t n = 0;
    if (wast::parse_integer(text, 64, &n)) {
      out->kind = Kind::Extern;
      out->bits = extern_token(n);
      return true;
    }
  }
  *why = "unsupported result form " + form;
  return false;
}

bool value_matches(uint64_t actual, const GLWASM_ValType &type,
    const ExpectedValue &expected) {
  using Kind = ExpectedValue::Kind;
  switch (expected.kind) {
    case Kind::Bits32:
      return (type.kind == GLWASM_VK_I32 || type.kind == GLWASM_VK_F32) &&
          (uint32_t)actual == (uint32_t)expected.bits;
    case Kind::Bits64:
      return (type.kind == GLWASM_VK_I64 || type.kind == GLWASM_VK_F64) &&
          actual == expected.bits;
    case Kind::Nan32:
      return type.kind == GLWASM_VK_F32 &&
          wast::nan_matches(actual, true, expected.nan);
    case Kind::Nan64:
      return type.kind == GLWASM_VK_F64 &&
          wast::nan_matches(actual, false, expected.nan);
    case Kind::Null:
      return type.kind == GLWASM_VK_REF && actual == 0;
    case Kind::NonNullFunc:
      return type.kind == GLWASM_VK_REF && actual != 0 &&
          (type.heap == GLWASM_HT_FUNC || type.heap == GLWASM_HT_NOFUNC);
    case Kind::Extern:
      return type.kind == GLWASM_VK_REF && actual == expected.bits;
    case Kind::AnyExtern:
      return type.kind == GLWASM_VK_REF && actual != 0;
    case Kind::AnyRef:
      return type.kind == GLWASM_VK_REF;
  }
  return false;
}

std::string hex(uint64_t value) {
  std::ostringstream out;
  out << "0x" << std::hex << value;
  return out.str();
}

/* ------------------------------------------------------------------ */
/* Traps                                                               */
/* ------------------------------------------------------------------ */

const char *trap_message(GLWASM_Trap trap) {
  switch (trap) {
    case GLWASM_TRAP_UNREACHABLE: return "unreachable";
    case GLWASM_TRAP_DIVIDE_BY_ZERO: return "integer divide by zero";
    case GLWASM_TRAP_INTEGER_OVERFLOW: return "integer overflow";
    case GLWASM_TRAP_TABLE_OUT_OF_BOUNDS: return "out of bounds table access";
    case GLWASM_TRAP_UNINITIALIZED_ELEMENT: return "uninitialized element";
    case GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH:
      return "indirect call type mismatch";
    case GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS: return "out of bounds memory access";
    case GLWASM_TRAP_INVALID_REFERENCE: return "invalid reference";
    case GLWASM_TRAP_UNDEFINED_ELEMENT: return "undefined element";
    case GLWASM_TRAP_INVALID_CONVERSION: return "invalid conversion to integer";
    case GLWASM_TRAP_NONE: break;
  }
  return "";
}

/* The suite's messages name the trap and may add detail after a space
 * ("uninitialized element 2") or stop at a prefix ("out of bounds"). */
bool trap_matches(GLWASM_Trap trap, const std::string &expected) {
  const std::string actual = trap_message(trap);
  if (actual.empty() || expected.empty()) return false;
  if (actual == expected) return true;
  if (actual.rfind(expected, 0) == 0) return true;
  return expected.rfind(actual + " ", 0) == 0;
}

/* ------------------------------------------------------------------ */
/* Engine adapter: the only code that knows how instances are made      */
/* ------------------------------------------------------------------ */

enum class Tier { Interpreter, Jit };
const char *tier_name(Tier tier) {
  return tier == Tier::Interpreter ? "interpreter" : "jit";
}

/* A module handle and the instance the store made from it. The store owns
 * the instance; the module handle is ours. */
struct Instance {
  GLWASM_Module *module = nullptr;
  GLWASM_Runtime *runtime = nullptr;
  ~Instance() {
    if (module) glwasm_module_destroy(module);
  }
};

enum class InstantiateStatus { Ok, Unlinkable, Trap, Failed, Skipped };
struct InstantiateOutcome {
  InstantiateStatus status = InstantiateStatus::Failed;
  std::string detail;
  GLWASM_Trap trap = GLWASM_TRAP_NONE;
};

/* Instances the script registered under a module name, and the names whose
 * module was skipped for a deferred feature. */
using Registry = std::map<std::string, Instance *>;
using SkippedRegistry = std::map<std::string, std::string>;

InstantiateOutcome instantiate(GLWASM_Store *store, GLWASM_Module *module,
    const Registry &registered, const SkippedRegistry &skipped,
    Instance &out) {
  /* A module that links to a skipped module is skipped for its reason. */
  for (uint32_t i = 0; i < module->nimports; ++i) {
    const GLWASM_Import &import = module->imports[i];
    const auto reason = skipped.find(
        std::string(import.module_name.bytes, import.module_name.length));
    if (reason != skipped.end())
      return {InstantiateStatus::Skipped, reason->second, GLWASM_TRAP_NONE};
  }
  /* Every import names a registered instance's export, or nothing. */
  std::vector<GLWASM_HostImport> imports;
  std::vector<std::string> names;
  names.reserve(2u * module->nimports);
  imports.reserve(module->nimports);
  for (uint32_t i = 0; i < module->nimports; ++i) {
    const GLWASM_Import &import = module->imports[i];
    const std::string from(import.module_name.bytes, import.module_name.length);
    const std::string field(import.field.bytes, import.field.length);
    const auto exporter = registered.find(from);
    GLWASM_Extern object{};
    if (exporter == registered.end() ||
        glwasm_runtime_export(exporter->second->runtime, field.data(),
            field.size(), &object) != GLWASM_OK)
      continue;
    names.push_back(from);
    const char *module_name = names.back().c_str();
    names.push_back(field);
    GLWASM_HostImport binding{};
    binding.module = module_name;
    binding.field = names.back().c_str();
    binding.kind = GLWASM_HOST_IMPORT_EXTERN;
    binding.resource.linked = object;
    imports.push_back(binding);
  }
  GLWASM_InstantiateError error{};
  GLWASM_Runtime *runtime = nullptr;
  const GLWASM_Result created = glwasm_store_instantiate(store, module,
      imports.data(), imports.size(), 1, &error, &runtime);
  if (created == GLWASM_ERR_LINK)
    return {InstantiateStatus::Unlinkable,
            std::string(error.link.module ? error.link.module : "?") + "." +
            (error.link.field ? error.link.field : "?"), GLWASM_TRAP_NONE};
  if (created == GLWASM_ERR_GUEST)
    return {InstantiateStatus::Trap, "instantiation trapped", error.trap};
  if (created != GLWASM_OK)
    return {InstantiateStatus::Failed,
            "instantiation returned " + std::to_string(created) + " at stage " +
            std::to_string(error.stage), GLWASM_TRAP_NONE};
  out.runtime = runtime;
  if (glwasm_runtime_test_reset_observed_functions(runtime) != GLWASM_OK)
    return {InstantiateStatus::Failed, "could not observe the JIT tier",
            GLWASM_TRAP_NONE};
  return {InstantiateStatus::Ok, {}, GLWASM_TRAP_NONE};
}

enum class InvokeStatus { Returned, Trapped, Exception, Exhausted, Failed };
struct InvokeOutcome {
  InvokeStatus status = InvokeStatus::Failed;
  std::vector<uint64_t> results;
  GLWASM_Trap trap = GLWASM_TRAP_NONE;
  std::string detail;
};

/* The cross-tier frame differential's instrument for one store. Each
 * invocation runs natively on a fuel allowance drawn from its position in
 * the script, pauses where the allowance runs out (a test seam makes
 * exhausted fuel pause rather than unwind), and is recorded there
 * through the debugger: every frame's identity and scopes, and every
 * memory. Both tiers draw the same allowances and charge the same fuel, so
 * they stop at the same instruction, and the JIT tier's record is rebuilt
 * from compiled frames. The store runs on a torture heap that is collected
 * at every stop with a rooted probe that must survive (and move, when
 * relocation is built in), on a stack that moves at every push. */
class FrameDiff {
 public:
  explicit FrameDiff(uint64_t seed) : state_(seed) {}
  FrameDiff(const FrameDiff &) = delete;
  FrameDiff &operator=(const FrameDiff &) = delete;
  ~FrameDiff() {
    if (rooted_) (void)grheap_root_remove(heap_, &probe_);
  }

  bool attach(GLWASM_Store *store, std::string *why) {
    store_ = store;
    GRCORE_Context *context = glwasm_store_context(store);
    GRHEAP_Options *options = nullptr;
#ifdef GLWASM_SPEC_RELOCATE
    /* The relocation build exists to require it. */
    relocate_ = grheap_relocation_available();
    if (!relocate_) { *why = "relocation is not available"; return false; }
#endif
    const bool heap_made = context &&
        grheap_options_create(nullptr, &options) == GRHEAP_OK && options &&
        grheap_options_set_torture(options, true) == GRHEAP_OK &&
#ifdef GLWASM_SPEC_RELOCATE
        (!relocate_ || grheap_options_set_relocate(options, true) ==
            GRHEAP_OK) &&
#endif
        grheap_heap_create(context, options, &heap_) == GRHEAP_OK && heap_;
    grheap_options_destroy(options);
    if (!heap_made) { *why = "no torture heap"; return false; }
    static const GRHEAP_Type probe_type = GRHEAP_TYPE_INIT(
        "spec-suite-probe", sizeof(uint64_t), nullptr, nullptr, nullptr,
        nullptr, nullptr);
    if (grheap_alloc(heap_, &probe_type, &probe_) != GRHEAP_OK || !probe_) {
      *why = "no probe";
      return false;
    }
    *static_cast<uint64_t *>(probe_) = kProbe;
    if (grheap_root_add(heap_, &probe_) != GRHEAP_OK) {
      *why = "probe not rooted";
      return false;
    }
    rooted_ = true;
    stack_ = grcore_context_stack(context);
    if (!stack_ || grcore_stack_set_always_move(stack_, true) != GRCORE_OK) {
      *why = "stack cannot be made to move";
      return false;
    }
    moves_before_ = grcore_stack_move_count(stack_);
    if (grdbg_debugger_attach(context, nullptr, &debugger_) != GRDBG_OK ||
        !debugger_) {
      *why = "no debugger";
      return false;
    }
    if (glwasm_store_test_pause_on_fuel(store, 1) != GLWASM_OK) {
      *why = "fuel cannot pause";
      return false;
    }
    return true;
  }

  /* Log-uniform up to 2^20, so short invocations stop near their start
   * and long ones are also stopped deep inside. */
  uint64_t next_allowance() {
    const uint64_t r = next();
    return 1u + (next() & ((UINT64_C(1) << (r % 21u)) - 1u));
  }

  /* At a fuel pause: collect, check the probe, and describe the stack. */
  std::string record() {
    ++stops_;
    void *before = probe_;
    if (grheap_collect(heap_) != GRHEAP_OK || !probe_ ||
        *static_cast<uint64_t *>(probe_) != kProbe)
      probe_failed_ = true;
    if (probe_ != before) ++probe_moves_;
    GRDBG_Stop stop{};
    if (grdbg_debugger_stopped(debugger_, &stop) != GRDBG_OK)
      return "<not stopped>";
    size_t total = 0, available = 0;
    if (grdbg_debugger_frames(debugger_, &total, &available) != GRDBG_OK)
      return "<no frames>";
    std::string out = "frames " + std::to_string(total) + "/" +
        std::to_string(available);
    for (size_t f = 0; f < available; ++f) {
      GRDBG_FrameIdentity id = GRDBG_FRAME_IDENTITY_INIT;
      if (grdbg_debugger_frame_identity(debugger_, f, &id) != GRDBG_OK)
        return out + " <identity>";
      /* A function identity's high half is its module's debug id, which
       * each tier's decode allocates afresh; name the module by the order
       * it first appears instead. */
      const auto module = modules_.emplace(id.function >> 32,
          modules_.size()).first->second;
      out += " | m" + std::to_string(module) + ".f" +
          std::to_string(id.function & UINT32_MAX) + "@" +
          std::to_string(id.offset) + " " +
          (id.function_name ? id.function_name : "");
      size_t scopes = 0;
      if (grdbg_debugger_scope_count(debugger_, f, &scopes) != GRDBG_OK)
        return out + " <scopes>";
      for (size_t sc = 0; sc < scopes; ++sc) {
        GRDBG_Scope scope{};
        if (grdbg_debugger_scope(debugger_, f, sc, &scope) != GRDBG_OK)
          return out + " <scope>";
        out += std::string(" [") + scope.name;
        for (size_t v = 0; v < scope.variable_count; ++v) {
          GRDBG_Variable variable{};
          if (grdbg_debugger_variable(debugger_, f, sc, v, &variable) !=
              GRDBG_OK)
            return out + " <variable>";
          out += std::string(" ") + variable.name + "=" + variable.text;
        }
        out += "]";
      }
    }
    size_t memories = 0;
    if (available &&
        grdbg_debugger_memory_count(debugger_, 0, &memories) != GRDBG_OK)
      return out + " <memories>";
    for (size_t m = 0; m < memories; ++m) {
      GRDBG_Memory memory{};
      if (grdbg_debugger_memory(debugger_, 0, m, &memory) != GRDBG_OK)
        return out + " <memory>";
      /* The size, and a digest of the first MiB: every memory the corpus
       * writes to is smaller. */
      uint64_t digest = UINT64_C(0xcbf29ce484222325);
      std::vector<uint8_t> chunk(65536);
      const uint64_t limit = std::min<uint64_t>(memory.size_bytes, 1u << 20);
      for (uint64_t at = 0; at < limit; at += chunk.size()) {
        const size_t n = static_cast<size_t>(
            std::min<uint64_t>(chunk.size(), limit - at));
        if (grdbg_debugger_read_memory(debugger_, 0, m, at, chunk.data(), n) !=
            GRDBG_OK)
          return out + " <memory read>";
        for (size_t i = 0; i < n; ++i)
          digest = (digest ^ chunk[i]) * UINT64_C(0x100000001b3);
      }
      out += " | memory " + std::to_string(memory.size_bytes) + " " +
          hex(digest);
    }
    return out;
  }

  GLWASM_Store *store() const { return store_; }
  uint64_t stops() const { return stops_; }
  /* Did the instruments hold: the probe survived every collection and,
   * with relocation, moved; the stack moved. */
  std::string verdict() const {
    if (probe_failed_) return "the rooted probe did not survive a collection";
    if (relocate_ && stops_ && !probe_moves_)
      return "relocation never moved the rooted probe";
    if (stops_ && grcore_stack_move_count(stack_) <= moves_before_)
      return "the guest stack never moved";
    return {};
  }

 private:
  static constexpr uint64_t kProbe = UINT64_C(0x7370656373756974);
  uint64_t next() { /* splitmix64 */
    uint64_t z = (state_ += UINT64_C(0x9e3779b97f4a7c15));
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
  }
  uint64_t state_;
  GLWASM_Store *store_ = nullptr;
  GRHEAP_Heap *heap_ = nullptr;
  void *probe_ = nullptr;
  bool rooted_ = false, relocate_ = false, probe_failed_ = false;
  GRCORE_Stack *stack_ = nullptr;
  uint64_t moves_before_ = 0, stops_ = 0, probe_moves_ = 0;
  GRDBG_Debugger *debugger_ = nullptr;
  std::map<uint64_t, size_t> modules_;
};

InvokeOutcome invoke(Instance &instance, uint32_t function,
    const std::vector<uint64_t> &args, FrameDiff *diff = nullptr,
    std::string *stop = nullptr) {
  InvokeOutcome outcome;
  const GLWASM_TypeDef &type = instance.module->types[
      instance.module->funcs[function].type_index];
  outcome.results.assign(type.nresults, 0);
  uint32_t count = type.nresults;
  if (diff) (void)glwasm_store_set_fuel(diff->store(), diff->next_allowance());
  GLWASM_Result result = glwasm_runtime_invoke_results(instance.runtime,
      function, args.data(), static_cast<uint32_t>(args.size()),
      outcome.results.data(), &count);
  if (diff) {
    /* One stop per invocation: record it, lift the allowance, finish. */
    *stop = "ran to the end";
    if (result == GLWASM_ERR_PAUSED) {
      *stop = diff->record();
      (void)glwasm_store_set_fuel(diff->store(), UINT64_MAX);
      int paused = 0;
      count = type.nresults;
      result = glwasm_runtime_resume_results(instance.runtime,
          outcome.results.data(), &count, &paused);
      if (result == GLWASM_OK && paused) result = GLWASM_ERR_PAUSED;
    }
    (void)glwasm_store_set_fuel(diff->store(), UINT64_MAX);
  }
  if (result == GLWASM_OK) {
    if (count != type.nresults) {
      outcome.detail = "result count " + std::to_string(count);
      return outcome;
    }
    outcome.status = InvokeStatus::Returned;
    return outcome;
  }
  if (result == GLWASM_ERR_GUEST) {
    outcome.trap = glwasm_store_last_trap(glwasm_runtime_store(instance.runtime));
    outcome.status = outcome.trap == GLWASM_TRAP_NONE ? InvokeStatus::Exception
                                                      : InvokeStatus::Trapped;
    return outcome;
  }
  if (result == GLWASM_ERR_LIMIT) {
    outcome.status = InvokeStatus::Exhausted;
    return outcome;
  }
  outcome.detail = "invocation returned " + std::to_string(result);
  return outcome;
}

/* The spectest module, as the reference interpreter registers it: Wasm
 * objects in the store, and print functions that are typed host imports. */
const char kSpectest[] =
    "(module"
    " (import \"host\" \"print\" (func $print))"
    " (import \"host\" \"print_i32\" (func $print_i32 (param i32)))"
    " (import \"host\" \"print_i64\" (func $print_i64 (param i64)))"
    " (import \"host\" \"print_f32\" (func $print_f32 (param f32)))"
    " (import \"host\" \"print_f64\" (func $print_f64 (param f64)))"
    " (import \"host\" \"print_i32_f32\" (func $print_i32_f32 (param i32 f32)))"
    " (import \"host\" \"print_f64_f64\" (func $print_f64_f64 (param f64 f64)))"
    " (export \"print\" (func $print))"
    " (export \"print_i32\" (func $print_i32))"
    " (export \"print_i64\" (func $print_i64))"
    " (export \"print_f32\" (func $print_f32))"
    " (export \"print_f64\" (func $print_f64))"
    " (export \"print_i32_f32\" (func $print_i32_f32))"
    " (export \"print_f64_f64\" (func $print_f64_f64))"
    " (global (export \"global_i32\") i32 (i32.const 666))"
    " (global (export \"global_i64\") i64 (i64.const 666))"
    " (global (export \"global_f32\") f32 (f32.const 666.6))"
    " (global (export \"global_f64\") f64 (f64.const 666.6))"
    " (table (export \"table\") 10 20 funcref)"
    " (table (export \"table64\") i64 10 20 funcref)"
    " (memory (export \"memory\") 1 2))";

GLWASM_Result spectest_print(const GLWASM_HostCall *, uint64_t *) {
  return GLWASM_OK;
}

/* The JIT tier's account of one instance: what compiled, what was refused,
 * and which opcodes exited, checked against the named rules. */
struct JitAccount {
  uint64_t compiled_pauses = 0; /* differential stops inside compiled code */
  uint64_t functions_executed = 0;
  uint64_t functions_compiled = 0;
  uint64_t compiled_entries = 0;
  uint64_t compiled_returns = 0;
  std::map<std::string, uint64_t> refusals_by_rule;
  std::map<std::string, uint64_t> exits_by_rule;
};

void audit_jit(Instance &instance, const JitRules &rules, JitAccount &account,
    std::vector<std::string> &failures, const std::string &where) {
  GLWASM_Module *module = instance.module;
  std::vector<uint8_t> observed(module->nfuncs);
  uint32_t required = 0;
  if (glwasm_runtime_test_observed_functions(instance.runtime,
          observed.empty() ? nullptr : observed.data(),
          static_cast<uint32_t>(observed.size()), &required) != GLWASM_OK ||
      required != module->nfuncs) {
    failures.push_back(where + ": JIT observations unavailable");
    return;
  }
  for (uint32_t f = module->nimported_funcs; f < module->nfuncs; ++f) {
    GLWASM_JitCacheStats stats{};
    if (glwasm_runtime_test_jit_stats(instance.runtime, f, &stats) != GLWASM_OK)
      continue;
    account.compiled_entries += stats.compiled_entries;
    account.compiled_returns += stats.exit_counts[GLWASM_JIT_EXIT_RETURN];
    if (!observed[f]) continue;
    ++account.functions_executed;
    if (stats.compile_successes) ++account.functions_compiled;
    for (int reason = 0; reason < GLWASM_JIT_REFUSAL_COUNT; ++reason) {
      if (!stats.refusal_counts[reason]) continue;
      if (reason == GLWASM_JIT_REFUSAL_UNSUPPORTED_IR) {
        const auto rule = rules.refusals.find(stats.last_refused_opcode);
        if (rule == rules.refusals.end()) {
          failures.push_back(where + ": function " + std::to_string(f) +
              " refused for opcode " + hex(stats.last_refused_opcode) +
              " with no named rule");
          continue;
        }
        account.refusals_by_rule[rule->second] += stats.refusal_counts[reason];
        continue;
      }
      if (reason == GLWASM_JIT_REFUSAL_RETIRED_EXIT_RATE &&
          !rules.retirement.empty()) {
        account.refusals_by_rule[rules.retirement] +=
            stats.refusal_counts[reason];
        continue;
      }
      static const char *const names[] = {"none", "backend unavailable",
          "unsupported IR", "resource limit", "profitability",
          "retired by exit rate"};
      failures.push_back(where + ": function " + std::to_string(f) +
          " refused: " + names[reason]);
    }
    if (!stats.compile_successes && !stats.refusal_counts[
            GLWASM_JIT_REFUSAL_UNSUPPORTED_IR])
      failures.push_back(where + ": function " + std::to_string(f) +
          " ran without compiling and without a refusal");
  }
}

void audit_jit_exits(Instance &instance, const JitRules &rules,
    JitAccount &account, std::vector<std::string> &failures,
    const std::string &where, std::map<uint16_t, uint64_t> &seen) {
  auto check = [&](uint16_t opcode) {
    const uint64_t count =
        glwasm_runtime_test_jit_opcode_exit_count(instance.runtime, opcode);
    const uint64_t before = seen[opcode];
    seen[opcode] = count;
    if (count <= before) return;
    const auto rule = rules.exits.find(opcode);
    if (rule == rules.exits.end()) {
      failures.push_back(where + ": " + std::to_string(count - before) +
          " exits at opcode " + hex(opcode) + " with no named rule");
      return;
    }
    account.exits_by_rule[rule->second] += count - before;
  };
  for (uint16_t op = 0; op <= 0xff; ++op) check(op);
  for (uint16_t sub = 0; sub <= 0x11; ++sub) check(0xfc00u | sub);
}

/* ------------------------------------------------------------------ */
/* Script execution                                                    */
/* ------------------------------------------------------------------ */

enum class Verdict { Pass, Fail, Skip };
struct CommandResult {
  Verdict verdict = Verdict::Fail;
  std::string detail;     /* failure message, or the skip reason */
  std::vector<uint64_t> values; /* plain invoke results, compared across tiers */
};

/* Valgrind's VEX converts an i64 to f32 through binary64, rounding twice, so
 * a few spec conversions come out one ulp off under Memcheck in both tiers.
 * test-valgrind asks for an accommodation by GLWASM_MEMCHECK_F32_I64_DOUBLE_ROUNDING=1,
 * honoured only under a live Valgrind client: a result that is exactly the
 * double-rounded conversion of the one i64 argument, where the script expects
 * something else, is counted and not failed, and the count must be the
 * corpus's (as runtime-jit's differential does for its one u64 input). */
constexpr uint64_t kMemcheckDoubleRoundedCommands = 8;
bool running_under_valgrind() {
#if defined(GLWASM_TEST_HAS_VALGRIND_CLIENT)
  return RUNNING_ON_VALGRIND != 0;
#else
  return false;
#endif
}
bool memcheck_double_rounding_requested() {
  const char *value = std::getenv("GLWASM_MEMCHECK_F32_I64_DOUBLE_ROUNDING");
  return value != nullptr && std::strcmp(value, "1") == 0;
}
/* Whether `actual` is an i64 -> f32 conversion of `arg` rounded through
 * binary64, signed or unsigned. */
bool double_rounded_f32_of_i64(uint64_t arg, uint64_t actual) {
  const float through_signed = static_cast<float>(
      static_cast<double>(static_cast<int64_t>(arg)));
  const float through_unsigned = static_cast<float>(static_cast<double>(arg));
  uint32_t a = 0, b = 0;
  std::memcpy(&a, &through_signed, sizeof a);
  std::memcpy(&b, &through_unsigned, sizeof b);
  return (uint32_t)actual == a || (uint32_t)actual == b;
}

struct Counts {
  std::map<std::string, uint64_t> passed;   /* by command kind */
  std::map<std::string, uint64_t> skipped;  /* by reason */
  uint64_t failed = 0;
  uint64_t memcheck_double_rounded = 0;
};

const char *command_name(GLWASM_CommandKind kind) {
  switch (kind) {
    case GLWASM_COMMAND_MODULE_TEXT: return "module";
    case GLWASM_COMMAND_MODULE_BINARY: return "module binary";
    case GLWASM_COMMAND_MODULE_QUOTE: return "module quote";
    case GLWASM_COMMAND_MODULE_DEFINITION: return "module definition";
    case GLWASM_COMMAND_MODULE_INSTANCE: return "module instance";
    case GLWASM_COMMAND_MODULE_FIELD: return "module field";
    case GLWASM_COMMAND_REGISTER: return "register";
    case GLWASM_COMMAND_INVOKE: return "invoke";
    case GLWASM_COMMAND_GET: return "get";
    case GLWASM_COMMAND_ASSERT_RETURN: return "assert_return";
    case GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN:
      return "assert_return_canonical_nan";
    case GLWASM_COMMAND_ASSERT_RETURN_ARITHMETIC_NAN:
      return "assert_return_arithmetic_nan";
    case GLWASM_COMMAND_ASSERT_TRAP: return "assert_trap";
    case GLWASM_COMMAND_ASSERT_EXHAUSTION: return "assert_exhaustion";
    case GLWASM_COMMAND_ASSERT_EXCEPTION: return "assert_exception";
    case GLWASM_COMMAND_ASSERT_MALFORMED: return "assert_malformed";
    case GLWASM_COMMAND_ASSERT_INVALID: return "assert_invalid";
    case GLWASM_COMMAND_ASSERT_UNLINKABLE: return "assert_unlinkable";
    case GLWASM_COMMAND_ASSERT_UNINSTANTIABLE: return "assert_uninstantiable";
    case GLWASM_COMMAND_ASSERT_SUSPENSION: return "assert_suspension";
    case GLWASM_COMMAND_ASSERT_MALFORMED_CUSTOM: return "assert_malformed_custom";
    case GLWASM_COMMAND_ASSERT_INVALID_CUSTOM: return "assert_invalid_custom";
  }
  return "unknown";
}

/* The suite decides malformed and invalid by the format alone, so every
 * structural cap is lifted: a cap would turn "too many locals" into a limit
 * verdict instead of the malformation the suite asserts. */
GLWASM_Limits suite_limits() {
  GLWASM_Limits limits{};
  limits.module_bytes = std::numeric_limits<uint64_t>::max();
  limits.types = limits.imports = limits.exports = limits.functions =
      limits.globals = limits.tables = limits.memories = limits.tags =
      limits.data_segments = limits.element_segments = limits.locals =
      limits.instructions = limits.nesting = limits.br_table =
      limits.name_length = std::numeric_limits<uint32_t>::max();
  limits.table_size = limits.memory_maximum = limits.memory_initial =
      std::numeric_limits<uint64_t>::max();
  return limits;
}

/* Every in-scope feature, no structural cap, no fuel limit, and the
 * suite's call depth. Modules carry it, and their instances' budgets come
 * from it. */
GLWASM_Policy suite_policy(Tier tier) {
  GLWASM_Policy policy{};
  glwasm_policy_preset(GLWASM_POLICY_TRUSTED, &policy);
  policy.limits = suite_limits();
  policy.features = GLWASM_FEATURE_TAIL_CALLS | GLWASM_FEATURE_EXCEPTIONS |
      GLWASM_FEATURE_MEMORY64 | GLWASM_FEATURE_TABLE64 |
      GLWASM_FEATURE_MULTI_MEMORY | GLWASM_FEATURE_BULK_MEMORY |
      GLWASM_FEATURE_REFERENCE_TYPES | GLWASM_FEATURE_EXTENDED_CONST;
  policy.default_fuel = UINT64_MAX;
  policy.default_guest_depth = kGuestDepth;
  /* A finite budget, as every real store has: the suite's growth tests
   * expect huge requests to fail, and without one they would be granted. */
  policy.default_memory_bytes = UINT64_C(4) << 30;
  /* The interpreter tier is a module that has no compiled code at all. */
  if (tier == Tier::Interpreter) policy.max_jit_code_bytes = 0;
  return policy;
}

/* A module form of any spelling, decoded to a validated module. */
struct ModuleForm {
  std::string id;          /* $name, or empty */
  bool definition = false; /* (module definition ...) is not instantiated */
  enum class Encoding { Text, Binary, Quote } encoding = Encoding::Text;
  std::string payload;     /* text for Text/Quote, bytes for Binary */
};

bool read_module_form(const std::string &source, const GLWASM_Text *syntax,
    ModuleForm &form, std::string *why) {
  size_t next = 1;
  const GLWASM_Text *child = semantic_child(syntax, next);
  if (atom_is(child, "definition")) { form.definition = true; child = semantic_child(syntax, ++next); }
  if (child && glwasm_text_kind(child) == GLWASM_TEXT_ATOM &&
      text_bytes(child).rfind("$", 0) == 0) {
    form.id = text_bytes(child);
    child = semantic_child(syntax, ++next);
  }
  if (atom_is(child, "binary") || atom_is(child, "quote")) {
    const bool binary = atom_is(child, "binary");
    form.encoding = binary ? ModuleForm::Encoding::Binary
                           : ModuleForm::Encoding::Quote;
    for (size_t i = next + 1; i < semantic_size(syntax); ++i) {
      const GLWASM_Text *part = semantic_child(syntax, i);
      if (glwasm_text_kind(part) != GLWASM_TEXT_STRING) {
        *why = "module string expected";
        return false;
      }
      form.payload += text_bytes(part);
    }
    if (!binary && form.payload.find("(module") == std::string::npos)
      form.payload = "(module " + form.payload + ")";
    else if (!binary) {
      /* A quoted module may be a whole (module ...) form or its fields. */
      const size_t at = form.payload.find_first_not_of(" \t\r\n");
      if (at == std::string::npos || form.payload.compare(at, 7, "(module") != 0)
        form.payload = "(module " + form.payload + ")";
    }
    return true;
  }
  form.payload = source_form_at(source, glwasm_text_location(syntax));
  if (form.payload.empty()) {
    *why = "could not recover the module's source form";
    return false;
  }
  if (form.definition) {
    const size_t at = form.payload.find("definition");
    if (at != std::string::npos) form.payload.erase(at, 10);
  }
  return true;
}

/* `verbatim` keeps annotations in a quoted module for malformation checks;
 * a module that will be instantiated is lowered with the suite policy. */
GLWASM_Result decode_module_form(const ModuleForm &form, Tier tier,
    GLWASM_Module **out, std::string *why, bool verbatim = false) {
  const GLWASM_Limits limits = suite_limits();
  const GLWASM_Policy policy = suite_policy(tier);
  const auto *bytes = reinterpret_cast<const uint8_t *>(form.payload.data());
  if (form.encoding == ModuleForm::Encoding::Binary) {
    GLWASM_Failure failure{};
    const GLWASM_Result result = glwasm_module_decode_with_policy(bytes,
        form.payload.size(), &policy, nullptr, &failure, out);
    if (result != GLWASM_OK)
      *why = "decode returned " + std::to_string(result) +
          (failure.limit ? std::string(" limit ") + failure.limit : std::string());
    return result;
  }
  GLWASM_ParseError error{};
  const GLWASM_Result result =
      verbatim && form.encoding == ModuleForm::Encoding::Quote
      ? glwasm_wat_module_parse_verbatim(bytes, form.payload.size(), &limits,
            nullptr, &error, out)
      : glwasm_wat_module_parse_with_policy(bytes, form.payload.size(),
            &policy, nullptr, &error, out);
  if (result != GLWASM_OK)
    *why = "text module returned " + std::to_string(result) + " at " +
        std::to_string(error.location.line) + ":" +
        std::to_string(error.location.column) + ": " +
        (error.message ? error.message : "");
  return result;
}

class ScriptRun {
 public:
  ScriptRun(const std::string &relative, const std::string &source,
      const std::map<uint32_t, std::string> &line_skips, Tier tier,
      const JitRules &rules, JitAccount &jit,
      std::vector<std::string> &failures,
      std::vector<std::string> *stops = nullptr, uint64_t seed = 0)
      : relative_(relative), source_(source), line_skips_(line_skips),
        tier_(tier), rules_(rules), jit_(jit), failures_(failures),
        stops_(stops) {
    const GLWASM_Policy policy = suite_policy(tier);
    if (glwasm_store_create(&policy, &store_) != GLWASM_OK) {
      failures_.push_back(relative_ + ": could not create a store");
      return;
    }
    /* With a stop list, this is a frame-differential run. */
    if (stops_) {
      diff_ = std::make_unique<FrameDiff>(seed);
      std::string why;
      if (!diff_->attach(store_, &why)) {
        failures_.push_back(relative_ + ": frame differential: " + why);
        diff_.reset();
        return;
      }
    }
    /* spectest goes first, registered under its name. */
    static const GLWASM_HostValueType i32[] = {GLWASM_HOST_I32},
        i64[] = {GLWASM_HOST_I64}, f32[] = {GLWASM_HOST_F32},
        f64[] = {GLWASM_HOST_F64}, i32_f32[] = {GLWASM_HOST_I32, GLWASM_HOST_F32},
        f64_f64[] = {GLWASM_HOST_F64, GLWASM_HOST_F64};
    struct { const char *field; const GLWASM_HostValueType *types; uint32_t n; }
        prints[] = {{"print", nullptr, 0}, {"print_i32", i32, 1},
            {"print_i64", i64, 1}, {"print_f32", f32, 1},
            {"print_f64", f64, 1}, {"print_i32_f32", i32_f32, 2},
            {"print_f64_f64", f64_f64, 2}};
    std::vector<GLWASM_HostImport> host;
    for (const auto &print : prints) {
      GLWASM_HostImport binding{};
      binding.module = "host";
      binding.field = print.field;
      binding.kind = GLWASM_HOST_IMPORT_FUNCTION;
      binding.function = spectest_print;
      binding.parameters = print.types;
      binding.parameter_count = print.n;
      host.push_back(binding);
    }
    ModuleForm form;
    form.payload = kSpectest;
    std::string why;
    auto spectest = std::make_unique<Instance>();
    GLWASM_InstantiateError error{};
    if (decode_module_form(form, tier_, &spectest->module, &why) != GLWASM_OK ||
        glwasm_store_instantiate(store_, spectest->module, host.data(),
            host.size(), 1, &error, &spectest->runtime) != GLWASM_OK ||
        glwasm_runtime_test_reset_observed_functions(spectest->runtime) !=
            GLWASM_OK) {
      failures_.push_back(relative_ + ": spectest did not instantiate " + why);
      return;
    }
    registered_["spectest"] = spectest.get();
    instances_.push_back(std::move(spectest));
  }

  ~ScriptRun() {
    for (auto &instance : instances_) finish_instance(*instance);
    instances_.clear();
    if (diff_) {
      const std::string verdict = diff_->verdict();
      if (!verdict.empty())
        failures_.push_back(relative_ + " (" + tier_name(tier_) + "): " +
            verdict);
      diff_.reset(); /* its root lives in the store's heap */
    }
    glwasm_store_destroy(store_);
  }

  CommandResult run(const GLWASM_Command &command) {
    line_ = command.location.line;
    const auto skip = line_skips_.find(command.location.line);
    if (skip != line_skips_.end()) {
      used_skips_.insert(command.location.line);
      if (is_module_command(command.kind)) mark_skipped(command, skip->second);
      return {Verdict::Skip, skip->second, {}};
    }
    switch (command.kind) {
      case GLWASM_COMMAND_MODULE_TEXT:
      case GLWASM_COMMAND_MODULE_BINARY:
      case GLWASM_COMMAND_MODULE_QUOTE:
      case GLWASM_COMMAND_MODULE_DEFINITION:
        return define_module(command);
      case GLWASM_COMMAND_MODULE_INSTANCE: return instance_command(command);
      case GLWASM_COMMAND_REGISTER: return register_command(command);
      case GLWASM_COMMAND_INVOKE: return invoke_command(command.syntax);
      case GLWASM_COMMAND_GET: return get_command(command.syntax, nullptr);
      case GLWASM_COMMAND_ASSERT_RETURN:
      case GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN:
      case GLWASM_COMMAND_ASSERT_RETURN_ARITHMETIC_NAN:
        return assert_return(command);
      case GLWASM_COMMAND_ASSERT_TRAP: return assert_trap(command);
      case GLWASM_COMMAND_ASSERT_EXHAUSTION: return assert_exhaustion(command);
      case GLWASM_COMMAND_ASSERT_EXCEPTION: return assert_exception(command);
      case GLWASM_COMMAND_ASSERT_MALFORMED:
      case GLWASM_COMMAND_ASSERT_INVALID:
        return assert_rejected(command);
      case GLWASM_COMMAND_ASSERT_UNLINKABLE: return assert_unlinkable(command);
      case GLWASM_COMMAND_ASSERT_UNINSTANTIABLE:
        return assert_uninstantiable(command);
      default:
        return fail(std::string(command_name(command.kind)) +
            " is not a core command");
    }
  }

  /* Top-level module fields with no (module ...) around them form one
   * implicit module, as in inline-module.wast. */
  CommandResult inline_module(const std::string &fields) {
    ModuleForm form;
    form.payload = "(module " + fields + ")";
    return instantiate_form(form, {});
  }

  std::set<uint32_t> used_skips_;
  std::vector<uint64_t> call_args_;
  uint32_t call_arg_types_ = 0;  /* how many of the arguments are i64 */
  bool memcheck_double_rounding_ = false;
  uint64_t memcheck_double_rounded_ = 0;

 private:
  static bool is_module_command(GLWASM_CommandKind kind) {
    return kind == GLWASM_COMMAND_MODULE_TEXT ||
        kind == GLWASM_COMMAND_MODULE_BINARY ||
        kind == GLWASM_COMMAND_MODULE_QUOTE ||
        kind == GLWASM_COMMAND_MODULE_DEFINITION ||
        kind == GLWASM_COMMAND_MODULE_INSTANCE;
  }

  static CommandResult fail(std::string detail) {
    return {Verdict::Fail, std::move(detail), {}};
  }
  static CommandResult pass() { return {Verdict::Pass, {}, {}}; }

  /* A skipped module stands for its deferred feature: a command that uses
   * it is skipped for the same reason, never run against a stale module. */
  struct Slot {
    Instance *instance = nullptr;
    std::string skipped;
  };

  void mark_skipped(const GLWASM_Command &command, const std::string &reason) {
    ModuleForm form;
    std::string why;
    (void)read_module_form(source_, command.syntax, form, &why);
    if (command.kind == GLWASM_COMMAND_MODULE_INSTANCE) {
      const GLWASM_Text *id = semantic_child(command.syntax, 2);
      form.id = id ? text_bytes(id) : std::string();
    }
    Slot slot{nullptr, reason};
    if (!form.definition) current_ = slot;
    if (!form.id.empty()) {
      if (form.definition) skipped_definitions_[form.id] = reason;
      else named_[form.id] = slot;
    }
  }

  CommandResult define_module(const GLWASM_Command &command) {
    ModuleForm form;
    std::string why;
    if (!read_module_form(source_, command.syntax, form, &why)) return fail(why);
    if (form.definition) {
      GLWASM_Module *module = nullptr;
      if (decode_module_form(form, tier_, &module, &why) != GLWASM_OK)
        return fail(why);
      glwasm_module_destroy(module);
      if (!form.id.empty()) definitions_[form.id] = form;
      return pass();
    }
    return instantiate_form(form, form.id);
  }

  CommandResult instance_command(const GLWASM_Command &command) {
    const GLWASM_Text *instance_id = semantic_child(command.syntax, 2);
    const GLWASM_Text *module_id = semantic_child(command.syntax, 3);
    std::string name = instance_id ? text_bytes(instance_id) : std::string();
    std::string definition = module_id ? text_bytes(module_id) : name;
    if (!module_id) { definition = name; name.clear(); }
    if (skipped_definitions_.count(definition)) {
      Slot slot{nullptr, skipped_definitions_[definition]};
      current_ = slot;
      if (!name.empty()) named_[name] = slot;
      return {Verdict::Skip, slot.skipped, {}};
    }
    const auto found = definitions_.find(definition);
    if (found == definitions_.end())
      return fail("unknown module definition " + definition);
    return instantiate_form(found->second, name);
  }

  CommandResult instantiate_form(const ModuleForm &form,
      const std::string &name) {
    std::string why;
    GLWASM_Module *module = nullptr;
    if (decode_module_form(form, tier_, &module, &why) != GLWASM_OK) return fail(why);
    auto instance = std::make_unique<Instance>();
    instance->module = module;
    const InstantiateOutcome outcome = instantiate(store_, module,
        registered_, skipped_registrations_, *instance);
    if (outcome.status == InstantiateStatus::Skipped) {
      Slot slot{nullptr, outcome.detail};
      current_ = slot;
      if (!name.empty()) named_[name] = slot;
      return {Verdict::Skip, outcome.detail, {}};
    }
    if (outcome.status != InstantiateStatus::Ok) {
      current_ = Slot{nullptr, {}};
      return fail("instantiation failed (" + outcome.detail + ")");
    }
    Slot slot{instance.get(), {}};
    current_ = slot;
    if (!name.empty()) named_[name] = slot;
    instances_.push_back(std::move(instance));
    return pass();
  }

  CommandResult register_command(const GLWASM_Command &command) {
    const GLWASM_Text *as = semantic_child(command.syntax, 1);
    const GLWASM_Text *id = semantic_child(command.syntax, 2);
    Slot slot = current_;
    if (id) {
      const auto found = named_.find(text_bytes(id));
      if (found == named_.end())
        return fail("register names unknown module " + text_bytes(id));
      slot = found->second;
    }
    if (!slot.skipped.empty()) {
      skipped_registrations_[text_bytes(as)] = slot.skipped;
      return {Verdict::Skip, slot.skipped, {}};
    }
    if (!slot.instance) return fail("register has no instance");
    registered_[text_bytes(as)] = slot.instance;
    return pass();
  }

  /* The instance an (invoke ...) or (get ...) names. */
  bool target(const GLWASM_Text *action, size_t *next, Slot *slot,
      std::string *why) {
    const GLWASM_Text *first = semantic_child(action, 1);
    *slot = current_;
    *next = 1;
    if (first && glwasm_text_kind(first) == GLWASM_TEXT_ATOM &&
        text_bytes(first).rfind("$", 0) == 0) {
      const auto found = named_.find(text_bytes(first));
      if (found == named_.end()) {
        *why = "unknown module " + text_bytes(first);
        return false;
      }
      *slot = found->second;
      *next = 2;
    }
    if (slot->skipped.empty() && !slot->instance) {
      *why = "no module instance";
      return false;
    }
    return true;
  }

  struct Call {
    Instance *instance = nullptr;
    uint32_t function = 0;
    std::vector<uint64_t> args;
    const GLWASM_TypeDef *type = nullptr;
    std::string skipped;
  };

  /* Every invocation goes through here, so a differential run stops in
   * each one and records where, by script line. */
  InvokeOutcome call_invoke(const Call &call) {
    if (!diff_) return invoke(*call.instance, call.function, call.args);
    std::string stop;
    InvokeOutcome outcome = invoke(*call.instance, call.function, call.args,
        diff_.get(), &stop);
    stops_->push_back(std::to_string(line_) + ": " + stop);
    return outcome;
  }

  bool prepare_call(const GLWASM_Text *action, Call *call, std::string *why) {
    if (!action || glwasm_text_kind(action) != GLWASM_TEXT_LIST ||
        !atom_is(semantic_child(action, 0), "invoke")) {
      *why = "expected (invoke ...)";
      return false;
    }
    Slot slot;
    size_t next = 1;
    if (!target(action, &next, &slot, why)) return false;
    if (!slot.skipped.empty()) { call->skipped = slot.skipped; return true; }
    Instance &instance = *slot.instance;
    const std::string name = text_bytes(semantic_child(action, next));
    uint32_t function = UINT32_MAX;
    for (uint32_t i = 0; i < instance.module->nexports; ++i) {
      const GLWASM_Export &e = instance.module->exports[i];
      if (e.kind == GLWASM_EXT_FUNC &&
          std::string(e.name.bytes, e.name.length) == name) {
        function = e.index;
        break;
      }
    }
    if (function == UINT32_MAX) {
      *why = "no exported function \"" + name + "\"";
      return false;
    }
    const GLWASM_TypeDef &type = instance.module->types[
        instance.module->funcs[function].type_index];
    const size_t given = semantic_size(action) - next - 1;
    if (given != type.nparams) {
      *why = "invoke of \"" + name + "\" passes " + std::to_string(given) +
          " arguments for " + std::to_string(type.nparams) + " parameters";
      return false;
    }
    call->args.assign(type.nparams, 0);
    for (uint32_t i = 0; i < type.nparams; ++i)
      if (!parse_argument(semantic_child(action, next + 1 + i), type.params[i],
              &call->args[i], why))
        return false;
    call->instance = &instance;
    call->function = function;
    call->type = &type;
    return true;
  }

  CommandResult invoke_command(const GLWASM_Text *action) {
    Call call;
    std::string why;
    if (!prepare_call(action, &call, &why)) return fail(why);
    if (!call.skipped.empty()) return {Verdict::Skip, call.skipped, {}};
    const InvokeOutcome outcome = call_invoke(call);
    if (outcome.status != InvokeStatus::Returned)
      return fail("invoke did not return: " + describe(outcome));
    return {Verdict::Pass, {}, outcome.results};
  }

  CommandResult get_command(const GLWASM_Text *action,
      std::vector<uint64_t> *values) {
    Slot slot;
    size_t next = 1;
    std::string why;
    if (!target(action, &next, &slot, &why)) return fail(why);
    if (!slot.skipped.empty()) return {Verdict::Skip, slot.skipped, {}};
    const std::string name = text_bytes(semantic_child(action, next));
    uint64_t value = 0;
    if (!read_global(*slot.instance, name, &value))
      return fail("no readable exported global \"" + name + "\"");
    if (values) values->assign(1, value);
    return {Verdict::Pass, {}, {value}};
  }

  static std::string describe(const InvokeOutcome &outcome) {
    switch (outcome.status) {
      case InvokeStatus::Returned: return "returned";
      case InvokeStatus::Trapped:
        return std::string("trapped: ") + trap_message(outcome.trap);
      case InvokeStatus::Exception: return "uncaught exception";
      case InvokeStatus::Exhausted: return "resource limit";
      case InvokeStatus::Failed: return "failed: " + outcome.detail;
    }
    return "?";
  }

  CommandResult assert_return(const GLWASM_Command &command) {
    const GLWASM_Text *action = semantic_child(command.syntax, 1);
    std::vector<uint64_t> actual;
    std::vector<GLWASM_ValType> types;
    std::string why;
    if (atom_is(semantic_child(action, 0), "get")) {
      Slot slot;
      size_t next = 1;
      if (!target(action, &next, &slot, &why)) return fail(why);
      if (!slot.skipped.empty()) return {Verdict::Skip, slot.skipped, {}};
      const std::string name = text_bytes(semantic_child(action, next));
      uint64_t value = 0;
      GLWASM_ValType type{};
      if (!read_global(*slot.instance, name, &value) ||
          !global_type(*slot.instance, name, &type))
        return fail("no readable exported global \"" + name + "\"");
      actual.push_back(value);
      types.push_back(type);
    } else {
      Call call;
      if (!prepare_call(action, &call, &why)) return fail(why);
      if (!call.skipped.empty()) return {Verdict::Skip, call.skipped, {}};
      const InvokeOutcome outcome = call_invoke(call);
      if (outcome.status != InvokeStatus::Returned)
        return fail("expected a return, " + describe(outcome));
      call_args_ = call.args;
      call_arg_types_ = 0;
      for (uint32_t k = 0; k < call.type->nparams; ++k)
        if (call.type->params[k].kind == GLWASM_VK_I64) ++call_arg_types_;
      actual = outcome.results;
      types.assign(call.type->results, call.type->results + call.type->nresults);
    }
    std::vector<ExpectedResult> expected;
    if (command.kind != GLWASM_COMMAND_ASSERT_RETURN) {
      /* The legacy NaN assertions name one float result's class. */
      if (types.size() != 1 || (types[0].kind != GLWASM_VK_F32 &&
                                types[0].kind != GLWASM_VK_F64))
        return fail("NaN assertion on a non-float result");
      ExpectedValue value;
      value.kind = types[0].kind == GLWASM_VK_F32 ? ExpectedValue::Kind::Nan32
                                                  : ExpectedValue::Kind::Nan64;
      value.nan = command.kind == GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN
          ? wast::NanPattern::Canonical : wast::NanPattern::Arithmetic;
      expected.push_back({value});
    } else {
      for (size_t i = 2; i < semantic_size(command.syntax); ++i) {
        const GLWASM_Text *node = semantic_child(command.syntax, i);
        ExpectedResult result;
        if (atom_is(semantic_child(node, 0), "either")) {
          for (size_t j = 1; j < semantic_size(node); ++j) {
            ExpectedValue value;
            if (!parse_expected_value(semantic_child(node, j), &value, &why))
              return fail(why);
            result.push_back(value);
          }
        } else {
          ExpectedValue value;
          if (!parse_expected_value(node, &value, &why)) return fail(why);
          result.push_back(value);
        }
        expected.push_back(std::move(result));
      }
    }
    if (expected.size() != actual.size())
      return fail("expected " + std::to_string(expected.size()) +
          " results, got " + std::to_string(actual.size()));
    for (size_t i = 0; i < actual.size(); ++i) {
      bool matched = false;
      for (const ExpectedValue &value : expected[i])
        matched = matched || value_matches(actual[i], types[i], value);
      if (!matched && memcheck_double_rounding_ && actual.size() == 1 &&
          types[0].kind == GLWASM_VK_F32 && call_args_.size() == 1 &&
          call_arg_types_ == 1 && expected[0].size() == 1 &&
          expected[0].front().bits != actual[0] &&
          double_rounded_f32_of_i64(call_args_[0], actual[0])) {
        ++memcheck_double_rounded_;
        matched = true;
      }
      if (!matched)
        return fail("result " + std::to_string(i) + " is " + hex(actual[i]) +
            ", expected " + hex(expected[i].front().bits));
    }
    return pass();
  }

  static bool read_global(const Instance &instance, const std::string &name,
      uint64_t *value) {
    GLWASM_Extern object{};
    return glwasm_runtime_export(instance.runtime, name.data(), name.size(),
               &object) == GLWASM_OK &&
        glwasm_store_global_get(glwasm_runtime_store(instance.runtime), object,
            value) == GLWASM_OK;
  }

  static bool global_type(const Instance &instance, const std::string &name,
      GLWASM_ValType *type) {
    for (uint32_t i = 0; i < instance.module->nexports; ++i) {
      const GLWASM_Export &e = instance.module->exports[i];
      if (e.kind == GLWASM_EXT_GLOBAL &&
          std::string(e.name.bytes, e.name.length) == name &&
          e.index < instance.module->nglobals) {
        *type = instance.module->globals[e.index].type;
        return true;
      }
    }
    return false;
  }

  /* (assert_trap (invoke ...) msg) or (assert_trap (module ...) msg). */
  CommandResult assert_trap(const GLWASM_Command &command) {
    const GLWASM_Text *action = semantic_child(command.syntax, 1);
    const std::string expected = text_bytes(semantic_child(command.syntax, 2));
    if (atom_is(semantic_child(action, 0), "module"))
      return expect_instantiation(action, InstantiateStatus::Trap, expected);
    Call call;
    std::string why;
    if (!prepare_call(action, &call, &why)) return fail(why);
    if (!call.skipped.empty()) return {Verdict::Skip, call.skipped, {}};
    const InvokeOutcome outcome = call_invoke(call);
    if (outcome.status != InvokeStatus::Trapped)
      return fail("expected trap \"" + expected + "\", " + describe(outcome));
    if (!trap_matches(outcome.trap, expected))
      return fail("expected trap \"" + expected + "\", trapped: " +
          trap_message(outcome.trap));
    return pass();
  }

  CommandResult assert_exhaustion(const GLWASM_Command &command) {
    Call call;
    std::string why;
    if (!prepare_call(semantic_child(command.syntax, 1), &call, &why))
      return fail(why);
    if (!call.skipped.empty()) return {Verdict::Skip, call.skipped, {}};
    const InvokeOutcome outcome = call_invoke(call);
    if (outcome.status != InvokeStatus::Exhausted)
      return fail("expected call stack exhaustion, " + describe(outcome));
    return pass();
  }

  CommandResult assert_exception(const GLWASM_Command &command) {
    Call call;
    std::string why;
    if (!prepare_call(semantic_child(command.syntax, 1), &call, &why))
      return fail(why);
    if (!call.skipped.empty()) return {Verdict::Skip, call.skipped, {}};
    const InvokeOutcome outcome = call_invoke(call);
    if (outcome.status != InvokeStatus::Exception)
      return fail("expected an uncaught exception, " + describe(outcome));
    return pass();
  }

  CommandResult assert_unlinkable(const GLWASM_Command &command) {
    return expect_instantiation(semantic_child(command.syntax, 1),
        InstantiateStatus::Unlinkable, {});
  }

  CommandResult assert_uninstantiable(const GLWASM_Command &command) {
    return expect_instantiation(semantic_child(command.syntax, 1),
        InstantiateStatus::Trap, {});
  }

  CommandResult expect_instantiation(const GLWASM_Text *module_syntax,
      InstantiateStatus wanted, const std::string &trap) {
    ModuleForm form;
    std::string why;
    if (!read_module_form(source_, module_syntax, form, &why)) return fail(why);
    GLWASM_Module *module = nullptr;
    if (decode_module_form(form, tier_, &module, &why) != GLWASM_OK) return fail(why);
    auto instance = std::make_unique<Instance>();
    instance->module = module;
    const InstantiateOutcome outcome = instantiate(store_, module,
        registered_, skipped_registrations_, *instance);
    if (outcome.status == InstantiateStatus::Skipped)
      return {Verdict::Skip, outcome.detail, {}};
    if (outcome.status != wanted)
      return fail(std::string("instantiation outcome ") +
          (outcome.status == InstantiateStatus::Ok ? "succeeded"
           : outcome.status == InstantiateStatus::Unlinkable ? "unlinkable"
           : outcome.status == InstantiateStatus::Trap ? "trapped"
           : "failed") + (outcome.detail.empty() ? "" : " (" + outcome.detail + ")"));
    if (wanted == InstantiateStatus::Trap && !trap.empty() &&
        !trap_matches(outcome.trap, trap))
      return fail("expected instantiation trap \"" + trap + "\", trapped: " +
          trap_message(outcome.trap));
    if (outcome.status == InstantiateStatus::Ok)
      instances_.push_back(std::move(instance));
    return pass();
  }

  /* assert_malformed and assert_invalid: decode or lower must reject the
   * module with the matching category, and never instantiate it. */
  CommandResult assert_rejected(const GLWASM_Command &command) {
    ModuleForm form;
    std::string why;
    if (!read_module_form(source_, semantic_child(command.syntax, 1), form,
            &why))
      return fail(why);
    GLWASM_Module *module = nullptr;
    const GLWASM_Result result = decode_module_form(form, tier_, &module, &why, true);
    if (module) glwasm_module_destroy(module);
    const GLWASM_Result wanted = command.kind == GLWASM_COMMAND_ASSERT_MALFORMED
        ? GLWASM_ERR_CORRUPT : GLWASM_ERR_VALIDATE;
    if (result != wanted)
      return fail(std::string(command_name(command.kind)) + " got result " +
          std::to_string(result) + (result == GLWASM_OK ? "" : " " + why));
    return pass();
  }

  void finish_instance(Instance &instance) {
    if (tier_ != Tier::Jit || !instance.runtime) return;
    /* A differential run's stops are pause exits; count those taken in
     * compiled code, and leave the audit to the main run. */
    if (diff_) {
      for (uint32_t f = instance.module->nimported_funcs;
           f < instance.module->nfuncs; ++f) {
        GLWASM_JitCacheStats stats{};
        if (glwasm_runtime_test_jit_stats(instance.runtime, f, &stats) ==
            GLWASM_OK)
          jit_.compiled_pauses += stats.exit_counts[GLWASM_JIT_EXIT_PAUSE];
      }
      return;
    }
    const std::string where = relative_ + " (" + tier_name(tier_) + ")";
    audit_jit(instance, rules_, jit_, failures_, where);
    std::map<uint16_t, uint64_t> seen;
    audit_jit_exits(instance, rules_, jit_, failures_, where, seen);
  }

  const std::string &relative_;
  const std::string &source_;
  const std::map<uint32_t, std::string> &line_skips_;
  Tier tier_;
  const JitRules &rules_;
  JitAccount &jit_;
  std::vector<std::string> &failures_;
  std::vector<std::string> *stops_ = nullptr;
  std::unique_ptr<FrameDiff> diff_;
  uint32_t line_ = 0;
  GLWASM_Store *store_ = nullptr;
  std::vector<std::unique_ptr<Instance>> instances_;
  Slot current_;
  std::map<std::string, Slot> named_;
  std::map<std::string, ModuleForm> definitions_;
  std::map<std::string, std::string> skipped_definitions_;
  std::map<std::string, std::string> skipped_registrations_;
  Registry registered_;
};

const char *env_or_fail(const char *name) {
  const char *value = std::getenv(name);
  if (!value) throw std::runtime_error(std::string("Makefile must set ") + name);
  return value;
}

} // namespace

TEST(WastValues, IntegersUseTheScriptGrammarAndWidths) {
  uint64_t value = 0;
  EXPECT_TRUE(wast::parse_integer("08", 32, &value));
  EXPECT_EQ(value, 8u);
  EXPECT_TRUE(wast::parse_integer("4294967295", 32, &value));
  EXPECT_EQ(value, UINT32_MAX);
  EXPECT_TRUE(wast::parse_integer("-2147483648", 32, &value));
  EXPECT_EQ(value, UINT32_C(0x80000000));
  EXPECT_FALSE(wast::parse_integer("4294967296", 32, &value));
  EXPECT_FALSE(wast::parse_integer("-2147483649", 32, &value));
  EXPECT_TRUE(wast::parse_integer("18446744073709551615", 64, &value));
  EXPECT_EQ(value, UINT64_MAX);
  EXPECT_TRUE(wast::parse_integer("-9223372036854775808", 64, &value));
  EXPECT_EQ(value, UINT64_C(0x8000000000000000));
  EXPECT_FALSE(wast::parse_integer("18446744073709551616", 64, &value));
  EXPECT_TRUE(wast::parse_integer("0x1_0000", 32, &value));
  EXPECT_EQ(value, 0x10000u);
  EXPECT_FALSE(wast::parse_integer("12x", 32, &value));
}

TEST(WastValues, FloatsRoundCorrectlyAndKeepNaNPayloads) {
  struct Case { const char *text; bool f32; uint64_t bits; };
  for (const Case &c : std::vector<Case>{
           {"1.5", true, UINT32_C(0x3fc00000)},
           {"0x1.8p+1", true, UINT32_C(0x40400000)},
           {"1e-50", true, 0},
           {"-1e-50", true, UINT32_C(0x80000000)},
           {"1e39", true, UINT32_C(0x7f800000)},
           {"3.4028234663852886e38", true, UINT32_C(0x7f7fffff)},
           {"1.401298464324817e-45", true, 1},
           {"0x1p-149", true, 1},
           {"inf", true, UINT32_C(0x7f800000)},
           {"-inf", false, UINT64_C(0xfff0000000000000)},
           {"1e-400", false, 0},
           {"1e400", false, UINT64_C(0x7ff0000000000000)},
           {"1.7976931348623157e308", false, UINT64_C(0x7fefffffffffffff)},
           {"4.9406564584124654e-324", false, 1},
           {"0x1p-1074", false, 1},
           {"nan", true, UINT32_C(0x7fc00000)},
           {"-nan", false, UINT64_C(0xfff8000000000000)},
           {"nan:0x200000", true, UINT32_C(0x7fa00000)},
           {"-nan:0x1", false, UINT64_C(0xfff0000000000001)}}) {
    uint64_t bits = 0;
    wast::NanPattern pattern = wast::NanPattern::None;
    ASSERT_TRUE(wast::parse_float_bits(c.text, c.f32, &bits, &pattern)) << c.text;
    EXPECT_EQ(bits, c.bits) << c.text;
    EXPECT_EQ(pattern, wast::NanPattern::None) << c.text;
  }
  uint64_t bits = 0;
  EXPECT_FALSE(wast::parse_float_bits("nan:0x123xyz", true, &bits, nullptr));
  EXPECT_FALSE(wast::parse_float_bits("nan:0x800000", true, &bits, nullptr));
  EXPECT_FALSE(wast::parse_float_bits("nan:canonical", true, &bits, nullptr))
      << "a NaN pattern is not an argument value";
}

TEST(WastValues, NaNPatternsSeparateCanonicalFromArithmetic) {
  wast::NanPattern pattern = wast::NanPattern::None;
  uint64_t bits = 0;
  ASSERT_TRUE(wast::parse_float_bits("nan:canonical", true, &bits, &pattern));
  EXPECT_EQ(pattern, wast::NanPattern::Canonical);
  EXPECT_TRUE(wast::nan_matches(UINT32_C(0x7fc00000), true, pattern));
  EXPECT_TRUE(wast::nan_matches(UINT32_C(0xffc00000), true, pattern));
  EXPECT_FALSE(wast::nan_matches(UINT32_C(0x7fc00001), true, pattern));
  EXPECT_FALSE(wast::nan_matches(UINT32_C(0x7f800000), true, pattern));
  ASSERT_TRUE(wast::parse_float_bits("nan:arithmetic", false, &bits, &pattern));
  EXPECT_EQ(pattern, wast::NanPattern::Arithmetic);
  EXPECT_TRUE(wast::nan_matches(UINT64_C(0x7ff8000000000001), false, pattern));
  EXPECT_FALSE(wast::nan_matches(UINT64_C(0x7ff0000000000001), false, pattern))
      << "a signalling NaN is not arithmetic";
}

TEST(WastValues, TrapMessagesMatchByPrefixOrAddedDetail) {
  EXPECT_TRUE(trap_matches(GLWASM_TRAP_UNINITIALIZED_ELEMENT,
      "uninitialized element 2"));
  EXPECT_TRUE(trap_matches(GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS, "out of bounds"));
  EXPECT_FALSE(trap_matches(GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS,
      "out of bounds table access"));
  EXPECT_FALSE(trap_matches(GLWASM_TRAP_UNINITIALIZED_ELEMENT,
      "uninitialized elements"));
  EXPECT_FALSE(trap_matches(GLWASM_TRAP_INTEGER_OVERFLOW,
      "invalid conversion to integer"));
  EXPECT_TRUE(trap_matches(GLWASM_TRAP_INVALID_CONVERSION,
      "invalid conversion to integer"));
  EXPECT_FALSE(trap_matches(GLWASM_TRAP_TABLE_OUT_OF_BOUNDS,
      "undefined element"));
}

/* Run the corpus in both tiers. With `differential`, every invocation is
 * also stopped once and its stack compared across tiers (FrameDiff). */
void run_suite(bool differential) {
  const fs::path root(env_or_fail("GLWASM_WAST_CORPUS_ROOT"));
  const auto classes = read_classifications(env_or_fail("GLWASM_WAST_CLASSIFICATION"));
  const auto skips = read_line_skips(env_or_fail("GLWASM_WAST_ASSERTION_SKIPS"));
  const JitRules rules = read_jit_rules(env_or_fail("GLWASM_WAST_JIT_RULES"));
  const bool memcheck = memcheck_double_rounding_requested();
  ASSERT_FALSE(memcheck && !running_under_valgrind())
      << "the Valgrind-only conversion accommodation needs a live Valgrind client";
  /* Run a subset while iterating: GLWASM_WAST_ONLY=<substring>. The gate
   * itself never sets it, and a filtered run cannot pass the totals below. */
  const char *only = std::getenv("GLWASM_WAST_ONLY");

  std::vector<fs::path> files;
  for (const auto &entry : fs::recursive_directory_iterator(root))
    if (entry.is_regular_file() && entry.path().extension() == ".wast")
      files.push_back(entry.path());
  std::sort(files.begin(), files.end());

  std::vector<std::string> failures;
  std::map<Tier, Counts> counts;
  std::map<std::string, uint64_t> skipped_files;
  JitAccount jit;
  size_t run_files = 0;
  const std::map<uint32_t, std::string> no_skips;
  std::set<std::pair<std::string, uint32_t>> used_skips;
  uint64_t compared = 0, stopped = 0;

  for (const fs::path &path : files) {
    const std::string relative = "test/" + fs::relative(path, root).generic_string();
    const auto classification = classes.find(relative);
    if (classification == classes.end()) {
      failures.push_back(relative + ": no classification");
      continue;
    }
    if (classification->second != "run") {
      const std::string &c = classification->second;
      if (c.rfind("skip(", 0) != 0 || c.back() != ')')
        failures.push_back(relative + ": invalid classification " + c);
      else ++skipped_files[c.substr(5, c.size() - 6)];
      continue;
    }
    if (only && relative.find(only) == std::string::npos) continue;
    ++run_files;
    const std::string source = read_file(path);
    GLWASM_ParseError error{};
    GLWASM_Script *script = nullptr;
    if (glwasm_script_parse(reinterpret_cast<const uint8_t *>(source.data()),
            source.size(), nullptr, &error, &script) != GLWASM_OK) {
      failures.push_back(relative + ": script does not parse at " +
          std::to_string(error.location.line) + ": " +
          (error.message ? error.message : ""));
      continue;
    }
    const auto file_skips = skips.find(relative);
    const auto &line_skips = file_skips == skips.end() ? no_skips
                                                       : file_skips->second;
    std::vector<std::vector<uint64_t>> interpreter_values;
    std::map<Tier, std::vector<std::string>> stops;
    /* Both tiers draw the same fuel allowances, from the file's name. */
    const uint64_t seed = std::hash<std::string>{}(relative);
    for (Tier tier : {Tier::Interpreter, Tier::Jit}) {
      /* Name the file before it runs, so a crash names its file. */
      std::cout << "  running " << relative << " (" << tier_name(tier) << ")"
                << std::endl;
      ScriptRun run(relative, source, line_skips, tier, rules, jit, failures,
          differential ? &stops[tier] : nullptr, seed);
      run.memcheck_double_rounding_ = memcheck;
      for (size_t i = 0; i < glwasm_script_size(script); ++i) {
        GLWASM_Command command{};
        if (glwasm_script_at(script, i, &command) != GLWASM_OK) {
          failures.push_back(relative + ": command " + std::to_string(i) +
              " unreadable");
          continue;
        }
        CommandResult result;
        if (command.kind == GLWASM_COMMAND_MODULE_FIELD) {
          std::string fields;
          size_t last = i;
          for (size_t j = i; j < glwasm_script_size(script); ++j) {
            GLWASM_Command field{};
            if (glwasm_script_at(script, j, &field) != GLWASM_OK ||
                field.kind != GLWASM_COMMAND_MODULE_FIELD)
              break;
            fields += source_form_at(source, glwasm_text_location(field.syntax));
            fields += ' ';
            last = j;
          }
          result = run.inline_module(fields);
          i = last;
        } else {
          result = run.run(command);
        }
        Counts &c = counts[tier];
        const std::string where = relative + ":" +
            std::to_string(command.location.line) + " (" + tier_name(tier) +
            ") " + command_name(command.kind);
        switch (result.verdict) {
          case Verdict::Pass: ++c.passed[command_name(command.kind)]; break;
          case Verdict::Skip: ++c.skipped[result.detail]; break;
          case Verdict::Fail:
            ++c.failed;
            failures.push_back(where + ": " + result.detail);
            break;
        }
        if (command.kind == GLWASM_COMMAND_INVOKE &&
            result.verdict == Verdict::Pass) {
          if (tier == Tier::Interpreter)
            interpreter_values.push_back(result.values);
          else if (interpreter_values.empty() ||
                   interpreter_values.front() != result.values)
            failures.push_back(where + ": results differ from the interpreter");
          else interpreter_values.erase(interpreter_values.begin());
        }
      }
      for (uint32_t line : run.used_skips_) used_skips.emplace(relative, line);
      counts[tier].memcheck_double_rounded += run.memcheck_double_rounded_;
    }
    if (differential) {
      const auto &a = stops[Tier::Interpreter], &b = stops[Tier::Jit];
      if (a.size() != b.size())
        failures.push_back(relative + ": " + std::to_string(a.size()) +
            " interpreter stops against " + std::to_string(b.size()) +
            " in the JIT tier");
      auto clip = [](const std::string &text) {
        return text.size() > 400 ? text.substr(0, 400) + "..." : text;
      };
      for (size_t k = 0; k < std::min(a.size(), b.size()); ++k) {
        ++compared;
        if (a[k].find(": ran to the end") == std::string::npos) ++stopped;
        if (a[k] != b[k])
          failures.push_back(relative + ":" + clip(a[k]) + "\n  jit: " +
              clip(b[k]));
      }
    }
    glwasm_script_destroy(script);
  }

  size_t listed_skips = 0;
  for (const auto &[file, lines] : skips) listed_skips += lines.size();
  if (!only) {
    EXPECT_EQ(used_skips.size(), listed_skips)
        << "every line skip must name a command that exists";
  }

  std::cout << "wg-3.0 spec suite" << (differential ? " frame differential" : "")
            << ": run-files=" << run_files << '\n';
  if (differential)
    std::cout << "  invocations compared=" << compared << " stopped="
              << stopped << " jit stops in compiled code="
              << jit.compiled_pauses << '\n';
  for (const auto &[reason, n] : skipped_files)
    std::cout << "  skipped file (" << reason << "): " << n << '\n';
  for (Tier tier : {Tier::Interpreter, Tier::Jit}) {
    const Counts &c = counts[tier];
    uint64_t passed = 0;
    for (const auto &[kind, n] : c.passed) passed += n;
    std::cout << "  " << tier_name(tier) << ": passed=" << passed
              << " failed=" << c.failed << '\n';
    for (const auto &[kind, n] : c.passed)
      std::cout << "    " << kind << " " << n << '\n';
    for (const auto &[reason, n] : c.skipped)
      std::cout << "    skipped (" << reason << ") " << n << '\n';
  }
  std::cout << "  jit: functions executed=" << jit.functions_executed
            << " compiled=" << jit.functions_compiled
            << " compiled-entries=" << jit.compiled_entries
            << " compiled-returns=" << jit.compiled_returns << '\n';
  for (const auto &[rule, n] : jit.refusals_by_rule)
    std::cout << "    refusal (" << rule << ") " << n << '\n';
  for (const auto &[rule, n] : jit.exits_by_rule)
    std::cout << "    exit (" << rule << ") " << n << '\n';

  for (Tier tier : {Tier::Interpreter, Tier::Jit}) {
    const uint64_t n = counts[tier].memcheck_double_rounded;
    if (memcheck) {
      std::cout << "  " << tier_name(tier) << ": accepted as Valgrind's "
                   "double-rounded i64 -> f32 conversion: " << n << '\n';
      if (!only) {
        EXPECT_EQ(n, kMemcheckDoubleRoundedCommands)
            << "the Valgrind accommodation must cover exactly the conversions "
               "Valgrind rounds twice";
      }
    } else {
      EXPECT_EQ(n, 0u);
    }
  }
  for (const auto &failure : failures) ADD_FAILURE() << failure;
  EXPECT_EQ(counts[Tier::Interpreter].passed, counts[Tier::Jit].passed)
      << "both tiers must pass the same commands";
  if (differential) {
    /* The instrument must have looked: invocations stopped, and stopped
     * inside compiled code, or the comparison is the interpreter's with
     * itself. The floors are well below the corpus's counts. */
    if (!only) {
      EXPECT_GT(stopped, 10000u);
      EXPECT_GT(jit.compiled_pauses, 5000u);
    }
  } else {
    EXPECT_GT(jit.compiled_returns, 0u);
  }
  if (!only) {
    EXPECT_EQ(classes.size(), files.size())
        << "classification and fetched file sets must agree";
  }
}

TEST(SpecSuite, EveryInScopeCommandPassesInBothTiers) { run_suite(false); }

TEST(SpecSuite, FramesMatchAcrossTiersWhereEveryInvocationStops) {
  run_suite(true);
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
