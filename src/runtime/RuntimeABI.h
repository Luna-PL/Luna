#pragma once

#include <stddef.h>
#include <stdint.h>

// Stable, C-compatible host boundary used by generated Luna code and future
// MoonRuntime/module loaders.  This header describes contracts only; the
// default runtime implementation may use the platform C/C++ runtime behind
// the boundary.
#ifdef __cplusplus
extern "C" {
#endif

#define LUNA_RUNTIME_ABI_V1 1u
#define LUNA_HOST_SERVICES_MAGIC_V1 0x4c485331u /* "LHS1" */
#define LUNA_DEFAULT_HOST_ALIGNMENT ((size_t)16u)

enum LunaRuntimeStatusV1 {
    LUNA_RUNTIME_STATUS_OK = 0,
    LUNA_RUNTIME_STATUS_INVALID_ARGUMENT = -1,
    LUNA_RUNTIME_STATUS_UNSUPPORTED_ABI = -2,
    LUNA_RUNTIME_STATUS_ALREADY_ACTIVE = -3,
    LUNA_RUNTIME_STATUS_UNSUPPORTED_OPERATION = -4,
    LUNA_RUNTIME_STATUS_BUFFER_TOO_SMALL = -5,
};

// Stable error identities for recoverable Runtime boundaries. Diagnostic text
// is deliberately not an identity and may be omitted by callers that cannot
// allocate storage for it.
enum LunaRuntimeErrorDomainV1 {
    LUNA_RUNTIME_ERROR_DOMAIN_NONE = 0,
    LUNA_RUNTIME_ERROR_DOMAIN_FRAGMENT_PLUGIN = 1,
    LUNA_RUNTIME_ERROR_DOMAIN_GPU = 2,
};

enum LunaRuntimeErrorCodeV1 {
    LUNA_RUNTIME_ERROR_NONE = 0,
    LUNA_RUNTIME_ERROR_UNKNOWN = 1,
    LUNA_RUNTIME_ERROR_INVALID_ARGUMENT = 2,
    LUNA_RUNTIME_ERROR_UNSUPPORTED_ABI = 3,
    LUNA_RUNTIME_ERROR_DYNAMIC_LIBRARY = 4,
    LUNA_RUNTIME_ERROR_MISSING_SYMBOL = 5,
    LUNA_RUNTIME_ERROR_INVALID_DESCRIPTOR = 6,
    LUNA_RUNTIME_ERROR_DUPLICATE_REGISTRATION = 7,
    LUNA_RUNTIME_ERROR_NOT_FOUND = 8,
    LUNA_RUNTIME_ERROR_INVALID_RESULT = 9,
    LUNA_RUNTIME_ERROR_BACKEND_UNAVAILABLE = 10,
    LUNA_RUNTIME_ERROR_BACKEND_OPERATION = 11,
    LUNA_RUNTIME_ERROR_INVALID_STATE = 12,
};

typedef struct LunaRuntimeErrorSnapshotV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t domain;
    int32_t code;
    // UTF-8 bytes excluding the trailing NUL. A zero value means that no
    // diagnostic text is available.
    uint64_t message_size;
} LunaRuntimeErrorSnapshotV1;

enum LunaHostCapabilityV1 {
    LUNA_HOST_CAP_ALLOCATOR = UINT64_C(1) << 0,
    LUNA_HOST_CAP_CONSOLE = UINT64_C(1) << 1,
    // Executable memory is intentionally optional. A MoonRuntime/JIT must not
    // assume it is available merely because ordinary allocation is present.
    LUNA_HOST_CAP_EXECUTABLE_MEMORY = UINT64_C(1) << 2,
};

enum LunaConsoleStreamV1 {
    LUNA_CONSOLE_STDOUT = 1u,
    LUNA_CONSOLE_STDERR = 2u,
};

typedef void* (*LunaAllocateFnV1)(void* context, size_t size, size_t alignment);
typedef void* (*LunaReallocateFnV1)(void* context, void* pointer,
                                    size_t old_size, size_t new_size,
                                    size_t alignment);
