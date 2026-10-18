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
 * @file decode.h
 * @stability free
 *
 * Decode a WebAssembly 3.0 binary, then validate it. The module handed back
 * is the resolved form; the caller frees it with one destroy. Nothing is
 * instantiated and no import is resolved.
 */

#ifndef GHOTI_IO_GLWASM_DECODE_H
#define GHOTI_IO_GLWASM_DECODE_H

#include <ghoti.io/lang-wasm/core.h>
#include <ghoti.io/lang-wasm/limits.h>
#include <ghoti.io/lang-wasm/macros.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A decoded, validated module. Opaque. The caller owns it.
 */
typedef struct GLWASM_Module GLWASM_Module;

/**
 * @brief Why a decode refused, when the caller wants the reason.
 *
 * Not an output of a successful module: @c result is written on every call
 * when this pointer is non-NULL, including failure, and @c limit is a static
 * field name when @c result is ::GLWASM_ERR_LIMIT and NULL otherwise. The
 * module pointer is a separate parameter and is written only on success.
 */
typedef struct GLWASM_Failure {
  GLWASM_Result result; ///< The same code the function returned.
  const char * limit;   ///< The ::GLWASM_Limits field, or NULL.
} GLWASM_Failure;

/**
 * @brief Decode @p bytes and validate the module.
 *
 * A NULL @p limits uses ::glwasm_limits_default. A NULL @p allocator uses
 * the default. @p out_module is written only on success; on failure it is
 * left untouched and nothing is left allocated. @p failure may be NULL.
 *
 * ::GLWASM_ERR_CORRUPT is bytes that are not Wasm. ::GLWASM_ERR_UNSUPPORTED
 * is a feature the grammar accepts and this library does not implement
 * (a typed function reference, a `v128`, the legacy `try` / `catch` /
 * `rethrow` encodings). ::GLWASM_ERR_VALIDATE is a well-formed module that
 * breaks a rule. ::GLWASM_ERR_LIMIT is a cap, reported before the allocation
 * that would have satisfied it, and @c failure->limit names the field.
 * ::GLWASM_ERR_INVALID is a bad caller argument (a NULL module pointer, or
 * a NULL buffer with a non-zero length).
 *
 * @param bytes The binary. NULL is allowed only when @p length is 0, and
 *   that input is ::GLWASM_ERR_CORRUPT.
 * @param length Byte length of @p bytes.
 * @param limits Caps, or NULL for the default.
 * @param allocator Allocator, or NULL for the default.
 * @param failure Optional status written on every return.
 * @param out_module Receives the module on success. Required.
 * @return A ::GLWASM_Result. On failure the caller frees nothing.
 */
GLWASM_API GLWASM_Result glwasm_module_decode(
    const uint8_t * bytes,
    size_t length,
    const GLWASM_Limits * limits,
    const GLWASM_Allocator * allocator,
    GLWASM_Failure * failure,
    GLWASM_Module * * out_module);

/**
 * @brief Release the caller's reference to a module.
 *
 * A live runtime retains the module, so it remains usable until its final
 * runtime is destroyed. The module and its compiled-code cache are freed
 * after the caller and every runtime have released their references.
 * Callers must synchronize this release with operations that are beginning
 * to use the module pointer.
 *
 * @param module The module, or NULL.
 */
GLWASM_API void glwasm_module_destroy(GLWASM_Module * module);

/**
 * @brief Write a one-page description of @p module to @p out.
 *
 * @param module The module. NULL writes nothing and returns
 *   ::GLWASM_ERR_INVALID.
 * @param out The stream. NULL is ::GLWASM_ERR_INVALID.
 * @return ::GLWASM_OK, or ::GLWASM_ERR_INVALID.
 */
GLWASM_API GLWASM_Result glwasm_module_dump(
    const GLWASM_Module * module, FILE * out);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLWASM_DECODE_H */
