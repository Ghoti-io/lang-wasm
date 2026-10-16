/*
 * SPDX-License-Identifier: LGPL-3.0-only
 * Copyright (C) 2026 Corey Pennycuff
 */
/**
 * @file host.h
 * @stability free
 * Host-function imports and named export invocation.
 */
#ifndef GHOTI_IO_GLWASM_HOST_H
#define GHOTI_IO_GLWASM_HOST_H

#include <ghoti.io/lang-wasm/decode.h>
#include <ghoti.io/lang-wasm/macros.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GLWASM_Runtime GLWASM_Runtime;
typedef struct GLWASM_Snapshot GLWASM_Snapshot;

/** The value types passed to a host function, in declaration order. */
typedef enum GLWASM_HostValueType {
  GLWASM_HOST_I32 = 0x7f,
  GLWASM_HOST_I64 = 0x7e,
  GLWASM_HOST_F32 = 0x7d,
  GLWASM_HOST_F64 = 0x7c,
  GLWASM_HOST_FUNCREF = 0x70,
  GLWASM_HOST_EXTERNREF = 0x6f
} GLWASM_HostValueType;

/** One resolved Wasm function call. Numeric values use Wasm bit patterns. */
typedef struct GLWASM_HostCall {
  GLWASM_Runtime *instance;             /**< The instance making the call. */
  GRCORE_Context *context;              /**< Borrowed; valid during callback. */
  GRCORE_EngineId engine;               /**< Wasm engine in this context. */
  uintptr_t instance_token;             /**< Opaque context-unique token. */
  uint8_t has_resume;                   /**< This import has a continuation. */
  const GLWASM_HostValueType *types;    /**< Borrowed parameter types. */
  const uint64_t *arguments;            /**< Borrowed argument bit patterns. */
  uint32_t argument_count;
  GLWASM_HostValueType result_type;     /**< Valid when result_count is one. */
  uint32_t result_count;                /**< Zero or one in this API version. */
  void *user;
} GLWASM_HostCall;

/** Register a test or embedder engine in this runtime's context. The
 * descriptor is borrowed and must outlive the runtime. */
GLWASM_API GLWASM_Result glwasm_runtime_register_boundary_engine(
    GLWASM_Runtime *runtime,const GRCORE_EngineDescriptor *descriptor,
    GRCORE_EngineId *out_engine);
/** Register this Wasm engine in an adopted core context before a running
 * caller enters the mixed-engine boundary. The context must outlive any
 * nested invocation using it. */
GLWASM_API GLWASM_Result glwasm_runtime_bind_boundary_context(
    GLWASM_Runtime *runtime,GRCORE_Context *context,
    GRCORE_EngineId *out_wasm_engine);

/** Return GLWASM_OK and write the one result when requested. Return
 * ::GLWASM_ERR_PAUSED only when the import binding also supplies `resume`.
 * Side effects completed before that return remain complete. */
typedef GLWASM_Result (*GLWASM_HostFunction)(
    const GLWASM_HostCall *call, uint64_t *result);
/** Continue a host callback that previously returned ::GLWASM_ERR_PAUSED.
 * The same call arguments and user pointer are supplied. The host stores
 * continuation state in its user object; the initial callback is never
 * repeated after a pause. This entrypoint may return PAUSED again. */
typedef GLWASM_Result (*GLWASM_HostResumeFunction)(
    const GLWASM_HostCall *call, uint64_t *result);

/** An explicit capability granted to one exact function, memory, table,
 * global, or exception-tag import. */
typedef struct GLWASM_HostImport {
  const char *module;
  const char *field;
  GLWASM_HostFunction function;
  GLWASM_HostResumeFunction resume;
  void *user;
  uint8_t kind; /**< GLWASM_HOST_IMPORT_*; zero preserves function imports. */
  union {
    struct GLWASM_HostMemoryProvider *memory;
    struct GLWASM_HostTableProvider *table;
    struct GLWASM_HostGlobal *global;
    struct GLWASM_HostTag *tag;
  } resource;
} GLWASM_HostImport;

