/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
/**
 * @file script.h
 * @stability free
 * Parse WAST command streams into typed, source-located commands.
 *
 * Each command retains its syntax tree for a later executor. This story
 * recognizes script syntax; it does not execute assertions.
 */
#ifndef GHOTI_IO_GLWASM_SCRIPT_H
#define GHOTI_IO_GLWASM_SCRIPT_H

#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/parse.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLWASM_Script GLWASM_Script;

typedef enum GLWASM_CommandKind {
  GLWASM_COMMAND_MODULE_TEXT = 0,
  GLWASM_COMMAND_MODULE_BINARY,
  GLWASM_COMMAND_MODULE_QUOTE,
  GLWASM_COMMAND_MODULE_DEFINITION,
  GLWASM_COMMAND_MODULE_INSTANCE,
  GLWASM_COMMAND_MODULE_FIELD,
  GLWASM_COMMAND_REGISTER,
  GLWASM_COMMAND_INVOKE,
  GLWASM_COMMAND_GET,
  GLWASM_COMMAND_ASSERT_RETURN,
  GLWASM_COMMAND_ASSERT_RETURN_CANONICAL_NAN,
  GLWASM_COMMAND_ASSERT_RETURN_ARITHMETIC_NAN,
  GLWASM_COMMAND_ASSERT_TRAP,
  GLWASM_COMMAND_ASSERT_EXHAUSTION,
  GLWASM_COMMAND_ASSERT_EXCEPTION,
  GLWASM_COMMAND_ASSERT_MALFORMED,
  GLWASM_COMMAND_ASSERT_INVALID,
  GLWASM_COMMAND_ASSERT_UNLINKABLE,
  GLWASM_COMMAND_ASSERT_UNINSTANTIABLE,
  GLWASM_COMMAND_ASSERT_SUSPENSION,
  GLWASM_COMMAND_ASSERT_MALFORMED_CUSTOM,
  GLWASM_COMMAND_ASSERT_INVALID_CUSTOM
} GLWASM_CommandKind;

typedef struct GLWASM_Command {
  GLWASM_CommandKind kind;
  GLWASM_SourceLoc location;
  const GLWASM_Text * syntax; /* Borrowed from the owning script. */
} GLWASM_Command;

/**
 * @brief Parse a WAST command stream and classify each top-level command.
 *
 * Command syntax is retained, not executed. This preserves assertions as
 * pending until an execution adapter is supplied.
 *
 * @param source Input bytes. NULL is allowed only when @p length is zero.
 * @param length Number of input bytes.
 * @param allocator Allocator, or NULL for the default.
 * @param error Optional source-located parse failure details.
 * @param out_script Receives the script only on success; untouched on failure.
 *   The caller owns the script.
 * @return ::GLWASM_OK, ::GLWASM_ERR_INVALID, ::GLWASM_ERR_CORRUPT, or
 *   ::GLWASM_ERR_OOM.
 */
GLWASM_API GLWASM_Result glwasm_script_parse(const uint8_t * source,
                                             size_t length,
                                             const GLWASM_Allocator * allocator,
                                             GLWASM_ParseError * error,
                                             GLWASM_Script ** out_script);
/** @brief Release a script and its syntax tree. */
GLWASM_API void glwasm_script_destroy(GLWASM_Script * script);
/** @brief Return the number of commands in the script. */
GLWASM_API size_t glwasm_script_size(const GLWASM_Script * script);
/**
 * @brief Read one command into caller storage.
 *
 * @param script Parsed script.
 * @param index Zero-based command index.
 * @param out_command Receives the command on success; @p syntax is borrowed.
 * @return ::GLWASM_OK or ::GLWASM_ERR_INVALID.
 */
GLWASM_API GLWASM_Result glwasm_script_at(const GLWASM_Script * script,
                                          size_t index,
                                          GLWASM_Command * out_command);

#ifdef __cplusplus
}
#endif
#endif
