/* SPDX-License-Identifier: LGPL-3.0-only */
#include <ghoti.io/lang-wasm/host.h>
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/runtime-heap/runtime-heap.h>
#ifdef GLWASM_CORPUS_RELOCATE
#include <ghoti.io/runtime-heap/relocate.h>
#endif
#include "../../src/runtime/runtime.h"
#include <gtest/gtest.h>

#include <array>
#include <charconv>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

struct ModuleDeleter {
  void operator()(GLWASM_Module *module) const { glwasm_module_destroy(module); }
};
struct RuntimeDeleter {
  void operator()(GLWASM_Runtime *runtime) const { glwasm_runtime_destroy(runtime); }
};
struct HeapRootGuard {
  GRHEAP_Heap *heap = nullptr;
  void **slot = nullptr;
  ~HeapRootGuard() {
    if (heap && slot) (void)grheap_root_remove(heap, slot);
  }
};
struct PortDeleter {
  void operator()(GRCORE_Port *port) const { grcore_port_release(port); }
};

/* These helpers take the root slot, never the object pointer. Their frames
 * return before the next collection, so the conservatively scanned caller
 * cannot retain an unencoded probe address in a spill slot. */
#ifdef GLWASM_CORPUS_RELOCATE
[[gnu::noinline]] uint64_t encoded_probe_address(void *const *slot) {
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(*slot)) ^
      UINT64_C(0xa5a5a5a5a5a5a5a5);
}
#endif

[[gnu::noinline]] void set_probe_marker(void **slot, uint64_t marker) {
  *static_cast<uint64_t *>(*slot) = marker;
}

[[gnu::noinline]] bool probe_marker_matches(void *const *slot,
    uint64_t marker) {
  return *slot && *static_cast<uint64_t *>(*slot) == marker;
}

/* A keyed request keeps the slow poll active until a substantive callee of
 * corpus_run is on the guest stack. Allocation there takes the configured
 * torture collection while the interpreter or compiled guest is executing. */
struct CorpusPollProbe {
  const GLWASM_Module *module = nullptr;
  GRHEAP_Heap *heap = nullptr;
  void **root = nullptr;
  uint32_t entry = UINT32_MAX;
  uint32_t observed_function = UINT32_MAX;
  GRCORE_RequestKind request = 0;
  uint64_t expected_value = 0;
  uint64_t collections = 0;
  uint64_t relocations = 0;
  bool entered = false;
  bool armed = false;
  bool failed = false;
  bool require_relocation = false;
};

void corpus_stress_poll(GRCORE_Context *context, void *value,
    GRCORE_PollCall *call) {
  auto *probe = static_cast<CorpusPollProbe *>(value);
  if (!probe->armed || !grcore_pollcall_pending(call, probe->request)) return;
  GRCORE_PollIdentity identity{};
  if (grcore_context_poll_identity(context, &identity) != GRCORE_OK ||
      (identity.function >> 32) != probe->module->debug_id) return;
  const uint32_t function = static_cast<uint32_t>(identity.function);
  if (function == probe->entry) {
    probe->entered = true;
    return;
  }
  if (!probe->entered || function < probe->module->nimported_funcs ||
      function >= probe->module->nfuncs ||
      probe->module->funcs[function].nins < 32) return;
  GRHEAP_Stats before{}, after{};
  static const GRHEAP_Type trigger_type = GRHEAP_TYPE_INIT(
      "corpus-gc-poll-trigger", sizeof(uint64_t), nullptr, nullptr,
      nullptr, nullptr, nullptr);
  void *trigger = nullptr;
#ifdef GLWASM_CORPUS_RELOCATE
  GRHEAP_RelocationStats relocation_before{}, relocation_after{};
  const bool relocation_ready = !probe->require_relocation ||
      grheap_relocation_stats(probe->heap, &relocation_before) == GRHEAP_OK;
  const uint64_t root_before_encoded = probe->require_relocation
      ? encoded_probe_address(probe->root) : 0;
#endif
  const bool collected = grheap_stats(probe->heap, &before) == GRHEAP_OK &&
      grheap_alloc(probe->heap, &trigger_type, &trigger) == GRHEAP_OK &&
      grheap_stats(probe->heap, &after) == GRHEAP_OK &&
      after.torture_collections > before.torture_collections &&
      *probe->root &&
      *static_cast<uint64_t *>(*probe->root) == probe->expected_value;
#ifdef GLWASM_CORPUS_RELOCATE
  const bool relocated = !probe->require_relocation ||
      (relocation_ready &&
       grheap_relocation_stats(probe->heap, &relocation_after) == GRHEAP_OK &&
       relocation_after.moved > relocation_before.moved &&
       encoded_probe_address(probe->root) != root_before_encoded);
#endif
  const bool cleared =
      grcore_context_clear_request(context, probe->request) == GRCORE_OK;
  if (!collected || !cleared
#ifdef GLWASM_CORPUS_RELOCATE
      || !relocated
#endif
      ) {
    probe->failed = true;
  } else {
    probe->observed_function = function;
    ++probe->collections;
#ifdef GLWASM_CORPUS_RELOCATE
    if (probe->require_relocation) ++probe->relocations;
#endif
  }
  probe->armed = false;
}

