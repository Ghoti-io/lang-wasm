/* SPDX-License-Identifier: LGPL-3.0-only */
/* Story 13 opt-in benchmark runner. This executable intentionally uses the
 * private tier/stat seams; it is not part of the installed ABI. */
#include <ghoti.io/lang-wasm/lang-wasm.h>
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/runtime-debug/model.h>
#include <ghoti.io/runtime-debug/runtime-debug.h>
#include <ghoti.io/runtime-core/a/boundary.h>
#include <ghoti.io/security/sha256.h>
#include "module_internal.h"
#include "parse/wat_lower_internal.h"
#include "runtime/runtime.h"

#include <array>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <charconv>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
const char *kHeader =
  "run_id,source_commit,source_manifest_sha256,environment_manifest_path,environment_manifest,environment_manifest_sha256,image_id,compiler,compiler_version,flags,machine,architecture,fixture_converter,fixture_converter_source_sha256,workload,workload_sha256,input_manifest,input_sha256,policy,fuel,work_unit,tier,debugger,phase,segment,warmup_work_units,sample_index,work_units,compiled_entries,compiled_returns,jit_side_exits,elapsed_ns,result_sha256,exit_code,error_message,command\n";
struct Meta {
  std::string run, commit, manifest, image, compiler, compiler_version, flags;
  std::string machine, arch, command, environment_manifest, environment_manifest_contents,
      environment_manifest_sha256;
  std::string converter = "lang-wasm-wat-lower", converter_source_sha256;
  std::string policy_name;
  GLWASM_PolicyPreset policy = GLWASM_POLICY_UNTRUSTED;
  uint64_t fuel = 0;
  uint64_t iterations = 20000;
  uint32_t samples = 30;
};
struct FailureContext {
  bool active = false;
  std::string workload, workload_hash, input_manifest, input_hash, work_unit;
  std::string tier, debugger, phase, segment;
  uint64_t warmup = 0, work_units = 1;
  uint32_t sample = 0;
  uint64_t elapsed_ns = 0, result = 0, entries = 0, returns = 0, side_exits = 0;
};
FailureContext failure_context;
struct BoundaryState {
  GRCORE_EngineId engine = 0;
  uint64_t calls = 0;
  GLWASM_Runtime *nested = nullptr;
};
static const GRCORE_EngineDescriptor kBoundaryEngine =
    GRCORE_ENGINE_DESCRIPTOR_INIT("wasm-benchmark-boundary", nullptr, nullptr,
        nullptr, {nullptr, nullptr, nullptr}, {0, 0, 0}, nullptr, nullptr,
        nullptr, nullptr);

