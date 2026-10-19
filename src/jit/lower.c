/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include "lower_internal.h"
#include "cache_internal.h"
#include "../runtime/runtime_layout.h"

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/cutil/safemath.h>

#include <limits.h>
#include <string.h>

/* runtime-jit's IR currently caps callable arguments/results at 16. */
#define WASM_JIT_MAX_CALL_VALUES 16u

typedef struct GLWASM_JitValue {
  GRJIT_VReg reg;
        GRJIT_Type type;
} GLWASM_JitValue;

static GLWASM_Result map_jit_result(GRJIT_Result result) {
  switch (result) {
    case GRJIT_OK: return GLWASM_OK;
    case GRJIT_ERR_OOM: return GLWASM_ERR_OOM;
    case GRJIT_ERR_LIMIT: return GLWASM_ERR_LIMIT;
    case GRJIT_ERR_UNSUPPORTED: return GLWASM_ERR_UNSUPPORTED;
    case GRJIT_ERR_INVALID: return GLWASM_ERR_INVALID;
    default: return GLWASM_ERR_INTERNAL;
  }
}

static GLWASM_Result map_jit_result_capture(GRJIT_Result result,
    uint32_t *raw_result) {
  if (raw_result) *raw_result = (uint32_t)result;
  return map_jit_result(result);
}

static int wasm_jit_type(GLWASM_ValType type, GRJIT_Type *out) {
  if (type.kind == GLWASM_VK_I32) *out = GRJIT_TYPE_I32;
  else if (type.kind == GLWASM_VK_I64) *out = GRJIT_TYPE_I64;
  else if (type.kind == GLWASM_VK_F32) *out = GRJIT_TYPE_F32;
  else if (type.kind == GLWASM_VK_F64) *out = GRJIT_TYPE_F64;
  /* Function and exception references are stable module/instance tokens,
   * retained by their owning table or exception list. Externref and typed
   * heap references still require a separate GC-root representation. */
  else if (type.kind == GLWASM_VK_REF &&
      (type.heap == GLWASM_HT_FUNC || type.heap == GLWASM_HT_NOFUNC ||
       type.heap == GLWASM_HT_EXN || type.heap == GLWASM_HT_NOEXN))
    *out = GRJIT_TYPE_I64;
  else return 0;
  return 1;
}

static int wasm_jit_same_type(GLWASM_ValType a, GLWASM_ValType b) {
  return a.kind == b.kind && a.nullable == b.nullable && a.heap == b.heap &&
      a.mut == b.mut && a.index == b.index;
}

static int wasm_jit_stack_suffix_matches(const GLWASM_Func *function,
    uint32_t type_at, uint32_t count, const GLWASM_JitValue *stack,
    size_t stack_count) {
  if (stack_count < count || type_at > function->nv ||
      count > function->nv - type_at)
    return 0;
  for (uint32_t i = 0; i < count; ++i) {
    GRJIT_Type type;
    if (!wasm_jit_type(function->vpool[type_at + i], &type) ||
        stack[stack_count - count + i].type != type)
      return 0;
  }
  return 1;
}

static int wasm_jit_block_results_supported(const GLWASM_Func *function,
    const GLWASM_BlockType *block) {
  if (block->result_at > function->nv ||
      block->nresults > function->nv - block->result_at)
    return 0;
  for (uint32_t i = 0; i < block->nresults; ++i)
    if (!wasm_jit_type(function->vpool[block->result_at + i],
            &(GRJIT_Type){0})) return 0;
  return 1;
}

typedef struct GLWASM_JitEntryShape {
  GRJIT_Type *types;
  size_t count;
  uint8_t reachable;
} GLWASM_JitEntryShape;

/* Every compiled edge to a Wasm PC must agree on its operand layout. The
 * spill slots carry values between IR blocks; this shape tells each block
 * which slots to reload, including after a branch discards operands. */
static GLWASM_Result mark_reachable_pc(uint32_t pc, uint32_t nins,
    GLWASM_JitEntryShape *shapes, const GLWASM_JitValue *stack,
    size_t count, const GCU_Allocator *alloc) {
  if (pc >= nins) return GLWASM_OK;
  GLWASM_JitEntryShape *shape = &shapes[pc];
  if (!shape->reachable) {
    if (count) {
      shape->types = gcu_allocator_calloc(alloc, count,
          sizeof(*shape->types));
      if (!shape->types) return GLWASM_ERR_OOM;
      for (size_t i = 0; i < count; ++i)
        shape->types[i] = stack[i].type;
    }
    shape->count = count;
    shape->reachable = 1;
    return GLWASM_OK;
  }
  if (shape->count != count) return GLWASM_ERR_UNSUPPORTED;
  for (size_t i = 0; i < count; ++i)
    if (shape->types[i] != stack[i].type)
      return GLWASM_ERR_UNSUPPORTED;
  return GLWASM_OK;
}

/* A branch keeps the label's arity at its declared base and discards every
 * intervening operand. The values are still at the top of the source stack;
 * describe the destination without changing the source shape needed by a
 * br_if fallthrough or by another br_table arm. */
static GLWASM_Result mark_branch_pc(uint32_t pc, uint32_t nins,
    GLWASM_JitEntryShape *shapes, const GLWASM_JitValue *stack,
    size_t source_count, size_t base, size_t arity,
    const GCU_Allocator *alloc) {
  if (pc >= nins) return GLWASM_OK;
  if (source_count < arity || base > source_count - arity)
    return GLWASM_ERR_INTERNAL;
  const size_t count = base + arity;
  GLWASM_JitEntryShape *shape = &shapes[pc];
  int already_reachable = shape->reachable;
  if (!already_reachable) {
    if (count) {
      shape->types = gcu_allocator_calloc(alloc, count,
          sizeof(*shape->types));
      if (!shape->types) return GLWASM_ERR_OOM;
    }
    shape->count = count;
    shape->reachable = 1;
  } else if (shape->count != count) {
    return GLWASM_ERR_UNSUPPORTED;
  }
  for (size_t i = 0; i < count; ++i) {
    GRJIT_Type type = i < base ? stack[i].type :
        stack[source_count - arity + i - base].type;
    if (already_reachable && shape->types[i] != type)
      return GLWASM_ERR_UNSUPPORTED;
    shape->types[i] = type;
  }
  return GLWASM_OK;
}

static GRJIT_Result spill_branch_values(GRJIT_Builder *builder,
    GRJIT_VReg spill_base, size_t local_count,
    const GLWASM_JitValue *stack, size_t source_count,
    size_t base, size_t arity) {
  for (size_t i = 0; i < arity; ++i) {
    const GLWASM_JitValue value = stack[source_count - arity + i];
    GRJIT_Result jr = grjit_builder_store(builder, spill_base,
        (int32_t)((local_count + base + i) * 8u),
        value.type == GRJIT_TYPE_I32 || value.type == GRJIT_TYPE_F32
            ? 32u : 64u, grjit_operand_vreg(value.reg));
    if (jr != GRJIT_OK) return jr;
  }
  return GRJIT_OK;
}

static GRJIT_Result return_branch_values(GRJIT_Builder *builder,
    const GLWASM_JitValue *stack, size_t source_count, size_t arity) {
  GRJIT_Operand values[arity ? arity : 1u];
  for (size_t i = 0; i < arity; ++i)
    values[i] = grjit_operand_vreg(stack[source_count - arity + i].reg);
  return grjit_builder_ret_multi(builder, values, arity);
}

typedef struct GLWASM_JitLabel {
  size_t base;
  size_t arity;
  uint32_t target_pc;
  uint8_t function_label;
} GLWASM_JitLabel;

static GLWASM_Result wasm_jit_label(GLWASM_Func *function,
    const GLWASM_TypeDef *signature, const uint32_t *control_openers,
    const uint32_t *control_bases, size_t control_depth, uint32_t depth,
    uint32_t target_pc, const GLWASM_JitValue *stack, size_t stack_count,
    GLWASM_JitLabel *out) {
  if (depth > control_depth) return GLWASM_ERR_INTERNAL;
  if (depth == control_depth) {
    if (target_pc != function->nins || stack_count < signature->nresults)
      return GLWASM_ERR_INTERNAL;
    for (uint32_t i = 0; i < signature->nresults; ++i) {
      GRJIT_Type type;
      if (!wasm_jit_type(signature->results[i], &type) ||
          stack[stack_count - signature->nresults + i].type != type)
        return GLWASM_ERR_UNSUPPORTED;
    }
    *out = (GLWASM_JitLabel){0, signature->nresults, target_pc, 1};
    return GLWASM_OK;
  }
  const size_t label = control_depth - 1u - depth;
  const uint32_t opener = control_openers[label];
  if (opener >= function->nins || target_pc >= function->nins ||
      function->ins[opener].a >= function->nblocks)
    return GLWASM_ERR_INTERNAL;
  const GLWASM_BlockType *block = &function->blocks[function->ins[opener].a];
  const int loop = function->ins[opener].op == 0x03;
  const uint32_t arity = loop ? block->nparams : block->nresults;
  const uint32_t type_at = loop ? block->param_at : block->result_at;
  const size_t base = control_bases[label];
  if (!wasm_jit_stack_suffix_matches(function, type_at, arity,
          stack, stack_count) || stack_count < arity ||
      base > stack_count - arity)
    return GLWASM_ERR_UNSUPPORTED;
  *out = (GLWASM_JitLabel){base, arity, target_pc, 0};
  return GLWASM_OK;
}

static GLWASM_Result wasm_jit_catch_label(GLWASM_Func *function,
    const GLWASM_TypeDef *signature, const GLWASM_Catch *clause,
    const uint32_t *control_openers, const uint32_t *control_bases,
    size_t control_depth, size_t stack_count, GLWASM_JitLabel *label,
    const GLWASM_ValType **types) {
  if (!clause->resolved) return GLWASM_ERR_INTERNAL;
  if (clause->target_opener == UINT32_MAX) {
    if (clause->target_pc != function->nins)
      return GLWASM_ERR_INTERNAL;
    *label = (GLWASM_JitLabel){0, signature->nresults,
        clause->target_pc, 1};
    *types = signature->results;
    return GLWASM_OK;
  }
  for (size_t i = control_depth; i > 0; --i) {
    const size_t index = i - 1u;
    if (control_openers[index] != clause->target_opener) continue;
    const uint32_t opener = control_openers[index];
    if (opener >= function->nins ||
        function->ins[opener].a >= function->nblocks ||
        clause->target_pc >= function->nins)
      return GLWASM_ERR_INTERNAL;
    const GLWASM_BlockType *block =
        &function->blocks[function->ins[opener].a];
    const int loop = function->ins[opener].op == 0x03;
    const size_t base = control_bases[index];
    const size_t arity = loop ? block->nparams : block->nresults;
    const uint32_t at = loop ? block->param_at : block->result_at;
    if (base > stack_count || at > function->nv ||
        arity > function->nv - at)
      return GLWASM_ERR_INTERNAL;
    *label = (GLWASM_JitLabel){base, arity, clause->target_pc, 0};
    *types = arity ? &function->vpool[at] : NULL;
    return GLWASM_OK;
  }
  return GLWASM_ERR_INTERNAL;
}

static GLWASM_Result mark_catch_pc(const GLWASM_JitLabel *label,
    const GLWASM_ValType *types, const GLWASM_JitValue *stack,
    size_t stack_count, uint32_t nins, GLWASM_JitEntryShape *shapes,
    const GCU_Allocator *alloc) {
  if (label->base > stack_count) return GLWASM_ERR_INTERNAL;
  if (label->target_pc >= nins) return GLWASM_OK;
  const size_t count = label->base + label->arity;
  GLWASM_JitEntryShape *shape = &shapes[label->target_pc];
  const int already_reachable = shape->reachable;
  if (!already_reachable) {
    if (count) {
      shape->types = gcu_allocator_calloc(alloc, count,
          sizeof(*shape->types));
      if (!shape->types) return GLWASM_ERR_OOM;
    }
    shape->count = count;
    shape->reachable = 1;
  } else if (shape->count != count) {
    return GLWASM_ERR_UNSUPPORTED;
  }
  for (size_t i = 0; i < count; ++i) {
    GRJIT_Type type;
    if (i < label->base) type = stack[i].type;
    else if (!wasm_jit_type(types[i - label->base], &type))
      return GLWASM_ERR_UNSUPPORTED;
    if (already_reachable && shape->types[i] != type)
      return GLWASM_ERR_UNSUPPORTED;
    shape->types[i] = type;
  }
  return GLWASM_OK;
}

static GLWASM_Result emit_exception_dispatch(GRJIT_Builder *builder,
    GLWASM_Module *module, GLWASM_Func *function,
    const GLWASM_TypeDef *signature, uint32_t function_index,
    const uint32_t *control_openers, const uint32_t *control_bases,
    size_t control_depth, const GLWASM_JitValue *stack, size_t stack_count,
    size_t local_count, GRJIT_VReg instance, GRJIT_VReg spill_base,
    GRJIT_BlockId *pc_blocks, GLWASM_JitEntryShape *shapes,
    GRCORE_PollIdentity identity, uintptr_t exception_take_helper,
    GRJIT_Result *jit_failure) {
  *jit_failure = GRJIT_OK;
  for (size_t depth = control_depth; depth > 0; --depth) {
    const uint32_t opener = control_openers[depth - 1u];
    if (opener >= function->nins) return GLWASM_ERR_INTERNAL;
    if (function->ins[opener].op != 0x1f) continue;
    const GLWASM_Instr instruction = function->ins[opener];
    if (instruction.b > function->ncatches ||
        instruction.d > function->ncatches - instruction.b)
      return GLWASM_ERR_INTERNAL;
    for (uint32_t i = 0; i < instruction.d; ++i) {
      const uint32_t clause_index = instruction.b + i;
      const GLWASM_Catch *clause = &function->catches[clause_index];
      GLWASM_JitLabel label;
      const GLWASM_ValType *types;
      GLWASM_Result result = wasm_jit_catch_label(function, signature,
          clause, control_openers, control_bases, control_depth,
          stack_count, &label, &types);
      if (result != GLWASM_OK) return result;
      result = mark_catch_pc(&label, types, stack, stack_count,
          function->nins, shapes, &module->alloc);
      if (result != GLWASM_OK) return result;
      GRJIT_BlockId matched, next;
      GRJIT_Result jr = grjit_builder_block(builder, &matched);
      if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &next);
      GRJIT_VReg accepted = GRJIT_NO_VREG;
      if (jr == GRJIT_OK)
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &accepted);
      GRJIT_Operand args[6] = {grjit_operand_vreg(instance),
          grjit_operand_imm(function_index),
          grjit_operand_imm(clause_index), grjit_operand_vreg(spill_base),
          grjit_operand_imm(label.base), grjit_operand_imm(label.arity)};
      if (jr == GRJIT_OK)
        jr = grjit_builder_call(builder, accepted,
            (uint64_t)exception_take_helper, GRJIT_CALL_NO_GC,
            GRCORE_SITE_GC_POINT_CALL, args, 6, identity, NULL, 0);
      if (jr == GRJIT_OK)
        jr = grjit_builder_br_if(builder, grjit_operand_vreg(accepted),
            matched, next);
      if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, matched);
      if (jr == GRJIT_OK && label.function_label) {
        GRJIT_Operand returned[label.arity ? label.arity : 1u];
        for (size_t ri = 0; jr == GRJIT_OK && ri < label.arity; ++ri) {
          GRJIT_Type type;
          GRJIT_VReg value;
          if (!wasm_jit_type(types[ri], &type))
            return GLWASM_ERR_UNSUPPORTED;
          jr = grjit_builder_vreg(builder, type, &value);
          if (jr == GRJIT_OK)
            jr = grjit_builder_load(builder, value, spill_base,
                (int32_t)((local_count + ri) * 8u),
                type == GRJIT_TYPE_I32 || type == GRJIT_TYPE_F32 ? 32u : 64u,
                false);
          returned[ri] = grjit_operand_vreg(value);
        }
        if (jr == GRJIT_OK)
          jr = grjit_builder_ret_multi(builder, returned, label.arity);
      } else if (jr == GRJIT_OK) {
        jr = grjit_builder_br(builder, pc_blocks[label.target_pc]);
      }
      if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, next);
      if (jr != GRJIT_OK) {
        *jit_failure = jr;
        return map_jit_result(jr);
      }
    }
  }
  *jit_failure = grjit_builder_rethrow(builder);
  return map_jit_result(*jit_failure);
}