static const GRCORE_Key corpus_stress_key = GRCORE_KEY_INIT(
    "wasm-real-corpus-stress", GRCORE_CARDINALITY_ONE,
    GRCORE_PHASE_YIELD, nullptr, corpus_stress_poll, nullptr, nullptr, nullptr);

using ModulePtr = std::unique_ptr<GLWASM_Module, ModuleDeleter>;
using RuntimePtr = std::unique_ptr<GLWASM_Runtime, RuntimeDeleter>;
using PortPtr = std::unique_ptr<GRCORE_Port, PortDeleter>;

uint32_t export_function(const GLWASM_Module *module, const char *wanted) {
  const size_t length = std::strlen(wanted);
  for (uint32_t i = 0; i < module->nexports; ++i) {
    const GLWASM_Export &entry = module->exports[i];
    if (entry.kind == GLWASM_EXT_FUNC && entry.name.length == length &&
        std::memcmp(entry.name.bytes, wanted, length) == 0)
      return entry.index;
  }
  return UINT32_MAX;
}

std::string runtime_failure_diagnostic(GLWASM_Runtime *runtime,
    const GLWASM_Module *module, uint32_t function) {
  GLWASM_TestRuntimeFailureInfo failure{};
  if (function == UINT32_MAX ||
      glwasm_runtime_test_failure_info(runtime, function, &failure) != GLWASM_OK)
    return "; runtime diagnostic unavailable";
  static constexpr const char *refusal_names[] = {
      "none", "backend_unavailable", "unsupported_ir", "resource_limit",
      "profitability", "retired_exit_rate"};
  std::ostringstream diagnostic;
  diagnostic << "; runtime={run_error=" << failure.run_error
      << " trap=" << failure.trap << " core_result=" << failure.core_result
      << " outcome=" << failure.outcome << " last_instruction=";
  if (failure.has_last_instruction)
    diagnostic << failure.last_function << ':' << failure.last_pc
        << ":0x" << std::hex << failure.last_opcode << std::dec;
  else
    diagnostic << "none";
  diagnostic << " jit_cache=" << static_cast<unsigned>(failure.has_jit_cache)
      << " jit_pages=" << static_cast<unsigned>(failure.has_jit_pages)
      << " jit_slots=" << static_cast<unsigned>(failure.has_jit_slots)
      << " jit_registered=" << static_cast<unsigned>(failure.has_jit_registered)
      << " function_slot=" << static_cast<unsigned>(failure.has_function_slot)
      << " function_registered=" << static_cast<unsigned>(failure.has_function_registered)
      << " context_match=" << static_cast<unsigned>(failure.context_matches_runtime)
      << " engine_match=" << static_cast<unsigned>(failure.engine_matches_runtime)
      << " call_target=";
  if (failure.call_target == UINT32_MAX) diagnostic << "none";
  else diagnostic << failure.call_target;
  diagnostic << " call_reason=" << static_cast<unsigned>(failure.diagnostic_reason)
      << " call_sp=" << failure.call_sp
      << " call_base=" << failure.call_base
      << " call_required=" << failure.call_required
      << " caller_params=" << failure.caller_params
      << " caller_locals=" << failure.caller_locals
      << " callee_params=" << failure.callee_params
      << " frame_entry=";
  if (failure.frame_entry_function == UINT32_MAX) diagnostic << "none";
  else diagnostic << failure.frame_entry_function << ':'
      << failure.frame_entry_sp << '/' << failure.frame_entry_base;
  diagnostic << " first_bad_sp=";
  if (failure.first_bad_sp_function == UINT32_MAX) diagnostic << "none";
  else diagnostic << failure.first_bad_sp_function << ':'
      << failure.first_bad_sp_pc << ':' << failure.first_bad_sp << '/'
      << failure.first_bad_base;
  diagnostic
      << " jit_compile_attempted=" << static_cast<unsigned>(failure.jit_compile_attempted)
      << " jit_compile_result=" << failure.jit_compile_result
      << " jit_lower_pc=";
  if (failure.jit_lower_pc == UINT32_MAX) diagnostic << "none";
  else diagnostic << failure.jit_lower_pc;
  diagnostic << " jit_lower_raw=" << failure.jit_lower_raw_result
      << " jit_lower_opcode=";
  if (failure.jit_lower_opcode == UINT16_MAX) diagnostic << "none";
  else diagnostic << "0x" << std::hex << failure.jit_lower_opcode << std::dec;
  diagnostic << " jit_lower_stack=" << failure.jit_lower_stack_count
      << " jit_lower_site=" << failure.jit_lower_site;
  if (failure.deopt_count != 0) {
    const auto describe_deopt = [&diagnostic](
        const GLWASM_TestDeoptOrigin &origin) {
      diagnostic << "cause=" << origin.cause << " frame=";
      if (origin.has_frame)
        diagnostic << origin.function << ':' << origin.pc;
      else
        diagnostic << "none";
      diagnostic << " depth=" << origin.compiled_depth
          << " named_origin=";
      if (origin.named_opcode == UINT16_MAX)
        diagnostic << "none";
      else
        diagnostic << origin.named_function << ":0x" << std::hex
            << origin.named_opcode << std::dec;
    };
    diagnostic << " deopts=" << failure.deopt_count << " first={";
    describe_deopt(failure.first_deopt);
    diagnostic << "} last={";
    describe_deopt(failure.last_deopt);
    diagnostic << '}';
  }
  diagnostic << '}';
  GLWASM_JitCacheStats stats{};
  if (function >= module->nimported_funcs &&
      glwasm_runtime_test_jit_stats(runtime, function, &stats) == GLWASM_OK) {
    diagnostic << "; jit={function=" << function;
    if (module->function_names && function < module->function_names_count) {
      const GLWASM_Name &name = module->function_names[function];
      if (name.bytes && name.length != 0)
        diagnostic << " name=" << std::string(name.bytes, name.length);
    }
    diagnostic << " compile_attempts=" << stats.compile_attempts
        << " compile_successes=" << stats.compile_successes;
    for (size_t refusal = 1;
         refusal < sizeof(refusal_names) / sizeof(refusal_names[0]);
         ++refusal) {
      diagnostic << " refusal_" << refusal_names[refusal] << '='
          << stats.refusal_counts[refusal];
    }
    diagnostic << " signature_refusals=" << stats.signature_refusal_count
        << " last_refused_opcode=";
    if (stats.last_refused_opcode == UINT16_MAX)
      diagnostic << "none";
    else
      diagnostic << "0x" << std::hex << stats.last_refused_opcode << std::dec;
    diagnostic << " entries=" << stats.compiled_entries
        << " completed=" << stats.completed_without_deopt
        << " guest_visible=" << stats.guest_visible_exits
        << " window=" << stats.window_entries << '/'
        << stats.window_exits << " interpreter_only="
        << static_cast<unsigned>(stats.interpreter_only) << " exits=";
    for (size_t kind = 0; kind < GLWASM_JIT_EXIT_COUNT; ++kind)
      diagnostic << (kind ? "/" : "") << stats.exit_counts[kind];
    diagnostic << '}';
  }
  diagnostic << "; opcode_exits={";
  bool any_opcode_exit = false;
  for (uint16_t opcode = 0; opcode <= 0xffu; ++opcode) {
    uint64_t count = glwasm_runtime_test_jit_opcode_exit_count(runtime, opcode);
    if (count) {
      diagnostic << (any_opcode_exit ? "," : "") << "0x" << std::hex
          << opcode << std::dec << ':' << count;
      any_opcode_exit = true;
    }
  }
  for (uint16_t opcode = 0xfc00u; opcode <= 0xfc11u; ++opcode) {
    uint64_t count = glwasm_runtime_test_jit_opcode_exit_count(runtime, opcode);
    if (count) {
      diagnostic << (any_opcode_exit ? "," : "") << "0x" << std::hex
          << opcode << std::dec << ':' << count;
      any_opcode_exit = true;
    }
  }
  diagnostic << '}';
  if (failure.call_target < module->nfuncs &&
      failure.call_target >= module->nimported_funcs &&
      failure.call_target != function &&
      glwasm_runtime_test_jit_stats(runtime, failure.call_target, &stats) ==
          GLWASM_OK) {
    diagnostic << "; call_target_jit={function=" << failure.call_target
        << " entries=" << stats.compiled_entries
        << " returns=" << stats.exit_counts[GLWASM_JIT_EXIT_RETURN]
        << " completed=" << stats.completed_without_deopt
        << " compile_successes=" << stats.compile_successes
        << " unsupported_refusals="
        << stats.refusal_counts[GLWASM_JIT_REFUSAL_UNSUPPORTED_IR]
        << " resource_refusals="
        << stats.refusal_counts[GLWASM_JIT_REFUSAL_RESOURCE_LIMIT]
        << " last_refused_opcode=" << stats.last_refused_opcode
        << " guest_visible=" << stats.guest_visible_exits
        << " exits=";
    for (size_t kind = 0; kind < GLWASM_JIT_EXIT_COUNT; ++kind)
      diagnostic << (kind ? "/" : "") << stats.exit_counts[kind];
    diagnostic
        << " interpreter_only=" << static_cast<unsigned>(stats.interpreter_only)
        << '}';
  }
  return diagnostic.str();
}