typedef void (*LunaDeallocateFnV1)(void* context, void* pointer,
                                   size_t size, size_t alignment);

typedef struct LunaAllocatorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaAllocateFnV1 allocate;
    LunaReallocateFnV1 reallocate;
    LunaDeallocateFnV1 deallocate;
} LunaAllocatorV1;

typedef int (*LunaConsoleWriteFnV1)(void* context, uint32_t stream,
                                    const char* bytes, size_t byte_count);
typedef int (*LunaConsoleFlushFnV1)(void* context, uint32_t stream);

typedef struct LunaConsoleV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaConsoleWriteFnV1 write;
    LunaConsoleFlushFnV1 flush;
} LunaConsoleV1;

// Reserved W^X-capable interface for MoonRuntime/JIT code memory. `reserve`
// returns writable, non-executable memory; `seal` transitions it to executable
// and non-writable. The v1 runtime advertises this capability only when a host
// explicitly installs an implementation.
typedef void* (*LunaExecutableReserveFnV1)(void* context, size_t size,
                                           size_t alignment);
typedef int (*LunaExecutableSealFnV1)(void* context, void* pointer,
                                      size_t size);
typedef void (*LunaExecutableReleaseFnV1)(void* context, void* pointer,
                                          size_t size);

typedef struct LunaExecutableMemoryV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* context;
    LunaExecutableReserveFnV1 reserve;
    LunaExecutableSealFnV1 seal;
    LunaExecutableReleaseFnV1 release;
} LunaExecutableMemoryV1;

typedef struct LunaHostServicesV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    uint64_t capabilities;
    const LunaAllocatorV1* allocator;
    const LunaConsoleV1* console;
    const LunaExecutableMemoryV1* executable_memory;
} LunaHostServicesV1;

// Foreign resources keep their originating release capability. They are not
// Luna heap allocations and must never be passed to rt_dealloc. This carrier
// is reserved for typed C-FFI adapters and Moon container/module boundaries.
typedef void (*LunaForeignReleaseFnV1)(void* context, void* resource,
                                       size_t size, size_t alignment);

typedef struct LunaOwnedForeignMemoryV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    void* data;
    size_t size;
    size_t alignment;
    void* release_context;
    LunaForeignReleaseFnV1 release;
    uint64_t allocator_domain;
} LunaOwnedForeignMemoryV1;

// Future dynamically loaded Moon modules receive this context instead of
// importing ambient process symbols. Fields may only be appended in v1 and
// consumers must gate reads with struct_size.
typedef struct LunaRuntimeModuleContextV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    const LunaHostServicesV1* host_services;
    const char* module_id;
    uint64_t granted_capabilities;
} LunaRuntimeModuleContextV1;

// Process-wide host services. Installation is optional and must happen
// before the first allocation, console call, or explicit descriptor query.
int rt_install_host_services_v1(const LunaHostServicesV1* services);
const LunaHostServicesV1* rt_host_services_v1(void);

// Copies the most recent error for `domain` into caller-owned storage. The
// snapshot metadata is always written when `snapshot` and `domain` are valid.
// `message_capacity` includes room for the trailing NUL. A null/zero buffer is
// a size query; BUFFER_TOO_SMALL reports a truncated or omitted message while
// preserving domain, code, and the complete required message_size.
int rt_runtime_error_snapshot_v1(uint32_t domain,
                                 LunaRuntimeErrorSnapshotV1* snapshot,
                                 char* message, size_t message_capacity);

void* rt_alloc(size_t size, size_t alignment);
void* rt_realloc(void* pointer, size_t old_size, size_t new_size,
                 size_t alignment);
void rt_dealloc(void* pointer, size_t size, size_t alignment);
void* rt_rc_alloc(size_t size, size_t alignment);
void rt_rc_retain(void* pointer);
int32_t rt_rc_release(void* pointer);
void* rt_arc_alloc(size_t size, size_t alignment);
void rt_arc_retain(void* pointer);
int32_t rt_arc_release(void* pointer);
void rt_shared_dealloc(void* pointer);
void rt_panic_cstr(const char* message);

#ifdef __cplusplus
}
#endif