enum {
  GLWASM_HOST_IMPORT_FUNCTION = 0,
  GLWASM_HOST_IMPORT_MEMORY = 1,
  GLWASM_HOST_IMPORT_TABLE = 2,
  GLWASM_HOST_IMPORT_GLOBAL = 3,
  GLWASM_HOST_IMPORT_TAG = 4
};

/**
 * A short-lived memory view. `begin` opens a transaction and pins the backing
 * bytes; `end` closes it. The engine releases every lease before polling or
 * calling `grow`, so the provider may move storage only between leases. A
 * provider must not re-enter the engine from a lease callback.
 */
typedef struct GLWASM_HostMemoryView {
  uint8_t *bytes;
  uint64_t length_bytes;
} GLWASM_HostMemoryView;
typedef GLWASM_Result (*GLWASM_HostMemoryBegin)(void *user,
    GLWASM_HostMemoryView *view);
typedef void (*GLWASM_HostMemoryEnd)(void *user,
    GLWASM_HostMemoryView *view);
typedef GLWASM_Result (*GLWASM_HostMemoryGrow)(void *user, uint64_t delta_pages,
    uint64_t *old_pages);
typedef struct GLWASM_HostMemoryProvider {
  GLWASM_HostMemoryBegin begin;
  GLWASM_HostMemoryEnd end;
  GLWASM_HostMemoryGrow grow;
  void *user;
} GLWASM_HostMemoryProvider;

/**
 * A short-lived table view containing opaque 64-bit reference tokens. A begin
 * transaction roots all tokens in the view until end. For externref tables,
 * validate_token must reject stale or foreign tokens; the provider owns the
 * cross-heap mapping and relocation behavior.
 */
typedef struct GLWASM_HostTableView {
  uint64_t *elements;
  uint64_t length;
} GLWASM_HostTableView;
typedef GLWASM_Result (*GLWASM_HostTableBegin)(void *user,
    GLWASM_HostTableView *view);
typedef void (*GLWASM_HostTableEnd)(void *user,
    GLWASM_HostTableView *view);
typedef GLWASM_Result (*GLWASM_HostTableGrow)(void *user, uint64_t delta,
    uint64_t initial, uint64_t *old_size);
typedef GLWASM_Result (*GLWASM_HostReferenceCheck)(void *user, uint64_t token);
typedef struct GLWASM_HostTableProvider {
  GLWASM_HostTableBegin begin;
  GLWASM_HostTableEnd end;
  GLWASM_HostTableGrow grow;
  GLWASM_HostValueType element_type;
  GLWASM_HostReferenceCheck validate_token;
  void *user;
} GLWASM_HostTableProvider;

typedef struct GLWASM_HostGlobal {
  GLWASM_HostValueType type;
  uint8_t mutable_;
  uint64_t *value;
  GLWASM_HostReferenceCheck validate_token;
  void *user;
} GLWASM_HostGlobal;

/** Type-checked identity for an imported exception tag. */
typedef struct GLWASM_HostTag {
  uint64_t identity;
  const GLWASM_HostValueType *parameters;
  uint32_t parameter_count;
} GLWASM_HostTag;

typedef enum GLWASM_LinkFailure {
  GLWASM_LINK_UNRESOLVED = 0,
  GLWASM_LINK_UNSUPPORTED_KIND,
  GLWASM_LINK_INVALID_BINDING,
  GLWASM_LINK_START_NOT_ALLOWED
} GLWASM_LinkFailure;

/** Import diagnostic. Names are borrowed from the module. */
typedef struct GLWASM_LinkError {
  const char *module;
  const char *field;
  GLWASM_LinkFailure reason;
} GLWASM_LinkError;

