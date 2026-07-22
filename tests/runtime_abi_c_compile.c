#include "runtime/RuntimeABI.h"

// Build-only C translation unit: the public host ABI must not require C++.
size_t luna_runtime_abi_c_layout_probe(void) {
    void* (*allocate)(size_t, size_t) = rt_alloc;
    void (*deallocate)(void*, size_t, size_t) = rt_dealloc;
    (void)allocate;
    (void)deallocate;
    return sizeof(LunaAllocatorV1) + sizeof(LunaHostServicesV1) +
           sizeof(LunaOwnedForeignMemoryV1) + sizeof(LunaRuntimeModuleContextV1);
}
