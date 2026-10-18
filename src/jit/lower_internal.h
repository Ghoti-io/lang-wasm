/*
 * SPDX-License-Identifier: LGPL-3.0-only
 */

#ifndef GHOTI_IO_GLWASM_JIT_LOWER_INTERNAL_H
#define GHOTI_IO_GLWASM_JIT_LOWER_INTERNAL_H

#include "../module_internal.h"
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/runtime-core/a/code_pages.h>
#include <ghoti.io/runtime-jit/runtime-jit.h>

GLWASM_Result glwasm_jit_lower_function(GLWASM_Module *module,
    uint32_t function_index, GRCORE_CodePages *pages,
    GRJIT_CallHooks *hooks, GRJIT_PollHelper poll_helper,
    uintptr_t charge_helper, uintptr_t named_exit_helper,
    uintptr_t trap_exit_helper,
    uintptr_t spill_base_helper, uintptr_t memory_helper,
    uintptr_t global_get_helper, uintptr_t global_set_helper,
    uintptr_t helper_status_helper, uintptr_t slot_entry_helper,
    uintptr_t indirect_target_helper, uintptr_t host_call_helper,
    uintptr_t table_bulk_helper, uintptr_t memory_bulk_helper,
    uintptr_t exception_take_helper, uintptr_t exception_throw_helper,
    uintptr_t exception_throw_ref_helper,
    GRJIT_Code **out_code,
    uint16_t *unsupported_opcode, uint32_t *failure_pc,
    uint32_t *failure_jit_result, uint16_t *failure_opcode,
    uint32_t *failure_stack_count, uint32_t *failure_site);

#endif
