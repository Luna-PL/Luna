#include "runtime/RuntimeABI.h"

// Build-only C translation unit: the public host ABI must not require C++.
size_t luna_runtime_abi_c_layout_probe(void) {
    void* (*allocate)(size_t, size_t) = rt_alloc;
    void (*deallocate)(void*, size_t, size_t) = rt_dealloc;
    int (*snapshot_error)(uint32_t, LunaRuntimeErrorSnapshotV1*, char*, size_t) =
        rt_runtime_error_snapshot_v1;
    (void)allocate;
    (void)deallocate;
    (void)snapshot_error;
    return sizeof(LunaAllocatorV1) + sizeof(LunaHostServicesV1) +
           sizeof(LunaOwnedForeignMemoryV1) + sizeof(LunaRuntimeModuleContextV1) +
           sizeof(LunaRuntimeErrorSnapshotV1);
}
