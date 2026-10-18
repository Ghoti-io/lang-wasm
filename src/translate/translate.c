/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 *
 * Ghoti.io Lang-wasm is free software: you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License version
 * 3 as published by the Free Software Foundation.
 *
 * Ghoti.io Lang-wasm is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * Resolve structured control destinations once, after validation, and retain
 * the binary byte offset for each instruction boundary.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/cutil/allocator.h>

#include "../module_internal.h"

#include <string.h>

typedef struct Label {
  uint32_t opener;
  uint32_t end;
  uint32_t alternate;
  uint8_t kind;
} Label;

static int is_open(uint16_t op) {
  return op == 0x02 || op == 0x03 || op == 0x04 || op == 0x1f;
}

static int is_branch(uint16_t op) {
  return op == 0x0c || op == 0x0d || op == 0x0e;
}

/* Story 3's shared interpreter/JIT fuel table: each in-scope Wasm opcode
 * costs one unit. Unknown encodings are not executable and cost nothing. */
uint8_t glwasm_opcode_cost(uint16_t opcode) {
  if (opcode <= 0xffu ||
      (opcode >= 0xfc00u && opcode <= 0xfc11u)) {
    return 1u;
  }
  return 0u;
}

static uint32_t label_destination(const Label * label) {
  return label->kind == 0x03 ? label->opener + 1u : label->end + 1u;
}

static void translated_free(GLWASM_Module * module,
    GLWASM_TranslatedFunc * translated) {
  if (!translated) {
    return;
  }
  gcu_allocator_free(&module->alloc, translated->ins);
  gcu_allocator_free(&module->alloc, translated->offsets);
  gcu_allocator_free(&module->alloc, translated->branch_table_targets);
  gcu_allocator_free(&module->alloc, translated);
}

void glwasm_translate_func_destroy(GLWASM_Module * module,
    GLWASM_TranslatedFunc * translated) {
  translated_free(module, translated);
}