GLWASM_Result reject_unexpected_wasi_call(const GLWASM_HostCall *,
    uint64_t *) {
  // The real-corpus module needs these exact wasi-sdk libc imports to link,
  // but the deterministic workload must never use a host service. A call is
  // a test failure, surfaced as an execution error by the normal assertions.
  return GLWASM_ERR_UNSUPPORTED;
}

/* The reviewed WASI preview 1 signatures the Lua workload imports; the
 * binding declares each, and the engine checks it against the module. */
GLWASM_HostImport make_wasi_test_import(const char *field) {
  static const GLWASM_HostValueType i32 = GLWASM_HOST_I32;
  static const GLWASM_HostValueType clock[] = {GLWASM_HOST_I32,
      GLWASM_HOST_I64, GLWASM_HOST_I32};
  static const GLWASM_HostValueType one[] = {GLWASM_HOST_I32};
  static const GLWASM_HostValueType two[] = {GLWASM_HOST_I32, GLWASM_HOST_I32};
  static const GLWASM_HostValueType three[] = {GLWASM_HOST_I32,
      GLWASM_HOST_I32, GLWASM_HOST_I32};
  static const GLWASM_HostValueType seek[] = {GLWASM_HOST_I32,
      GLWASM_HOST_I64, GLWASM_HOST_I32, GLWASM_HOST_I32};
  static const GLWASM_HostValueType write[] = {GLWASM_HOST_I32,
      GLWASM_HOST_I32, GLWASM_HOST_I32, GLWASM_HOST_I32};
  struct Signature { const char *field; const GLWASM_HostValueType *params;
                     uint32_t nparams; uint32_t nresults; };
  static const Signature signatures[] = {
      {"clock_time_get", clock, 3, 1}, {"fd_close", one, 1, 1},
      {"fd_prestat_get", two, 2, 1}, {"fd_prestat_dir_name", three, 3, 1},
      {"fd_seek", seek, 4, 1}, {"fd_write", write, 4, 1},
      {"proc_exit", one, 1, 0}};
  GLWASM_HostImport result{};
  result.module = "wasi_snapshot_preview1";
  result.field = field;
  result.kind = GLWASM_HOST_IMPORT_FUNCTION;
  result.function_results = reject_unexpected_wasi_call;
  for (const Signature &signature : signatures)
    if (std::strcmp(signature.field, field) == 0) {
      result.parameters = signature.params;
      result.parameter_count = signature.nparams;
      result.results = signature.nresults ? &i32 : nullptr;
      result.result_count = signature.nresults;
    }
  return result;
}

