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
 * Result strings and the version this build reports.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include <ghoti.io/lang-wasm/core.h>

const char * glwasm_result_string(GLWASM_Result result) {
  switch (result) {
    case GLWASM_OK:
      return "No error";
    case GLWASM_ERR_IO:
      return "I/O error";
    case GLWASM_ERR_FORMAT:
      return "Not a format handled here";
    case GLWASM_ERR_UNSUPPORTED:
      return "Unsupported feature";
    case GLWASM_ERR_LIMIT:
      return "Limit exceeded";
    case GLWASM_ERR_CORRUPT:
      return "Corrupt data";
    case GLWASM_ERR_OOM:
      return "Out of memory";
    case GLWASM_ERR_INVALID:
      return "Invalid argument";
    case GLWASM_ERR_INTERNAL:
      return "Internal error";
    case GLWASM_ERR_VALIDATE:
      return "Validation failed";
    case GLWASM_ERR_GUEST:
      return "Guest execution trapped";
    case GLWASM_ERR_PAUSED:
      return "Guest paused";
    case GLWASM_RESULT_COUNT:
    default:
      return "Unknown error";
  }
}

const char * glwasm_version_string(void) {
  return GLWASM_VERSION_STRING;
}

unsigned glwasm_version_number(void) {
  return GLWASM_VERSION_NUMBER;
}
