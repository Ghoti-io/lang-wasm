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
 * The untrusted starting caps. The four numbered values are host-policy.md;
 * the rest are the defaults documentation/design.md records.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/lang-wasm/limits.h>

GLWASM_Result glwasm_limits_default(GLWASM_Limits * limits) {
  if (!limits) {
    return GLWASM_ERR_INVALID;
  }
  limits->module_bytes = 4ull * 1024ull * 1024ull;
  limits->types = 10000u;
  limits->imports = 1000u;
  limits->exports = 1000u;
  limits->functions = 10000u;
  limits->globals = 1000u;
  limits->tables = 100u;
  limits->memories = 100u;
  limits->tags = 1000u;
  limits->data_segments = 10000u;
  limits->element_segments = 10000u;
  limits->locals = 1000u;
  limits->instructions = 100000u;
  limits->nesting = 128u;
  limits->br_table = 10000u;
  limits->name_length = 1024u;
  limits->table_size = 1000000ull;
  limits->memory_maximum = 64ull * 1024ull * 1024ull;
  limits->memory_initial = 64ull * 1024ull * 1024ull;
  return GLWASM_OK;
}
