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

/** Identity of a Wasm exception that crossed an adopted runtime-core
 * boundary. `identity` is an opaque token owned by the originating runtime;
 * it remains valid until that runtime is destroyed and can be passed back as
 * an `exnref` argument to the same runtime. */
typedef struct GLWASM_ForeignException {
  GRCORE_EngineId engine; /**< Originating engine in the adopted context. */
  uint64_t identity; /**< Opaque token owned by the originating runtime. */
} GLWASM_ForeignException;

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
  const GLWASM_HostValueType *result_types; /**< Borrowed result types. */
  const uint64_t *arguments;            /**< Borrowed argument bit patterns. */
  uint32_t argument_count; /**< Number of entries in types and arguments. */
  GLWASM_HostValueType result_type;     /**< Compatibility field, for one result. */
  uint32_t result_count;                /**< Number of declared result values. */
  void *user; /**< Borrowed binding state; valid during the callback. */
} GLWASM_HostCall;

/** Register a test or embedder engine in this runtime's context. The
 * descriptor is borrowed and must outlive the runtime. */
GLWASM_API GLWASM_Result glwasm_runtime_register_boundary_engine(
    GLWASM_Runtime *runtime,const GRCORE_EngineDescriptor *descriptor,
    GRCORE_EngineId *out_engine);
/** Register this Wasm engine in an adopted core context before a running
 * caller enters the mixed-engine boundary. The context must outlive the
 * binding unless it is explicitly unbound. Bind/unbind and runtime
 * destruction must occur on the context's owner thread while parked. Call
 * ::glwasm_runtime_unbind_boundary_context before destroying the context. */
GLWASM_API GLWASM_Result glwasm_runtime_bind_boundary_context(
    GLWASM_Runtime *runtime,GRCORE_Context *context,
    GRCORE_EngineId *out_wasm_engine);
/** Release an adopted-context binding. The runtime must be idle; the context
 * must still be alive, parked, and owned by the calling thread. Safe to call
 * when no binding exists. */
GLWASM_API GLWASM_Result glwasm_runtime_unbind_boundary_context(
    GLWASM_Runtime *runtime);

/** Scalar compatibility callback. It supports zero or one result. Return
 * ::GLWASM_ERR_PAUSED only when the import binding also supplies `resume`.
 * Side effects completed before that return remain complete. */
typedef GLWASM_Result (*GLWASM_HostFunction)(
    const GLWASM_HostCall *call, uint64_t *result);
/** Continue a scalar host callback that previously returned ::GLWASM_ERR_PAUSED.
 * The same call arguments and user pointer are supplied. The host stores
 * continuation state in its user object; the initial callback is never
 * repeated after a pause. This entrypoint may return PAUSED again. */
typedef GLWASM_Result (*GLWASM_HostResumeFunction)(
    const GLWASM_HostCall *call, uint64_t *result);
/** Result-slice host callback. `results` has `call->result_count` slots; a
 * callback that can pause must have the matching resume callback installed. */
typedef GLWASM_Result (*GLWASM_HostFunctionResults)(
    const GLWASM_HostCall *call, uint64_t *results);
typedef GLWASM_Result (*GLWASM_HostResumeFunctionResults)(
    const GLWASM_HostCall *call, uint64_t *results);

/** An explicit capability granted to one exact function, memory, table,
 * global, or exception-tag import. */
typedef struct GLWASM_HostImport {
  const char *module; /**< Exact import module name; borrowed at link time. */
  const char *field; /**< Exact import field name; borrowed at link time. */
  GLWASM_HostFunction function; /**< Scalar callback for a function import. */
  GLWASM_HostResumeFunction resume; /**< Scalar continuation after pause. */
  void *user; /**< Borrowed callback/provider state; outlives the instance. */
  uint8_t kind; /**< GLWASM_HOST_IMPORT_*; zero preserves function imports. */
  union {
    struct GLWASM_HostMemoryProvider *memory;
    struct GLWASM_HostTableProvider *table;
    struct GLWASM_HostGlobal *global;
    struct GLWASM_HostTag *tag;
  } resource; /**< Provider for the selected non-function import kind. */
  /** Optional result-slice callbacks. When set, these take precedence over
   * the scalar callbacks above and support all declared result values. The
   * result array length is `call->result_count`; `result_types` gives its
   * types. */
  GLWASM_HostFunctionResults function_results;
  /** Result-slice continuation paired with `function_results` after pause. */
  GLWASM_HostResumeFunctionResults resume_results;
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
  uint8_t *bytes; /**< Borrowed bytes, pinned until the matching end call. */
  uint64_t length_bytes; /**< Current byte length of the leased memory. */
} GLWASM_HostMemoryView;
typedef GLWASM_Result (*GLWASM_HostMemoryBegin)(void *user,
    GLWASM_HostMemoryView *view);