static GRJIT_FrameSlot frame_constant(uint64_t value) {
  return grjit_frame_slot_constant((int64_t)value);
}

static GLWASM_Result make_state(GLWASM_Func *function, uint32_t function_index,
    uint32_t pc, const GLWASM_JitValue *locals,
    const GLWASM_JitValue *stack, size_t stack_count,
    const uint32_t *control_openers, const uint32_t *control_bases,
    size_t control_depth, GRJIT_VReg instance,
    GRJIT_FrameSlot *state, size_t slot_count) {
  if (slot_count != glwasm_frame_slots(function) ||
      stack_count > glwasm_operand_capacity(function))
    return GLWASM_ERR_INTERNAL;
  for (size_t i = 0; i < slot_count; ++i)
    state[i] = grjit_frame_slot_dead();
  state[H_FUNC] = frame_constant(function_index);
  state[H_PC] = frame_constant(pc);
  state[H_SP] = frame_constant(H_OPERANDS + function->nlocals + stack_count);
  state[H_NLOCALS] = frame_constant(function->nlocals);
  if (control_depth > function->nins) return GLWASM_ERR_INTERNAL;
  state[H_CTRL_DEPTH] = frame_constant(control_depth);
  /* Fuel is charged before this poll. Resumption must execute this operation
   * without charging it again. */
  state[H_SKIP_CHARGE] = frame_constant(1);
  state[H_INSTANCE] = grjit_frame_slot_vreg(instance);
  size_t local_count = function->nlocals;
  for (size_t i = 0; i < local_count; ++i)
    state[H_OPERANDS + i] = grjit_frame_slot_vreg(locals[i].reg);
  size_t operand_base = H_OPERANDS + local_count;
  for (size_t i = 0; i < stack_count; ++i)
    state[operand_base + i] = grjit_frame_slot_vreg(stack[i].reg);
  size_t cb = glwasm_control_base(function);
  for (size_t i = 0; i < control_depth; ++i) {
    state[cb + 2u * i] = frame_constant(control_openers[i]);
    state[cb + 2u * i + 1u] = frame_constant(
        H_OPERANDS + local_count + control_bases[i]);
  }
  return GLWASM_OK;
}

static GLWASM_Result emit_named_exit(GRJIT_Builder *builder,
    uint32_t function_index, uint16_t opcode, uintptr_t helper,
    GRJIT_VReg instance, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot *state, size_t slot_count,
    const GLWASM_TypeDef *signature) {
  GRJIT_VReg signal;
  GRJIT_Result jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &signal);
  if (jr != GRJIT_OK) return map_jit_result(jr);
  GRJIT_Operand args[3] = {grjit_operand_vreg(instance),
      grjit_operand_imm(function_index), grjit_operand_imm(opcode)};
  jr = grjit_builder_call(builder, signal, (uint64_t)helper,
      GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 3, identity,
      NULL, 0);
  if (jr == GRJIT_OK)
    jr = grjit_builder_guard(builder, grjit_operand_vreg(signal), identity,
        state, slot_count);
  GRJIT_Operand return_values[signature->nresults ? signature->nresults : 1u];
  for (uint32_t i = 0; jr == GRJIT_OK && i < signature->nresults; ++i) {
    GRJIT_Type type;
    GRJIT_VReg zero_result;
    if (!wasm_jit_type(signature->results[i], &type))
      return GLWASM_ERR_UNSUPPORTED;
    jr = grjit_builder_vreg(builder, type, &zero_result);
    if (jr == GRJIT_OK) jr = grjit_builder_const(builder, zero_result, 0);
    return_values[i] = grjit_operand_vreg(zero_result);
  }
  if (jr == GRJIT_OK)
    jr = grjit_builder_ret_multi(builder, return_values, signature->nresults);
  return map_jit_result(jr);
}

/* Arithmetic traps are reconstructed by the interpreter to preserve its
 * precise trap identity. Record that handoff separately from unsupported IR. */
static GLWASM_Result emit_trap_exit(GRJIT_Builder *builder,
    uint32_t function_index, uint16_t opcode, uintptr_t helper,
    GRJIT_VReg instance, GRCORE_PollIdentity identity,
    const GRJIT_FrameSlot *state, size_t slot_count,
    const GLWASM_TypeDef *signature) {
  return emit_named_exit(builder, function_index, opcode, helper, instance,
      identity, state, slot_count, signature);
}

