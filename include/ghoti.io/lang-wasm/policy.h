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
#include <ghoti.io/runtime-jit/features.h>
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
/** Immutable decode and runtime policy copied into a validated module.
 * Start with ::glwasm_policy_preset, then adjust fields before decoding. */
typedef struct GLWASM_Policy {
  GLWASM_Limits limits; /**< Structural and resource limits for decoding. */
  uint64_t features; /**< Enabled GLWASM_FEATURE_* proposal bit mask. */
  uint64_t default_fuel; /**< Initial instruction fuel for each instance. */
  uint64_t default_guest_depth; /**< Maximum guest call depth. */
  uint64_t default_native_depth; /**< Maximum nested native call depth. */
  uint64_t default_memory_bytes; /**< Default runtime-core memory budget. */
  /** Privileged Tang boundary imports require both this capability and an
   * exact versioned host binding. Presets leave it disabled. */
  uint8_t allow_tang_v1_imports;
  /** Maximum aggregate executable mapping bytes cached by one decoded module.
   * This is copied into the module before decode and is shared by all of its
   * instances. Zero disables compilation. */
  uint64_t max_jit_code_bytes;
  /** Optional native CPU features excluded from this decoded module's JIT.
   * Presets disable every optional feature for portable baseline code.
   * Set to GRJIT_FEATURES_NONE to use features detected on this host.
   * Unknown bits are invalid. The mask and compiled cache are immutable for
   * the module's lifetime; it cannot enable a feature absent from the CPU. */
  GRJIT_Features jit_disabled_cpu_features;
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
