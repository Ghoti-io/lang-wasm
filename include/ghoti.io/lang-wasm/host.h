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
#include <ghoti.io/lang-wasm/policy.h>
#include <ghoti.io/runtime-core/b/context.h>
#include <ghoti.io/runtime-core/a/engine.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A store: the runtime-core context, budgets and every object (function,
 * table, memory, global, tag) that its instances define or import. Linked
 * instances share one store; a call into another instance's function is an
 * ordinary guest call in the same context. */
typedef struct GLWASM_Store GLWASM_Store;
/** One instance of a module in a store. The store owns it. */
typedef struct GLWASM_Runtime GLWASM_Runtime;
typedef struct GLWASM_Snapshot GLWASM_Snapshot;

/** The kind of a store object, numbered as the binary format's export kinds. */
typedef enum GLWASM_ExternKind {
  GLWASM_EXTERN_FUNC = 0,
  GLWASM_EXTERN_TABLE = 1,
  GLWASM_EXTERN_MEMORY = 2,
  GLWASM_EXTERN_GLOBAL = 3,
  GLWASM_EXTERN_TAG = 4
} GLWASM_ExternKind;

/** A reference to one object of a store, as an export yields it. It is
 * valid for the store's lifetime and only with that store. */
typedef struct GLWASM_Extern {
  const GLWASM_Store *store; /**< The owning store. */
  uint8_t kind;              /**< ::GLWASM_ExternKind. */
  uint32_t address;          /**< Object index within the store, per kind. */
} GLWASM_Extern;

/** Why guest execution trapped. The suite's messages name each one. */
typedef enum GLWASM_Trap {
  GLWASM_TRAP_NONE = 0,
  GLWASM_TRAP_UNREACHABLE,               /**< "unreachable" */
  GLWASM_TRAP_DIVIDE_BY_ZERO,            /**< "integer divide by zero" */
  GLWASM_TRAP_INTEGER_OVERFLOW,          /**< "integer overflow" */
  GLWASM_TRAP_TABLE_OUT_OF_BOUNDS,       /**< "out of bounds table access" */
  GLWASM_TRAP_UNINITIALIZED_ELEMENT,     /**< "uninitialized element" */
  GLWASM_TRAP_INDIRECT_CALL_TYPE_MISMATCH, /**< "indirect call type mismatch" */
  GLWASM_TRAP_MEMORY_OUT_OF_BOUNDS,      /**< "out of bounds memory access" */
  GLWASM_TRAP_INVALID_REFERENCE,         /**< a null or foreign exnref */
  GLWASM_TRAP_UNDEFINED_ELEMENT,         /**< "undefined element" */
  GLWASM_TRAP_INVALID_CONVERSION         /**< "invalid conversion to integer" */
} GLWASM_Trap;

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

/** An explicit capability granted to one exact import: a host function,
 * memory, table, global or exception tag, or an object of the same store
 * exported by another instance (::GLWASM_HOST_IMPORT_EXTERN). */
typedef struct GLWASM_HostImport {
  const char *module; /**< Exact import module name; borrowed at link time. */
  const char *field; /**< Exact import field name; borrowed at link time. */
  GLWASM_HostFunction function; /**< Scalar callback for a function import. */
  GLWASM_HostResumeFunction resume; /**< Scalar continuation after pause. */
  void *user; /**< Borrowed callback/provider state; outlives the store. */
  uint8_t kind; /**< GLWASM_HOST_IMPORT_*. */
  union {
    struct GLWASM_HostMemoryProvider *memory;
    struct GLWASM_HostTableProvider *table;
    struct GLWASM_HostGlobal *global;
    struct GLWASM_HostTag *tag;
    GLWASM_Extern linked; /**< For ::GLWASM_HOST_IMPORT_EXTERN. */
  } resource; /**< The object for every import kind but a host function. */
  /** Optional result-slice callbacks. When set, these take precedence over
   * the scalar callbacks above and support all declared result values. The
   * result array length is `call->result_count`; `result_types` gives its
   * types. */
  GLWASM_HostFunctionResults function_results;
  /** Result-slice continuation paired with `function_results` after pause. */
  GLWASM_HostResumeFunctionResults resume_results;
  /** A host function's type. The import links only when its declared type
   * is exactly this one ("incompatible import type" otherwise). The arrays
   * are copied at link time. */
  const GLWASM_HostValueType *parameters;
  uint32_t parameter_count;
  const GLWASM_HostValueType *results;
  uint32_t result_count;
} GLWASM_HostImport;

