/* SPDX-License-Identifier: LGPL-3.0-only */
#ifndef GHOTI_IO_GLWASM_WAT_LOWER_INTERNAL_H
#define GHOTI_IO_GLWASM_WAT_LOWER_INTERNAL_H

#include <ghoti.io/lang-wasm/parse.h>
#include <ghoti.io/lang-wasm/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Benchmark/test seam: lower and validate WAT, returning the exact canonical
 * Wasm binary bytes consumed by the decoder. The caller frees *out_binary
 * with free(). */
GLWASM_Result glwasm_wat_module_lower_binary(
    const uint8_t *source, size_t source_length,
    const GLWASM_Limits *limits, const GLWASM_Allocator *allocator,
    GLWASM_ParseError *error, uint8_t **out_binary,
    size_t *out_binary_length);

#ifdef __cplusplus
}
#endif
#endif
