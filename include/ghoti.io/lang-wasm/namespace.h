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
 * @file namespace.h
 * @stability free
 *
 * Maps every public name of this library into its version namespace.
 *
 * One list, included from macros.h, so a type rename is in effect before any
 * struct tag that uses the name. See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GLWASM_NAMESPACE_H
#define GHOTI_IO_GLWASM_NAMESPACE_H

#include <ghoti.io/lang-wasm/libver.h>

/// @cond HIDDEN_SYMBOLS

#define GLWASM_Allocator GHOTIIO_LANG_WASM(GLWASM_Allocator)
#define GLWASM_Failure GHOTIIO_LANG_WASM(GLWASM_Failure)
#define GLWASM_Limits GHOTIIO_LANG_WASM(GLWASM_Limits)
#define GLWASM_Module GHOTIIO_LANG_WASM(GLWASM_Module)
#define GLWASM_Result GHOTIIO_LANG_WASM(GLWASM_Result)

#define glwasm_allocator_default GHOTIIO_LANG_WASM(glwasm_allocator_default)
#define glwasm_limits_default GHOTIIO_LANG_WASM(glwasm_limits_default)
#define glwasm_module_decode GHOTIIO_LANG_WASM(glwasm_module_decode)
#define glwasm_module_destroy GHOTIIO_LANG_WASM(glwasm_module_destroy)
#define glwasm_module_dump GHOTIIO_LANG_WASM(glwasm_module_dump)
#define glwasm_result_string GHOTIIO_LANG_WASM(glwasm_result_string)
#define glwasm_version_number GHOTIIO_LANG_WASM(glwasm_version_number)
#define glwasm_version_string GHOTIIO_LANG_WASM(glwasm_version_string)

/// @endcond

#endif /* GHOTI_IO_GLWASM_NAMESPACE_H */