bool wasi_signature_matches(const GLWASM_Module *module,
    const GLWASM_Import *import) {
  if (!module || !import || import->kind != GLWASM_EXT_FUNC ||
      import->index >= module->nfuncs ||
      import->module_name.length != sizeof("wasi_snapshot_preview1") - 1 ||
      std::memcmp(import->module_name.bytes, "wasi_snapshot_preview1",
          sizeof("wasi_snapshot_preview1") - 1) != 0)
    return false;
  const GLWASM_Func *function = &module->funcs[import->index];
  if (function->type_index >= module->ntypes) return false;
  const GLWASM_TypeDef *type = &module->types[function->type_index];
  if (type->comp != GLWASM_COMP_FUNC) return false;
  auto is = [&](uint32_t index, uint8_t kind) {
    return index < type->nparams && type->params[index].kind == kind;
  };
  auto one_i32_result = [&] {
    return type->nresults == 1 &&
        type->results[0].kind == GLWASM_VK_I32;
  };
  auto field_is = [&](const char *field) {
    const size_t length = std::strlen(field);
    return import->field.length == length &&
        std::memcmp(import->field.bytes, field, length) == 0;
  };
  if (field_is("clock_time_get"))
    return type->nparams == 3 && is(0, GLWASM_VK_I32) &&
        is(1, GLWASM_VK_I64) && is(2, GLWASM_VK_I32) && one_i32_result();
  if (field_is("fd_close"))
    return type->nparams == 1 && is(0, GLWASM_VK_I32) && one_i32_result();
  if (field_is("fd_prestat_get"))
    return type->nparams == 2 && is(0, GLWASM_VK_I32) &&
        is(1, GLWASM_VK_I32) && one_i32_result();
  if (field_is("fd_prestat_dir_name"))
    return type->nparams == 3 && is(0, GLWASM_VK_I32) &&
        is(1, GLWASM_VK_I32) && is(2, GLWASM_VK_I32) && one_i32_result();
  if (field_is("fd_seek"))
    return type->nparams == 4 && is(0, GLWASM_VK_I32) &&
        is(1, GLWASM_VK_I64) && is(2, GLWASM_VK_I32) &&
        is(3, GLWASM_VK_I32) && one_i32_result();
  if (field_is("fd_write"))
    return type->nparams == 4 && is(0, GLWASM_VK_I32) &&
        is(1, GLWASM_VK_I32) && is(2, GLWASM_VK_I32) &&
        is(3, GLWASM_VK_I32) && one_i32_result();
  if (field_is("proc_exit"))
    return type->nparams == 1 && is(0, GLWASM_VK_I32) &&
        type->nresults == 0;
  return false;
}

std::vector<GLWASM_HostImport> wasi_test_imports(const GLWASM_Module *module,
    const std::string &workload) {
  static const std::array<const char *, 7> allowed{
      "clock_time_get", "fd_close", "fd_prestat_get",
      "fd_prestat_dir_name", "fd_seek", "fd_write", "proc_exit"};
  if (!module) throw std::runtime_error("missing real-corpus module");
  if (workload != "lua") {
    if (module->nimports != 0)
      throw std::runtime_error("unexpected imports in non-Lua corpus workload");
    return {};
  }
  if (module->nimports != allowed.size())
    throw std::runtime_error("Lua import set differs from its reviewed manifest");
  std::array<bool, 7> seen{};
  for (uint32_t i = 0; i < module->nimports; ++i) {
    const GLWASM_Import *import = &module->imports[i];
    if (!wasi_signature_matches(module, import))
      throw std::runtime_error("unexpected or mistyped real-corpus host import");
    bool matched = false;
    for (size_t j = 0; j < allowed.size(); ++j) {
      const size_t length = std::strlen(allowed[j]);
      if (import->field.length == length &&
          std::memcmp(import->field.bytes, allowed[j], length) == 0) {
        if (seen[j]) throw std::runtime_error("duplicate real-corpus host import");
        seen[j] = true;
        matched = true;
        break;
      }
    }
    if (!matched) throw std::runtime_error("unlisted real-corpus host import");
  }
  std::vector<GLWASM_HostImport> result;
  result.reserve(allowed.size());
  for (const char *field : allowed) result.push_back(make_wasi_test_import(field));
  return result;
}

