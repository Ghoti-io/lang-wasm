/* Internal Story 3 interpreter interface. */
#ifndef GHOTI_IO_GLWASM_RUNTIME_H
#define GHOTI_IO_GLWASM_RUNTIME_H

#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/host.h>
#include "../module_internal.h"
#include "../jit/cache_internal.h"
#include <ghoti.io/runtime-core/runtime-core.h>

typedef struct GLWASM_Runtime GLWASM_Runtime;
typedef enum GLWASM_TestJitMode {
  GLWASM_TEST_JIT_INTERPRETER = 0,
  GLWASM_TEST_JIT_AUTO = 1,
  GLWASM_TEST_JIT_REQUIRE = 2
} GLWASM_TestJitMode;
/* Trap kinds are public: GLWASM_Trap in host.h. */
typedef struct GLWASM_TestDeoptOrigin {
  uint64_t cause;
  uint32_t function;
  uint32_t pc;
  uint32_t named_function;
  uint32_t compiled_depth;
  uint16_t named_opcode;
  uint8_t has_frame;
} GLWASM_TestDeoptOrigin;
typedef struct GLWASM_TestRuntimeFailureInfo {
  GLWASM_Result run_error;
  GLWASM_Trap trap;
  GRCORE_Result core_result;
  GRCORE_Outcome outcome;
  uint32_t last_function;
  uint32_t last_pc;
  uint32_t call_target;
  uint64_t call_sp;
  uint64_t call_base;
  uint64_t call_required;
  uint32_t caller_params;
  uint32_t caller_locals;
  uint32_t callee_params;
  uint32_t frame_entry_function;
  uint64_t frame_entry_sp;
  uint64_t frame_entry_base;
  uint32_t first_bad_sp_function;
  uint32_t first_bad_sp_pc;
  uint64_t first_bad_sp;
  uint64_t first_bad_base;
  uint32_t jit_lower_pc;
  uint32_t jit_lower_raw_result;
  uint32_t jit_lower_stack_count;
  uint32_t jit_lower_site;
  uint64_t deopt_count;
  GLWASM_TestDeoptOrigin first_deopt;
  GLWASM_TestDeoptOrigin last_deopt;
  uint16_t jit_lower_opcode;
  uint16_t last_opcode;
  uint8_t diagnostic_reason;
  uint8_t has_last_instruction;
  uint8_t has_jit_cache;
  uint8_t has_jit_pages;
  uint8_t has_jit_slots;
  uint8_t has_jit_registered;
  uint8_t has_function_slot;
  uint8_t has_function_registered;
  uint8_t context_matches_runtime;
  uint8_t engine_matches_runtime;
  uint8_t last_step;
  uint8_t context_state_after_step;
  GLWASM_Result jit_compile_result;
  uint8_t jit_compile_attempted;
} GLWASM_TestRuntimeFailureInfo;
#ifdef __cplusplus
extern "C" {
#endif
/* Test seams: one instance in a store of its own (made from the module's
 * policy), destroyed together by glwasm_runtime_destroy. An instance made
 * by glwasm_store_instantiate belongs to its store; destroying it here is
 * a no-op. */
GLWASM_Result glwasm_runtime_create(GLWASM_Module *module, GLWASM_Runtime **out);
GLWASM_Result glwasm_runtime_create_with_imports(GLWASM_Module *module,
    const GLWASM_HostImport *imports, size_t import_count, int allow_start,
    GLWASM_LinkError *link_error, GLWASM_Runtime **out);

/* The runtime retains the module until runtime destruction. */
void glwasm_runtime_destroy(GLWASM_Runtime *runtime);
/* Arguments and the result use Wasm value bits in zero-extended 64-bit words:
 * i32/f32 occupy the low 32 bits, and i64/f64 use all 64 bits. The function
 * signature determines each word's type. */
GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime *runtime, uint32_t function,
    const uint64_t *args, uint32_t nargs, uint64_t *result);
GLWASM_Result glwasm_runtime_invoke_results(GLWASM_Runtime *runtime,
    uint32_t function,const uint64_t *args,uint32_t nargs,uint64_t *results,
    uint32_t *result_count);
/* Test-only mixed-engine entry. Executes one Wasm export synchronously on an
 * already-running runtime-core context, preserving the caller's frames. */
GLWASM_Result glwasm_runtime_invoke_nested(GLWASM_Runtime *runtime,
    GRCORE_Context *context, GRCORE_EngineId caller, uintptr_t instance_token,
    const char *export_name, const uint64_t *args, uint32_t nargs,
    uint64_t *result);
/* A pause returns GLWASM_ERR_PAUSED while keeping the run active; resume it or
 * destroy the runtime before starting another invocation. Fuel and depth
 * exhaustion return GLWASM_ERR_LIMIT and do not leave a resumable run. */
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime *runtime, uint64_t *result,
    int *paused);
GLWASM_Result glwasm_runtime_resume_results(GLWASM_Runtime *runtime,
    uint64_t *results,uint32_t *result_count,int *paused);
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime *runtime);
/* Test-only: make exhausted fuel pause, resumably, instead of unwinding,
 * so a host can stop a run at a fuel count it chose. */
GLWASM_Result glwasm_store_test_pause_on_fuel(GLWASM_Store *store,
    int enabled);
/* Test-only: interrupt at the poll after the named 4096-byte compiled bulk
 * chunk. Zero disarms the one-shot request. */
GLWASM_Result glwasm_runtime_test_pause_after_bulk_chunks(
    GLWASM_Runtime *runtime, uint32_t chunks);
/* Private test seam: reset enables observation while the runtime is idle.
 * The caller supplies storage for one byte per defined or imported function.
 * A short buffer returns LIMIT and reports the required count without writes. */
GLWASM_Result glwasm_runtime_test_observed_functions(
    const GLWASM_Runtime *runtime, uint8_t *out, uint32_t capacity,
    uint32_t *required);
GLWASM_Result glwasm_runtime_test_reset_observed_functions(
    GLWASM_Runtime *runtime);
GLWASM_Result glwasm_runtime_set_test_limits(GLWASM_Runtime *runtime,
    uint64_t fuel, uint64_t guest_depth, int always_move_stack);
GLWASM_Trap glwasm_runtime_last_trap(const GLWASM_Runtime *runtime);
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime *runtime);
GLWASM_Result glwasm_runtime_test_set_jit_mode(GLWASM_Runtime *runtime,
    GLWASM_TestJitMode mode);
GLWASM_Result glwasm_runtime_test_failure_info(GLWASM_Runtime *runtime,
    uint32_t function, GLWASM_TestRuntimeFailureInfo *out);
GLWASM_Result glwasm_runtime_test_jit_stats(GLWASM_Runtime *runtime,
    uint32_t function, GLWASM_JitCacheStats *out);
GLWASM_Result glwasm_runtime_test_retire_jit_function(
    GLWASM_Runtime *runtime, uint32_t function);
uint64_t glwasm_runtime_test_jit_opcode_exit_count(GLWASM_Runtime *runtime,
    uint16_t opcode);
#ifdef __cplusplus
}
#endif

#endif
