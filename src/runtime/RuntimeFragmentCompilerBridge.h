#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Compiler-private bridge, not a source, container or published host ABI.
// The parent and Ref are borrowed live native capabilities. A successful
// output owns a distinct immutable context snapshot suitable for the existing
// runtime Fragment dispatch entry; generated code must drop it on every exit.
// Do not pass arbitrary/stale pointers or mutate either owner concurrently.
enum LunaCompilerFragmentOverrideStatus {
    LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS = 0,
    LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_OUTPUT = -1,
    LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT = -2,
    LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_REFERENCE = -3,
    LUNA_COMPILER_FRAGMENT_OVERRIDE_FAILED = -4,
};

int32_t luna_compiler_fragment_context_override_from_ref(
    const void* parent_context, const void* reference,
    const char* slot_id, const char* slot_contract_id,
    void** output_context);

// Clears the owning cell before releasing the snapshot. A null cell or empty
// cell is a no-op. Only a pointer produced by the function above is valid.
void luna_compiler_fragment_context_drop(void** context);

#ifdef __cplusplus
}
#endif