const std::array<const char *, 3> kWorkloads{"sqlite", "zlib", "lua"};

enum class StressMode { None, GcTorture, Relocation };

std::string corpus_root() {
  const char *value = std::getenv("GLWASM_REAL_CORPUS_DIR");
  return value ? value : "";
}

std::string module_root() {
  const char *value = std::getenv("GLWASM_REAL_CORPUS_MODULE_DIR");
  return value ? value : "";
}

std::unordered_map<std::string, std::vector<uint8_t>> read_vectors() {
  std::unordered_map<std::string, std::vector<uint8_t>> result;
  std::ifstream input(corpus_root() + "/vectors.tsv");
  if (!input) return result;
  std::string line;
  while (std::getline(input, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream fields(line);
    std::string workload, test_case, status, length_text, path, extra;
    if (!std::getline(fields, workload, '\t') ||
        !std::getline(fields, test_case, '\t') ||
        !std::getline(fields, status, '\t') ||
        !std::getline(fields, length_text, '\t') ||
        !std::getline(fields, path, '\t') || std::getline(fields, extra, '\t') ||
        test_case != "default")
      return {};
    if (status != "1" || path != "vectors/" + workload + ".hex") return {};
    uint32_t length = 0;
    const char *length_begin = length_text.data();
    const char *length_end = length_begin + length_text.size();
    const auto parsed = std::from_chars(length_begin, length_end, length);
    if (parsed.ec != std::errc{} || parsed.ptr != length_end || length > 8192 ||
        !result.emplace(workload, std::vector<uint8_t>{}).second)
      return {};
    auto &bytes = result.at(workload);
    bytes.reserve(length);
    std::ifstream vector_file(corpus_root() + "/" + path);
    if (!vector_file) return {};
    std::string hex, line;
    while (std::getline(vector_file, line)) hex += line;
    if (hex.size() != static_cast<size_t>(length) * 2) return {};
    for (size_t i = 0; i < hex.size(); i += 2) {
      unsigned value = 0;
      const auto converted = std::from_chars(hex.data() + i, hex.data() + i + 2,
          value, 16);
      if (converted.ec != std::errc{} || converted.ptr != hex.data() + i + 2 ||
          (hex[i] >= 'A' && hex[i] <= 'F') || (hex[i + 1] >= 'A' && hex[i + 1] <= 'F'))
        return {};
      bytes.push_back(static_cast<uint8_t>(value));
    }
  }
  return result;
}

void invoke(const std::string &path, bool jit,
    const std::vector<uint8_t> &expected, StressMode stress,
    const std::string &workload) {
  std::ifstream input(path, std::ios::binary);
  if (!input) throw std::runtime_error("cannot read module: " + path);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(input)), {});
  GLWASM_Policy policy{};
  if (glwasm_policy_preset(GLWASM_POLICY_TRUSTED, &policy) != GLWASM_OK)
    throw std::runtime_error("cannot initialize trusted policy");
  policy.default_fuel = UINT64_C(50000000);
  policy.max_jit_code_bytes = jit ? UINT64_C(67108864) : 0;
  GLWASM_Module *raw_module = nullptr;
  GLWASM_Failure failure{};
  const GLWASM_Result decoded = glwasm_module_decode_with_policy(bytes.data(),
      bytes.size(), &policy, nullptr, &failure, &raw_module);
  if (decoded != GLWASM_OK)
    throw std::runtime_error("module decode failed: " +
        std::string(glwasm_result_string(decoded)));
  ModulePtr module(raw_module);
  std::vector<GLWASM_HostImport> host_imports =
      wasi_test_imports(module.get(), workload);
  GLWASM_Runtime *raw_runtime = nullptr;
  const GLWASM_Result created = glwasm_runtime_create_with_imports(
      module.get(), host_imports.empty() ? nullptr : host_imports.data(),
      host_imports.size(), 1, nullptr, &raw_runtime);
  if (created != GLWASM_OK)
    throw std::runtime_error("runtime creation failed: " +
        std::string(glwasm_result_string(created)));
  CorpusPollProbe poll_probe{};
  RuntimePtr runtime(raw_runtime);
  PortPtr stress_port;
  if (glwasm_runtime_test_set_jit_mode(runtime.get(), jit
          ? GLWASM_TEST_JIT_REQUIRE : GLWASM_TEST_JIT_INTERPRETER) != GLWASM_OK)
    throw std::runtime_error("cannot select requested execution tier");
  GRHEAP_Heap *heap = nullptr;
  GRCORE_Stack *stack = nullptr;
  GRCORE_Context *stress_context = nullptr;
  std::unique_ptr<void *> stress_slot;
  HeapRootGuard stress_root;
  static const GRHEAP_Type stress_probe_type = GRHEAP_TYPE_INIT(
      "corpus-gc-stress-probe", sizeof(uint64_t), nullptr, nullptr,
      nullptr, nullptr, nullptr);
  constexpr uint64_t kStressProbeValue = UINT64_C(0x6c7761736d726f6f);
  if (stress != StressMode::None) {
    GRCORE_Context *context = glwasm_runtime_context(runtime.get());
    if (!context) throw std::runtime_error("runtime has no runtime-core context");
    stress_context = context;
    GRHEAP_Options *heap_options = nullptr;
    if (grheap_options_create(nullptr, &heap_options) != GRHEAP_OK)
      throw std::runtime_error("cannot create runtime-heap torture options");
    GRHEAP_Result heap_result = grheap_options_set_torture(heap_options, true);
#ifdef GLWASM_CORPUS_RELOCATE
    if (heap_result == GRHEAP_OK && stress == StressMode::Relocation) {
      if (!grheap_relocation_available()) {
        grheap_options_destroy(heap_options);
        throw std::runtime_error("runtime-heap was not built with RELOCATE=yes");
      }
      heap_result = grheap_options_set_relocate(heap_options, true);
    }
#else
    if (stress == StressMode::Relocation) {
      grheap_options_destroy(heap_options);
      throw std::runtime_error("relocation test requires the relocation build");
    }
#endif
    if (heap_result != GRHEAP_OK ||
        grheap_heap_create(context, heap_options, &heap) != GRHEAP_OK) {
      grheap_options_destroy(heap_options);
      throw std::runtime_error("cannot attach configured runtime-heap");
    }
    grheap_options_destroy(heap_options);
    stack = grcore_context_stack(context);
    if (!stack || grcore_stack_set_always_move(stack, true) != GRCORE_OK)
      throw std::runtime_error("cannot enable runtime-core always-move stack mode");
    /* Keep the precise root slot off the C activation stack. Runtime-core
     * conservatively scans that stack, so a raw C local copy would pin the
     * object and prevent this test from observing relocation. */
    stress_slot = std::make_unique<void *>(nullptr);
    if (grheap_alloc(heap, &stress_probe_type, stress_slot.get()) != GRHEAP_OK)
      throw std::runtime_error("cannot allocate GC stress probe object");
    set_probe_marker(stress_slot.get(), kStressProbeValue);
    if (grheap_root_add(heap, stress_slot.get()) != GRHEAP_OK)
      throw std::runtime_error("cannot root GC stress probe object");
    stress_root.heap = heap;
    stress_root.slot = stress_slot.get();
  }
  const uint32_t corpus_function = export_function(module.get(), "corpus_run");
  if (corpus_function == UINT32_MAX)
    throw std::runtime_error("corpus_run function export not found");
  const uint32_t output_size_function =
      export_function(module.get(), "corpus_output_size");
  const uint32_t output_chunk_function =
      export_function(module.get(), "corpus_output_chunk");
  if (output_size_function == UINT32_MAX ||
      output_chunk_function == UINT32_MAX)
    throw std::runtime_error("corpus output interface is incomplete");
  if (stress != StressMode::None) {
    poll_probe.module = module.get();
    poll_probe.heap = heap;
    poll_probe.root = stress_slot.get();
    poll_probe.entry = corpus_function;
    poll_probe.expected_value = kStressProbeValue;
    poll_probe.require_relocation = stress == StressMode::Relocation;
    GRCORE_Port *raw_port = nullptr;
    if (grcore_context_request_kind(stress_context, &corpus_stress_key,
            &poll_probe.request) != GRCORE_OK ||
        grcore_context_register(stress_context, &corpus_stress_key,
            &poll_probe) != GRCORE_OK ||
        grcore_context_port(stress_context, &raw_port) != GRCORE_OK)
      throw std::runtime_error("cannot install in-guest GC stress poll");
    stress_port.reset(raw_port);
  }
  uint64_t value = 0;
  const auto invoke_export = [&](const char *name, const uint64_t *args,
                                 uint32_t nargs, uint64_t *result) {
    const GLWASM_Result invoked = glwasm_runtime_invoke_export(
        runtime.get(), name, args, nargs, result);
    if (invoked != GLWASM_OK)
      throw std::runtime_error("workload=" + workload + " export=" + name +
          " stage=invoke failed: " + glwasm_result_string(invoked) +
          runtime_failure_diagnostic(runtime.get(), module.get(),
              corpus_function));
  };
  /* Compare status and every output byte after each invocation, so only the
   * final call cannot mask state or determinism defects in earlier calls. */
  for (unsigned call = 0; call < 8; ++call) {
    uint64_t collections_before = 0;
    uint64_t moves_before = 0;
#ifdef GLWASM_CORPUS_RELOCATE
    uint64_t relocation_before = 0;
#endif
    if (stress != StressMode::None) {
      GRHEAP_Stats before{};
      if (grheap_stats(heap, &before) != GRHEAP_OK)
        throw std::runtime_error("cannot read GC stress baseline");
      collections_before = before.torture_collections;
      moves_before = grcore_stack_move_count(stack);
#ifdef GLWASM_CORPUS_RELOCATE
      if (stress == StressMode::Relocation) {
        GRHEAP_RelocationStats before_relocation{};
        if (grheap_relocation_stats(heap, &before_relocation) != GRHEAP_OK)
          throw std::runtime_error("cannot read relocation baseline");
        relocation_before = before_relocation.moved;
      }
#endif
      poll_probe.entered = false;
      poll_probe.armed = true;
      poll_probe.failed = false;
      poll_probe.observed_function = UINT32_MAX;
      if (grcore_port_post(stress_port.get(), poll_probe.request) != GRCORE_OK)
        throw std::runtime_error("cannot arm in-guest GC stress poll");
    }
    invoke_export("corpus_run", nullptr, 0, &value);
    if (jit) {
      /* Inspect every defined function before the byte-query exports run.
       * REQUIRE mode refuses an inner call it cannot compile; this sweep also
       * rejects any compiled callee that exited to interpreted execution.
       * The size threshold is only additional evidence of real library work,
       * not the boundary of the no-deopt assertion. */
      size_t compiled_workload_functions = 0;
      for (uint32_t function = module->nimported_funcs;
           function < module->nfuncs; ++function) {
        GLWASM_JitCacheStats inner{};
        if (glwasm_runtime_test_jit_stats(runtime.get(), function, &inner) !=
            GLWASM_OK)
          throw std::runtime_error("cannot read workload JIT function stats");
        if (inner.guest_visible_exits != 0 ||
            inner.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT] != 0 ||
            inner.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED] != 0 ||
            inner.exit_counts[GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE] != 0 ||
            (inner.compiled_entries != 0 &&
                (inner.compile_successes == 0 ||
                 inner.exit_counts[GLWASM_JIT_EXIT_RETURN] !=
                     inner.compiled_entries ||
                 inner.completed_without_deopt != inner.compiled_entries)))
          throw std::runtime_error("workload=" + workload + " function=" +
              std::to_string(function) + " invocation=" +
              std::to_string(call + 1) +
              " did not complete every compiled entry with a normal return; " +
              "entries=" + std::to_string(inner.compiled_entries) +
              " returns=" + std::to_string(
                  inner.exit_counts[GLWASM_JIT_EXIT_RETURN]) +
              " completed=" +
              std::to_string(inner.completed_without_deopt) +
              " host_calls=" + std::to_string(
                  inner.exit_counts[GLWASM_JIT_EXIT_HOST_CALL]) +
              " guard_deopts=" + std::to_string(
                  inner.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT]) +
              " named_exits=" + std::to_string(
                  inner.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED]) +
              runtime_failure_diagnostic(runtime.get(), module.get(),
                  function));
        if (call == 0 && function != corpus_function &&
            function != output_size_function &&
            function != output_chunk_function &&
            module->funcs[function].nins >= 32 &&
            inner.compiled_entries != 0)
          ++compiled_workload_functions;
      }
      if (call == 0 && compiled_workload_functions < 2)
        throw std::runtime_error("workload=" + workload +
            " did not execute at least two substantive inner functions "
            "in compiled code during corpus_run");
    }
    if (stress != StressMode::None) {
      GRHEAP_Stats after{};
      if (poll_probe.failed || poll_probe.armed || !poll_probe.entered ||
          poll_probe.observed_function == UINT32_MAX ||
          poll_probe.collections != call + 1u ||
          grheap_stats(heap, &after) != GRHEAP_OK ||
          after.torture_collections <= collections_before ||
          grcore_stack_move_count(stack) <= moves_before ||
          !probe_marker_matches(stress_slot.get(), kStressProbeValue))
        throw std::runtime_error("workload=" + workload +
            " corpus_run did not preserve the root and moving stack across "
            "an in-guest torture collection on invocation " +
            std::to_string(call + 1));
      if (jit) {
        GLWASM_JitCacheStats observed{};
        if (glwasm_runtime_test_jit_stats(runtime.get(),
                poll_probe.observed_function, &observed) != GLWASM_OK ||
            observed.compiled_entries == 0 ||
            observed.exit_counts[GLWASM_JIT_EXIT_RETURN] == 0 ||
            observed.guest_visible_exits != 0)
          throw std::runtime_error("workload=" + workload +
              " in-guest collection did not occur in a compiled inner "
              "function on invocation " + std::to_string(call + 1));
      }
#ifdef GLWASM_CORPUS_RELOCATE
      if (stress == StressMode::Relocation) {
        GRHEAP_RelocationStats after_relocation{};
        const GRHEAP_Result relocation_result =
            grheap_relocation_stats(heap, &after_relocation);
        if (relocation_result != GRHEAP_OK ||
            after_relocation.moved <= relocation_before ||
            poll_probe.relocations != call + 1u)
          throw std::runtime_error("workload=" + workload +
              " corpus_run did not relocate the rooted object during "
              "invocation " + std::to_string(call + 1) +
              "; relocation_result=" + std::to_string(relocation_result) +
              " collections=" +
              std::to_string(after_relocation.collections) +
              " moved_before=" + std::to_string(relocation_before) +
              " moved_after=" + std::to_string(after_relocation.moved) +
              " stayed_conservative=" +
              std::to_string(after_relocation.stayed_conservative) +
              " stayed_pinned=" +
              std::to_string(after_relocation.stayed_pinned) +
              " skipped=" + std::to_string(after_relocation.skipped) +
              " in_guest_relocations=" +
              std::to_string(poll_probe.relocations));
      }
#endif
    }
    if (value != 1)
      throw std::runtime_error("corpus_run reported failure on invocation " +
          std::to_string(call + 1));
    invoke_export("corpus_output_size", nullptr, 0, &value);
    if (value != expected.size())
      throw std::runtime_error("corpus output length differs from vector on invocation " +
          std::to_string(call + 1));
    for (uint32_t offset = 0; offset < expected.size(); offset += 7) {
      const uint64_t argument = offset / 7;
      invoke_export("corpus_output_chunk", &argument, 1, &value);
      if ((value >> 56) != 1)
        throw std::runtime_error("corpus output chunk reported failure");
      for (uint32_t byte = 0; byte < 7 && offset + byte < expected.size(); ++byte) {
        const uint8_t actual = static_cast<uint8_t>(value >> (byte * 8));
        if (actual != expected[offset + byte])
          throw std::runtime_error("corpus output byte differs from vector at " +
              std::to_string(offset + byte) + " on invocation " +
              std::to_string(call + 1));
      }
    }
  }
  if (jit) {
    GLWASM_JitCacheStats stats{};
    if (glwasm_runtime_test_jit_stats(runtime.get(), corpus_function,
            &stats) != GLWASM_OK || stats.compile_successes == 0 ||
        stats.exit_counts[GLWASM_JIT_EXIT_RETURN] < 8 ||
        stats.guest_visible_exits != 0 ||
        stats.exit_counts[GLWASM_JIT_EXIT_GUARD_DEOPT] != 0 ||
        stats.exit_counts[GLWASM_JIT_EXIT_NAMED_UNSUPPORTED] != 0 ||
        stats.exit_counts[GLWASM_JIT_EXIT_SEMANTICALLY_UNSAFE] != 0)
      throw std::runtime_error("workload=" + workload +
          " export=corpus_run stage=jit-stats did not complete all eight "
          "invocations in compiled code without deopt" +
          runtime_failure_diagnostic(runtime.get(), module.get(),
              corpus_function));
  }
  if (stress != StressMode::None) {
    GRHEAP_Stats stats{};
    if (grheap_stats(heap, &stats) != GRHEAP_OK ||
        stats.torture_collections == 0)
      throw std::runtime_error("GC torture was configured but no torture collections ran");
    if (grcore_stack_move_count(stack) == 0)
      throw std::runtime_error("always-move stack was configured but never moved");
#ifdef GLWASM_CORPUS_RELOCATE
    if (stress == StressMode::Relocation) {
      GRHEAP_RelocationStats relocation{};
      if (grheap_relocation_stats(heap, &relocation) != GRHEAP_OK ||
          relocation.collections == 0 || relocation.moved == 0)
        throw std::runtime_error("relocation was configured but no live object moved");
      if (!probe_marker_matches(stress_slot.get(), kStressProbeValue) ||
          poll_probe.relocations != 8u)
        throw std::runtime_error("relocation did not update and preserve the rooted probe");
    }
#endif
    if (!probe_marker_matches(stress_slot.get(), kStressProbeValue))
      throw std::runtime_error("GC torture did not preserve the rooted probe");
    if (grheap_root_remove(heap, stress_slot.get()) != GRHEAP_OK)
      throw std::runtime_error("cannot remove GC stress probe root");
    stress_root.slot = nullptr;
  }
}