typedef void (*GLWASM_HostMemoryEnd)(void *user,
    GLWASM_HostMemoryView *view);
typedef GLWASM_Result (*GLWASM_HostMemoryGrow)(void *user, uint64_t delta_pages,
    uint64_t *old_pages);
/** Host-owned memory import with short-lived access leases. */
typedef struct GLWASM_HostMemoryProvider {
  GLWASM_HostMemoryBegin begin; /**< Open and pin a memory view. */
  GLWASM_HostMemoryEnd end; /**< Close the matching view. */
  GLWASM_HostMemoryGrow grow; /**< Grow by pages outside a lease. */
  void *user; /**< Borrowed provider state; outlives the instance. */
} GLWASM_HostMemoryProvider;

/**
 * A short-lived table view containing opaque 64-bit reference tokens. A begin
 * transaction roots all tokens in the view until end. For externref tables,
 * validate_token must reject stale or foreign tokens; the provider owns the
 * cross-heap mapping and relocation behavior.
 */
typedef struct GLWASM_HostTableView {
  uint64_t *elements; /**< Borrowed rooted token array until end. */
  uint64_t length; /**< Number of tokens in elements. */
} GLWASM_HostTableView;
typedef GLWASM_Result (*GLWASM_HostTableBegin)(void *user,
    GLWASM_HostTableView *view);
typedef void (*GLWASM_HostTableEnd)(void *user,
    GLWASM_HostTableView *view);
typedef GLWASM_Result (*GLWASM_HostTableGrow)(void *user, uint64_t delta,
    uint64_t initial, uint64_t *old_size);
typedef GLWASM_Result (*GLWASM_HostReferenceCheck)(void *user, uint64_t token);
/** Host-owned table import with rooted, short-lived token views. */
typedef struct GLWASM_HostTableProvider {
  GLWASM_HostTableBegin begin; /**< Open and root a table view. */
  GLWASM_HostTableEnd end; /**< Close the matching view. */
  GLWASM_HostTableGrow grow; /**< Grow by elements outside a lease. */
  GLWASM_HostValueType element_type; /**< Declared reference element type. */
  GLWASM_HostReferenceCheck validate_token; /**< Reject stale/foreign tokens. */
  void *user; /**< Borrowed provider state; outlives the instance. */
} GLWASM_HostTableProvider;

/** Host-owned global import backed by a borrowed value slot. */
typedef struct GLWASM_HostGlobal {
  GLWASM_HostValueType type; /**< Declared Wasm value type. */
  uint8_t mutable_; /**< Nonzero when guest writes are allowed. */
  /** Borrowed storage. The descriptor and value slot must remain alive until
   * the importing runtime is destroyed. The host may update a mutable slot
   * only while that runtime is parked or idle on its owner thread. Debugger
   * inspection reads the current slot value, not a cached guest-read value. */
  uint64_t *value;
  GLWASM_HostReferenceCheck validate_token; /**< Check reference tokens. */
  void *user; /**< Borrowed validation state; outlives the instance. */
} GLWASM_HostGlobal;

/** Type-checked identity for an imported exception tag. */
typedef struct GLWASM_HostTag {
  uint64_t identity; /**< Host-selected identity shared with matching tags. */
  const GLWASM_HostValueType *parameters; /**< Borrowed payload types. */
  uint32_t parameter_count; /**< Number of payload types. */
} GLWASM_HostTag;

typedef enum GLWASM_LinkFailure {
  GLWASM_LINK_UNRESOLVED = 0,
  GLWASM_LINK_UNSUPPORTED_KIND,
  GLWASM_LINK_INVALID_BINDING,
  GLWASM_LINK_START_NOT_ALLOWED
} GLWASM_LinkFailure;

/** Import diagnostic. Names are borrowed from the module. */
typedef struct GLWASM_LinkError {
  const char *module; /**< Borrowed failed import module name. */
  const char *field; /**< Borrowed failed import field name. */
  GLWASM_LinkFailure reason; /**< Failure category. */
} GLWASM_LinkError;

/**
 * Link every function import against the explicit allowlist and instantiate.
 * Imports without an exact entry are denied. `tang.*` additionally requires
 * the decoded module policy's privileged Tang capability; only `tang.v1` is
 * accepted, and the exact versioned import module must still be supplied. This
 * first host API supports function imports; imported memories, tables, globals
 * and tags use provider entries with matching type and limits.
 *
 * `allow_start` must be true to run a module's start function. The instance
 * retains `module`; the caller may release its module handle after successful
 * creation, and the module is freed after both caller and instance references
 * are released. Binding strings need remain valid only through instantiation;
 * callback pointers and user pointers are copied into the instance. Provider
 * structs, provider state, imported global slots, and callback user state must
 * outlive that instance.
 */