static GLWASM_Result emit_guard_return(GRJIT_Builder *builder,
    GRCORE_PollIdentity identity, const GRJIT_FrameSlot *state,
    size_t slot_count, const GLWASM_TypeDef *signature) {
  GRJIT_VReg zero_condition;
  GRJIT_Result jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
      &zero_condition);
  if (jr == GRJIT_OK) jr = grjit_builder_const(builder, zero_condition, 0);
  if (jr == GRJIT_OK)
    jr = grjit_builder_guard(builder, grjit_operand_vreg(zero_condition),
        identity, state, slot_count);
  GRJIT_Operand return_values[signature->nresults ? signature->nresults : 1u];
  for (uint32_t i = 0; jr == GRJIT_OK && i < signature->nresults; ++i) {
    GRJIT_Type result_type;
    GRJIT_VReg zero_result;
    if (!wasm_jit_type(signature->results[i], &result_type))
      return GLWASM_ERR_UNSUPPORTED;
    jr = grjit_builder_vreg(builder, result_type, &zero_result);
    if (jr == GRJIT_OK) jr = grjit_builder_const(builder, zero_result, 0);
    return_values[i] = grjit_operand_vreg(zero_result);
  }
  if (jr == GRJIT_OK)
    jr = grjit_builder_ret_multi(builder, return_values, signature->nresults);
  return map_jit_result(jr);
}

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
    uintptr_t exception_throw_ref_helper, uintptr_t funcref_helper,
    GRJIT_Code **out_code,
    uint16_t *unsupported_opcode, uint32_t *failure_pc,
    uint32_t *failure_jit_result, uint16_t *failure_opcode,
    uint32_t *failure_stack_count, uint32_t *failure_site) {
  if (!module || function_index >= module->nfuncs || !pages || !hooks ||
      !hooks->deopt || !poll_helper || !charge_helper || !named_exit_helper ||
      !trap_exit_helper ||
      !spill_base_helper || !memory_helper || !global_get_helper ||
      !global_set_helper || !helper_status_helper ||
      !slot_entry_helper || !indirect_target_helper || !host_call_helper ||
      !table_bulk_helper || !memory_bulk_helper ||
      !exception_take_helper || !exception_throw_helper ||
      !exception_throw_ref_helper || !funcref_helper ||
      !out_code)
    return GLWASM_ERR_INVALID;
  GLWASM_JitCacheKey cache_key;
  if (GHOTIIO_LANG_WASM(glwasm_jit_cache_key)(module->jit_cache,
      &cache_key) != GLWASM_OK)
    return GLWASM_ERR_INVALID;
  *out_code = NULL;
  if (unsupported_opcode) *unsupported_opcode = UINT16_MAX;
  if (failure_pc) *failure_pc = UINT32_MAX;
  if (failure_jit_result) *failure_jit_result = UINT32_MAX;
  if (failure_opcode) *failure_opcode = UINT16_MAX;
  if (failure_stack_count) *failure_stack_count = UINT32_MAX;
  if (failure_site) *failure_site = UINT32_MAX;
  GLWASM_Func *function = &module->funcs[function_index];
  if (function->imported || function->type_index >= module->ntypes ||
      !function->translated || function->nins == 0)
    return GLWASM_ERR_UNSUPPORTED;
  GLWASM_TypeDef *signature = &module->types[function->type_index];
  if (signature->nparams != function->nparams)
    return GLWASM_ERR_UNSUPPORTED;
  /* The current callable IR ABI carries at most 16 parameters/results. Keep
   * valid larger Wasm signatures on the interpreter path with an explicit
   * signature refusal instead of discovering the backend cap mid-build. */
  if (signature->nparams > WASM_JIT_MAX_CALL_VALUES ||
      signature->nresults > WASM_JIT_MAX_CALL_VALUES)
    return GLWASM_ERR_UNSUPPORTED;
  for (uint32_t ri = 0; ri < signature->nresults; ++ri)
    if (!wasm_jit_type(signature->results[ri], &(GRJIT_Type){0}))
      return GLWASM_ERR_UNSUPPORTED;

  const size_t local_count = function->nlocals;
  const size_t max_stack = glwasm_operand_capacity(function);
  size_t max_module_results = 0;
  for (uint32_t ti = 0; ti < module->ntypes; ++ti)
    if (module->types[ti].nresults > max_module_results)
      max_module_results = module->types[ti].nresults;
  const size_t slot_count = glwasm_frame_slots(function);
  GLWASM_JitValue *locals = gcu_allocator_calloc(&module->alloc,
      local_count, sizeof(*locals));
  GLWASM_JitValue *stack = gcu_allocator_calloc(&module->alloc,
      max_stack, sizeof(*stack));
  GRJIT_Type *local_types = gcu_allocator_calloc(&module->alloc,
      local_count, sizeof(*local_types));
  GRJIT_FrameSlot *state = gcu_allocator_calloc(&module->alloc,
      slot_count, sizeof(*state));
  GRJIT_FrameSlot *state_after = gcu_allocator_calloc(&module->alloc,
      slot_count, sizeof(*state_after));
  GRJIT_BlockId *pc_blocks = gcu_allocator_calloc(&module->alloc,
      function->nins, sizeof(*pc_blocks));
  GLWASM_JitEntryShape *shapes = gcu_allocator_calloc(&module->alloc,
      function->nins, sizeof(*shapes));
  uint32_t *control_openers = gcu_allocator_calloc(&module->alloc,
      function->nins, sizeof(*control_openers));
  uint32_t *control_bases = gcu_allocator_calloc(&module->alloc,
      function->nins, sizeof(*control_bases));
  if (!locals || !stack || !local_types || !state || !state_after || !pc_blocks ||
      !shapes ||
      !control_openers || !control_bases) {
    gcu_allocator_free(&module->alloc, locals);
    gcu_allocator_free(&module->alloc, stack);
    gcu_allocator_free(&module->alloc, local_types);
    gcu_allocator_free(&module->alloc, state);
    gcu_allocator_free(&module->alloc, state_after);
    gcu_allocator_free(&module->alloc, pc_blocks);
    gcu_allocator_free(&module->alloc, shapes);
    gcu_allocator_free(&module->alloc, control_openers);
    gcu_allocator_free(&module->alloc, control_bases);
    return GLWASM_ERR_OOM;
  }

  GLWASM_Result result = GLWASM_ERR_UNSUPPORTED;
  GRJIT_Builder *builder = NULL;
  GRJIT_Function *ir = NULL;
  GRJIT_Code *code = NULL;
  GRJIT_VReg instance = GRJIT_NO_VREG;
  GRJIT_VReg spill_base = GRJIT_NO_VREG;
  size_t stack_count = 0;
  size_t control_depth = 0;
  shapes[0].reachable = 1;
  /* Frame states describe only validator-proven live operand/control slots.
   * A legitimately deep function can still exceed runtime-jit's default
   * 4096-slot budget; request its exact verified size at both IR stages. */
  GRJIT_Limits jit_limits = {0};
  jit_limits.max_frame_state_slots = slot_count;
  /* Baseline code has a block per translated PC, plus a bounded number of
   * helper blocks per opcode. Keep that budget proportional to the validated
   * body; runtime-jit's operation and code-byte caps remain in force. */
  if (!gcu_safe_mul_size((size_t)function->nins, 32u,
          &jit_limits.max_blocks) ||
      jit_limits.max_blocks > UINT32_MAX - 1u) {
    result = GLWASM_ERR_LIMIT;
    if (failure_site) *failure_site = __LINE__;
    goto done;
  }
  GRJIT_Result jr = grjit_builder_create("wasm_baseline", slot_count,
      &jit_limits, NULL, &builder);
  if (jr != GRJIT_OK) {
    result = map_jit_result_capture(jr, failure_jit_result);
    if (failure_site) *failure_site = __LINE__;
    goto done;
  }
  jr = grjit_builder_set_callable(builder, hooks, signature->nresults);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  uint64_t token = ((uint64_t)module->debug_id << 32) | function_index;
  jr = grjit_builder_set_token(builder, token);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  jr = grjit_builder_set_guest_function(builder, function_index);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }

  for (uint32_t i = 0; i < function->nparams; ++i) {
    if (!wasm_jit_type(signature->params[i], &local_types[i])) {
      result = GLWASM_ERR_UNSUPPORTED;
      if (failure_site) *failure_site = __LINE__;
      goto done;
    }
    jr = grjit_builder_param(builder, local_types[i], &locals[i].reg);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
    locals[i].type = local_types[i];
  }
  for (uint32_t i = function->nparams; i < function->nlocals; ++i) {
    size_t local_index = i;
    if (!wasm_jit_type(function->locals[i], &local_types[local_index])) {
      result = GLWASM_ERR_UNSUPPORTED;
      if (failure_site) *failure_site = __LINE__;
      goto done;
    }
    jr = grjit_builder_vreg(builder, local_types[local_index],
        &locals[local_index].reg);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
    locals[local_index].type = local_types[local_index];
  }
  GRJIT_BlockId entry;
  jr = grjit_builder_block(builder, &entry);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  pc_blocks[0] = entry;
  for (uint32_t pc = 1; pc < function->nins; ++pc) {
    jr = grjit_builder_block(builder, &pc_blocks[pc]);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  }
  jr = grjit_builder_set_block(builder, entry);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &instance);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  jr = grjit_builder_instance(builder, instance);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &spill_base);
  if (jr == GRJIT_OK) {
    GRJIT_Operand args[2] = {grjit_operand_vreg(instance),
        grjit_operand_imm(function_index)};
    jr = grjit_builder_call(builder, spill_base,
        (uint64_t)spill_base_helper, GRJIT_CALL_NO_GC,
        GRCORE_SITE_GC_POINT_CALL, args, 2,
        (GRCORE_PollIdentity){token, 0, 0}, NULL, 0);
  }
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  jr = grjit_builder_set_poll_helper(builder, poll_helper);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  for (uint32_t i = function->nparams; i < local_count; ++i) {
    jr = grjit_builder_const(builder, locals[i].reg, 0);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  }
  GRJIT_VReg valid_spill;
  jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &valid_spill);
  if (jr == GRJIT_OK)
    jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, valid_spill,
        grjit_operand_vreg(spill_base), grjit_operand_imm(0));
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  GLWASM_Result initial_state = make_state(function, function_index, 0,
      locals, stack, 0, control_openers, control_bases, 0, instance, state,
      slot_count);
  if (initial_state != GLWASM_OK) { result = initial_state; if (failure_site) *failure_site = __LINE__; goto done; }
  state[H_SKIP_CHARGE] = frame_constant(0);
  jr = grjit_builder_guard(builder, grjit_operand_vreg(valid_spill),
      (GRCORE_PollIdentity){token, function->translated->offsets[0], 0},
      state, slot_count);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  if (local_count > (size_t)INT32_MAX / 8u || max_stack >
      (size_t)INT32_MAX / 8u - local_count || max_module_results >
      (size_t)INT32_MAX / 8u - local_count - max_stack) {
    result = GLWASM_ERR_LIMIT; if (failure_site) *failure_site = __LINE__; goto done;
  }
  for (size_t i = 0; i < local_count; ++i) {
    uint32_t width = locals[i].type == GRJIT_TYPE_I32 ||
        locals[i].type == GRJIT_TYPE_F32 ? 32u : 64u;
    jr = grjit_builder_store(builder, spill_base, (int32_t)(i * 8u), width,
        grjit_operand_vreg(locals[i].reg));
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  }

  for (uint32_t pc = 0; pc < function->nins; ++pc) {
    if (failure_pc) *failure_pc = pc;
    if (failure_opcode)
      *failure_opcode = function->translated->ins[pc].source.op;
    if (failure_stack_count) *failure_stack_count = (uint32_t)stack_count;
    int side_exit = 0;
    int terminates = 0;
    GLWASM_JitLabel branch_label = {0};
    jr = grjit_builder_set_block(builder, pc_blocks[pc]);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
    GLWASM_Instr instruction = function->translated->ins[pc].source;
    if (!shapes[pc].reachable) {
      /* No compiled predecessor reaches this instruction. Keep the lexical
       * control stack aligned for later branch targets, but do not interpret
       * dead operand effects after a return or named exit. */
      if (instruction.op == 0x02 || instruction.op == 0x03 ||
          instruction.op == 0x04 || instruction.op == 0x1f) {
        if (control_depth >= function->nins) {
          result = GLWASM_ERR_INTERNAL;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        control_openers[control_depth] = pc;
        control_bases[control_depth++] = (uint32_t)stack_count;
      } else if (instruction.op == 0x0b &&
          pc + 1u < function->nins && control_depth) {
        --control_depth;
      }
      jr = grjit_builder_trap(builder, GRJIT_TRAP_UNREACHABLE);
      if (jr != GRJIT_OK) {
        result = map_jit_result_capture(jr, failure_jit_result);
        if (failure_site) *failure_site = __LINE__;
        goto done;
      }
      continue;
    }
    /* The previous source instruction is not necessarily a predecessor:
     * returns, exits and structured branches can join here from elsewhere. */
    stack_count = shapes[pc].count;
    if (failure_stack_count) *failure_stack_count = (uint32_t)stack_count;
    for (size_t i = 0; i < stack_count; ++i)
      stack[i].type = shapes[pc].types[i];
    /* Each Wasm local has one mutable native-frame vreg. Its value flows
     * through branches and backedges in that fixed slot; local.set updates
     * both the vreg and interpreter spill. Reloading every local at every PC
     * made code size and vreg count grow with nins * nlocals. */
    for (size_t i = 0; i < stack_count; ++i) {
      GRJIT_VReg loaded;
      jr = grjit_builder_vreg(builder, stack[i].type, &loaded);
      if (jr == GRJIT_OK)
        jr = grjit_builder_load(builder, loaded, spill_base,
            (int32_t)((local_count + i) * 8u),
            stack[i].type == GRJIT_TYPE_I32 ||
            stack[i].type == GRJIT_TYPE_F32 ? 32u : 64u, false);
      if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
      stack[i].reg = loaded;
    }
    GRCORE_PollIdentity identity = {
      ((uint64_t)module->debug_id << 32) | function_index,
      function->translated->offsets[pc], 0};
    GLWASM_Result state_result = make_state(function, function_index, pc,
        locals, stack, stack_count, control_openers, control_bases,
        control_depth, instance, state, slot_count);
    if (state_result != GLWASM_OK) { result = state_result; if (failure_site) *failure_site = __LINE__; goto done; }
    GRJIT_Operand charge_args[2] = {
      grjit_operand_vreg(instance),
      grjit_operand_imm(glwasm_opcode_cost(instruction.op))};
    jr = grjit_builder_call(builder, GRJIT_NO_VREG,
        (uint64_t)charge_helper, GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL,
        charge_args, 2, identity, NULL, 0);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
    jr = grjit_builder_poll(builder, identity, state, slot_count);
    if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }

    GRJIT_VReg value = GRJIT_NO_VREG;
    GRJIT_Type type = GRJIT_TYPE_COUNT;
    switch (instruction.op) {
      case 0x23: { /* global.get */
        if (instruction.a >= module->nglobals) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        if (!wasm_jit_type(module->globals[instruction.a].type, &type)) {
          result = GLWASM_ERR_UNSUPPORTED;
          if (unsupported_opcode) *unsupported_opcode = instruction.op;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        GRJIT_Operand args[2] = {grjit_operand_vreg(instance),
            grjit_operand_imm(instruction.a)};
        GRJIT_VReg loaded, succeeded;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &loaded);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, loaded, (uint64_t)global_get_helper,
              GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 2, identity,
              NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &succeeded);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, succeeded,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId access_block, failure_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &access_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &failure_block);
        if (jr == GRJIT_OK) jr = grjit_builder_br_if(builder,
            grjit_operand_vreg(succeeded), access_block, failure_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, failure_block);
        if (jr == GRJIT_OK) result = emit_named_exit(builder, function_index,
            instruction.op, named_exit_helper, instance, identity, state,
            slot_count, signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, access_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        if (type == GRJIT_TYPE_I32 || type == GRJIT_TYPE_F32) {
          GRJIT_VReg bits = loaded;
          if (type == GRJIT_TYPE_I32) {
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &value);
            if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
                GRJIT_OP_WRAP, value, grjit_operand_vreg(loaded));
          } else {
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &bits);
            if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
                GRJIT_OP_WRAP, bits, grjit_operand_vreg(loaded));
            if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder,
                GRJIT_TYPE_F32, &value);
            if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
                GRJIT_OP_REINTERPRET, value, grjit_operand_vreg(bits));
          }
        } else if (type == GRJIT_TYPE_F64) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_F64, &value);
          if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
              GRJIT_OP_REINTERPRET, value, grjit_operand_vreg(loaded));
        } else value = loaded;
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count++] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x24: { /* global.set */
        if (instruction.a >= module->nglobals || !stack_count) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        if (!module->globals[instruction.a].mut ||
            !wasm_jit_type(module->globals[instruction.a].type, &type)) {
          result = GLWASM_ERR_UNSUPPORTED;
          if (unsupported_opcode) *unsupported_opcode = instruction.op;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        GLWASM_JitValue stored = stack[stack_count - 1u];
        if (stored.type != type) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_VReg raw = stored.reg;
        jr = GRJIT_OK;
        if (type == GRJIT_TYPE_I32 || type == GRJIT_TYPE_F32) {
          GRJIT_VReg bits = stored.reg;
          if (type == GRJIT_TYPE_F32) {
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &bits);
            if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
                GRJIT_OP_REINTERPRET, bits, grjit_operand_vreg(stored.reg));
          }
          if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder,
              GRJIT_TYPE_I64, &raw);
          if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
              GRJIT_OP_EXTEND_U, raw, grjit_operand_vreg(bits));
        } else if (type == GRJIT_TYPE_F64) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &raw);
          if (jr == GRJIT_OK) jr = grjit_builder_unary(builder,
              GRJIT_OP_REINTERPRET, raw, grjit_operand_vreg(stored.reg));
        } else jr = GRJIT_OK;
        GRJIT_Operand args[3] = {grjit_operand_vreg(instance),
            grjit_operand_imm(instruction.a), grjit_operand_vreg(raw)};
        if (jr == GRJIT_OK) jr = grjit_builder_call(builder, GRJIT_NO_VREG,
            (uint64_t)global_set_helper, GRJIT_CALL_NO_GC,
            GRCORE_SITE_GC_POINT_CALL, args, 3, identity, NULL, 0);
        GRJIT_VReg succeeded;
        if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder,
            GRJIT_TYPE_I32, &succeeded);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, succeeded,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId write_block, failure_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &write_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &failure_block);
        if (jr == GRJIT_OK) jr = grjit_builder_br_if(builder,
            grjit_operand_vreg(succeeded), write_block, failure_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, failure_block);
        if (jr == GRJIT_OK) result = emit_named_exit(builder, function_index,
            instruction.op, named_exit_helper, instance, identity, state,
            slot_count, signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, write_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        --stack_count;
        break;
      }
      case 0x01: /* nop */
        break;
      case 0x02: case 0x03: case 0x1f: { /* block / loop / try_table */
        if (instruction.a >= function->nblocks ||
            control_depth >= function->nins) {
          side_exit = 1;
          break;
        }
        const GLWASM_BlockType *block = &function->blocks[instruction.a];
        if (!wasm_jit_stack_suffix_matches(function, block->param_at,
                block->nparams, stack, stack_count) ||
            !wasm_jit_block_results_supported(function, block)) {
          side_exit = 1;
          break;
        }
        control_openers[control_depth] = pc;
        control_bases[control_depth++] =
            (uint32_t)(stack_count - block->nparams);
        break;
      }
      case 0x04: { /* if; both arms enter with the same block parameters */
        if (instruction.a >= function->nblocks ||
            !stack_count || stack[stack_count - 1u].type != GRJIT_TYPE_I32 ||
            control_depth >= function->nins) {
          side_exit = 1;
          break;
        }
        const GLWASM_BlockType *block = &function->blocks[instruction.a];
        if (!wasm_jit_stack_suffix_matches(function, block->param_at,
                block->nparams, stack, stack_count - 1u) ||
            !wasm_jit_block_results_supported(function, block)) {
          side_exit = 1;
          break;
        }
        GRJIT_VReg condition = stack[--stack_count].reg;
        control_openers[control_depth] = pc;
        control_bases[control_depth++] =
            (uint32_t)(stack_count - block->nparams);
        uint32_t false_pc = function->translated->ins[pc].branch_target;
        if (false_pc >= function->nins || pc + 1u >= function->nins) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        jr = grjit_builder_br_if(builder, grjit_operand_vreg(condition),
            pc_blocks[pc + 1u], pc_blocks[false_pc]);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        terminates = 1;
        break;
      }
      case 0x05: { /* else */
        uint32_t target = function->translated->ins[pc].alternate_target;
        if (!control_depth || target >= function->nins) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        uint32_t opener = control_openers[control_depth - 1u];
        if (function->ins[opener].a >= function->nblocks) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        const GLWASM_BlockType *block = &function->blocks[function->ins[opener].a];
        if (stack_count != control_bases[control_depth - 1u] +
                block->nresults ||
            !wasm_jit_stack_suffix_matches(function, block->result_at,
                block->nresults, stack, stack_count)) {
          side_exit = 1;
          break;
        }
        jr = grjit_builder_br(builder, pc_blocks[target]);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        terminates = 1;
        break;
      }
      case 0x0c: case 0x0d: { /* branch / conditional branch */
        int conditional = instruction.op == 0x0d;
        if ((conditional && (!stack_count ||
                stack[stack_count - 1u].type != GRJIT_TYPE_I32)) ||
            instruction.a > control_depth) {
          side_exit = 1;
          break;
        }
        GLWASM_JitValue condition = {GRJIT_NO_VREG, GRJIT_TYPE_I32};
        if (conditional) condition = stack[--stack_count];
        uint32_t target_pc = function->translated->ins[pc].branch_target;
        result = wasm_jit_label(function, signature, control_openers,
            control_bases, control_depth, instruction.a, target_pc,
            stack, stack_count, &branch_label);
        if (result == GLWASM_ERR_UNSUPPORTED) { side_exit = 1; break; }
        if (result != GLWASM_OK) {
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        if (conditional) {
          /* Only the taken edge may compact label values. The fallthrough
           * still needs the unmodified source stack after popping i32. */
          if (pc + 1u >= function->nins) {
            result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
          }
          GRJIT_BlockId taken;
          jr = grjit_builder_block(builder, &taken);
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
          jr = grjit_builder_br_if(builder,
              grjit_operand_vreg(condition.reg), taken,
              pc_blocks[pc + 1u]);
          if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, taken);
        } else {
          jr = GRJIT_OK;
        }
        if (jr == GRJIT_OK) {
          if (branch_label.function_label)
            jr = return_branch_values(builder, stack, stack_count,
                branch_label.arity);
          else {
            jr = spill_branch_values(builder, spill_base, local_count,
                stack, stack_count, branch_label.base, branch_label.arity);
            if (jr == GRJIT_OK)
              jr = grjit_builder_br(builder,
                  pc_blocks[branch_label.target_pc]);
          }
        }
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        terminates = 1;
        break;
      }
      case 0x0e: { /* br_table; every arm has the same label value types */
        if (!stack_count || stack[stack_count - 1u].type != GRJIT_TYPE_I32 ||
            instruction.b == 0 || instruction.a > function->nu32 ||
            instruction.b > function->nu32 - instruction.a ||
            function->translated->ins[pc].alternate_target >
                function->translated->branch_table_count ||
            instruction.b > function->translated->branch_table_count -
                function->translated->ins[pc].alternate_target) {
          side_exit = 1;
          break;
        }
        uint32_t selector = stack[--stack_count].reg;
        GRJIT_BlockId *targets = gcu_allocator_calloc(&module->alloc,
            instruction.b, sizeof(*targets));
        GLWASM_JitLabel *labels = gcu_allocator_calloc(&module->alloc,
            instruction.b, sizeof(*labels));
        if (!targets || !labels) {
          gcu_allocator_free(&module->alloc, targets);
          gcu_allocator_free(&module->alloc, labels);
          result = GLWASM_ERR_OOM; if (failure_site) *failure_site = __LINE__; goto done;
        }
        result = GLWASM_OK;
        for (uint32_t j = 0; j < instruction.b; ++j) {
          uint32_t depth = function->u32s[instruction.a + j];
          uint32_t target_pc = function->translated->branch_table_targets[
              function->translated->ins[pc].alternate_target + j];
          result = wasm_jit_label(function, signature, control_openers,
              control_bases, control_depth, depth, target_pc, stack,
              stack_count, &labels[j]);
          if (result != GLWASM_OK) break;
        }
        if (result != GLWASM_OK) {
          gcu_allocator_free(&module->alloc, targets);
          gcu_allocator_free(&module->alloc, labels);
          if (result == GLWASM_ERR_UNSUPPORTED) { side_exit = 1; break; }
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        jr = GRJIT_OK;
        for (uint32_t j = 0; j < instruction.b; ++j) {
          jr = grjit_builder_block(builder, &targets[j]);
          if (jr != GRJIT_OK) break;
        }
        if (jr != GRJIT_OK) {
          gcu_allocator_free(&module->alloc, targets);
          gcu_allocator_free(&module->alloc, labels);
          result = map_jit_result_capture(jr, failure_jit_result);
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        jr = grjit_builder_br_table(builder,
            grjit_operand_vreg(selector), targets, instruction.b - 1u,
            targets[instruction.b - 1u]);
        for (uint32_t j = 0; jr == GRJIT_OK && j < instruction.b; ++j) {
          jr = grjit_builder_set_block(builder, targets[j]);
          if (jr != GRJIT_OK) break;
          if (labels[j].function_label)
            jr = return_branch_values(builder, stack, stack_count,
                labels[j].arity);
          else {
            jr = spill_branch_values(builder, spill_base, local_count,
                stack, stack_count, labels[j].base, labels[j].arity);
            if (jr == GRJIT_OK)
              jr = grjit_builder_br(builder, pc_blocks[labels[j].target_pc]);
          }
        }
        if (jr == GRJIT_OK) {
          for (uint32_t j = 0; j < instruction.b; ++j) {
            result = mark_branch_pc(labels[j].target_pc, function->nins,
                shapes, stack, stack_count, labels[j].base,
                labels[j].arity, &module->alloc);
            if (result != GLWASM_OK) break;
          }
        }
        gcu_allocator_free(&module->alloc, targets);
        gcu_allocator_free(&module->alloc, labels);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        if (result != GLWASM_OK) {
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        terminates = 1;
        break;
      }
      case 0x08: case 0x0a: { /* throw / throw_ref */
        GRJIT_VReg raised;
        GRJIT_Operand args[5];
        size_t argument_count;
        uintptr_t helper;
        if (instruction.op == 0x08) {
          if (instruction.a >= module->ntags ||
              module->tags[instruction.a].type_index >= module->ntypes) {
            side_exit = 1;
            break;
          }
          const GLWASM_TypeDef *tag_type = &module->types[
              module->tags[instruction.a].type_index];
          if (stack_count < tag_type->nparams) {
            side_exit = 1;
            break;
          }
          for (uint32_t i = 0; i < tag_type->nparams; ++i) {
            GRJIT_Type expected;
            if (!wasm_jit_type(tag_type->params[i], &expected) ||
                stack[stack_count - tag_type->nparams + i].type != expected)
              side_exit = 1;
          }
          if (side_exit) break;
          args[0] = grjit_operand_vreg(instance);
          args[1] = grjit_operand_imm(function_index);
          args[2] = grjit_operand_imm(instruction.a);
          args[3] = grjit_operand_vreg(spill_base);
          args[4] = grjit_operand_imm(
              local_count + stack_count - tag_type->nparams);
          argument_count = 5;
          helper = exception_throw_helper;
          stack_count -= tag_type->nparams;
        } else {
          if (!stack_count || stack[stack_count - 1u].type != GRJIT_TYPE_I64) {
            side_exit = 1;
            break;
          }
          args[0] = grjit_operand_vreg(instance);
          args[1] = grjit_operand_vreg(stack[stack_count - 1u].reg);
          argument_count = 2;
          helper = exception_throw_ref_helper;
          --stack_count;
        }
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &raised);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, raised, (uint64_t)helper,
              instruction.op == 0x08 ? GRJIT_CALL_GC_POINT : GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, args, argument_count, identity,
              instruction.op == 0x08 ? state : NULL,
              instruction.op == 0x08 ? slot_count : 0u);
        GRJIT_BlockId success, failure;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &success);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &failure);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(raised),
              success, failure);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, failure);
        if (jr == GRJIT_OK)
          jr = grjit_builder_trap(builder, GRJIT_TRAP_UNREACHABLE);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, success);
        if (jr != GRJIT_OK) {
          result = map_jit_result_capture(jr, failure_jit_result);
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        result = emit_exception_dispatch(builder, module, function,
            signature, function_index, control_openers, control_bases,
            control_depth, stack, stack_count, local_count, instance,
            spill_base, pc_blocks, shapes, identity, exception_take_helper,
            &jr);
        if (result != GLWASM_OK) {
          if (jr != GRJIT_OK && failure_jit_result)
            *failure_jit_result = (uint32_t)jr;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        terminates = 1;
        break;
      }
      case 0x41: case 0x42: case 0x43: case 0x44:
      case 0xd0: case 0xd2: {
        /* Function and exception references use stable owning tokens. */
        if (instruction.op == 0xd0 &&
            (instruction.a >= function->nv ||
             function->vpool[instruction.a].kind != GLWASM_VK_REF ||
             (function->vpool[instruction.a].heap != GLWASM_HT_FUNC &&
              function->vpool[instruction.a].heap != GLWASM_HT_NOFUNC &&
              function->vpool[instruction.a].heap != GLWASM_HT_EXN &&
              function->vpool[instruction.a].heap != GLWASM_HT_NOEXN))) {
          side_exit = 1; break;
        }
        type = instruction.op == 0x41 ? GRJIT_TYPE_I32
            : instruction.op == 0x43 ? GRJIT_TYPE_F32
            : instruction.op == 0x44 ? GRJIT_TYPE_F64
            : GRJIT_TYPE_I64;
        jr = grjit_builder_vreg(builder, type, &value);
        uint64_t token = instruction.op == 0x41
            ? (uint32_t)instruction.c
            : instruction.op == 0x43 ? instruction.a : instruction.c;
        if (instruction.op == 0xd0) token = 0;
        if (instruction.op == 0xd2) {
          /* A funcref is a store address, and the module's code serves
           * every instance, so the instance supplies its token. */
          GRJIT_Operand args[2] = {grjit_operand_vreg(instance),
              grjit_operand_imm(instruction.a)};
          if (jr == GRJIT_OK)
            jr = grjit_builder_call(builder, value, (uint64_t)funcref_helper,
                GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 2,
                identity, NULL, 0);
        } else if (jr == GRJIT_OK) jr = grjit_builder_const(builder, value,
            (int64_t)token);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        if (stack_count >= max_stack) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count++] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x20: { /* local.get */
        if (instruction.a >= local_count || stack_count >= max_stack) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        /* Wasm pushes a value copy: a later local.set must not mutate a
         * previously pushed operand, including across a branch/backedge. */
        jr = grjit_builder_vreg(builder, locals[instruction.a].type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_move(builder, value,
              grjit_operand_vreg(locals[instruction.a].reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count++] = (GLWASM_JitValue){value,
            locals[instruction.a].type};
        break;
      }
      case 0x21: case 0x22: { /* local.set / local.tee */
        if (instruction.a >= local_count || stack_count == 0 ||
            locals[instruction.a].type != stack[stack_count - 1].type) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        GRJIT_VReg assigned = locals[instruction.a].reg;
        jr = grjit_builder_move(builder, assigned,
              grjit_operand_vreg(stack[stack_count - 1].reg));
        if (jr == GRJIT_OK)
          jr = grjit_builder_store(builder, spill_base,
              (int32_t)((size_t)instruction.a * 8u),
              locals[instruction.a].type == GRJIT_TYPE_I32 ||
              locals[instruction.a].type == GRJIT_TYPE_F32 ? 32u : 64u,
              grjit_operand_vreg(assigned));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        if (instruction.op == 0x21) --stack_count;
        break;
      }
      case 0x1a: /* drop */
        if (stack_count == 0) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        --stack_count;
        break;
      case 0x1b: case 0x1c: { /* select / select with one explicit type */
        if (stack_count < 3u || (instruction.op == 0x1c &&
            (instruction.d != 1u || instruction.a >= function->nv))) {
          side_exit = 1; break;
        }
        size_t base = stack_count - 3u;
        GLWASM_JitValue first = stack[base];
        GLWASM_JitValue second = stack[base + 1u];
        GLWASM_JitValue condition = stack[base + 2u];
        if (condition.type != GRJIT_TYPE_I32 || first.type != second.type ||
            (instruction.op == 0x1c &&
             !wasm_jit_type(function->vpool[instruction.a], &type)) ||
            (instruction.op == 0x1c && first.type != type)) {
          side_exit = 1; break;
        }
        uint32_t width = first.type == GRJIT_TYPE_I32 ||
            first.type == GRJIT_TYPE_F32 ? 32u : 64u;
        GRJIT_BlockId choose_first, choose_second, merge;
        jr = grjit_builder_block(builder, &choose_first);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &choose_second);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &merge);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder,
              grjit_operand_vreg(condition.reg), choose_first, choose_second);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, choose_first);
        if (jr == GRJIT_OK)
          jr = grjit_builder_store(builder, spill_base,
              (int32_t)((local_count + base) * 8u), width,
              grjit_operand_vreg(first.reg));
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, merge);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, choose_second);
        if (jr == GRJIT_OK)
          jr = grjit_builder_store(builder, spill_base,
              (int32_t)((local_count + base) * 8u), width,
              grjit_operand_vreg(second.reg));
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, merge);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, merge);
        if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder, first.type,
            &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_load(builder, value, spill_base,
              (int32_t)((local_count + base) * 8u), width, false);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count = base;
        stack[stack_count++] = (GLWASM_JitValue){value, first.type};
        break;
      }
      case 0x6a: case 0x6b: case 0x6c: case 0x71: case 0x72: case 0x73:
      case 0x74: case 0x75: case 0x76: case 0x77: case 0x78:
      case 0x7c: case 0x7d: case 0x7e: case 0x83: case 0x84: case 0x85:
      case 0x86: case 0x87: case 0x88: case 0x89: case 0x8a: {
        if (stack_count < 2 ||
            stack[stack_count - 1].type != stack[stack_count - 2].type ||
            stack[stack_count - 1].type != (instruction.op <= 0x78
                ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64)) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        type = stack[stack_count - 1].type;
        GRJIT_VReg left = stack[stack_count - 2].reg;
        GRJIT_VReg right = stack[stack_count - 1].reg;
        jr = grjit_builder_vreg(builder, type, &value);
        GRJIT_OpKind operation;
        switch (instruction.op) {
          case 0x6a: case 0x7c: operation = GRJIT_OP_ADD; break;
          case 0x6b: case 0x7d: operation = GRJIT_OP_SUB; break;
          case 0x6c: case 0x7e: operation = GRJIT_OP_MUL; break;
          case 0x71: case 0x83: operation = GRJIT_OP_AND; break;
          case 0x72: case 0x84: operation = GRJIT_OP_OR; break;
          case 0x73: case 0x85: operation = GRJIT_OP_XOR; break;
          case 0x74: case 0x86: operation = GRJIT_OP_SHL; break;
          case 0x75: case 0x87: operation = GRJIT_OP_SAR; break;
          case 0x76: case 0x88: operation = GRJIT_OP_SHR; break;
          case 0x77: case 0x89: operation = GRJIT_OP_ROTL; break;
          default: operation = GRJIT_OP_ROTR; break;
        }
        if (jr == GRJIT_OK)
          jr = grjit_builder_binary(builder, operation, value,
              grjit_operand_vreg(left), grjit_operand_vreg(right));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count -= 2;
        stack[stack_count++] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x6d: case 0x6e: case 0x6f: case 0x70:
      case 0x7f: case 0x80: case 0x81: case 0x82: {
        if (stack_count < 2u) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        type = instruction.op < 0x7f ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
        GLWASM_JitValue left = stack[stack_count - 2u];
        GLWASM_JitValue right = stack[stack_count - 1u];
        if (left.type != type || right.type != type) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        int signed_op = instruction.op == 0x6d || instruction.op == 0x6f ||
            instruction.op == 0x7f || instruction.op == 0x81;
        int is_div = instruction.op == 0x6d || instruction.op == 0x6e ||
            instruction.op == 0x7f || instruction.op == 0x80;
        GRJIT_VReg nonzero, safe = GRJIT_NO_VREG;
        GRJIT_VReg no_overflow = GRJIT_NO_VREG;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &nonzero);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, nonzero,
              grjit_operand_vreg(right.reg), grjit_operand_imm(0));
        if (jr == GRJIT_OK && signed_op && is_div) {
          GRJIT_VReg is_min, is_negative_one, overflow;
          int64_t min_value = type == GRJIT_TYPE_I32
              ? INT32_MIN : INT64_MIN;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &is_min);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, is_min,
                grjit_operand_vreg(left.reg), grjit_operand_imm(min_value));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
                &is_negative_one);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, is_negative_one,
                grjit_operand_vreg(right.reg), grjit_operand_imm(-1));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &overflow);
          if (jr == GRJIT_OK)
            jr = grjit_builder_binary(builder, GRJIT_OP_AND, overflow,
                grjit_operand_vreg(is_min),
                grjit_operand_vreg(is_negative_one));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &no_overflow);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, no_overflow,
                grjit_operand_vreg(overflow), grjit_operand_imm(0));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &safe);
          if (jr == GRJIT_OK)
            jr = grjit_builder_binary(builder, GRJIT_OP_AND, safe,
                grjit_operand_vreg(nonzero),
                grjit_operand_vreg(no_overflow));
        } else safe = nonzero;
        GRJIT_BlockId safe_block, exit_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &safe_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &exit_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(safe),
              safe_block, exit_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, exit_block);
        if (jr == GRJIT_OK)
          result = emit_trap_exit(builder, function_index, instruction.op,
              trap_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, safe_block);
        /* Rebuild the IR verifier's partial-operation proofs in this block.
         * The branch condition above chooses the named trap exit, but facts
         * from its predecessor block are intentionally not trusted here. */
        GRJIT_VReg checked_nonzero;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
              &checked_nonzero);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, checked_nonzero,
              grjit_operand_vreg(right.reg), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_guard(builder,
              grjit_operand_vreg(checked_nonzero), identity, state,
              slot_count);
        if (jr == GRJIT_OK && signed_op && is_div) {
          GRJIT_VReg checked_min, checked_neg_one, checked_overflow;
          GRJIT_VReg checked_no_overflow;
          int64_t min_value = type == GRJIT_TYPE_I32
              ? INT32_MIN : INT64_MIN;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &checked_min);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, checked_min,
                grjit_operand_vreg(left.reg), grjit_operand_imm(min_value));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
                &checked_neg_one);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, checked_neg_one,
                grjit_operand_vreg(right.reg), grjit_operand_imm(-1));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
                &checked_overflow);
          if (jr == GRJIT_OK)
            jr = grjit_builder_binary(builder, GRJIT_OP_AND,
                checked_overflow, grjit_operand_vreg(checked_min),
                grjit_operand_vreg(checked_neg_one));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
                &checked_no_overflow);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ,
                checked_no_overflow,
                grjit_operand_vreg(checked_overflow),
                grjit_operand_imm(0));
          if (jr == GRJIT_OK)
            jr = grjit_builder_guard(builder,
                grjit_operand_vreg(checked_no_overflow), identity, state,
                slot_count);
        }
        if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder, type, &value);
        GRJIT_OpKind operation = instruction.op == 0x6d ||
            instruction.op == 0x7f ? GRJIT_OP_DIV_S :
            instruction.op == 0x6e || instruction.op == 0x80
                ? GRJIT_OP_DIV_U : instruction.op == 0x6f ||
                    instruction.op == 0x81 ? GRJIT_OP_REM_S : GRJIT_OP_REM_U;
        if (jr == GRJIT_OK)
          jr = grjit_builder_binary(builder, operation, value,
              grjit_operand_vreg(left.reg), grjit_operand_vreg(right.reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count -= 2u;
        stack[stack_count++] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x45: case 0x50: /* eqz */
      case 0x46: case 0x47: case 0x48: case 0x49: case 0x4a: case 0x4b:
      case 0x4c: case 0x4d: case 0x4e: case 0x4f:
      case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56:
      case 0x57: case 0x58: case 0x59: case 0x5a:
      case 0x5b: case 0x5c: case 0x5d: case 0x5e: case 0x5f: case 0x60:
      case 0x61: case 0x62: case 0x63: case 0x64: case 0x65: case 0x66: {
        int is_eqz = instruction.op == 0x45 || instruction.op == 0x50;
        if (stack_count < (is_eqz ? 1u : 2u)) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        GLWASM_JitValue right = stack[stack_count - 1];
        GLWASM_JitValue left = is_eqz
            ? (GLWASM_JitValue){GRJIT_NO_VREG, right.type}
            : stack[stack_count - 2];
        int is_float = instruction.op >= 0x5b;
        GRJIT_Type expected = instruction.op < 0x50 ? GRJIT_TYPE_I32 :
            instruction.op < 0x5b ? GRJIT_TYPE_I64 :
            instruction.op < 0x61 ? GRJIT_TYPE_F32 : GRJIT_TYPE_F64;
        if (right.type != expected || (!is_eqz && left.type != expected)) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        GRJIT_Cmp comparison = GRJIT_CMP_EQ;
        switch (instruction.op) {
          case 0x47: case 0x52: comparison = GRJIT_CMP_NE; break;
          case 0x48: case 0x53: comparison = GRJIT_CMP_LT; break;
          case 0x49: case 0x54: comparison = GRJIT_CMP_ULT; break;
          case 0x4a: case 0x55: comparison = GRJIT_CMP_GT; break;
          case 0x4b: case 0x56: comparison = GRJIT_CMP_UGT; break;
          case 0x4c: case 0x57: comparison = GRJIT_CMP_LE; break;
          case 0x4d: case 0x58: comparison = GRJIT_CMP_ULE; break;
          case 0x4e: case 0x59: comparison = GRJIT_CMP_GE; break;
          case 0x4f: case 0x5a: comparison = GRJIT_CMP_UGE; break;
          case 0x5c: case 0x62: comparison = GRJIT_CMP_NE; break;
          case 0x5d: case 0x63: comparison = GRJIT_CMP_LT; break;
          case 0x5e: case 0x64: comparison = GRJIT_CMP_GT; break;
          case 0x5f: case 0x65: comparison = GRJIT_CMP_LE; break;
          case 0x60: case 0x66: comparison = GRJIT_CMP_GE; break;
          default: break;
        }
        GRJIT_Operand a = is_eqz ? grjit_operand_vreg(right.reg)
            : grjit_operand_vreg(left.reg);
        GRJIT_Operand b = is_eqz ? grjit_operand_imm(0)
            : grjit_operand_vreg(right.reg);
        if (is_float && is_eqz) { side_exit = 1; break; }
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, comparison, value, a, b);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count -= is_eqz ? 1u : 2u;
        stack[stack_count++] = (GLWASM_JitValue){value, GRJIT_TYPE_I32};
        break;
      }
      case 0x8b: case 0x8c: case 0x8d: case 0x8e: case 0x8f: case 0x90:
      case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96:
      case 0x97: case 0x98:
      case 0x99: case 0x9a: case 0x9b: case 0x9c: case 0x9d: case 0x9e:
      case 0x9f: case 0xa0: case 0xa1: case 0xa2: case 0xa3: case 0xa4:
      case 0xa5: case 0xa6: {
        int is_f32 = instruction.op <= 0x98;
        int binary = instruction.op == 0x92 || instruction.op == 0x93 ||
            instruction.op == 0x94 || instruction.op == 0x95 ||
            instruction.op == 0x96 || instruction.op == 0x97 ||
            instruction.op == 0x98 || instruction.op == 0xa0 ||
            instruction.op == 0xa1 || instruction.op == 0xa2 ||
            instruction.op == 0xa3 || instruction.op == 0xa4 ||
            instruction.op == 0xa5 || instruction.op == 0xa6;
        size_t arity = binary ? 2u : 1u;
        type = is_f32 ? GRJIT_TYPE_F32 : GRJIT_TYPE_F64;
        if (stack_count < arity ||
            stack[stack_count - 1u].type != type ||
            (binary && stack[stack_count - 2u].type != type)) {
          side_exit = 1; break;
        }
        GRJIT_OpKind operation;
        switch (instruction.op) {
          case 0x8b: case 0x99: operation = GRJIT_OP_FABS; break;
          case 0x8c: case 0x9a: operation = GRJIT_OP_FNEG; break;
          case 0x8d: case 0x9b: operation = GRJIT_OP_FCEIL; break;
          case 0x8e: case 0x9c: operation = GRJIT_OP_FFLOOR; break;
          case 0x8f: case 0x9d: operation = GRJIT_OP_FTRUNC; break;
          case 0x90: case 0x9e: operation = GRJIT_OP_FNEAREST; break;
          case 0x91: case 0x9f: operation = GRJIT_OP_FSQRT; break;
          case 0x92: case 0xa0: operation = GRJIT_OP_FADD; break;
          case 0x93: case 0xa1: operation = GRJIT_OP_FSUB; break;
          case 0x94: case 0xa2: operation = GRJIT_OP_FMUL; break;
          case 0x95: case 0xa3: operation = GRJIT_OP_FDIV; break;
          case 0x96: case 0xa4: operation = GRJIT_OP_FMIN; break;
          case 0x97: case 0xa5: operation = GRJIT_OP_FMAX; break;
          default: operation = GRJIT_OP_FCOPYSIGN; break;
        }
        GRJIT_VReg output;
        jr = grjit_builder_vreg(builder, type, &output);
        if (jr == GRJIT_OK) {
          if (binary)
            jr = grjit_builder_binary(builder, operation, output,
                grjit_operand_vreg(stack[stack_count - 2u].reg),
                grjit_operand_vreg(stack[stack_count - 1u].reg));
          else
            jr = grjit_builder_unary(builder, operation, output,
                grjit_operand_vreg(stack[stack_count - 1u].reg));
        }
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count -= arity;
        stack[stack_count++] = (GLWASM_JitValue){output, type};
        break;
      }
      case 0xd1: { /* ref.is_null for stable nullable reference tokens */
        if (!stack_count || stack[stack_count - 1u].type != GRJIT_TYPE_I64) {
          side_exit = 1; break;
        }
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, value,
              grjit_operand_vreg(stack[stack_count - 1u].reg),
              grjit_operand_imm(0));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1u] = (GLWASM_JitValue){value, GRJIT_TYPE_I32};
        break;
      }
      case 0x67: case 0x68: case 0x69:
      case 0x79: case 0x7a: case 0x7b: {
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        type = instruction.op < 0x70 ? GRJIT_TYPE_I32 : GRJIT_TYPE_I64;
        if (stack[stack_count - 1].type != type) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        GRJIT_OpKind operation = instruction.op == 0x67 ||
            instruction.op == 0x79 ? GRJIT_OP_CLZ :
            instruction.op == 0x68 || instruction.op == 0x7a
                ? GRJIT_OP_CTZ : GRJIT_OP_POPCNT;
        jr = grjit_builder_vreg(builder, type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, operation, value,
              grjit_operand_vreg(stack[stack_count - 1].reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0xa7: case 0xac: case 0xad:
      case 0xc0: case 0xc1: case 0xc2: case 0xc3: case 0xc4: {
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_OpKind operation;
        switch (instruction.op) {
          case 0xa7:
            type = GRJIT_TYPE_I32; operation = GRJIT_OP_WRAP; break;
          case 0xac:
            type = GRJIT_TYPE_I64; operation = GRJIT_OP_EXTEND_S; break;
          case 0xad:
            type = GRJIT_TYPE_I64; operation = GRJIT_OP_EXTEND_U; break;
          case 0xc0:
            type = GRJIT_TYPE_I32; operation = GRJIT_OP_EXT8_S; break;
          case 0xc1:
            type = GRJIT_TYPE_I32; operation = GRJIT_OP_EXT16_S; break;
          case 0xc2:
            type = GRJIT_TYPE_I64; operation = GRJIT_OP_EXT8_S; break;
          case 0xc3:
            type = GRJIT_TYPE_I64; operation = GRJIT_OP_EXT16_S; break;
          default:
            type = GRJIT_TYPE_I64; operation = GRJIT_OP_EXT32_S; break;
        }
        GLWASM_JitValue input = stack[stack_count - 1];
        GRJIT_Type input_type = instruction.op == 0xa7 ||
            instruction.op == 0xc2 || instruction.op == 0xc3 ||
            instruction.op == 0xc4 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
        if (input.type != input_type) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        jr = grjit_builder_vreg(builder, type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, operation, value,
              grjit_operand_vreg(input.reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0xb2: case 0xb3: case 0xb4: case 0xb5: case 0xb6:
      case 0xb7: case 0xb8: case 0xb9: case 0xba: case 0xbb:
      case 0xbc: case 0xbd: case 0xbe: case 0xbf: {
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_Type input_type, output_type;
        GRJIT_OpKind operation;
        switch (instruction.op) {
          case 0xb2: input_type = GRJIT_TYPE_I32; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_CONVERT_S; break;
          case 0xb3: input_type = GRJIT_TYPE_I32; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_CONVERT_U; break;
          case 0xb4: input_type = GRJIT_TYPE_I64; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_CONVERT_S; break;
          case 0xb5: input_type = GRJIT_TYPE_I64; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_CONVERT_U; break;
          case 0xb6: input_type = GRJIT_TYPE_F64; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_DEMOTE; break;
          case 0xb7: input_type = GRJIT_TYPE_I32; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_CONVERT_S; break;
          case 0xb8: input_type = GRJIT_TYPE_I32; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_CONVERT_U; break;
          case 0xb9: input_type = GRJIT_TYPE_I64; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_CONVERT_S; break;
          case 0xba: input_type = GRJIT_TYPE_I64; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_CONVERT_U; break;
          case 0xbb: input_type = GRJIT_TYPE_F32; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_PROMOTE; break;
          case 0xbc: input_type = GRJIT_TYPE_F32; output_type = GRJIT_TYPE_I32;
            operation = GRJIT_OP_REINTERPRET; break;
          case 0xbd: input_type = GRJIT_TYPE_F64; output_type = GRJIT_TYPE_I64;
            operation = GRJIT_OP_REINTERPRET; break;
          case 0xbe: input_type = GRJIT_TYPE_I32; output_type = GRJIT_TYPE_F32;
            operation = GRJIT_OP_REINTERPRET; break;
          default: input_type = GRJIT_TYPE_I64; output_type = GRJIT_TYPE_F64;
            operation = GRJIT_OP_REINTERPRET; break;
        }
        if (stack[stack_count - 1u].type != input_type) {
          side_exit = 1; break;
        }
        jr = grjit_builder_vreg(builder, output_type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, operation, value,
              grjit_operand_vreg(stack[stack_count - 1u].reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1u] = (GLWASM_JitValue){value, output_type};
        break;
      }
      case 0xa8: case 0xa9: case 0xaa: case 0xab:
      case 0xae: case 0xaf: case 0xb0: case 0xb1: {
        /* A trapping truncation of an in-range input equals the saturating
         * one. NaN and out-of-range inputs take the trap exit, and the
         * interpreter raises the precise trap. */
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        uint32_t sub = instruction.op <= 0xabu ? instruction.op - 0xa8u
            : instruction.op - 0xaau;
        int source64 = sub == 2u || sub == 3u || sub == 6u || sub == 7u;
        int dest64 = sub >= 4u, is_unsigned = (int)(sub & 1u);
        GRJIT_Type input_type = source64 ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
        type = dest64 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
        if (stack[stack_count - 1u].type != input_type) {
          side_exit = 1; break;
        }
        /* Valid inputs lie in (low, high), or [low, high) when low is
         * itself representable and in range. Every bound is exact in both
         * float widths, and ordered comparisons reject NaN. */
        double low = is_unsigned ? -1.0 : dest64 ? -9223372036854775808.0
            : source64 ? -2147483649.0 : -2147483648.0;
        int low_inclusive = !is_unsigned && (dest64 || !source64);
        double high = is_unsigned
            ? (dest64 ? 18446744073709551616.0 : 4294967296.0)
            : (dest64 ? 9223372036854775808.0 : 2147483648.0);
        int64_t low_bits, high_bits;
        if (source64) {
          memcpy(&low_bits, &low, sizeof(low_bits));
          memcpy(&high_bits, &high, sizeof(high_bits));
        } else {
          float low_f = (float)low, high_f = (float)high;
          uint32_t low_u, high_u;
          memcpy(&low_u, &low_f, sizeof(low_u));
          memcpy(&high_u, &high_f, sizeof(high_u));
          low_bits = low_u;
          high_bits = high_u;
        }
        GRJIT_VReg input = stack[stack_count - 1u].reg, low_reg, high_reg,
            above, below, in_range;
        jr = grjit_builder_vreg(builder, input_type, &low_reg);
        if (jr == GRJIT_OK) jr = grjit_builder_const(builder, low_reg, low_bits);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, input_type, &high_reg);
        if (jr == GRJIT_OK)
          jr = grjit_builder_const(builder, high_reg, high_bits);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &above);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder,
              low_inclusive ? GRJIT_CMP_GE : GRJIT_CMP_GT, above,
              grjit_operand_vreg(input), grjit_operand_vreg(low_reg));
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &below);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_LT, below,
              grjit_operand_vreg(input), grjit_operand_vreg(high_reg));
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &in_range);
        if (jr == GRJIT_OK)
          jr = grjit_builder_binary(builder, GRJIT_OP_AND, in_range,
              grjit_operand_vreg(above), grjit_operand_vreg(below));
        GRJIT_BlockId safe_block, exit_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &safe_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &exit_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(in_range),
              safe_block, exit_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, exit_block);
        if (jr == GRJIT_OK)
          result = emit_trap_exit(builder, function_index, instruction.op,
              trap_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, safe_block);
        if (jr == GRJIT_OK) jr = grjit_builder_vreg(builder, type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, is_unsigned
              ? GRJIT_OP_TRUNC_SAT_U : GRJIT_OP_TRUNC_SAT_S, value,
              grjit_operand_vreg(input));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1u] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0xfc00: case 0xfc01: case 0xfc02: case 0xfc03:
      case 0xfc04: case 0xfc05: case 0xfc06: case 0xfc07: {
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        uint32_t sub = instruction.op & 0xffu;
        int source64 = sub == 2u || sub == 3u || sub == 6u || sub == 7u;
        int dest64 = sub >= 4u;
        type = dest64 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
        GRJIT_Type input_type = source64 ? GRJIT_TYPE_F64 : GRJIT_TYPE_F32;
        GRJIT_OpKind operation = (sub & 1u)
            ? GRJIT_OP_TRUNC_SAT_U : GRJIT_OP_TRUNC_SAT_S;
        if (stack[stack_count - 1u].type != input_type) {
          side_exit = 1; break;
        }
        jr = grjit_builder_vreg(builder, type, &value);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, operation, value,
              grjit_operand_vreg(stack[stack_count - 1u].reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack[stack_count - 1u] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x28: case 0x29: case 0x2a: case 0x2b:
      case 0x2c: case 0x2d: case 0x2e: case 0x2f:
      case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: {
        if (!stack_count) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        jr = GRJIT_OK;
        GLWASM_JitValue address = stack[stack_count - 1u];
        GRJIT_VReg widened = address.reg;
        if (address.type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
                grjit_operand_vreg(address.reg));
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        } else if (address.type != GRJIT_TYPE_I64) {
          side_exit = 1; break;
        }
        GRJIT_Operand args[5] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_vreg(widened), grjit_operand_imm(0)};
        GRJIT_VReg loaded;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &loaded);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, loaded, (uint64_t)memory_helper,
              GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 5, identity,
              NULL, 0);
        GRJIT_VReg failed;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &failed);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, failed,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId access_block, failure_block;
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &access_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &failure_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(failed),
              access_block, failure_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, failure_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, access_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        --stack_count;
        type = instruction.op == 0x29 || instruction.op >= 0x30
            ? GRJIT_TYPE_I64 : instruction.op == 0x2a ? GRJIT_TYPE_F32
            : instruction.op == 0x2b ? GRJIT_TYPE_F64 : GRJIT_TYPE_I32;
        if (type == GRJIT_TYPE_F32) {
          GRJIT_VReg bits;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &bits);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_WRAP, bits,
                grjit_operand_vreg(loaded));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_F32, &value);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_REINTERPRET, value,
                grjit_operand_vreg(bits));
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        } else if (type != GRJIT_TYPE_I64) {
          value = GRJIT_NO_VREG;
          jr = grjit_builder_vreg(builder, type, &value);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder,
                type == GRJIT_TYPE_I32 ? GRJIT_OP_WRAP : GRJIT_OP_REINTERPRET,
                value, grjit_operand_vreg(loaded));
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        } else value = loaded;
        stack[stack_count++] = (GLWASM_JitValue){value, type};
        break;
      }
      case 0x36: case 0x37: case 0x38: case 0x39:
      case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: {
        if (stack_count < 2u) { result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done; }
        jr = GRJIT_OK;
        GLWASM_JitValue address = stack[stack_count - 2u];
        GLWASM_JitValue stored = stack[stack_count - 1u];
        GRJIT_VReg widened = address.reg;
        if (address.type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
                grjit_operand_vreg(address.reg));
        } else if (address.type != GRJIT_TYPE_I64) {
          side_exit = 1; break;
        }
        GRJIT_Type value_type = instruction.op == 0x37 ||
            (instruction.op >= 0x3c && instruction.op <= 0x3e)
            ? GRJIT_TYPE_I64 :
            instruction.op == 0x38 ? GRJIT_TYPE_F32 :
            instruction.op == 0x39 ? GRJIT_TYPE_F64 : GRJIT_TYPE_I32;
        if (stored.type != value_type) { side_exit = 1; break; }
        GRJIT_VReg value64 = stored.reg;
        if (stored.type == GRJIT_TYPE_F32) {
          GRJIT_VReg bits;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &bits);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_REINTERPRET, bits,
                grjit_operand_vreg(stored.reg));
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &value64);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, value64,
                grjit_operand_vreg(bits));
        } else if (stored.type == GRJIT_TYPE_F64) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &value64);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_REINTERPRET, value64,
                grjit_operand_vreg(stored.reg));
        } else if (stored.type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &value64);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, value64,
                grjit_operand_vreg(stored.reg));
        }
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_Operand args[5] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_vreg(widened), grjit_operand_vreg(value64)};
        GRJIT_VReg ignored, failed;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &ignored);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, ignored, (uint64_t)memory_helper,
              GRJIT_CALL_NO_GC, GRCORE_SITE_GC_POINT_CALL, args, 5, identity,
              NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &failed);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, failed,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId access_block, failure_block;
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &access_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &failure_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(failed),
              access_block, failure_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, failure_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, access_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count -= 2u;
        break;
      }
      case 0x40: { /* memory.grow: an allocation-capable guest operation */
        if (instruction.a >= module->nmems || !stack_count) {
          result = GLWASM_ERR_INTERNAL;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        GRJIT_Type address_type = module->mems[instruction.a].addr64
            ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
        GLWASM_JitValue delta = stack[stack_count - 1u];
        if (delta.type != address_type) {
          side_exit = 1;
          break;
        }
        GRJIT_VReg widened = delta.reg;
        if (address_type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
                grjit_operand_vreg(delta.reg));
        }
        GRJIT_Operand args[5] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_vreg(widened), grjit_operand_imm(0)};
        GRJIT_VReg old_pages, succeeded;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &old_pages);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, old_pages,
              (uint64_t)memory_helper, GRJIT_CALL_GC_POINT,
              GRCORE_SITE_GC_POINT_CALL, args, 5, identity, state,
              slot_count);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &succeeded);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, succeeded,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId success_block, fallback_block;
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &success_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(succeeded),
              success_block, fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, fallback_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index,
              instruction.op, named_exit_helper, instance, identity, state,
              slot_count, signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, success_block);
        if (jr != GRJIT_OK) {
          result = map_jit_result_capture(jr, failure_jit_result);
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        GRJIT_VReg grow_result = old_pages;
        if (address_type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &grow_result);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_WRAP, grow_result,
                grjit_operand_vreg(old_pages));
          if (jr != GRJIT_OK) {
            result = map_jit_result_capture(jr, failure_jit_result);
            if (failure_site) *failure_site = __LINE__;
            goto done;
          }
        }
        stack[stack_count - 1u] =
            (GLWASM_JitValue){grow_result, address_type};
        break;
      }
      case 0x25: case 0x26: case 0x3f:
      case 0xfc09: case 0xfc0d: case 0xfc0f: case 0xfc10: {
        /* table.get, table.set, memory.size, data.drop, elem.drop,
         * table.grow and table.size: one helper call each, through the
         * memory helper, which leaves imported storage, invalid references
         * and out-of-range indexes to a pre-effect exit. */
        int is_table = instruction.op == 0x25 || instruction.op == 0x26 ||
            instruction.op == 0xfc0f || instruction.op == 0xfc10;
        GRJIT_Type address_type = GRJIT_TYPE_I32;
        if (is_table) {
          /* table.size reads no reference, so any table's size compiles. */
          if (instruction.a >= module->ntables ||
              (instruction.op != 0xfc10 &&
               (module->tables[instruction.a].elem.kind != GLWASM_VK_REF ||
                (module->tables[instruction.a].elem.heap != GLWASM_HT_FUNC &&
                 module->tables[instruction.a].elem.heap !=
                     GLWASM_HT_NOFUNC)))) {
            side_exit = 1; break;
          }
          if (module->tables[instruction.a].addr64)
            address_type = GRJIT_TYPE_I64;
        } else if (instruction.op == 0x3f) {
          if (instruction.a >= module->nmems) { side_exit = 1; break; }
          if (module->mems[instruction.a].addr64)
            address_type = GRJIT_TYPE_I64;
        }
        /* Operands, top last: table.get index; table.set index, value;
         * table.grow value, delta. */
        uint32_t pops = instruction.op == 0x25 ? 1u
            : instruction.op == 0x26 || instruction.op == 0xfc0f ? 2u : 0u;
        if (stack_count < pops) { side_exit = 1; break; }
        size_t arg_base = stack_count - pops;
        GRJIT_VReg address_reg = 0, value_reg = 0;
        int have_address = 0, have_value = 0;
        if (instruction.op == 0x25 || instruction.op == 0x26) {
          if (stack[arg_base].type != address_type ||
              (instruction.op == 0x26 &&
               stack[arg_base + 1u].type != GRJIT_TYPE_I64)) {
            side_exit = 1; break;
          }
          address_reg = stack[arg_base].reg;
          have_address = 1;
          if (instruction.op == 0x26) {
            value_reg = stack[arg_base + 1u].reg;
            have_value = 1;
          }
        } else if (instruction.op == 0xfc0f) {
          if (stack[arg_base].type != GRJIT_TYPE_I64 ||
              stack[arg_base + 1u].type != address_type) {
            side_exit = 1; break;
          }
          value_reg = stack[arg_base].reg;
          address_reg = stack[arg_base + 1u].reg;
          have_address = have_value = 1;
        }
        if (have_address && address_type == GRJIT_TYPE_I32) {
          GRJIT_VReg widened;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
                grjit_operand_vreg(address_reg));
          address_reg = widened;
        }
        GRJIT_Operand args[5] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            have_address ? grjit_operand_vreg(address_reg)
                : grjit_operand_imm(0),
            have_value ? grjit_operand_vreg(value_reg) : grjit_operand_imm(0)};
        GRJIT_VReg helper_result, succeeded;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &helper_result);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, helper_result,
              (uint64_t)memory_helper, GRJIT_CALL_GC_POINT,
              GRCORE_SITE_GC_POINT_CALL, args, 5, identity, state,
              slot_count);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &succeeded);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, succeeded,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId success_block, fallback_block;
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &success_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(succeeded),
              success_block, fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, fallback_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index,
              instruction.op, named_exit_helper, instance, identity, state,
              slot_count, signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, success_block);
        stack_count = arg_base;
        if (jr == GRJIT_OK && instruction.op != 0x26 &&
            instruction.op != 0xfc09 && instruction.op != 0xfc0d) {
          GRJIT_Type result_type = instruction.op == 0x25 ? GRJIT_TYPE_I64
              : address_type;
          GRJIT_VReg pushed = helper_result;
          if (result_type == GRJIT_TYPE_I32) {
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &pushed);
            if (jr == GRJIT_OK)
              jr = grjit_builder_unary(builder, GRJIT_OP_WRAP, pushed,
                  grjit_operand_vreg(helper_result));
          }
          if (jr == GRJIT_OK && stack_count >= max_stack) {
            result = GLWASM_ERR_INTERNAL;
            if (failure_site) *failure_site = __LINE__;
            goto done;
          }
          stack[stack_count++] = (GLWASM_JitValue){pushed, result_type};
        }
        if (jr != GRJIT_OK) {
          result = map_jit_result_capture(jr, failure_jit_result);
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        break;
      }
      case 0x0b: { /* final function end */
        if (pc + 1u == function->nins) {
          if (stack_count != signature->nresults) {
            /* This block has a compiled predecessor; a mismatched linear
             * operand shape cannot safely reconstruct that path. */
            result = GLWASM_ERR_UNSUPPORTED;
            if (failure_site) *failure_site = __LINE__;
            goto done;
          }
          GRJIT_Operand result_values[signature->nresults ?
              signature->nresults : 1u];
          for (uint32_t ri = 0; ri < signature->nresults; ++ri)
            result_values[ri] = grjit_operand_vreg(
                stack[stack_count - signature->nresults + ri].reg);
          jr = grjit_builder_ret_multi(builder, result_values,
              signature->nresults);
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
          terminates = 1;
        } else if (control_depth) {
          uint32_t opener = control_openers[control_depth - 1u];
          uint32_t block = function->ins[opener].a;
          if (block >= function->nblocks ||
              stack_count != control_bases[control_depth - 1u] +
                  function->blocks[block].nresults ||
              !wasm_jit_stack_suffix_matches(function,
                  function->blocks[block].result_at,
                  function->blocks[block].nresults, stack, stack_count)) {
            side_exit = 1;
            break;
          }
          --control_depth;
        } else {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        break;
      }
      case 0x0f: { /* return */
        if (stack_count < signature->nresults) {
          result = GLWASM_ERR_INTERNAL; if (failure_site) *failure_site = __LINE__; goto done;
        }
        GRJIT_Operand result_values[signature->nresults ?
            signature->nresults : 1u];
        for (uint32_t ri = 0; ri < signature->nresults; ++ri)
          result_values[ri] = grjit_operand_vreg(
              stack[stack_count - signature->nresults + ri].reg);
        jr = grjit_builder_ret_multi(builder, result_values,
            signature->nresults);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        /* The validator makes all remaining instructions unreachable. */
        terminates = 1;
        break;
      }
      case 0x12: { /* direct return_call through a registered entry slot */
        uint32_t callee = instruction.a;
        if (callee >= module->nfuncs ||
            module->funcs[callee].imported ||
            module->funcs[callee].type_index >= module->ntypes) {
          side_exit = 1; break;
        }
        GLWASM_Func *callee_func = &module->funcs[callee];
        GLWASM_TypeDef *callee_type = &module->types[callee_func->type_index];
        if (callee_type->nparams > WASM_JIT_MAX_CALL_VALUES ||
            callee_type->nresults > WASM_JIT_MAX_CALL_VALUES ||
            callee_type->nresults != signature->nresults ||
            stack_count < callee_type->nparams) {
          side_exit = 1; break;
        }
        for (uint32_t ri = 0; ri < callee_type->nresults; ++ri) {
          if (!wasm_jit_same_type(callee_type->results[ri],
              signature->results[ri])) {
            side_exit = 1; break;
          }
        }
        if (side_exit) break;
        size_t arg_base = stack_count - callee_type->nparams;
        GRJIT_Operand call_args[WASM_JIT_MAX_CALL_VALUES];
        for (uint32_t ai = 0; ai < callee_type->nparams; ++ai) {
          GRJIT_Type expected;
          if (!wasm_jit_type(callee_type->params[ai], &expected) ||
              stack[arg_base + ai].type != expected) {
            side_exit = 1; break;
          }
          call_args[ai] = grjit_operand_vreg(stack[arg_base + ai].reg);
        }
        if (side_exit) break;
        GRJIT_VReg slot_address, target_entry;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &slot_address);
        GRJIT_Operand slot_args[2] = {grjit_operand_vreg(instance),
            grjit_operand_imm(((uint64_t)module->debug_id << 32) | callee)};
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, slot_address,
              (uint64_t)slot_entry_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, slot_args, 2, identity, NULL, 0);
        GRJIT_VReg target_available;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
              &target_available);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, target_available,
              grjit_operand_vreg(slot_address), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_guard(builder,
              grjit_operand_vreg(target_available), identity, state,
              slot_count);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &target_entry);
        if (jr == GRJIT_OK)
          jr = grjit_builder_load(builder, target_entry, slot_address, 0,
              (uint32_t)(sizeof(uintptr_t) * 8u), false);
        if (jr == GRJIT_OK)
          jr = grjit_builder_tail_call_ptr_env(builder,
              grjit_operand_vreg(target_entry),
              ((uint64_t)module->debug_id << 32) | callee,
              callee_type->nresults, grjit_operand_vreg(instance), call_args,
              callee_type->nparams, identity, state, slot_count);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        terminates = 1;
        break;
      }
      case 0x13: { /* tail call through a validated dynamic table target */
        if (instruction.a >= module->ntypes ||
            instruction.b >= module->ntables) { side_exit = 1; break; }
        GLWASM_TypeDef *call_type = &module->types[instruction.a];
        if (call_type->nparams > WASM_JIT_MAX_CALL_VALUES ||
            call_type->nresults > WASM_JIT_MAX_CALL_VALUES ||
            call_type->nresults != signature->nresults ||
            stack_count < (size_t)call_type->nparams + 1u) {
          side_exit = 1; break;
        }
        for (uint32_t ri = 0; ri < call_type->nresults; ++ri)
          if (!wasm_jit_same_type(call_type->results[ri],
              signature->results[ri])) side_exit = 1;
        if (side_exit) break;
        size_t arg_base = stack_count - call_type->nparams - 1u;
        GLWASM_JitValue table_index = stack[stack_count - 1u];
        GRJIT_VReg table_index64 = table_index.reg;
        jr = GRJIT_OK;
        if (table_index.type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &table_index64);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U,
                table_index64, grjit_operand_vreg(table_index.reg));
        } else if (table_index.type != GRJIT_TYPE_I64) {
          side_exit = 1; break;
        }
        GRJIT_Operand call_args[WASM_JIT_MAX_CALL_VALUES];
        for (uint32_t ai = 0; !side_exit && ai < call_type->nparams; ++ai) {
          GRJIT_Type expected;
          if (!wasm_jit_type(call_type->params[ai], &expected) ||
              stack[arg_base + ai].type != expected) side_exit = 1;
          else call_args[ai] = grjit_operand_vreg(stack[arg_base + ai].reg);
        }
        if (side_exit) break;
        GRJIT_Operand target_args[4] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_vreg(table_index64)};
        GRJIT_VReg target_function, failed;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &target_function);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, target_function,
              (uint64_t)indirect_target_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, target_args, 4, identity, NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &failed);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, failed,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId valid_block, invalid_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &valid_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &invalid_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(failed),
              valid_block, invalid_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, invalid_block);
        if (jr == GRJIT_OK) {
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
          if (result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        }
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, valid_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_VReg widened, callee_token, slot_address, available;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
              grjit_operand_vreg(target_function));
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &callee_token);
        if (jr == GRJIT_OK)
          jr = grjit_builder_binary(builder, GRJIT_OP_OR, callee_token,
              grjit_operand_vreg(widened),
              grjit_operand_imm((uint64_t)module->debug_id << 32));
        GRJIT_Operand slot_args[2] = {grjit_operand_vreg(instance),
            grjit_operand_vreg(callee_token)};
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &slot_address);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, slot_address,
              (uint64_t)slot_entry_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, slot_args, 2, identity, NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &available);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, available,
              grjit_operand_vreg(slot_address), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_guard(builder,
              grjit_operand_vreg(available), identity, state, slot_count);
        if (jr == GRJIT_OK)
          jr = grjit_builder_tail_call_slot_dynamic_env(builder,
              grjit_operand_vreg(slot_address),
              grjit_operand_vreg(callee_token), call_type->nresults,
              grjit_operand_vreg(instance), call_args, call_type->nparams,
              identity, state, slot_count);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        terminates = 1;
        break;
      }
      case 0x11: { /* call through a validated dynamic table target */
        if (instruction.a >= module->ntypes ||
            instruction.b >= module->ntables) { side_exit = 1; break; }
        GLWASM_TypeDef *call_type = &module->types[instruction.a];
        if (call_type->nresults > WASM_JIT_MAX_CALL_VALUES ||
            call_type->nparams > WASM_JIT_MAX_CALL_VALUES ||
            stack_count < (size_t)call_type->nparams + 1u) {
          side_exit = 1; break;
        }
        size_t arg_base = stack_count - call_type->nparams - 1u;
        GLWASM_JitValue table_index = stack[stack_count - 1u];
        GRJIT_VReg table_index64 = table_index.reg;
        jr = GRJIT_OK;
        if (table_index.type == GRJIT_TYPE_I32) {
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &table_index64);
          if (jr == GRJIT_OK)
            jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U,
                table_index64, grjit_operand_vreg(table_index.reg));
        } else if (table_index.type != GRJIT_TYPE_I64) {
          side_exit = 1; break;
        }
        size_t result_count = call_type->nresults;
        GRJIT_Type result_types[result_count ? result_count : 1u];
        for (size_t ri = 0; ri < result_count; ++ri)
          if (!wasm_jit_type(call_type->results[ri], &result_types[ri]))
            side_exit = 1;
        GRJIT_Operand call_args[WASM_JIT_MAX_CALL_VALUES];
        for (uint32_t ai = 0; !side_exit && ai < call_type->nparams; ++ai) {
          GRJIT_Type expected;
          if (!wasm_jit_type(call_type->params[ai], &expected) ||
              stack[arg_base + ai].type != expected) side_exit = 1;
          else call_args[ai] = grjit_operand_vreg(stack[arg_base + ai].reg);
        }
        if (side_exit) break;
        GRJIT_Operand target_args[4] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_vreg(table_index64)};
        GRJIT_VReg target_function, failed;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &target_function);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, target_function,
              (uint64_t)indirect_target_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, target_args, 4, identity, NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &failed);
        if (jr == GRJIT_OK) {
          GRJIT_Operand status_args[1] = {grjit_operand_vreg(instance)};
          jr = grjit_builder_call(builder, failed,
              (uint64_t)helper_status_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, status_args, 1, identity, NULL, 0);
        }
        GRJIT_BlockId valid_block, invalid_block;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &valid_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &invalid_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(failed),
              valid_block, invalid_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, invalid_block);
        if (jr == GRJIT_OK) {
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
          if (result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        }
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, valid_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        GRJIT_VReg widened, callee_token, slot_address, available;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &widened);
        if (jr == GRJIT_OK)
          jr = grjit_builder_unary(builder, GRJIT_OP_EXTEND_U, widened,
              grjit_operand_vreg(target_function));
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I64, &callee_token);
        if (jr == GRJIT_OK)
          jr = grjit_builder_binary(builder, GRJIT_OP_OR, callee_token,
              grjit_operand_vreg(widened),
              grjit_operand_imm((uint64_t)module->debug_id << 32));
        GRJIT_Operand slot_args[2] = {grjit_operand_vreg(instance),
            grjit_operand_vreg(callee_token)};
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &slot_address);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, slot_address,
              (uint64_t)slot_entry_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, slot_args, 2, identity, NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &available);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, available,
              grjit_operand_vreg(slot_address), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_guard(builder,
              grjit_operand_vreg(available), identity, state, slot_count);
        size_t saved_count = stack_count;
        stack_count = arg_base;
        GLWASM_Result after_result = make_state(function, function_index,
            pc + 1u, locals, stack, stack_count, control_openers,
            control_bases, control_depth, instance, state_after, slot_count);
        stack_count = saved_count;
        if (after_result != GLWASM_OK) {
          result = after_result; if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        state_after[H_SKIP_CHARGE] = frame_constant(0);
        GRJIT_VReg results[result_count ? result_count : 1u];
        for (size_t ri = 0; jr == GRJIT_OK && ri < result_count; ++ri)
          jr = grjit_builder_vreg(builder, result_types[ri], &results[ri]);
        GRJIT_BlockId landing, resumed;
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &landing);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &resumed);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call_slot_dynamic_catch_results_env(builder,
              result_count ? results : NULL, result_count,
              grjit_operand_vreg(slot_address),
              grjit_operand_vreg(callee_token),
              grjit_operand_vreg(instance), call_args, call_type->nparams,
              identity, state_after, slot_count, identity, state, slot_count,
              landing);
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, resumed);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, landing);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        result = emit_exception_dispatch(builder, module, function,
            signature, function_index, control_openers, control_bases,
            control_depth, stack, stack_count, local_count, instance,
            spill_base, pc_blocks, shapes, identity, exception_take_helper,
            &jr);
        if (result != GLWASM_OK) {
          if (jr != GRJIT_OK && failure_jit_result)
            *failure_jit_result = (uint32_t)jr;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        jr = grjit_builder_set_block(builder, resumed);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count = arg_base;
        for (size_t ri = 0; ri < result_count; ++ri)
          stack[stack_count++] = (GLWASM_JitValue){results[ri],
              result_types[ri]};
        break;
      }
      case 0x10: { /* direct call through the instance's runtime-core slot */
        uint32_t callee = instruction.a;
        if (callee >= module->nfuncs ||
            module->funcs[callee].type_index >= module->ntypes) {
          side_exit = 1; break;
        }
        GLWASM_Func *callee_func = &module->funcs[callee];
        GLWASM_TypeDef *callee_type = &module->types[callee_func->type_index];
        if (callee_type->nresults > WASM_JIT_MAX_CALL_VALUES ||
            callee_type->nparams > WASM_JIT_MAX_CALL_VALUES ||
            stack_count < callee_type->nparams) {
          side_exit = 1; break;
        }
        size_t arg_base = stack_count - callee_type->nparams;
        GRJIT_Operand call_args[WASM_JIT_MAX_CALL_VALUES];
        for (uint32_t ai = 0; ai < callee_type->nparams; ++ai) {
          GRJIT_Type expected;
          if (!wasm_jit_type(callee_type->params[ai], &expected) ||
              stack[arg_base + ai].type != expected) {
            side_exit = 1; break;
          }
          call_args[ai] = grjit_operand_vreg(stack[arg_base + ai].reg);
        }
        if (side_exit) break;
        if (callee_func->imported) {
          GRJIT_Type call_result_types[callee_type->nresults ?
              callee_type->nresults : 1u];
          for (uint32_t ri = 0; ri < callee_type->nresults; ++ri)
            if (!wasm_jit_type(callee_type->results[ri],
                &call_result_types[ri])) side_exit = 1;
          if (side_exit) break;
          size_t saved_count = stack_count;
          stack_count = arg_base;
          GLWASM_Result after_result = make_state(function, function_index,
              pc + 1u, locals, stack, stack_count, control_openers,
              control_bases, control_depth, instance, state_after, slot_count);
          stack_count = saved_count;
          if (after_result != GLWASM_OK) {
            result = after_result; if (failure_site) *failure_site = __LINE__; goto done;
          }
          state_after[H_SKIP_CHARGE] = frame_constant(0);
          GRJIT_VReg call_status;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &call_status);
          GRJIT_Operand host_args[4] = {grjit_operand_vreg(instance),
              grjit_operand_imm(function_index), grjit_operand_imm(pc),
              grjit_operand_imm(arg_base)};
          if (jr == GRJIT_OK)
            jr = grjit_builder_call(builder, call_status,
                (uint64_t)host_call_helper, GRJIT_CALL_GC_POINT,
                GRCORE_SITE_GC_POINT_CALL, host_args, 4, identity, state,
                slot_count);
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
          GRJIT_VReg is_success;
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &is_success);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, is_success,
                grjit_operand_vreg(call_status), grjit_operand_imm(0));
          GRJIT_BlockId success_block, failure_block, pause_block,
              error_block;
          if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &success_block);
          if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &failure_block);
          if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &pause_block);
          if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &error_block);
          GRJIT_VReg is_pause;
          if (jr == GRJIT_OK)
            jr = grjit_builder_br_if(builder, grjit_operand_vreg(is_success),
                success_block, failure_block);
          if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder,
              failure_block);
          if (jr == GRJIT_OK)
            jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &is_pause);
          if (jr == GRJIT_OK)
            jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, is_pause,
                grjit_operand_vreg(call_status), grjit_operand_imm(1));
          if (jr == GRJIT_OK)
            jr = grjit_builder_br_if(builder, grjit_operand_vreg(is_pause),
                pause_block, error_block);
          if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, pause_block);
          if (jr == GRJIT_OK) {
            result = emit_guard_return(builder, identity, state, slot_count,
                signature);
            if (result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
          }
          if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, error_block);
          /* A failed host callback is terminal for this run. Rebuilding from
           * the post-call state would repeat or continue guest execution, and
           * may fail after the callback has already committed its effects.
           * The runtime maps this terminal trap exit back to the callback's
           * authoritative run.error. Pause remains the pre-call retry path. */
          if (jr == GRJIT_OK)
            jr = grjit_builder_trap(builder, GRJIT_TRAP_UNREACHABLE);
          if (jr == GRJIT_OK)
            jr = grjit_builder_set_block(builder, success_block);
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
          stack_count = arg_base;
          for (uint32_t ri = 0; ri < callee_type->nresults; ++ri) {
            jr = grjit_builder_vreg(builder, call_result_types[ri], &value);
            if (jr == GRJIT_OK)
              jr = grjit_builder_load(builder, value, spill_base,
                  (int32_t)((local_count + max_stack + ri) * 8u),
                  call_result_types[ri] == GRJIT_TYPE_I32 ||
                  call_result_types[ri] == GRJIT_TYPE_F32 ? 32u : 64u, false);
            if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
            stack[stack_count++] = (GLWASM_JitValue){value,
                call_result_types[ri]};
          }
          break;
        }
        size_t saved_count = stack_count;
        stack_count = arg_base;
        GLWASM_Result after_result = make_state(function, function_index,
            pc + 1u, locals, stack, stack_count, control_openers,
            control_bases, control_depth, instance, state_after, slot_count);
        stack_count = saved_count;
        if (after_result != GLWASM_OK) { result = after_result; if (failure_site) *failure_site = __LINE__; goto done; }
        state_after[H_SKIP_CHARGE] = frame_constant(0);
        GRJIT_VReg slot_address;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_PTR, &slot_address);
        GRJIT_Operand slot_args[2] = {grjit_operand_vreg(instance),
            grjit_operand_imm(((uint64_t)module->debug_id << 32) | callee)};
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, slot_address,
              (uint64_t)slot_entry_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, slot_args, 2, identity, NULL, 0);
        GRJIT_VReg target_available;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32,
              &target_available);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_NE, target_available,
              grjit_operand_vreg(slot_address), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_guard(builder,
              grjit_operand_vreg(target_available), identity, state,
              slot_count);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        size_t result_count = callee_type->nresults;
        GRJIT_VReg call_results[result_count ? result_count : 1u];
        GRJIT_Type call_result_types[result_count ? result_count : 1u];
        for (size_t ri = 0; ri < result_count; ++ri) {
          if (!wasm_jit_type(callee_type->results[ri],
              &call_result_types[ri])) {
            side_exit = 1; break;
          }
          jr = grjit_builder_vreg(builder, call_result_types[ri],
              &call_results[ri]);
          if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        }
        if (side_exit) break;
        uint64_t callee_token = ((uint64_t)module->debug_id << 32) | callee;
        GRJIT_BlockId landing, resumed;
        jr = grjit_builder_block(builder, &landing);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &resumed);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call_slot_operand_catch_results_env(builder,
            result_count ? call_results : NULL,
            result_count, grjit_operand_vreg(slot_address), callee_token,
            grjit_operand_vreg(instance), call_args, callee_type->nparams,
            identity, state_after, slot_count, identity, state, slot_count,
            landing);
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, resumed);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, landing);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        result = emit_exception_dispatch(builder, module, function,
            signature, function_index, control_openers, control_bases,
            control_depth, stack, stack_count, local_count, instance,
            spill_base, pc_blocks, shapes, identity, exception_take_helper,
            &jr);
        if (result != GLWASM_OK) {
          if (jr != GRJIT_OK && failure_jit_result)
            *failure_jit_result = (uint32_t)jr;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        jr = grjit_builder_set_block(builder, resumed);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count = arg_base;
        for (size_t ri = 0; ri < result_count; ++ri)
          stack[stack_count++] = (GLWASM_JitValue){call_results[ri],
              call_result_types[ri]};
        break;
      }
      case 0xfc08: case 0xfc0a: case 0xfc0b: {
        /* memory.init / memory.copy / memory.fill */
        uint32_t sub = instruction.op & 0xffu;
        uint32_t dst_memory = sub == 8u ? instruction.b : instruction.a;
        uint32_t src_memory = instruction.b;
        if (stack_count < 3u || dst_memory >= module->nmems ||
            (sub == 8u && instruction.a >= module->ndatas) ||
            (sub == 10u && src_memory >= module->nmems)) {
          side_exit = 1; break;
        }
        int addr64 = module->mems[dst_memory].addr64 != 0;
        GRJIT_Type address_type = addr64 ? GRJIT_TYPE_I64 : GRJIT_TYPE_I32;
        /* memory.init's segment offset and length are i32 for every
         * memory. */
        if ((sub == 10u &&
             addr64 != (module->mems[src_memory].addr64 != 0)) ||
            stack[stack_count - 3u].type != address_type ||
            (sub == 10u && stack[stack_count - 2u].type != address_type) ||
            (sub != 10u && stack[stack_count - 2u].type != GRJIT_TYPE_I32) ||
            stack[stack_count - 1u].type !=
                (sub == 8u ? GRJIT_TYPE_I32 : address_type)) {
          side_exit = 1; break;
        }
        size_t arg_base = stack_count - 3u;
        GRJIT_Operand helper_args[4] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_imm(arg_base)};
        /* After the first chunk the interpreter continuation owns the three
         * operands. A poll reconstructs at this PC with them popped and with
         * charging skipped; bulk_done prevents replaying committed chunks. */
        size_t saved_count = stack_count;
        stack_count = arg_base;
        GLWASM_Result bulk_state_result = make_state(function,
            function_index, pc, locals, stack, stack_count,
            control_openers, control_bases, control_depth, instance,
            state_after, slot_count);
        stack_count = saved_count;
        if (bulk_state_result != GLWASM_OK) {
          result = bulk_state_result;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        state_after[H_SKIP_CHARGE] = frame_constant(1);
        GRJIT_BlockId chunk_block, status_block, invalid_block,
            poll_block, success_block, fallback_block, resume_block;
        jr = grjit_builder_block(builder, &chunk_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &status_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &invalid_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &poll_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &success_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &fallback_block);
        if (jr == GRJIT_OK) jr = grjit_builder_block(builder, &resume_block);
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, chunk_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, chunk_block);
        GRJIT_VReg status, done, more, pre_effect;
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &status);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, status,
              (uint64_t)memory_bulk_helper, GRJIT_CALL_NO_GC,
              GRCORE_SITE_GC_POINT_CALL, helper_args, 4, identity, NULL, 0);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &done);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, done,
              grjit_operand_vreg(status), grjit_operand_imm(0));
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(done),
              success_block, status_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, status_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &more);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, more,
              grjit_operand_vreg(status), grjit_operand_imm(2));
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(more),
              poll_block, invalid_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, invalid_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &pre_effect);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, pre_effect,
              grjit_operand_vreg(status), grjit_operand_imm(1));
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(pre_effect),
              fallback_block, resume_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, poll_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_poll(builder, identity, state_after, slot_count);
        if (jr == GRJIT_OK) jr = grjit_builder_br(builder, chunk_block);
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, fallback_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK) jr = grjit_builder_set_block(builder, resume_block);
        if (jr == GRJIT_OK) {
          result = emit_guard_return(builder, identity, state_after,
              slot_count, signature);
          if (result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        }
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, success_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count = arg_base;
        break;
      }
      case 0xfc0c: case 0xfc0e: case 0xfc11: {
        /* table.init / table.copy / table.fill */
        uint32_t sub = instruction.op & 0xffu;
        uint32_t dst_table = sub == 12u ? instruction.b : instruction.a;
        uint32_t src_table = instruction.b;
        if (stack_count < 3u || dst_table >= module->ntables ||
            (sub == 12u && instruction.a >= module->nelems) ||
            (sub == 14u && src_table >= module->ntables) ||
            module->tables[dst_table].elem.kind != GLWASM_VK_REF ||
            /* Init and copy move tokens inside the helper; only fill takes
             * a reference from compiled code. */
            (sub == 17u &&
             module->tables[dst_table].elem.heap != GLWASM_HT_FUNC &&
             module->tables[dst_table].elem.heap != GLWASM_HT_NOFUNC) ||
            (sub == 14u && !wasm_jit_same_type(
                module->tables[dst_table].elem,
                module->tables[src_table].elem))) {
          side_exit = 1; break;
        }
        size_t arg_base = stack_count - 3u;
        if ((stack[arg_base].type != GRJIT_TYPE_I32 &&
             stack[arg_base].type != GRJIT_TYPE_I64) ||
            (sub == 12u && (stack[arg_base + 1u].type != GRJIT_TYPE_I32 ||
                stack[arg_base + 2u].type != GRJIT_TYPE_I32)) ||
            (sub == 14u && stack[arg_base + 1u].type !=
                stack[arg_base].type) ||
            (sub == 17u && stack[arg_base + 1u].type != GRJIT_TYPE_I64) ||
            (stack[arg_base + 2u].type != GRJIT_TYPE_I32 &&
             stack[arg_base + 2u].type != GRJIT_TYPE_I64)) {
          side_exit = 1; break;
        }
        GRJIT_Operand helper_args[4] = {grjit_operand_vreg(instance),
            grjit_operand_imm(function_index), grjit_operand_imm(pc),
            grjit_operand_imm(arg_base)};
        GRJIT_VReg status, success;
        jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &status);
        if (jr == GRJIT_OK)
          jr = grjit_builder_call(builder, status,
              (uint64_t)table_bulk_helper, GRJIT_CALL_GC_POINT,
              GRCORE_SITE_GC_POINT_CALL, helper_args, 4, identity, state,
              slot_count);
        if (jr == GRJIT_OK)
          jr = grjit_builder_vreg(builder, GRJIT_TYPE_I32, &success);
        if (jr == GRJIT_OK)
          jr = grjit_builder_cmp(builder, GRJIT_CMP_EQ, success,
              grjit_operand_vreg(status), grjit_operand_imm(0));
        GRJIT_BlockId success_block, fallback_block;
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &success_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_block(builder, &fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_br_if(builder, grjit_operand_vreg(success),
              success_block, fallback_block);
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, fallback_block);
        if (jr == GRJIT_OK)
          result = emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
        if (jr == GRJIT_OK && result != GLWASM_OK) jr = GRJIT_ERR_INTERNAL;
        if (jr == GRJIT_OK)
          jr = grjit_builder_set_block(builder, success_block);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
        stack_count = arg_base;
        break;
      }
      default:
        side_exit = 1;
        break;
    }
    if (side_exit) {
      /* `unreachable` always traps; the interpreter raises it. */
      result = instruction.op == 0x00
          ? emit_trap_exit(builder, function_index, instruction.op,
              trap_exit_helper, instance, identity, state, slot_count,
              signature)
          : emit_named_exit(builder, function_index, instruction.op,
              named_exit_helper, instance, identity, state, slot_count,
              signature);
      if (result != GLWASM_OK) {
        if (failure_site) *failure_site = __LINE__;
        goto done;
      }
      /* The named exit ends this compiled path, but later lexical targets
       * still need the correct enclosing-control depth. */
      if (instruction.op == 0x02 || instruction.op == 0x03 ||
          instruction.op == 0x04 || instruction.op == 0x1f) {
        if (control_depth >= function->nins) {
          result = GLWASM_ERR_INTERNAL;
          if (failure_site) *failure_site = __LINE__;
          goto done;
        }
        control_openers[control_depth] = pc;
        control_bases[control_depth++] = (uint32_t)stack_count;
      } else if (instruction.op == 0x0b &&
          pc + 1u < function->nins && control_depth) {
        --control_depth;
      }
      continue;
    }
    result = GLWASM_OK;
    if (terminates) {
      switch (instruction.op) {
        case 0x04: /* if */
          result = mark_reachable_pc(pc + 1u, function->nins, shapes,
              stack, stack_count, &module->alloc);
          if (result == GLWASM_OK)
            result = mark_reachable_pc(
                function->translated->ins[pc].branch_target,
                function->nins, shapes, stack, stack_count, &module->alloc);
          break;
        case 0x05: /* else */
          result = mark_reachable_pc(
              function->translated->ins[pc].alternate_target,
              function->nins, shapes, stack, stack_count, &module->alloc);
          break;
        case 0x0d: /* br_if */
          result = mark_reachable_pc(pc + 1u, function->nins, shapes,
              stack, stack_count, &module->alloc);
          if (result == GLWASM_OK)
            result = mark_branch_pc(branch_label.target_pc,
                function->nins, shapes, stack, stack_count,
                branch_label.base, branch_label.arity, &module->alloc);
          break;
        case 0x0c: /* br */
          result = mark_branch_pc(branch_label.target_pc,
              function->nins, shapes, stack, stack_count,
              branch_label.base, branch_label.arity, &module->alloc);
          break;
        case 0x0e: break; /* Each table edge recorded its compacted shape. */
        default: break; /* return, tail call, trap, or final end */
      }
    } else {
      result = mark_reachable_pc(pc + 1u, function->nins, shapes,
          stack, stack_count, &module->alloc);
    }
    if (result != GLWASM_OK) {
      if (failure_site) *failure_site = __LINE__;
      goto done;
    }
    if (!terminates) {
      for (size_t i = 0; i < stack_count; ++i) {
        jr = grjit_builder_store(builder, spill_base,
            (int32_t)((local_count + i) * 8u),
            stack[i].type == GRJIT_TYPE_I32 || stack[i].type == GRJIT_TYPE_F32
                ? 32u : 64u,
            grjit_operand_vreg(stack[i].reg));
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
      }
      if (pc + 1u < function->nins) {
        jr = grjit_builder_br(builder, pc_blocks[pc + 1u]);
        if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
      }
    }
  }
  jr = grjit_builder_finish(builder, &ir);
  if (jr != GRJIT_OK) { result = map_jit_result_capture(jr, failure_jit_result); if (failure_site) *failure_site = __LINE__; goto done; }
  builder = NULL;
  {
    GRJIT_CompileOptions options = {0};
    options.pages = grcore_code_pages_provider(pages);
    options.limits = &jit_limits;
    /* Compile from the immutable cache identity used to own this artifact.
     * Runtime-jit intersects it with host detection before emission. */
    options.disabled_features = cache_key.disabled_cpu_features;
    jr = grjit_compile(&options, ir, &code);
  }
  result = map_jit_result_capture(jr, failure_jit_result);
  if (result == GLWASM_OK) {
    *out_code = code;
    code = NULL;
  }

done:
  if (builder) grjit_builder_destroy(builder);
  if (ir) grjit_function_destroy(ir);
  if (code) grjit_code_destroy(code);
  gcu_allocator_free(&module->alloc, locals);
  gcu_allocator_free(&module->alloc, stack);
  gcu_allocator_free(&module->alloc, local_types);
  gcu_allocator_free(&module->alloc, state);
  gcu_allocator_free(&module->alloc, state_after);
  gcu_allocator_free(&module->alloc, pc_blocks);
  for (uint32_t pc = 0; pc < function->nins; ++pc)
    gcu_allocator_free(&module->alloc, shapes[pc].types);
  gcu_allocator_free(&module->alloc, shapes);
  gcu_allocator_free(&module->alloc, control_openers);
  gcu_allocator_free(&module->alloc, control_bases);
  return result;
}