std::string csv(const std::string &s) {
  std::string out = "\"";
  for (char c : s) { if (c == '"') out += '"'; out += c; }
  return out + '"';
}
std::string hash_bytes(const uint8_t *bytes, size_t length) {
  unsigned char digest[GSEC_SHA256_DIGEST_LEN]{};
  if (gsec_sha256(bytes, length, digest) != GSEC_OK)
    throw std::runtime_error("SHA-256 failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string result;
  result.reserve(2 * GSEC_SHA256_DIGEST_LEN);
  for (unsigned char byte : digest) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}
std::string hash_text(const std::string &s) {
  return hash_bytes(reinterpret_cast<const uint8_t *>(s.data()), s.size());
}
const char *tier_name(const std::string &workload,bool jit) {
  if(!jit)return "interpreter";
  return workload=="mixed-boundary"?"hybrid":"jit";
}
uint64_t expected(const std::string &workload, uint64_t n) {
  if (workload == "loop-numeric" || workload == "linear-memory")
    return n * (n - 1) / 2;
  if (workload == "calls") return n * (n + 3) / 2;
  if (workload == "table-bulk") return n * 84;
  if (workload == "mixed-boundary") return n * (n + 1) / 2;
  throw std::runtime_error("unknown workload: " + workload);
}
std::string read_file(const std::string &path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) throw std::runtime_error("cannot read fixture " + path);
  return std::string((std::istreambuf_iterator<char>(in)), {});
}
void require(GLWASM_Result r, const char *what);
std::vector<uint8_t> lower_workload_binary(const std::string &workload) {
  const std::string source=read_file("benchmarks/workloads/"+workload+".wat");
  GLWASM_ParseError error{};
  uint8_t *lowered=nullptr;
  size_t length=0;
  require(glwasm_wat_module_lower_binary(
      reinterpret_cast<const uint8_t *>(source.data()), source.size(),
      nullptr, nullptr, &error, &lowered, &length),
      "lower benchmark fixture to validated Wasm binary");
  std::vector<uint8_t> result(lowered,lowered+length);
  std::free(lowered);
  return result;
}
std::map<std::string, std::string> load_binary_pins() {
  std::ifstream input("benchmarks/workloads/module-hashes.tsv");
  if (!input) throw std::runtime_error("missing pinned Wasm module hash manifest");
  std::map<std::string, std::string> pins;
  std::string workload, digest;
  while (input >> workload >> digest) {
    if (workload.empty() || workload[0] == '#') {
      std::string ignored;
      std::getline(input, ignored);
      continue;
    }
    if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(),
        [](unsigned char c) { return std::isxdigit(c) != 0; }))
      throw std::runtime_error("invalid Wasm module hash pin for " + workload);
    if (!pins.emplace(workload, digest).second)
      throw std::runtime_error("duplicate Wasm module hash pin for " + workload);
  }
  if (pins.size() != 5) throw std::runtime_error("Wasm module hash manifest must pin all five fixtures");
  for (const char *name : {"loop-numeric", "calls", "linear-memory",
                           "table-bulk", "mixed-boundary"})
    if (pins.find(name) == pins.end())
      throw std::runtime_error(std::string("missing Wasm module hash pin for ") + name);
  return pins;
}
void require(GLWASM_Result r, const char *what) {
  if (r != GLWASM_OK) throw std::runtime_error(std::string(what) + ": " +
      glwasm_result_string(r));
}
GLWASM_Policy policy_for(const Meta &m) {
  GLWASM_Policy policy{};
  require(glwasm_policy_preset(m.policy, &policy), "load benchmark policy");
  policy.default_fuel = m.fuel;
  return policy;
}
GLWASM_Result boundary_call(const GLWASM_HostCall *call, uint64_t *out) {
  auto *state = static_cast<BoundaryState *>(call->user);
  if (!state || !state->nested || call->argument_count != 1)
    return GLWASM_ERR_INTERNAL;
  ++state->calls;
  return glwasm_runtime_invoke_nested(state->nested, call->context,
      state->engine, call->instance_token, "nested", call->arguments, 1, out);
}
struct Instance {
  GLWASM_Module *module = nullptr;
  GLWASM_Runtime *runtime = nullptr;
  GLWASM_Runtime *nested_runtime = nullptr;
  GRDBG_Debugger *debugger = nullptr;
  uint32_t function = 0;
  BoundaryState boundary;
  Instance() = default;
  Instance(const Instance &) = delete;
  Instance &operator=(const Instance &) = delete;
  ~Instance() {
    // runtime-debug attaches its debugger to the runtime context. Destroying
    // the context in glwasm_runtime_destroy invokes the registered key
    // destructor; the handle is borrowed and has no separate destroy API.
    if (nested_runtime) glwasm_runtime_destroy(nested_runtime);
    if (runtime) glwasm_runtime_destroy(runtime);
    if (module) glwasm_module_destroy(module);
  }
};
void make_instance(Instance &i, const std::vector<uint8_t> &binary,
                   const Meta &meta, const std::string &workload,
                   bool jit, bool debugger) {
  GLWASM_Policy policy = policy_for(meta);
  require(glwasm_module_decode_with_policy(binary.data(), binary.size(),
      &policy, nullptr, nullptr, &i.module), "decode benchmark Wasm binary");
  bool found_run = false;
  for (uint32_t n = 0; n < i.module->nexports; ++n) {
    const GLWASM_Export &e = i.module->exports[n];
    if (e.kind == GLWASM_EXT_FUNC && e.name.length == 3 &&
        std::memcmp(e.name.bytes, "run", 3) == 0) {
      i.function = e.index; found_run = true; break;
    }
  }
  if (!found_run || i.function >= i.module->nfuncs)
    throw std::runtime_error("run export missing");
  GLWASM_HostImport import{};
  GLWASM_HostImport *imports = nullptr;
  size_t import_count = 0;
  if (workload == "mixed-boundary") {
    import.module = "bench.v1"; import.field = "boundary";
    import.function = boundary_call; import.user = &i.boundary;
    imports = &import; import_count = 1;
  }
  require(glwasm_runtime_create_with_imports(i.module, imports, import_count, 0,
      nullptr, &i.runtime), "runtime creation");
  require(glwasm_runtime_test_set_jit_mode(i.runtime, jit
      ? GLWASM_TEST_JIT_REQUIRE : GLWASM_TEST_JIT_INTERPRETER), "set tier");
  if (workload == "mixed-boundary") {
    require(glwasm_runtime_register_boundary_engine(i.runtime,
        &kBoundaryEngine, &i.boundary.engine), "register boundary engine");
    require(glwasm_runtime_create_with_imports(i.module, imports, import_count,
        0, nullptr, &i.nested_runtime), "nested runtime creation");
    i.boundary.nested = i.nested_runtime;
    /* This runtime is entered on the outer runtime-core context through the
     * explicit mixed-boundary callback. Its JIT entry slots belong to its
     * private context, so keep this side of the boundary interpreted while
     * the exported outer workload remains in REQUIRE-JIT mode. */
    require(glwasm_runtime_test_set_jit_mode(i.nested_runtime,
        GLWASM_TEST_JIT_INTERPRETER), "set nested boundary tier");
  }
  if (debugger)
    if (grdbg_debugger_attach(glwasm_runtime_context(i.runtime), nullptr,
            &i.debugger) != GRDBG_OK)
      throw std::runtime_error("debugger attach failed");
}
uint64_t invoke(Instance &i, uint64_t n) {
  uint64_t arg = n, result = 0;
  require(glwasm_runtime_invoke(i.runtime, i.function, &arg, 1, &result),
      "invoke workload");
  return result;
}
uint64_t compiled_entries(Instance &i, bool jit) {
  uint64_t entries = 0;
  for (uint32_t function = 0; function < i.module->nfuncs; ++function) {
    GLWASM_JitCacheStats stats{};
    require(glwasm_runtime_test_jit_stats(i.runtime, function, &stats),
        "read compiled-entry stats");
    entries += stats.compiled_entries;
  }
  /* Both instances share this module-owned cache, so reading it through the
   * outer runtime already includes entries reached by nested dispatch. */
  if (jit && entries == 0)
    throw std::runtime_error("REQUIRE-JIT sample entered no compiled code");
  if (!jit && entries != 0)
    throw std::runtime_error("interpreter sample entered compiled code");
  return entries;
}
void require_compiled_completion(Instance &i, uint64_t minimum_run_returns,
                                const char *phase) {
  uint64_t entries = 0;
  for (uint32_t function = 0; function < i.module->nfuncs; ++function) {
    GLWASM_JitCacheStats stats{};
    require(glwasm_runtime_test_jit_stats(i.runtime, function, &stats),
        "read compiled-path stats");
    entries += stats.compiled_entries;
    for (uint32_t exit = 0; exit < GLWASM_JIT_EXIT_COUNT; ++exit) {
      if (exit != GLWASM_JIT_EXIT_RETURN && stats.exit_counts[exit] != 0)
        throw std::runtime_error(std::string(phase) +
            " used a JIT side exit (function=" + std::to_string(function) +
            ", exit=" + std::to_string(exit) + ")");
    }
  }
  GLWASM_JitCacheStats run_stats{};
  require(glwasm_runtime_test_jit_stats(i.runtime, i.function, &run_stats),
      "read run-function compiled-path stats");
  if (!entries || run_stats.exit_counts[GLWASM_JIT_EXIT_RETURN] <
          minimum_run_returns)
    throw std::runtime_error(std::string(phase) +
        " did not complete the named run function in compiled code");
}
std::pair<uint64_t, uint64_t> path_evidence(Instance &i, bool jit) {
  GLWASM_JitCacheStats run_stats{};
  require(glwasm_runtime_test_jit_stats(i.runtime, i.function, &run_stats),
      "read run-function compiled returns");
  const uint64_t returns = run_stats.exit_counts[GLWASM_JIT_EXIT_RETURN];
  uint64_t side_exits = 0;
  for (uint32_t function = 0; function < i.module->nfuncs; ++function) {
    GLWASM_JitCacheStats stats{};
    require(glwasm_runtime_test_jit_stats(i.runtime, function, &stats),
        "read compiled-path evidence");
    for (uint32_t exit = 0; exit < GLWASM_JIT_EXIT_COUNT; ++exit)
      if (exit != GLWASM_JIT_EXIT_RETURN) side_exits += stats.exit_counts[exit];
  }
  if (!jit && (returns || side_exits))
    throw std::runtime_error("interpreter sample reported compiled-path evidence");
  return {returns, side_exits};
}
void emit(const Meta &m, const std::string &workload, const std::string &whash,
          const std::string &input_manifest, const std::string &ihash,
          const std::string &unit,
          const std::string &tier, const std::string &debugger,
          const std::string &phase, const std::string &segment,
          uint64_t warmup, uint32_t sample, uint64_t work_units,
          uint64_t entries, uint64_t returns, uint64_t side_exits,
          uint64_t elapsed, uint64_t result, int exit_code = 0,
          const std::string &error_message = {}) {
  std::string result_text = std::to_string(result);
  std::cout << csv(m.run) << ',' << csv(m.commit) << ',' << csv(m.manifest)
      << ',' << csv(m.environment_manifest) << ','
      << csv(m.environment_manifest_contents) << ','
      << m.environment_manifest_sha256 << ',' << csv(m.image) << ',' << csv(m.compiler) << ','
      << csv(m.compiler_version) << ',' << csv(m.flags) << ','
      << csv(m.machine) << ',' << csv(m.arch) << ',' << m.converter << ','
      << m.converter_source_sha256 << ',' << csv(workload) << ','
      << whash << ',' << csv(input_manifest) << ',' << ihash << ','
      << m.policy_name << ',' << m.fuel << ',' << csv(unit) << ','
      << tier << ',' << debugger << ',' << phase << ',' << segment << ','
      << warmup << ',' << sample << ',' << work_units << ',' << entries << ','
      << returns << ',' << side_exits << ',' << elapsed << ','
      << hash_text(exit_code ? error_message : result_text) << ',' << exit_code
      << ',' << csv(error_message) << ',' << csv(m.command)
      << '\n';
  failure_context.active = false;
}
void start_sample(const std::string &workload, const std::string &whash,
    const std::string &input_manifest, const std::string &ihash,
    const std::string &unit, const std::string &tier,
    const std::string &debugger, const std::string &phase,
    const std::string &segment, uint64_t warmup, uint32_t sample,
    uint64_t work_units) {
  failure_context = {true, workload, whash, input_manifest, ihash, unit, tier,
      debugger, phase, segment, warmup, work_units, sample};
}
std::string make_input_manifest(const Meta &m, uint64_t warmup_count) {
  return "iterations=" + std::to_string(m.iterations) + ";warmup=" +
      std::to_string(warmup_count) + ";samples=" +
      std::to_string(m.samples) + ";policy=" + m.policy_name +
      ";fuel=" + std::to_string(m.fuel);
}
void record_sample_result(uint64_t elapsed_ns, uint64_t result) {
  if (!failure_context.active) return;
  failure_context.elapsed_ns = elapsed_ns;
  failure_context.result = result;
}
void record_sample_path(uint64_t entries, uint64_t returns, uint64_t side_exits) {
  if (!failure_context.active) return;
  failure_context.entries = entries;
  failure_context.returns = returns;
  failure_context.side_exits = side_exits;
}
void emit_failed_sample(const Meta &m, const std::string &message) {
  if (!failure_context.active) return;
  const FailureContext f = failure_context;
  emit(m, f.workload, f.workload_hash, f.input_manifest, f.input_hash,
      f.work_unit, f.tier, f.debugger, f.phase, f.segment, f.warmup,
      f.sample, f.work_units, f.entries, f.returns, f.side_exits,
      f.elapsed_ns, f.result, 1, message);
}
std::string env(const char *key) {
  const char *v = std::getenv(key);
  if (!v || !*v) throw std::runtime_error(std::string("required environment variable missing: ") + key);
  return v;
}
uint64_t parse_u64(const char *text, const char *name, uint64_t minimum,
                   uint64_t maximum) {
  if (!text || !*text) throw std::runtime_error(std::string("missing ") + name);
  uint64_t value = 0;
  const char *end = text + std::strlen(text);
  auto parsed = std::from_chars(text, end, value, 10);
  if (parsed.ec != std::errc{} || parsed.ptr != end || value < minimum ||
      value > maximum)
    throw std::runtime_error(std::string("invalid ") + name + ": " + text);
  return value;
}
void run_group(const Meta &m, const std::string &workload,
    const std::vector<uint8_t> &binary, const std::string &whash,
    const std::string &input_manifest, const std::string &ihash,
    bool jit, bool idle_debugger, uint32_t sample) {
  const uint64_t expected_result = expected(workload, m.iterations);
  uint64_t units_per_iteration = workload == "linear-memory" ? 8 :
      workload == "table-bulk" ? 4 : workload == "calls" ? 2 : 1;
  const char *work_unit = workload == "linear-memory" ? "byte" :
      workload == "table-bulk" ? "operation" :
      workload == "calls" ? "call" : workload == "mixed-boundary" ?
      "boundary-call" : "guest-iteration";
    start_sample(workload, whash, input_manifest, ihash, work_unit,
        tier_name(workload,jit), idle_debugger ? "attached-idle" : "none",
        "steady-state", "whole", 2 * m.iterations * units_per_iteration,
        sample, m.iterations * units_per_iteration);
    if (std::getenv("GLWASM_BENCH_TEST_FAIL_AFTER_SETUP"))
      throw std::runtime_error("injected failure after sample setup");
    Instance i;
    make_instance(i, binary, m, workload, jit, idle_debugger);
    for (unsigned w = 0; w < 2; ++w)
      if (invoke(i, m.iterations) != expected_result)
        throw std::runtime_error("warmup result differs from expected output");
    i.boundary.calls = 0;
    const auto start = Clock::now();
    uint64_t result = invoke(i, m.iterations);
    const auto stop = Clock::now();
    const uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();
    record_sample_result(elapsed, result);
    if (result != expected_result)
      throw std::runtime_error("timed result differs from expected output");
    if (workload == "mixed-boundary" && i.boundary.calls != m.iterations)
      throw std::runtime_error("timed boundary call count differs from guest work");
    uint64_t entries = compiled_entries(i, jit);
    record_sample_path(entries, 0, 0);
    auto [returns, side_exits] = path_evidence(i, jit);
    record_sample_path(entries, returns, side_exits);
    if (jit) require_compiled_completion(i, 3,
        "warmup and timed steady-state");
    emit(m, workload, whash, input_manifest, ihash, work_unit,
        tier_name(workload,jit), idle_debugger ? "attached-idle" : "none",
        "steady-state", "whole", 2 * m.iterations * units_per_iteration,
        sample, m.iterations * units_per_iteration,
        entries, returns, side_exits,
        elapsed, result);
}
void run_first_call(const Meta &m, const std::string &workload,
    const std::vector<uint8_t> &binary, const std::string &whash,
    uint32_t sample) {
    const std::string phase_input = make_input_manifest(m, 0);
    const std::string phase_input_hash = hash_text(phase_input);
    start_sample(workload, whash, phase_input, phase_input_hash, "first-call",
        tier_name(workload,true), "none", "first-call", "first-call", 0,
        sample, 1);
    Instance i;
    make_instance(i, binary, m, workload, true, false);
    i.boundary.calls = 0;
    const auto start = Clock::now();
    uint64_t result = invoke(i, m.iterations);
    const auto stop = Clock::now();
    const uint64_t elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();
    record_sample_result(elapsed, result);
    if (result != expected(workload, m.iterations))
      throw std::runtime_error("first-call result differs from expected output");
    if (workload == "mixed-boundary" && i.boundary.calls != m.iterations)
      throw std::runtime_error("cold boundary call count differs from guest work");
    uint64_t entries = compiled_entries(i, true);
    record_sample_path(entries, 0, 0);
    auto [returns, side_exits] = path_evidence(i, true);
    record_sample_path(entries, returns, side_exits);
    require_compiled_completion(i, 1, "first-call sample");
    emit(m, workload, whash, phase_input, phase_input_hash, "first-call",
        tier_name(workload,true), "none", "first-call", "first-call", 0,
        sample, 1, entries, returns, side_exits,
        elapsed, result);
}
void run_step(const Meta &m, const std::vector<uint8_t> &binary,
    const std::string &whash, uint32_t sample) {
    const std::string phase_input = make_input_manifest(m, 0);
    const std::string phase_input_hash = hash_text(phase_input);
    const uint64_t expected_result = expected("loop-numeric", m.iterations);
    Instance validation;
    make_instance(validation, binary, m, "loop-numeric", false, false);
    if (invoke(validation, m.iterations) != expected_result)
      throw std::runtime_error("step diagnostic preflight result differs from expected output");
    start_sample("loop-numeric", whash, phase_input, phase_input_hash, "event",
        "interpreter", "step-diagnostic", "poll-event", "stop", 0,
        sample, 1);
    Instance i;
    make_instance(i, binary, m, "loop-numeric", false, true);
    GRCORE_Context *context = glwasm_runtime_context(i.runtime);
    GRCORE_EngineId engine = 0;
    for (GRCORE_EngineId n = 1; n <= grcore_engine_count(context); ++n) {
      const GRCORE_EngineDescriptor *d = grcore_engine_descriptor(context, n);
      if (d && std::strcmp(d->name, "lang-wasm") == 0) engine = n;
    }
    if (!engine || !i.module->funcs[i.function].translated ||
        !i.module->funcs[i.function].translated->offset_count)
      throw std::runtime_error("cannot resolve first Wasm poll identity");
    const uint64_t identity_function =
        (uint64_t(i.module->debug_id) << 32) | i.function;
    GRCORE_PollIdentity identity{identity_function,
      i.module->funcs[i.function].translated->offsets[0], engine};
    uint64_t id = 0;
    const std::string debugger_module_identity =
        "wasm-module-" + std::to_string(i.module->debug_id);
    if (identity_function >> 32 != i.module->debug_id)
      throw std::runtime_error("poll function identity lost its decoded module id");
    if (grdbg_debugger_set_instruction_breakpoints(i.debugger,
            debugger_module_identity.c_str(), &identity, 1, &id) != GRDBG_OK)
      throw std::runtime_error("cannot set first-instruction breakpoint");
    uint64_t arg = m.iterations, result = 0;
    auto start = Clock::now();
    GLWASM_Result r = glwasm_runtime_invoke(i.runtime, i.function, &arg, 1, &result);
    auto stop = Clock::now();
    const uint64_t stop_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();
    record_sample_result(stop_ns, result);
    if (r != GLWASM_ERR_PAUSED) throw std::runtime_error("breakpoint did not pause execution");
    GRCORE_PollIdentity stopped_identity{};
    if (grcore_context_poll_identity(context, &stopped_identity) != GRCORE_OK ||
        stopped_identity.function != identity_function ||
        stopped_identity.engine != engine ||
        stopped_identity.offset != identity.offset)
      throw std::runtime_error("initial stop identity does not match the requested module/function/site");
    GRDBG_Stop stopped{};
    if (grdbg_debugger_stopped(i.debugger, &stopped) != GRDBG_OK ||
        stopped.reason != GRDBG_STOP_BREAKPOINT)
      throw std::runtime_error("initial debugger stop was not a breakpoint");
    emit(m, "loop-numeric", whash, phase_input, phase_input_hash, "event", "interpreter",
        "step-diagnostic", "poll-event", "stop", 0, sample, 1, 0, 0, 0,
        stop_ns, expected_result);
    start_sample("loop-numeric", whash, phase_input, phase_input_hash, "event",
        "interpreter", "step-diagnostic", "poll-event", "resume", 0,
        sample, 1);
    if (grdbg_debugger_step(i.debugger, GRDBG_STEP_INSTRUCTION) != GRDBG_OK)
      throw std::runtime_error("cannot request instruction step");
    start = Clock::now();
    int paused = 0;
    r = glwasm_runtime_resume(i.runtime, &result, &paused);
    stop = Clock::now();
    const uint64_t resume_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(stop-start).count();
    record_sample_result(resume_ns, result);
    if (r != GLWASM_ERR_PAUSED || !paused)
      throw std::runtime_error("instruction step did not stop with correct result");
    GRCORE_PollIdentity step_identity{};
    if (grcore_context_poll_identity(context, &step_identity) != GRCORE_OK ||
        step_identity.function != stopped_identity.function ||
        step_identity.engine != stopped_identity.engine ||
        step_identity.offset <= stopped_identity.offset)
      throw std::runtime_error("instruction step stop identity did not advance within the same Wasm function");
    if (grdbg_debugger_stopped(i.debugger, &stopped) != GRDBG_OK ||
        stopped.reason != GRDBG_STOP_STEP)
      throw std::runtime_error("resume stop was not a step");
    if (grdbg_debugger_continue(i.debugger) != GRDBG_OK)
      throw std::runtime_error("cannot clear debugger step request");
    while (r == GLWASM_ERR_PAUSED) r = glwasm_runtime_resume(i.runtime, &result, &paused);
    if (r != GLWASM_OK || result != expected_result)
      throw std::runtime_error("post-diagnostic invocation failed");
    const uint64_t entries = compiled_entries(i, false);
    auto [returns, side_exits] = path_evidence(i, false);
    record_sample_path(entries, returns, side_exits);
    emit(m, "loop-numeric", whash, phase_input, phase_input_hash, "event", "interpreter",
        "step-diagnostic", "poll-event", "resume", 0, sample, 1, entries, returns, side_exits,
        resume_ns, result);
}
} // namespace

