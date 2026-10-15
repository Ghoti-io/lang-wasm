/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
/**
 * @file parse.h
 * @stability free
 * Source-located parser for the WebAssembly text syntax.
 *
 * The returned tree owns all decoded string bytes and nodes. It is a syntax
 * representation; semantic validation remains a separate operation.
 */
#ifndef GHOTI_IO_GLWASM_PARSE_H
#define GHOTI_IO_GLWASM_PARSE_H

#include <ghoti.io/lang-wasm/allocator.h>
#include <ghoti.io/lang-wasm/core.h>
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLWASM_Text GLWASM_Text;
typedef enum {
  GLWASM_TEXT_LIST = 0,
  GLWASM_TEXT_ATOM,
  GLWASM_TEXT_STRING
} GLWASM_TextKind;

typedef struct GLWASM_SourceLoc {
  size_t offset;
  uint32_t line;
  uint32_t column;
} GLWASM_SourceLoc;

typedef struct GLWASM_ParseError {
  GLWASM_Result result;
  GLWASM_SourceLoc location;
  const char * message; /* Static description, never allocated. */
} GLWASM_ParseError;

/**
 * @brief Parse the parenthesized text syntax into an owned syntax tree.
 *
 * Nested comments and byte-string escapes are decoded during parsing. Source
 * bytes must be valid UTF-8; byte escapes can still represent arbitrary string
 * contents. Nesting beyond 256 levels is rejected to bound parser stack use.
 * Nodes retain their byte offset and one-based line and column. This syntax tree is
 * not a validated ::GLWASM_Module; binary decoding and validation remain
 * separate operations.
 *
 * @param source Input bytes. NULL is allowed only when @p length is zero.
 * @param length Number of input bytes.
 * @param allocator Allocator, or NULL for the default.
 * @param error Optional error details, written on parse failure.
 * @param out_text Receives the root list on success and is untouched on
 *   failure. The caller owns it and releases it with
 *   ::glwasm_text_destroy.
 * @return ::GLWASM_OK, ::GLWASM_ERR_INVALID, ::GLWASM_ERR_CORRUPT, or
 *   ::GLWASM_ERR_OOM.
 */
GLWASM_API GLWASM_Result glwasm_text_parse(const uint8_t * source,
                                           size_t length,
                                           const GLWASM_Allocator * allocator,
                                           GLWASM_ParseError * error,
                                           GLWASM_Text ** out_text);
/**
 * @brief Parse and lower a WAT module through the binary decoder/validator.
 *
 * The returned module is the same validated representation returned by
 * ::glwasm_module_decode. Source locations in @p error identify syntax or
 * lowering failures. The module is written only on success and is released
 * with ::glwasm_module_destroy. This entry point currently lowers the core
 * function/type/export subset documented by the parser; unsupported fields
 * fail explicitly rather than yielding a syntax tree as a module.
 */
GLWASM_API GLWASM_Result glwasm_wat_module_parse(
    const uint8_t * source, size_t length, const GLWASM_Limits * limits,
    const GLWASM_Allocator * allocator, GLWASM_ParseError * error,
    GLWASM_Module ** out_module);
/** @brief Lower verbatim WAT without removing annotation nodes. */
GLWASM_API GLWASM_Result glwasm_wat_module_parse_verbatim(
    const uint8_t * source, size_t length, const GLWASM_Limits * limits,
    const GLWASM_Allocator * allocator, GLWASM_ParseError * error,
    GLWASM_Module ** out_module);
/** @brief Release a syntax tree returned by ::glwasm_text_parse. */
GLWASM_API void glwasm_text_destroy(GLWASM_Text * text);
/** @brief Return the node kind; NULL is treated as an empty list. */
GLWASM_API GLWASM_TextKind glwasm_text_kind(const GLWASM_Text * node);
/** @brief Return the node location; NULL returns a zero location. */
GLWASM_API GLWASM_SourceLoc glwasm_text_location(const GLWASM_Text * node);
/** @brief Return the number of children of a list node. */
GLWASM_API size_t glwasm_text_size(const GLWASM_Text * node);
/** @brief Return a borrowed child, or NULL when the index is out of range. */
GLWASM_API const GLWASM_Text * glwasm_text_at(const GLWASM_Text * node,
                                              size_t index);
/**
 * @brief Return an atom or decoded string's borrowed bytes.
 * @param node The node to inspect.
 * @param length Receives the byte count when non-NULL.
 * @return A borrowed byte pointer, or NULL for a list or NULL node.
 */
GLWASM_API const uint8_t * glwasm_text_bytes(const GLWASM_Text * node,
                                             size_t * length);

#ifdef __cplusplus
}
#endif
#endif
