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
 * The same decode, with the first byte choosing which cap to tighten, so a
 * corpus explores the limit paths as well as the validator.
 */

#include <ghoti.io/lang-wasm/lang-wasm.h>

#include <stddef.h>
#include <stdint.h>

int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  GLWASM_Limits limits;
  GLWASM_Module * module = NULL;
  uint8_t which;

  if (size == 0) {
    glwasm_module_decode(data, 0, NULL, NULL, NULL, &module);
    glwasm_module_destroy(module);
    return 0;
  }
  glwasm_limits_default(&limits);
  which = data[0];
  switch (which % 19u) {
    case 0:
      limits.module_bytes = which;
      break;
    case 1:
      limits.types = which;
      break;
    case 2:
      limits.imports = which;
      break;
    case 3:
      limits.exports = which;
      break;
    case 4:
      limits.functions = which;
      break;
    case 5:
      limits.globals = which;
      break;
    case 6:
      limits.tables = which;
      break;
    case 7:
      limits.memories = which;
      break;
    case 8:
      limits.tags = which;
      break;
    case 9:
      limits.data_segments = which;
      break;
    case 10:
      limits.element_segments = which;
      break;
    case 11:
      limits.locals = which;
      break;
    case 12:
      limits.instructions = which;
      break;
    case 13:
      limits.nesting = which;
      break;
    case 14:
      limits.br_table = which;
      break;
    case 15:
      limits.name_length = which;
      break;
    case 16:
      limits.table_size = which;
      break;
    case 17:
      limits.memory_maximum = which;
      break;
    default:
      limits.memory_initial = which;
      break;
  }
  glwasm_module_decode(data + 1, size - 1, &limits, NULL, NULL, &module);
  glwasm_module_destroy(module);
  return 0;
}
