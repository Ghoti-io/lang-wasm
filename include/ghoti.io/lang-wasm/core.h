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
 * @file core.h
 * @stability free
 *
 * Result codes and the version this build reports.
 */

#ifndef GHOTI_IO_GLWASM_CORE_H
#define GHOTI_IO_GLWASM_CORE_H

#include <ghoti.io/lang-wasm/allocator.h>
#include <ghoti.io/lang-wasm/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result of an operation.
 *
 * Zero is success. ::GLWASM_RESULT_COUNT closes the enum so a test can check
 * the string table is complete.
 *
 * The nine shared constants keep the numbers they have in every other
 * library. ::GLWASM_ERR_VALIDATE is appended before the count: a module that
 * is Wasm and fails a rule is not a bad caller argument and not corrupt
 * bytes. See CONVENTIONS.md section 13.
 */
typedef enum {
  GLWASM_OK = 0,           ///< The operation succeeded.
  GLWASM_ERR_IO,           ///< A read, write, or seek failed.
  GLWASM_ERR_FORMAT,       ///< Well-formed bytes, but not a format handled
                           ///< here.
  GLWASM_ERR_UNSUPPORTED,  ///< The bytes are Wasm, and they use a feature
                           ///< this library does not implement.
  GLWASM_ERR_LIMIT,        ///< A ::GLWASM_Limits field was exceeded.
  GLWASM_ERR_CORRUPT,      ///< The bytes are not a Wasm binary.
  GLWASM_ERR_OOM,          ///< The allocator returned NULL.
  GLWASM_ERR_INVALID,      ///< A caller-supplied argument is wrong.
  GLWASM_ERR_INTERNAL,     ///< The library's own invariant failed.
  GLWASM_ERR_VALIDATE,     ///< The bytes are Wasm, and they break a rule.
  GLWASM_RESULT_COUNT
} GLWASM_Result;

/**
 * @brief Static description of a result code.
 *
 * The string is never NULL, never allocated, and never contains caller data.
 * A limit's field name is not this string; it is ::GLWASM_Failure::limit.
 *
 * @param result The result code, including values outside the enum.
 * @return A static string.
 */
GLWASM_API const char * glwasm_result_string(GLWASM_Result result);

/**
 * @brief This build's version, as the string the Makefile generated.
 *
 * @return A static string, never NULL.
 */
GLWASM_API const char * glwasm_version_string(void);

/**
 * @brief This build's version, packed as ::GLWASM_MAKE_VERSION packs it.
 *
 * @return `(major << 16) | (minor << 8) | patch`.
 */
GLWASM_API unsigned glwasm_version_number(void);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLWASM_CORE_H */