static GLWASM_Result translate_func(GLWASM_Module * module,
    GLWASM_Func * function) {
  GLWASM_TranslatedFunc * translated;
  Label * labels;
  uint32_t * active;
  uint32_t depth = 1;
  uint32_t i;

  if (function->imported || function->is_const || function->nins == 0) {
    return GLWASM_OK;
  }
  translated = gcu_allocator_calloc(&module->alloc, 1, sizeof(*translated));
  labels = gcu_allocator_calloc(&module->alloc,
      (size_t)function->nins + 1u, sizeof(*labels));
  active = gcu_allocator_calloc(&module->alloc,
      (size_t)function->nins + 1u, sizeof(*active));
  if (!translated || !labels || !active) {
    translated_free(module, translated);
    gcu_allocator_free(&module->alloc, labels);
    gcu_allocator_free(&module->alloc, active);
    return GLWASM_ERR_OOM;
  }
  translated->nins = function->nins;
  translated->offset_count = function->nins + 1u;
  translated->ins = gcu_allocator_calloc(&module->alloc,
      translated->nins, sizeof(*translated->ins));
  translated->offsets = gcu_allocator_calloc(&module->alloc,
      translated->offset_count, sizeof(*translated->offsets));
  if (!translated->ins || !translated->offsets) {
    translated_free(module, translated);
    gcu_allocator_free(&module->alloc, labels);
    gcu_allocator_free(&module->alloc, active);
    return GLWASM_ERR_OOM;
  }
  /* The function label targets the final end instruction. */
  labels[0] = (Label){0, function->nins - 1u, UINT32_MAX, 0};
  active[0] = 0;
  for (i = 0; i < function->nins; ++i) {
    uint16_t op = function->ins[i].op;
    translated->ins[i].source = function->ins[i];
    translated->offsets[i] = function->ins[i].source_offset;
    if (is_open(op)) {
      labels[i + 1u] = (Label){i, UINT32_MAX, UINT32_MAX,
          (uint8_t)op};
      active[depth++] = i + 1u;
    } else if (op == 0x05 && depth > 1u) {
      labels[active[depth - 1u]].alternate = i;
    } else if (op == 0x0b && depth > 1u) {
      labels[active[--depth]].end = i;
    }
  }
  /* Every position includes the terminal boundary after the final `end`
   * opcode. The terminal is a useful resume position, but is not itself an
   * executable instruction boundary. */
  translated->offsets[function->nins] =
      translated->offsets[function->nins - 1u] + 1u;
  depth = 1;
  for (i = 0; i < function->nins; ++i) {
    GLWASM_Instr * source = &function->ins[i];
    GLWASM_TranslatedInstr * target = &translated->ins[i];
    uint32_t label_index;

    target->branch_target = UINT32_MAX;
    target->alternate_target = UINT32_MAX;
    target->call_target = UINT32_MAX;
    if (source->op == 0x10 || source->op == 0x12) {
      if (source->a >= module->nfuncs) {
        translated_free(module, translated);
        gcu_allocator_free(&module->alloc, labels);
        gcu_allocator_free(&module->alloc, active);
        return GLWASM_ERR_INTERNAL;
      }
      target->call_target = source->a;
    }

    if (is_open(source->op) && source->op != 0x1f) {
      active[depth++] = i + 1u;
    }

    if (source->op == 0x1f) {
      uint32_t j;
      if (source->b > function->ncatches ||
          source->d > function->ncatches - source->b) {
        translated_free(module, translated);
        gcu_allocator_free(&module->alloc, labels);
        gcu_allocator_free(&module->alloc, active);
        return GLWASM_ERR_INTERNAL;
      }
      for (j = 0; j < source->d; ++j) {
        GLWASM_Catch * clause = &function->catches[source->b + j];
        uint32_t label_index;
        if (clause->label >= depth) {
          translated_free(module, translated);
          gcu_allocator_free(&module->alloc, labels);
          gcu_allocator_free(&module->alloc, active);
          return GLWASM_ERR_INTERNAL;
        }
        label_index = active[depth - 1u - clause->label];
        clause->target_pc = label_destination(&labels[label_index]);
        clause->target_opener = label_index == 0
            ? UINT32_MAX : labels[label_index].opener;
        clause->resolved = 1;
      }
      /* Clause labels are relative to the enclosing labels. The try_table's
       * own result label is not a legal catch target. */
      active[depth++] = i + 1u;
    }

    if (source->op == 0x04) {
      const Label * label = &labels[i + 1u];
      target->branch_target = label->alternate == UINT32_MAX
          ? label->end + 1u : label->alternate + 1u;
      target->alternate_target = label->end + 1u;
    }
    if (source->op == 0x05) {
      target->alternate_target = labels[active[depth - 1u]].end + 1u;
    }
    if (!is_branch(source->op)) {
      if (source->op == 0x0b && depth > 1u) {
        --depth;
      }
      continue;
    }
    if (source->op == 0x0e) {
      uint32_t j;
      uint32_t base = source->a;
      uint32_t count = source->b;
      uint32_t * grown;

      if (count == 0 || base > function->nu32 ||
          count > function->nu32 - base ||
          count > UINT32_MAX - translated->branch_table_count) {
        translated_free(module, translated);
        gcu_allocator_free(&module->alloc, labels);
        gcu_allocator_free(&module->alloc, active);
        return GLWASM_ERR_LIMIT;
      }
      grown = gcu_allocator_realloc(&module->alloc,
          translated->branch_table_targets,
          (size_t)(translated->branch_table_count + count) * sizeof(*grown));
      if (!grown) {
        translated_free(module, translated);
        gcu_allocator_free(&module->alloc, labels);
        gcu_allocator_free(&module->alloc, active);
        return GLWASM_ERR_OOM;
      }
      translated->branch_table_targets = grown;
      for (j = 0; j < count; ++j) {
        uint32_t branch_depth = function->u32s[base + j];
        if (branch_depth >= depth) {
          translated_free(module, translated);
          gcu_allocator_free(&module->alloc, labels);
          gcu_allocator_free(&module->alloc, active);
          return GLWASM_ERR_INTERNAL;
        }
        label_index = active[depth - 1u - branch_depth];
        grown[translated->branch_table_count + j] =
            label_destination(&labels[label_index]);
      }
      translated->branch_table_count += count;
      target->branch_target = grown[translated->branch_table_count - 1u];
      target->alternate_target = translated->branch_table_count - count;
    } else if (is_branch(source->op)) {
      if (source->a >= depth) {
        translated_free(module, translated);
        gcu_allocator_free(&module->alloc, labels);
        gcu_allocator_free(&module->alloc, active);
        return GLWASM_ERR_INTERNAL;
      }
      label_index = active[depth - 1u - source->a];
      target->branch_target = label_destination(&labels[label_index]);
    }
  }
  gcu_allocator_free(&module->alloc, labels);
  gcu_allocator_free(&module->alloc, active);
  function->translated = translated;
  return GLWASM_OK;
}

GLWASM_Result glwasm_translate_module(GLWASM_Module * module) {
  uint32_t i;

  if (!module) {
    return GLWASM_ERR_INVALID;
  }
  for (i = module->nimported_funcs; i < module->nfuncs; ++i) {
    GLWASM_Result result = translate_func(module, &module->funcs[i]);
    if (result != GLWASM_OK) {
      return result;
    }
  }
  return GLWASM_OK;
}