int main(int argc, char **argv) {
  std::streambuf *old_output = nullptr;
  std::ofstream output;
  Meta m;
  try {
    const std::array<std::string,5> workloads={"loop-numeric","calls","linear-memory","table-bulk","mixed-boundary"};
    if(argc==2&&std::strcmp(argv[1],"--print-fixture-hashes")==0){
      for(const auto &w:workloads){
        const auto bytes=lower_workload_binary(w);
        std::cout<<w<<'\t'<<hash_bytes(bytes.data(),bytes.size())<<'\n';
      }
      return 0;
    }
    if (argc > 3) throw std::runtime_error("usage: wasm-benchmark [iterations<=100000] [samples>=30, multiple of 30]");
    if (argc > 1) m.iterations = parse_u64(argv[1], "iterations", 1, 100000);
    if (argc > 2) m.samples = static_cast<uint32_t>(parse_u64(
        argv[2], "samples", 30, UINT32_MAX));
    if (!m.iterations || m.iterations > 100000 || m.samples < 30 || m.samples % 30 != 0)
      throw std::runtime_error("usage: wasm-benchmark [iterations<=100000] [samples>=30, multiple of 30]");
    m.run=env("GLWASM_BENCH_RUN_ID"); m.commit=env("GLWASM_BENCH_SOURCE_COMMIT");
    m.manifest=env("GLWASM_BENCH_SOURCE_MANIFEST_SHA256"); m.image=env("GLWASM_BENCH_IMAGE_ID");
    m.compiler=env("GLWASM_BENCH_COMPILER"); m.compiler_version=env("GLWASM_BENCH_COMPILER_VERSION");
    m.flags=env("GLWASM_BENCH_FLAGS"); m.machine=env("GLWASM_BENCH_MACHINE");
    m.arch=env("GLWASM_BENCH_ARCHITECTURE");
    m.converter_source_sha256=hash_text(read_file("src/parse/wat_lower.c") + std::string(1, '\0') +
        read_file("src/parse/wat_lower_internal.h"));
    m.command=env("GLWASM_BENCH_COMMAND");
    const std::string expected_command = "make benchmark BENCH_ITERATIONS=" +
        std::to_string(m.iterations) + " BENCH_SAMPLES=" +
        std::to_string(m.samples);
    if (m.command != expected_command)
      throw std::runtime_error("GLWASM_BENCH_COMMAND must exactly match " +
          expected_command);
    const std::string environment_manifest_file=env("GLWASM_BENCH_ENV_MANIFEST");
    m.environment_manifest_contents=read_file(environment_manifest_file);
    if(m.environment_manifest_contents.empty()) throw std::runtime_error("environment manifest is empty");
    // record_build.py requires the manifest beside the raw CSV. Record its
    // portable sibling name, not the absolute path used to open it, so a
    // fetched result bundle remains independently verifiable.
    const auto separator=environment_manifest_file.find_last_of("/\\");
    m.environment_manifest=environment_manifest_file.substr(
        separator==std::string::npos ? 0 : separator+1);
    if(m.environment_manifest.empty()) throw std::runtime_error("environment manifest has no filename");
    m.environment_manifest_sha256=hash_text(m.environment_manifest_contents);
    m.policy_name=env("GLWASM_BENCH_POLICY");
    if(m.policy_name=="untrusted")m.policy=GLWASM_POLICY_UNTRUSTED;
    else if(m.policy_name=="standard")m.policy=GLWASM_POLICY_STANDARD;
    else if(m.policy_name=="trusted")m.policy=GLWASM_POLICY_TRUSTED;
    else throw std::runtime_error("GLWASM_BENCH_POLICY must be untrusted, standard, or trusted");
    m.fuel=parse_u64(std::getenv("GLWASM_BENCH_FUEL"),"GLWASM_BENCH_FUEL",1,UINT64_MAX);
    output.open(env("GLWASM_BENCH_OUTPUT"), std::ios::trunc);
    if (!output) throw std::runtime_error("cannot open GLWASM_BENCH_OUTPUT");
    old_output = std::cout.rdbuf(output.rdbuf());
    const auto binary_pins=load_binary_pins();
    std::array<std::vector<uint8_t>,5> binaries;
    std::array<std::string,5> binary_hashes;
    for (size_t index = 0; index < workloads.size(); ++index) {
      const auto &workload = workloads[index];
      binaries[index] = lower_workload_binary(workload);
      binary_hashes[index] = hash_bytes(binaries[index].data(), binaries[index].size());
      if (binary_hashes[index] != binary_pins.at(workload))
        throw std::runtime_error("lowered Wasm fixture hash differs from its checked-in pin: " + workload);
    }
    std::cout << kHeader;
    for (uint32_t sample = 0; sample < m.samples; ++sample) {
      const uint8_t orders[6][3]={{0,1,2},{1,2,0},{2,0,1},
                                  {2,1,0},{1,0,2},{0,2,1}};
      // Rotate workload order across samples so a fixed workload block does
      // not confound per-workload results with thermal or background drift.
      for (size_t offset = 0; offset < workloads.size(); ++offset) {
        const size_t index = (offset + sample) % workloads.size();
        const auto &w = workloads[index];
        const auto &binary = binaries[index];
        const auto &whash = binary_hashes[index];
        const std::string input_manifest=make_input_manifest(m,2);
        const std::string ihash=hash_text(input_manifest);
        for(uint8_t tier : orders[sample%6]){
          if(tier==0)run_group(m,w,binary,whash,input_manifest,ihash,false,false,sample);
          else if(tier==1)run_group(m,w,binary,whash,input_manifest,ihash,true,false,sample);
          else run_group(m,w,binary,whash,input_manifest,ihash,false,true,sample);
        }
        if((sample%2)==0)run_first_call(m,w,binary,whash,sample);
        if(w=="loop-numeric")run_step(m,binary,whash,sample);
        if((sample%2)!=0)run_first_call(m,w,binary,whash,sample);
      }
    }
    std::cout.flush();
    std::cout.rdbuf(old_output);
    if (!output) throw std::runtime_error("raw CSV write failed");
  } catch (const std::exception &e) {
    emit_failed_sample(m, e.what());
    output.flush();
    if (old_output) std::cout.rdbuf(old_output);
    std::cerr << "wasm-benchmark: " << e.what() << '\n';
    return 1;
  }
  return 0;
}
