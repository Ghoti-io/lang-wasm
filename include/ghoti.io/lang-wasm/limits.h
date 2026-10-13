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
 * @file limits.h
 * @stability free
 *
 * Caps for every unbounded count a decode meets. A NULL limits pointer means
 * ::glwasm_limits_default. A field of zero is a real cap of zero, not a
 * request for the default.
 *
 * The numbered starting values are the untrusted row of host-policy.md:
 * module 4 MiB, 10,000 functions, 1,000 locals, memory maximum 64 MiB.
 * Every other field that page names has a default recorded in
 * documentation/design.md. Named presets are a later story; this struct is
 * the only way to set a cap.
 */

#ifndef GHOTI_IO_GLWASM_LIMITS_H
#define GHOTI_IO_GLWASM_LIMITS_H

#include <ghoti.io/lang-wasm/core.h>
#include <ghoti.io/lang-wasm/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Caps checked while decoding, before the allocation they bound.
 *
 * A failure names the field in ::GLWASM_Failure::limit. `locals` counts
 * parameters and declared locals together, and also bounds the number of
 * local groups. `functions`, `globals`, `tables`, `memories` and `tags`
 * count imports and definitions together. A missing memory or table maximum
 * is the address type's own maximum, so an unbounded memory fails
 * `memory_maximum` under the default.
 */
typedef struct GLWASM_Limits {
  uint64_t module_bytes;       ///< Bytes of the binary.
  uint32_t types;              ///< Type-section entries.
  uint32_t imports;            ///< Import-section entries.
  uint32_t exports;            ///< Export-section entries.
  uint32_t functions;          ///< Imported and defined functions.
  uint32_t globals;            ///< Imported and defined globals.
  uint32_t tables;             ///< Imported and defined tables.
  uint32_t memories;           ///< Imported and defined memories.
  uint32_t tags;               ///< Imported and defined tags.
  uint32_t data_segments;      ///< Data segments.
  uint32_t element_segments;   ///< Element segments.
  uint32_t locals;             ///< Locals per function, parameters included.
  uint32_t instructions;       ///< Instructions per function, `end` included.
  uint32_t nesting;            ///< Control frames, the function frame included.
  uint32_t br_table;           ///< `br_table` targets, the default included.
  uint32_t name_length;        ///< Bytes of one import, export or custom name.
  uint64_t table_size;         ///< Elements, minimum or effective maximum.
  uint64_t memory_maximum;     ///< Bytes of a memory's effective maximum.
  uint64_t memory_initial;     ///< Bytes of a memory's declared minimum.
} GLWASM_Limits;

/**
 * @brief Write the untrusted starting caps into @p limits.
 *
 * @param limits Caller-owned storage. NULL is ::GLWASM_ERR_INVALID and
 *   writes nothing.
 * @return ::GLWASM_OK, or ::GLWASM_ERR_INVALID.
 */
GLWASM_API GLWASM_Result glwasm_limits_default(GLWASM_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif /* GHOTI_IO_GLWASM_LIMITS_H */
