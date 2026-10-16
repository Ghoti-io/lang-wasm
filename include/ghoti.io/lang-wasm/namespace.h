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
#define GLWASM_Text GHOTIIO_LANG_WASM(GLWASM_Text)
#define GLWASM_TextKind GHOTIIO_LANG_WASM(GLWASM_TextKind)
#define GLWASM_SourceLoc GHOTIIO_LANG_WASM(GLWASM_SourceLoc)
#define GLWASM_ParseError GHOTIIO_LANG_WASM(GLWASM_ParseError)
#define GLWASM_Script GHOTIIO_LANG_WASM(GLWASM_Script)
#define GLWASM_CommandKind GHOTIIO_LANG_WASM(GLWASM_CommandKind)
#define GLWASM_Command GHOTIIO_LANG_WASM(GLWASM_Command)
#define GLWASM_Runtime GHOTIIO_LANG_WASM(GLWASM_Runtime)
#define GLWASM_Snapshot GHOTIIO_LANG_WASM(GLWASM_Snapshot)
#define GLWASM_HostValueType GHOTIIO_LANG_WASM(GLWASM_HostValueType)
#define GLWASM_HostCall GHOTIIO_LANG_WASM(GLWASM_HostCall)
#define GLWASM_HostFunction GHOTIIO_LANG_WASM(GLWASM_HostFunction)
#define GLWASM_HostResumeFunction GHOTIIO_LANG_WASM(GLWASM_HostResumeFunction)
#define GLWASM_HostImport GHOTIIO_LANG_WASM(GLWASM_HostImport)
#define GLWASM_HostMemoryView GHOTIIO_LANG_WASM(GLWASM_HostMemoryView)
#define GLWASM_HostMemoryProvider GHOTIIO_LANG_WASM(GLWASM_HostMemoryProvider)
#define GLWASM_HostTableView GHOTIIO_LANG_WASM(GLWASM_HostTableView)
#define GLWASM_HostTableProvider GHOTIIO_LANG_WASM(GLWASM_HostTableProvider)
#define GLWASM_HostGlobal GHOTIIO_LANG_WASM(GLWASM_HostGlobal)
#define GLWASM_HostTag GHOTIIO_LANG_WASM(GLWASM_HostTag)
#define GLWASM_LinkFailure GHOTIIO_LANG_WASM(GLWASM_LinkFailure)
#define GLWASM_LinkError GHOTIIO_LANG_WASM(GLWASM_LinkError)
#define GLWASM_Policy GHOTIIO_LANG_WASM(GLWASM_Policy)
#define GLWASM_PolicyPreset GHOTIIO_LANG_WASM(GLWASM_PolicyPreset)

#define glwasm_allocator_default GHOTIIO_LANG_WASM(glwasm_allocator_default)
#define glwasm_limits_default GHOTIIO_LANG_WASM(glwasm_limits_default)
#define glwasm_module_decode GHOTIIO_LANG_WASM(glwasm_module_decode)
#define glwasm_module_destroy GHOTIIO_LANG_WASM(glwasm_module_destroy)
#define glwasm_module_dump GHOTIIO_LANG_WASM(glwasm_module_dump)
#define glwasm_policy_preset GHOTIIO_LANG_WASM(glwasm_policy_preset)
#define glwasm_module_decode_with_policy GHOTIIO_LANG_WASM(glwasm_module_decode_with_policy)
#define glwasm_runtime_create_with_imports GHOTIIO_LANG_WASM(glwasm_runtime_create_with_imports)
#define glwasm_runtime_destroy GHOTIIO_LANG_WASM(glwasm_runtime_destroy)
#define glwasm_runtime_invoke GHOTIIO_LANG_WASM(glwasm_runtime_invoke)
#define glwasm_runtime_resume GHOTIIO_LANG_WASM(glwasm_runtime_resume)
#define glwasm_runtime_pause_position GHOTIIO_LANG_WASM(glwasm_runtime_pause_position)
#define glwasm_runtime_register_boundary_engine GHOTIIO_LANG_WASM(glwasm_runtime_register_boundary_engine)
#define glwasm_runtime_bind_boundary_context GHOTIIO_LANG_WASM(glwasm_runtime_bind_boundary_context)
#define glwasm_runtime_invoke_nested GHOTIIO_LANG_WASM(glwasm_runtime_invoke_nested)
#define glwasm_runtime_resume_nested GHOTIIO_LANG_WASM(glwasm_runtime_resume_nested)
#define glwasm_runtime_snapshot GHOTIIO_LANG_WASM(glwasm_runtime_snapshot)
#define glwasm_runtime_restore GHOTIIO_LANG_WASM(glwasm_runtime_restore)
#define glwasm_snapshot_release GHOTIIO_LANG_WASM(glwasm_snapshot_release)
#define glwasm_runtime_invoke_export GHOTIIO_LANG_WASM(glwasm_runtime_invoke_export)
#define glwasm_text_parse GHOTIIO_LANG_WASM(glwasm_text_parse)
#define glwasm_wat_module_parse GHOTIIO_LANG_WASM(glwasm_wat_module_parse)
#define glwasm_wat_module_parse_verbatim GHOTIIO_LANG_WASM(glwasm_wat_module_parse_verbatim)
#define glwasm_text_destroy GHOTIIO_LANG_WASM(glwasm_text_destroy)
#define glwasm_text_kind GHOTIIO_LANG_WASM(glwasm_text_kind)
#define glwasm_text_location GHOTIIO_LANG_WASM(glwasm_text_location)
#define glwasm_text_size GHOTIIO_LANG_WASM(glwasm_text_size)
#define glwasm_text_at GHOTIIO_LANG_WASM(glwasm_text_at)
#define glwasm_text_bytes GHOTIIO_LANG_WASM(glwasm_text_bytes)
#define glwasm_script_parse GHOTIIO_LANG_WASM(glwasm_script_parse)
#define glwasm_script_destroy GHOTIIO_LANG_WASM(glwasm_script_destroy)
#define glwasm_script_size GHOTIIO_LANG_WASM(glwasm_script_size)
#define glwasm_script_at GHOTIIO_LANG_WASM(glwasm_script_at)
#define glwasm_result_string GHOTIIO_LANG_WASM(glwasm_result_string)
#define glwasm_version_number GHOTIIO_LANG_WASM(glwasm_version_number)
#define glwasm_version_string GHOTIIO_LANG_WASM(glwasm_version_string)

/// @endcond

#endif /* GHOTI_IO_GLWASM_NAMESPACE_H */