void check_workload(const char *workload, bool jit, StressMode stress) {
  const auto vectors = read_vectors();
  ASSERT_EQ(vectors.size(), kWorkloads.size()) << "invalid or incomplete vectors.tsv";
  ASSERT_NE(vectors.find(workload), vectors.end()) << "missing expected output";
  const std::string module = module_root() + "/" + workload + ".wasm";
  try {
    invoke(module, jit, vectors.at(workload), stress, workload);
  } catch (const std::exception &error) {
    FAIL() << "workload=" << workload << " tier="
           << (jit ? "jit" : "interpreter") << " stress="
           << (stress == StressMode::None ? "none" :
               stress == StressMode::GcTorture ? "gc-torture+moving-stack" :
               "gc-torture+relocation+moving-stack") << ": " << error.what();
  }
}

}  // namespace

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  const int result = RUN_ALL_TESTS();
  const auto *tests = ::testing::UnitTest::GetInstance();
#ifdef GLWASM_CORPUS_RELOCATE
  constexpr int kExpectedTests = 18;
#else
  constexpr int kExpectedTests = 12;
#endif
  if (tests->total_test_count() != kExpectedTests ||
      tests->test_to_run_count() != kExpectedTests ||
      tests->successful_test_count() != kExpectedTests ||
      tests->skipped_test_count() != 0) {
    std::fprintf(stderr,
        "real-corpus gate requires all %d cases: registered=%d selected=%d "
        "passed=%d skipped=%d\n", kExpectedTests, tests->total_test_count(),
        tests->test_to_run_count(), tests->successful_test_count(),
        tests->skipped_test_count());
    return 2;
  }
  return result;
}

#define GLWASM_CORPUS_TESTS(suite, stress) \
  TEST(suite, SQLite) { check_workload("sqlite", false, stress); } \
  TEST(suite, Zlib) { check_workload("zlib", false, stress); } \
  TEST(suite, Lua) { check_workload("lua", false, stress); } \
  TEST(suite##Jit, SQLite) { check_workload("sqlite", true, stress); } \
  TEST(suite##Jit, Zlib) { check_workload("zlib", true, stress); } \
  TEST(suite##Jit, Lua) { check_workload("lua", true, stress); }

GLWASM_CORPUS_TESTS(RealCorpusInterpreter, StressMode::None)
GLWASM_CORPUS_TESTS(RealCorpusGcTortureInterpreter, StressMode::GcTorture)
#ifdef GLWASM_CORPUS_RELOCATE
GLWASM_CORPUS_TESTS(RealCorpusRelocationInterpreter, StressMode::Relocation)
#endif
