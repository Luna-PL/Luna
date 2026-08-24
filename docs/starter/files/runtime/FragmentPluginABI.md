# src/runtime/FragmentPluginABI.h

The stable C ABI specification header for Luna external fragment plugins, defining the plugin descriptor, invocation parameter bundle, entry-point function pointer types, and related constants.

## What This File Does

- Defines the fragment plugin ABI version macro `LUNA_FRAGMENT_PLUGIN_ABI_V1` (1) and the descriptor magic number `LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1` (`"LFP1"`, i.e., 0x4c465031).
- Defines the fragment kind enum: `LUNA_FRAGMENT_KIND_INTERCEPTOR` (interceptor, 1) and `LUNA_FRAGMENT_KIND_CONTEXT` (context, 2).
- Defines the cardinality enum: `LUNA_FRAGMENT_CARDINALITY_ONCE` (once, 1) and `LUNA_FRAGMENT_CARDINALITY_MANY` (many, 2).
- Defines the side-effect flag enum: `LUNA_FRAGMENT_EFFECT_HOST_ONLY` (host only, bit 0) and `LUNA_FRAGMENT_EFFECT_MAY_ABORT` (may abort, bit 1).
- Defines the plugin entry return-value enum: `LUNA_FRAGMENT_PLUGIN_CONTINUE` (0), `LUNA_FRAGMENT_PLUGIN_ABORT` (1), and `LUNA_FRAGMENT_PLUGIN_ERROR` (-1).
- Defines the data structures: `LunaFragmentInvocationV1` (invocation parameter bundle) and `LunaFragmentPluginDescriptorV1` (plugin descriptor).
- Defines the entry-point function pointer type `LunaFragmentPluginEntryV1` and the descriptor retrieval function pointer type `LunaFragmentPluginDescriptorFnV1`.

The ABI design principle is "metadata first": the host validates the slot contract before invoking the entry point.

## Key Structs, Classes, and Enums

### `LunaFragmentInvocationV1` — Invocation Parameter Bundle

```c
typedef struct LunaFragmentInvocationV1 {
    uint32_t abi_version;           // pinned to LUNA_FRAGMENT_PLUGIN_ABI_V1
    const void* const* args;        // array of argument pointers, each pointing to one argument value
    size_t arg_count;               // argument count
} LunaFragmentInvocationV1;
```

Parameters are passed as an array of pointers rather than a flattened parameter block. Each parameter value is allocated by the host and accessed read-only by the plugin.

### `LunaFragmentPluginDescriptorV1` — Plugin Descriptor

```c
typedef struct LunaFragmentPluginDescriptorV1 {
    uint32_t magic;                 // LUNA_FRAGMENT_PLUGIN_DESCRIPTOR_V1
    uint32_t abi_version;           // LUNA_FRAGMENT_PLUGIN_ABI_V1
    uint32_t descriptor_size;       // struct size, for forward compatibility
    const char* plugin_id;          // unique plugin identifier
    const char* fragment_name;      // fragment name
    const char* slot_name;          // slot name
    const char* contract_hash;      // compiler-produced contract hash, covering kind/cardinality/argument layout
    uint32_t fragment_kind;         // LUNA_FRAGMENT_KIND_INTERCEPTOR or _CONTEXT
    uint32_t cardinality;           // LUNA_FRAGMENT_CARDINALITY_ONCE or _MANY
    uint32_t effects;               // combination of effect flags
    LunaFragmentPluginEntryV1 entry; // entry point function pointer
} LunaFragmentPluginDescriptorV1;
```

### Enum Summary

| Enum Group | Value | Semantics |
|---|---|---|
| Fragment kind | `LUNA_FRAGMENT_KIND_INTERCEPTOR` (1) | Interceptor: the host call site is replaced by the plugin implementation |
|  | `LUNA_FRAGMENT_KIND_CONTEXT` (2) | Context: provides a runtime context value |
| Cardinality | `LUNA_FRAGMENT_CARDINALITY_ONCE` (1) | Once: the plugin is invoked only once |
|  | `LUNA_FRAGMENT_CARDINALITY_MANY` (2) | Many: the plugin may be invoked multiple times |
| Side effects | `LUNA_FRAGMENT_EFFECT_HOST_ONLY` (1) | Executes only on the host side; does not involve the Luna stack |
|  | `LUNA_FRAGMENT_EFFECT_MAY_ABORT` (2) | May terminate the process |
| Return value | `LUNA_FRAGMENT_PLUGIN_CONTINUE` (0) | Continue execution |
|  | `LUNA_FRAGMENT_PLUGIN_ABORT` (1) | Abort the current fragment |
|  | `LUNA_FRAGMENT_PLUGIN_ERROR` (-1) | An error occurred |

### Function Pointer Types

| Type | Signature | Purpose |
|---|---|---|
| `LunaFragmentPluginEntryV1` | `int(*)(const LunaFragmentInvocationV1*)` | Plugin entry point; receives the invocation parameter bundle and returns `CONTINUE`/ `ABORT`/ `ERROR` |
| `LunaFragmentPluginDescriptorFnV1` | `const LunaFragmentPluginDescriptorV1*(*)(void)` | Exported function in the shared library that returns the descriptor pointer. A plugin should export one function of this type named `luna_fragment_plugin_descriptor_v1` |

## Key Functions and Methods

This file is a pure ABI header with no function implementations; it only defines two function pointer types and three structs.

### Plugin-Side Requirements

A valid fragment plugin shared library must:
1. Export a function of type `LunaFragmentPluginDescriptorFnV1` named `luna_fragment_plugin_descriptor_v1`.
2. That function returns a constant pointer to a `LunaFragmentPluginDescriptorV1` valid for the lifetime of the process.
3. The `entry` field of the descriptor points to a valid `LunaFragmentPluginEntryV1` function.

### Host-Side Counterpart Functions

The host (`Runtime.cpp`) loads and invokes plugins through the following functions:
- `rt_fragment_plugin_load` — Loads the shared library, looks up the `luna_fragment_plugin_descriptor_v1` symbol, and validates the descriptor.
- `rt_fragment_plugin_is_registered` — Queries by the (slot_name, fragment_name, contract_hash) triple.
- `rt_fragment_plugin_invoke` — Looks up and calls the `entry` function of the matching plugin.

## Relationship to Surrounding Files and Pipeline Stages

- **Runtime.h** — Declares the C ABI entry functions related to fragment plugins (`rt_fragment_plugin_load`, `rt_fragment_plugin_invoke`, etc.); these functions use the types defined in this file.
- **Runtime.cpp** — Implements the plugin loading and invocation logic, using the structs in this file for descriptor validation.
- **RuntimeABI.h** — Provides error types such as `LunaRuntimeErrorSnapshotV1`; plugin loading errors are written to the corresponding error domain.
- This file is a standalone ABI specification that does not depend on any other Runtime header; it depends only on the standard `<stddef.h>` and `<stdint.h>`.
- Fragment plugin developers should include this file directly to construct plugin descriptors.

## Further Reading

- Declarations of `rt_fragment_plugin_load`, `rt_fragment_plugin_invoke`, and other functions in `Runtime.h`
- Implementation details of fragment plugin loading and validation in `Runtime.cpp`
- Definition of the `LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN` error domain in `RuntimeABI.h`
- Dynamic linking/loading: POSIX `dlopen`/`dlsym` and Windows `LoadLibrary`/`GetProcAddress`


---

---
title: Runtime.cpp
source: src/runtime/Runtime.cpp
language: en
audience: Luna runtime implementers / embedding hosts
---
