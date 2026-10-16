/* Internal Story 3 interpreter interface. */
#ifndef GHOTI_IO_GLWASM_RUNTIME_H
#define GHOTI_IO_GLWASM_RUNTIME_H

#include <ghoti.io/lang-wasm/macros.h>
#include "../module_internal.h"
#include <ghoti.io/runtime-core/runtime-core.h>

typedef struct GLWASM_Runtime GLWASM_Runtime;
typedef enum { GLWASM_TRAP_NONE=0, GLWASM_TRAP_UNREACHABLE,
  GLWASM_TRAP_DIVIDE_BY_ZERO, GLWASM_TRAP_INTEGER_OVERFLOW,
  GLWASM_TRAP_TABLE_OUT_OF_BOUNDS, GLWASM_TRAP_UNINITIALIZED_ELEMENT,
  GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH } GLWASM_RuntimeTrap;
#ifdef __cplusplus
extern "C" {
#endif
GLWASM_Result glwasm_runtime_create(GLWASM_Module *module, GLWASM_Runtime **out);
/* The runtime borrows its module; destroy the runtime before the module. */
void glwasm_runtime_destroy(GLWASM_Runtime *runtime);
/* Arguments and the result use Wasm value bits in zero-extended 64-bit words:
 * i32/f32 occupy the low 32 bits, and i64/f64 use all 64 bits. The function
 * signature determines each word's type. */
GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime *runtime, uint32_t function,
    const uint64_t *args, uint32_t nargs, uint64_t *result);
/* A pause returns GLWASM_ERR_PAUSED while keeping the run active; resume it or
 * destroy the runtime before starting another invocation. Fuel and depth
 * exhaustion return GLWASM_ERR_LIMIT and do not leave a resumable run. */
GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime *runtime, uint64_t *result,
    int *paused);
GLWASM_Result glwasm_runtime_request_test_pause(GLWASM_Runtime *runtime);
GLWASM_Result glwasm_runtime_set_test_limits(GLWASM_Runtime *runtime,
    uint64_t fuel, uint64_t guest_depth, int always_move_stack);
GLWASM_RuntimeTrap glwasm_runtime_last_trap(const GLWASM_Runtime *runtime);
uint64_t glwasm_runtime_test_fuel_used(const GLWASM_Runtime *runtime);
#ifdef __cplusplus
}
#endif

#endif
