/* SPDX-License-Identifier: LGPL-3.0-only */
/**
 * @file policy.h
 * @stability free
 * Host policy presets and feature gates.
 */
#ifndef GHOTI_IO_GLWASM_POLICY_H
#define GHOTI_IO_GLWASM_POLICY_H
#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/macros.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum GLWASM_PolicyPreset {
  GLWASM_POLICY_UNTRUSTED = 0,
  GLWASM_POLICY_STANDARD,
  GLWASM_POLICY_TRUSTED
} GLWASM_PolicyPreset;
enum {
  GLWASM_FEATURE_TAIL_CALLS = UINT64_C(1) << 0,
  GLWASM_FEATURE_EXCEPTIONS = UINT64_C(1) << 1,
  GLWASM_FEATURE_MEMORY64 = UINT64_C(1) << 2,
  GLWASM_FEATURE_TABLE64 = UINT64_C(1) << 3,
  GLWASM_FEATURE_MULTI_MEMORY = UINT64_C(1) << 4,
  GLWASM_FEATURE_BULK_MEMORY = UINT64_C(1) << 5,
  GLWASM_FEATURE_REFERENCE_TYPES = UINT64_C(1) << 6,
  GLWASM_FEATURE_EXTENDED_CONST = UINT64_C(1) << 7
};
typedef struct GLWASM_Policy {
  GLWASM_Limits limits;
  uint64_t features;
  uint64_t default_fuel;
  uint64_t default_guest_depth;
  uint64_t default_native_depth;
  uint64_t default_memory_bytes;
} GLWASM_Policy;
GLWASM_API GLWASM_Result glwasm_policy_preset(GLWASM_PolicyPreset preset,
    GLWASM_Policy *out_policy);
GLWASM_API GLWASM_Result glwasm_module_decode_with_policy(
    const uint8_t *bytes, size_t length, const GLWASM_Policy *policy,
    const GLWASM_Allocator *allocator, GLWASM_Failure *failure,
    GLWASM_Module **out_module);
#ifdef __cplusplus
}
#endif
#endif
