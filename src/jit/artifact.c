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
 * @file artifact.c
 *
 * Pins module code-page storage through final JIT code destruction.
 */

#include <ghoti.io/lang-wasm/macros.h>

#include "artifact_internal.h"

#include <ghoti.io/runtime-core/allocator.h>

typedef struct GLWASM_JitArtifact {
  GRJIT_Code *jit_code;
  GRCORE_CodePages *pages;
  const GRCORE_Allocator *allocator;
} GLWASM_JitArtifact;

static void jit_artifact_release(void *payload) {
  GLWASM_JitArtifact *artifact = payload;
  if (artifact == NULL) return;
  grjit_code_destroy(artifact->jit_code);
  GRCORE_CodePages *pages = artifact->pages;
  const GRCORE_Allocator *allocator = artifact->allocator;
  allocator->free_fn(allocator->ctx, artifact);
  grcore_code_pages_release(pages);
}

GLWASM_Result glwasm_jit_artifact_create(GRJIT_Code *jit_code,
    GRCORE_CodePages *pages, GRCORE_Code **out_code) {
  if (jit_code == NULL || pages == NULL || out_code == NULL)
    return GLWASM_ERR_INVALID;
  *out_code = NULL;
  const GRCORE_Allocator *allocator = grcore_allocator_default();
  if (allocator == NULL || allocator->malloc_fn == NULL ||
      allocator->free_fn == NULL) return GLWASM_ERR_INTERNAL;
  GLWASM_JitArtifact *artifact = allocator->malloc_fn(allocator->ctx,
      sizeof(*artifact));
  if (artifact == NULL) return GLWASM_ERR_OOM;
  artifact->jit_code = jit_code;
  artifact->pages = grcore_code_pages_retain(pages);
  artifact->allocator = allocator;
  GRCORE_Result result = grcore_code_create(NULL, artifact,
      jit_artifact_release, out_code);
  if (result == GRCORE_OK) return GLWASM_OK;
  grcore_code_pages_release(artifact->pages);
  allocator->free_fn(allocator->ctx, artifact);
  if (result == GRCORE_ERR_OOM) return GLWASM_ERR_OOM;
  if (result == GRCORE_ERR_INVALID) return GLWASM_ERR_INVALID;
  return GLWASM_ERR_INTERNAL;
}

GRJIT_Code *glwasm_jit_artifact_code(GRCORE_Code *handle) {
  GLWASM_JitArtifact *artifact = handle == NULL
      ? NULL : grcore_code_payload(handle);
  return artifact == NULL ? NULL : artifact->jit_code;
}