/**
 * Link every function import against the explicit allowlist and instantiate.
 * Imports without an exact entry are denied. `tang.*` is treated the same way:
 * the caller must explicitly supply the exact versioned import module. This
 * first host API supports function imports; imported memories, tables, globals
 * and tags use provider entries with matching type and limits.
 *
 * `allow_start` must be true to run a module's start function. The instance
 * borrows `module`; destroy it before destroying the module. Binding strings
 * and callback user data must remain valid through instantiation only; callback
 * and user pointers are copied into the instance. Provider structs, provider
 * user state, imported global slots, and callback user state must outlive the
 * instance.
 */
GLWASM_API GLWASM_Result glwasm_runtime_create_with_imports(
    GLWASM_Module *module, const GLWASM_HostImport *imports,
    size_t import_count, int allow_start, GLWASM_LinkError *link_error,
    GLWASM_Runtime **out_runtime);

/** Release an instance. The module must outlive the instance. */
GLWASM_API void glwasm_runtime_destroy(GLWASM_Runtime *runtime);

/** Invoke a function by module function index using Wasm value bit patterns. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime *runtime,
    uint32_t function, const uint64_t *arguments, uint32_t argument_count,
    uint64_t *result);

/** Resume an invocation that returned ::GLWASM_ERR_PAUSED. */
GLWASM_API GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime *runtime,
    uint64_t *result, int *paused);
/** Read the paused Wasm function index and module byte offset. */
GLWASM_API GLWASM_Result glwasm_runtime_pause_position(
    const GLWASM_Runtime *runtime, uint32_t *function, uint64_t *module_offset);

/** Capture a paused instance. Snapshots include guest frames, globals,
 * memories, tables, segment-drop state, and interpreter continuation state.
 * Modules with imports or externref/exnref state, and instances with a pending
 * host continuation, are refused because those owners have not supplied
 * snapshot hooks. The module must outlive the snapshot. */
GLWASM_API GLWASM_Result glwasm_runtime_snapshot(GLWASM_Runtime *runtime,
    GLWASM_Snapshot **out_snapshot);
/** Restore into a fresh instance created from the same module, with no start
 * function execution. The instance must have the same policy and no imports.
 * Runtime-core budgets are those of the destination and start fresh. */
GLWASM_API GLWASM_Result glwasm_runtime_restore(GLWASM_Runtime *runtime,
    const GLWASM_Snapshot *snapshot);
GLWASM_API void glwasm_snapshot_release(GLWASM_Snapshot *snapshot);

/** Invoke an exported function by exact byte name. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke_export(
    GLWASM_Runtime *runtime, const char *name, const uint64_t *arguments,
    uint32_t argument_count, uint64_t *result);

/** Invoke a named Wasm export across an engine boundary in an adopted core
 * context. `context` is borrowed and must outlive the call and any paused
 * continuation. `caller` must identify a registered engine in that context;
 * `instance_token` is a nonzero, context-unique opaque identity owned by the
 * caller. This function retains neither the context nor the token. If it
 * returns GLWASM_ERR_PAUSED, resume with the same context, caller, and token.
 * May return GLWASM_ERR_PAUSED again; after completion the nested state clears. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke_nested(
    GLWASM_Runtime *runtime, GRCORE_Context *context,
    GRCORE_EngineId caller, uintptr_t instance_token,
    const char *export_name, const uint64_t *arguments,
    uint32_t argument_count, uint64_t *result);

/** Resume a nested Wasm invocation that returned GLWASM_ERR_PAUSED. The same
 * borrowed context, caller engine, and instance token must be supplied and
 * remain valid. May return GLWASM_ERR_PAUSED again; after completion the
 * nested state is cleared. */
GLWASM_API GLWASM_Result glwasm_runtime_resume_nested(
    GLWASM_Runtime *runtime, GRCORE_Context *context,
    GRCORE_EngineId caller, uintptr_t instance_token, uint64_t *result);

#ifdef __cplusplus
}
#endif
#endif
