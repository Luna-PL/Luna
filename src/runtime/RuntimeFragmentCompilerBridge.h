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

#ifdef LUNA_PRIVATE_REF_JIT_TEST
// Versioned test-entry result profile. These values are deliberately separate
// from the native Ref and compiler context-check status domains. No public ABI.
enum LunaPrivateRefUnitApplyStatusV1Test {
    LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST = 0,
    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST = 1,
    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST = 2,
    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST = 3,
    LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST = 4,
};

// Private Result transfer experiment; these numbers are not the Native v3
// status domain and must not be used as a published callable contract.
enum LunaPrivateRefResultTransferStatusV1Test {
    LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST = 0,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST = 1,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_RESOURCE_V1_TEST = 2,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INJECTED_FAILURE_V1_TEST = 3,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_CONTEXT_V1_TEST = 4,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_HANDLE_V1_TEST = 5,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_TARGET_V1_TEST = 6,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST = 7,
    LUNA_PRIVATE_REF_RESULT_TRANSFER_ADOPTION_FAILURE_V1_TEST = 8,
};
#endif

// Read-only preflight for a borrowed, live Runtime-created parent context.
// This checks the runtime tag, not the safety of arbitrary or stale pointers.
int32_t luna_compiler_fragment_context_check(const void* parent_context);

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