enum {
  GLWASM_HOST_IMPORT_FUNCTION = 0,
  GLWASM_HOST_IMPORT_MEMORY = 1,
  GLWASM_HOST_IMPORT_TABLE = 2,
  GLWASM_HOST_IMPORT_GLOBAL = 3,
  GLWASM_HOST_IMPORT_TAG = 4,
  GLWASM_HOST_IMPORT_EXTERN = 5
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
/** Host-owned memory import with short-lived access leases. The declared
 * type is what an import is matched against: the current size must reach
 * the import's minimum, and an import with a maximum needs a declared
 * maximum no larger than it. */
typedef struct GLWASM_HostMemoryProvider {
  GLWASM_HostMemoryBegin begin; /**< Open and pin a memory view. */
  GLWASM_HostMemoryEnd end; /**< Close the matching view. */
  GLWASM_HostMemoryGrow grow; /**< Grow by pages outside a lease. */
  void *user; /**< Borrowed provider state; outlives the store. */
  uint64_t maximum_pages; /**< Declared maximum, when has_maximum. */
  uint8_t has_maximum; /**< Nonzero when the memory has a maximum. */
  uint8_t address64; /**< Nonzero for a 64-bit (memory64) memory. */
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
/** Host-owned table import with rooted, short-lived token views. Funcref
 * tokens in it are the store's own (see ::glwasm_store_funcref); the
 * declared type is matched as for a memory. */
typedef struct GLWASM_HostTableProvider {
  GLWASM_HostTableBegin begin; /**< Open and root a table view. */
  GLWASM_HostTableEnd end; /**< Close the matching view. */
  GLWASM_HostTableGrow grow; /**< Grow by elements outside a lease. */
  GLWASM_HostValueType element_type; /**< Declared reference element type. */
  GLWASM_HostReferenceCheck validate_token; /**< Reject stale/foreign tokens. */
  void *user; /**< Borrowed provider state; outlives the store. */
  uint64_t maximum; /**< Declared maximum element count, when has_maximum. */
  uint8_t has_maximum; /**< Nonzero when the table has a maximum. */
  uint8_t address64; /**< Nonzero for a 64-bit (table64) table. */
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
  GLWASM_LINK_UNRESOLVED = 0,     /**< "unknown import": no binding names it. */
  GLWASM_LINK_UNSUPPORTED_KIND,   /**< The binding's kind cannot be imported. */
  GLWASM_LINK_INVALID_BINDING,    /**< The binding is incomplete or malformed. */
  GLWASM_LINK_START_NOT_ALLOWED,  /**< A start function, and allow_start is 0. */
  GLWASM_LINK_INCOMPATIBLE        /**< "incompatible import type". */
} GLWASM_LinkFailure;

/** Import diagnostic. Names are borrowed from the module. */
typedef struct GLWASM_LinkError {
  const char *module; /**< Borrowed failed import module name. */
  const char *field; /**< Borrowed failed import field name. */
  GLWASM_LinkFailure reason; /**< Failure category. */
} GLWASM_LinkError;

/** Where an instantiation stopped. */
typedef enum GLWASM_InstantiateStage {
  GLWASM_INSTANTIATE_COMPLETE = 0, /**< It did not stop: the instance exists. */
  GLWASM_INSTANTIATE_LINK,         /**< An import did not link (see `link`). */
  GLWASM_INSTANTIATE_ALLOCATE,     /**< A budget or limit refused an object. */
  GLWASM_INSTANTIATE_SEGMENTS,     /**< An element or data segment trapped. */
  GLWASM_INSTANTIATE_START         /**< The start function trapped or failed. */
} GLWASM_InstantiateStage;

/** Why an instantiation failed. */
typedef struct GLWASM_InstantiateError {
  GLWASM_InstantiateStage stage; /**< Where it stopped. */
  GLWASM_LinkError link;         /**< For ::GLWASM_INSTANTIATE_LINK. */
  GLWASM_Trap trap;              /**< For a trap in segments or start. */
} GLWASM_InstantiateError;

/**
 * Create an empty store. Its runtime-core context takes its budgets (fuel,
 * guest and native call depth, memory bytes) from `policy`'s runtime fields;
 * NULL means the untrusted preset. A module's own decoded policy still
 * governs its features, limits, JIT and privileged imports.
 */
GLWASM_API GLWASM_Result glwasm_store_create(const GLWASM_Policy *policy,
    GLWASM_Store **out_store);

/** Destroy a store, every instance in it, and every object they made. The
 * store must not be running or paused inside a host callback. */
GLWASM_API void glwasm_store_destroy(GLWASM_Store *store);

/** The store's borrowed runtime-core context, valid until the store is
 * destroyed; inspect it only while readable and on its owner thread. */
GLWASM_API GRCORE_Context * glwasm_store_context(const GLWASM_Store *store);

/** Replace the fuel left for the store's next and current runs. */
GLWASM_API GLWASM_Result glwasm_store_set_fuel(GLWASM_Store *store,
    uint64_t fuel);

/**
 * Instantiate `module` in `store`, linking each import to the binding that
 * names its exact module and field. Imports without a binding are denied.
 * A binding of kind ::GLWASM_HOST_IMPORT_EXTERN links an object another
 * instance of the same store exported; the others supply host objects.
 * Each import's declared type must match its object exactly, with the
 * spec's limits matching for tables and memories. `tang.*` additionally
 * requires the decoded module policy's privileged Tang capability; only
 * `tang.v1` is accepted.
 *
 * Instantiation follows the spec's order: link, allocate, apply the element
 * segments then the data segments in order, run the start function (when
 * `allow_start`; otherwise a start function fails to link). A trap in a
 * segment or in start fails the instantiation, but what the instance had
 * already written into imported tables and memories stays written, and its
 * functions stay valid wherever it placed them: the store keeps it. No
 * instance is returned then, and `error` says where and why.
 *
 * The store retains `module`. Binding strings and type arrays need remain
 * valid only through this call; provider structs, provider state, imported
 * global slots and callback user state must outlive the store.
 */
GLWASM_API GLWASM_Result glwasm_store_instantiate(GLWASM_Store *store,
    GLWASM_Module *module, const GLWASM_HostImport *imports,
    size_t import_count, int allow_start, GLWASM_InstantiateError *error,
    GLWASM_Runtime **out_runtime);

/** The store an instance belongs to. */
GLWASM_API GLWASM_Store * glwasm_runtime_store(const GLWASM_Runtime *runtime);

/** The store's runtime-core context, as ::glwasm_store_context. */
GLWASM_API GRCORE_Context * glwasm_runtime_context(
    const GLWASM_Runtime *runtime);

/** Look up an export by its exact `length`-byte name. */
GLWASM_API GLWASM_Result glwasm_runtime_export(const GLWASM_Runtime *runtime,
    const char *name, size_t length, GLWASM_Extern *out_extern);

/** Read a global's current value as Wasm value bits. */
GLWASM_API GLWASM_Result glwasm_store_global_get(const GLWASM_Store *store,
    GLWASM_Extern global, uint64_t *value);
/** Write a mutable global. A reference value must be valid for its type. */
GLWASM_API GLWASM_Result glwasm_store_global_set(GLWASM_Store *store,
    GLWASM_Extern global, uint64_t value);

/** A memory's current size in bytes. */
GLWASM_API GLWASM_Result glwasm_store_memory_size(const GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t *bytes);
/** Copy `size` bytes out of a memory at `offset`. A host function may call
 * this during its callback, which is how it reads guest pointers. */
GLWASM_API GLWASM_Result glwasm_store_memory_read(const GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t offset, void *buffer, size_t size);
/** Copy `size` bytes into a memory at `offset`. */
GLWASM_API GLWASM_Result glwasm_store_memory_write(GLWASM_Store *store,
    GLWASM_Extern memory, uint64_t offset, const void *buffer, size_t size);

/** The funcref token of a function object: what a funcref value, a table
 * element or a host funcref argument carries. Zero is the null reference. */
GLWASM_API GLWASM_Result glwasm_store_funcref(const GLWASM_Store *store,
    GLWASM_Extern function, uint64_t *token);

/** The trap that ended the store's last invocation, or ::GLWASM_TRAP_NONE. */
GLWASM_API GLWASM_Trap glwasm_store_last_trap(const GLWASM_Store *store);

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