GLWASM_API GLWASM_Result glwasm_runtime_create_with_imports(
    GLWASM_Module *module, const GLWASM_HostImport *imports,
    size_t import_count, int allow_start, GLWASM_LinkError *link_error,
    GLWASM_Runtime **out_runtime);

/** Release an instance and its retained module reference. */
GLWASM_API void glwasm_runtime_destroy(GLWASM_Runtime *runtime);

/** The instance's borrowed runtime-core context. It is valid until the runtime
 * is destroyed and may be inspected only while the context is readable and
 * held by its owner thread. */
GLWASM_API GRCORE_Context * glwasm_runtime_context(
    const GLWASM_Runtime *runtime);

/** Invoke a function by module function index using Wasm value bit patterns. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke(GLWASM_Runtime *runtime,
    uint32_t function, const uint64_t *arguments, uint32_t argument_count,
    uint64_t *result);
/** Invoke a function with a caller-owned result slice. `result_count` is an
 * in/out value: capacity on entry and number of results on success. If the
 * capacity is too small, ::GLWASM_ERR_LIMIT is returned and `result_count` is
 * set to the required arity. The buffer must remain writable through resume. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke_results(
    GLWASM_Runtime *runtime,uint32_t function,const uint64_t *arguments,
    uint32_t argument_count,uint64_t *results,uint32_t *result_count);

/** Resume an invocation that returned ::GLWASM_ERR_PAUSED. */
GLWASM_API GLWASM_Result glwasm_runtime_resume(GLWASM_Runtime *runtime,
    uint64_t *result, int *paused);
/** Resume the result-slice invocation. Capacity and buffer rules match
 * ::glwasm_runtime_invoke_results; the buffer may be replaced on resume. */
GLWASM_API GLWASM_Result glwasm_runtime_resume_results(
    GLWASM_Runtime *runtime,uint64_t *results,uint32_t *result_count,
    int *paused);
/** Read the paused Wasm function index and module byte offset. */
GLWASM_API GLWASM_Result glwasm_runtime_pause_position(
    const GLWASM_Runtime *runtime, uint32_t *function, uint64_t *module_offset);

/** Capture a paused instance. Snapshots include guest frames, globals,
 * memories, tables, segment-drop state, and interpreter continuation state.
 * Modules with imports or externref/exnref state, and instances with a pending
 * host continuation, are refused because those owners have not supplied
 * snapshot hooks. A snapshot retains its module reference until released. */
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
/** Invoke an export with a caller-owned result slice. `result_count` is
 * capacity on entry and arity on success; when capacity is too small, the
 * function returns ::GLWASM_ERR_LIMIT and writes the required arity. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke_export_results(
    GLWASM_Runtime *runtime,const char *name,const uint64_t *arguments,
    uint32_t argument_count,uint64_t *results,uint32_t *result_count);

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
/** Nested result-slice form; capacity and reporting rules match
 * ::glwasm_runtime_invoke_results. On pause, the caller must retain a writable
 * result buffer through resume or supply a replacement to the result-slice
 * resume function. */
GLWASM_API GLWASM_Result glwasm_runtime_invoke_nested_results(
    GLWASM_Runtime *runtime,GRCORE_Context *context,GRCORE_EngineId caller,
    uintptr_t instance_token,const char *export_name,const uint64_t *arguments,
    uint32_t argument_count,uint64_t *results,uint32_t *result_count);

/** Read the opaque exception from the most recent nested invocation that
 * returned ::GLWASM_ERR_GUEST because of an uncaught Wasm exception. The
 * identity is stable for the lifetime of the originating runtime and is
 * accepted as an `exnref` only by that runtime. Returns ::GLWASM_ERR_INVALID
 * when no such exception is pending. */
GLWASM_API GLWASM_Result glwasm_runtime_last_foreign_exception(
    const GLWASM_Runtime *runtime,GLWASM_ForeignException *out_exception);

/** Resume a nested Wasm invocation that returned GLWASM_ERR_PAUSED. The same
 * borrowed context, caller engine, and instance token must be supplied and
 * remain valid. May return GLWASM_ERR_PAUSED again; after completion the
 * nested state is cleared. */
GLWASM_API GLWASM_Result glwasm_runtime_resume_nested(
    GLWASM_Runtime *runtime, GRCORE_Context *context,
    GRCORE_EngineId caller, uintptr_t instance_token, uint64_t *result);
/** Resume a nested result-slice call. The result buffer may be replaced. */
GLWASM_API GLWASM_Result glwasm_runtime_resume_nested_results(
    GLWASM_Runtime *runtime,GRCORE_Context *context,GRCORE_EngineId caller,
    uintptr_t instance_token,uint64_t *results,uint32_t *result_count);

#ifdef __cplusplus
}
#endif
#endif
