#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable in-memory ABI for a verified Fragment factory and execution thunk.
// The descriptor is data owned by one loaded module generation. Its function
// pointers remain valid only while that generation's ModuleLease is pinned.
#define LUNA_RUNTIME_FRAGMENT_MAGIC_V1 0x4c524631u /* "LRF1" */
#define LUNA_RUNTIME_FRAGMENT_ABI_V1 1u

enum LunaRuntimeFragmentFlagV1 {
    LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1 = 1u << 0,
};

// Factory arguments use the compiler-defined layout identified by
// factory_contract_id. Zero means success. A successful stateful factory must
// return a non-null environment aligned to environment_alignment. Rejected
// non-null factory output is returned to destroy exactly once. ABI callbacks
// must not unwind across this C boundary, including that failure cleanup.
// The C++ constructor pins the validated generation throughout the factory
// call and rejected-output cleanup, even if the host replaces its source
// binding handle synchronously. A successful reference keeps that same pin.
typedef int32_t (*LunaRuntimeFragmentFactoryFnV1)(
    const void* factory_arguments, void** output_environment);

typedef void (*LunaRuntimeFragmentDestroyFnV1)(void* environment);

// activation is compiler-owned and opaque to plugins and hosts. It carries the
// Slot arguments and the non-forgeable single-shot continuation state.
typedef void (*LunaRuntimeFragmentExecuteFnV1)(
    void* environment, void* activation);

// A generated Slot dispatch receives one explicit, host-owned execution
// context capability. The context pins an immutable BindingSet snapshot for
// the whole call; it is never recovered from process-global or thread-local
// state. The base continuation remains compiler-owned and synchronous.
// After dispatch enters, it retains the selected snapshot through handler
// unwind, even if a synchronous host callback releases the context owner.
// The opaque context must be live at entry; this does not authorize subsequent
// calls through a released pointer or concurrent mutation of its C++ owner.
// Nonempty argument storage must be aligned to arguments_alignment; empty
// storage is exactly (size=0, alignment=1, pointer=null). Invalid carriers fail
// before either a selected Fragment or the None/base continuation executes.
enum LunaRuntimeFragmentContinuationResultV1 {
    LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 = 0,
    LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1 = 1,
};

typedef int32_t (*LunaRuntimeFragmentContinuationFnV1)(void* context);

enum LunaRuntimeFragmentDispatchStatusV1 {
    LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 = 0,
    LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1 = 1,
    LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_CONTEXT_V1 = -1,
    LUNA_RUNTIME_FRAGMENT_DISPATCH_INVALID_INVOCATION_V1 = -2,
    LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1 = -3,
};

int32_t luna_runtime_fragment_dispatch_v1(
    const void* execution_context,
    const char* slot_id,
    const char* slot_contract_id,
    const char* arguments_layout_id,
    uint64_t arguments_size,
    uint64_t arguments_alignment,
    const void* arguments,
    LunaRuntimeFragmentContinuationFnV1 base_continuation,
    void* continuation_context);

// Compiler-generated thunks use these operations to access a Runtime-owned
// opaque activation. Callers cannot construct a valid activation from this C
// ABI; every identity/layout argument must match the state created by the C++
// host control plane. Resume is single-shot. It returns COMPLETED when the
// continuation returns locally, ESCAPED when it performs an enclosing
// return/error propagation, and a negative value on invalid use or failure.
// Resume failure is sticky on a live activation: its Fragment dispatch reports
// failure even if its handler ignores the negative result. Each new dispatch
// has fresh activation state; already-performed effects are not rolled back.
const void* luna_runtime_fragment_activation_arguments_v1(
    void* activation,
    const char* slot_id,
    const char* slot_contract_id,
    const char* arguments_layout_id,
    uint64_t arguments_size,
    uint64_t arguments_alignment);

int32_t luna_runtime_fragment_activation_resume_v1(void* activation);

typedef struct LunaRuntimeFragmentDescriptorV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t flags;
    uint32_t reserved_zero_0;
    uint32_t reserved_zero_1;
    uint32_t reserved_zero_2;
    uint32_t reserved_zero_3;
    const char* fragment_id;
    const char* fragment_contract_id;
    const char* slot_id;
    const char* slot_contract_id;
    const char* slot_arguments_layout_id;
    uint64_t slot_arguments_size;
    uint64_t slot_arguments_alignment;
    const char* factory_contract_id;
    const char* environment_layout_id;
    uint64_t environment_size;
    uint64_t environment_alignment;
    LunaRuntimeFragmentFactoryFnV1 factory;
    LunaRuntimeFragmentDestroyFnV1 destroy;
    LunaRuntimeFragmentExecuteFnV1 execute;
} LunaRuntimeFragmentDescriptorV1;

#ifdef __cplusplus
}
#endif
