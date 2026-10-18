/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 */

#ifndef GHOTI_IO_GLWASM_RUNTIME_LAYOUT_H
#define GHOTI_IO_GLWASM_RUNTIME_LAYOUT_H

#include "../module_internal.h"
#include <ghoti.io/lang-wasm/macros.h>

#include <stddef.h>

enum {
  H_FUNC, H_PC, H_SP, H_NLOCALS, H_CTRL_DEPTH, H_SKIP_CHARGE,
  H_INSTANCE, H_OPERANDS
};

static inline size_t glwasm_operand_capacity(const GLWASM_Func *function) {
  /* max_stack is measured by the validator over every reachable operand
   * effect. Keep two spare words for interpreter transitions and helpers. */
  return (size_t)function->max_stack + 2u;
}

static inline size_t glwasm_frame_slots(const GLWASM_Func *function) {
  return H_OPERANDS + function->nlocals +
      glwasm_operand_capacity(function) +
      2u * function->max_control_depth + 2u;
}

static inline size_t glwasm_jit_spill_words(const GLWASM_Func *function) {
  return H_OPERANDS + function->nlocals +
      glwasm_operand_capacity(function) + 1u;
}

static inline size_t glwasm_control_base(const GLWASM_Func *function) {
  return H_OPERANDS + function->nlocals +
      glwasm_operand_capacity(function);
}

#endif
