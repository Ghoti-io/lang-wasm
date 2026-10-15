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
 * Small helpers shared by the regression tests: hex text to bytes, and one
 * decode call that reports the result code.
 */

#ifndef GLWASM_TEST_HEXDECODE_H
#define GLWASM_TEST_HEXDECODE_H

#include <ghoti.io/lang-wasm/lang-wasm.h>

#include <cstdint>
#include <cstddef>
#include <vector>

struct Case {
  const char * name;
  const char * hex;
  GLWASM_Result expect;
};

inline std::vector<uint8_t> hex_bytes(const char * text) {
  std::vector<uint8_t> out;
  int hi = -1;

  for (const char * p = text; *p; p++) {
    int v;

    if (*p >= '0' && *p <= '9') {
      v = *p - '0';
    } else if (*p >= 'a' && *p <= 'f') {
      v = *p - 'a' + 10;
    } else if (*p >= 'A' && *p <= 'F') {
      v = *p - 'A' + 10;
    } else {
      continue;
    }
    if (hi < 0) {
      hi = v;
    } else {
      out.push_back(static_cast<uint8_t>((hi << 4) | v));
      hi = -1;
    }
  }
  return out;
}

/* Decodes with every cap raised to its maximum, so only the grammar and the
 * validation rules decide the result. */
inline GLWASM_Result decode_hex(const char * text) {
  std::vector<uint8_t> bytes = hex_bytes(text);
  GLWASM_Limits limits;
  GLWASM_Module * module = NULL;
  GLWASM_Result result;

  glwasm_limits_default(&limits);
  limits.module_bytes = UINT64_MAX;
  limits.table_size = UINT64_MAX;
  limits.memory_maximum = UINT64_MAX;
  limits.memory_initial = UINT64_MAX;
  result = glwasm_module_decode(bytes.data(), bytes.size(), &limits, NULL,
      NULL, &module);
  if (result == GLWASM_OK) {
    glwasm_module_destroy(module);
  } else if (module != NULL) {
    return GLWASM_ERR_INTERNAL;
  }
  return result;
}

#endif
