/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Lang-wasm.
 *
 * Ghoti.io Lang-wasm is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 */

/**
 * @file artifact_internal.h
 *
 * Runtime-core handle wrapper for a Wasm JIT artifact.
 */

#ifndef GHOTI_IO_GLWASM_SRC_JIT_ARTIFACT_INTERNAL_H
#define GHOTI_IO_GLWASM_SRC_JIT_ARTIFACT_INTERNAL_H

#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/lang-wasm/core.h>
#include <ghoti.io/runtime-core/a/code.h>
#include <ghoti.io/runtime-core/a/code_pages.h>
#include <ghoti.io/runtime-jit/code.h>

#ifdef __cplusplus
extern "C" {
#endif

/* On success transfers `jit_code` into a core code handle. The wrapper keeps
 * `pages` alive until grjit_code_destroy has freed metadata and unmapped code. */
GLWASM_Result glwasm_jit_artifact_create(GRJIT_Code *jit_code,
    GRCORE_CodePages *pages, GRCORE_Code **out_code);
GRJIT_Code *glwasm_jit_artifact_code(GRCORE_Code *handle);

#ifdef __cplusplus
}
#endif

#endif
