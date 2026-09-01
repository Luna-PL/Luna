#include "runtime/NativeArtifactABI.h"
#include "runtime/RuntimeABI.h"
#include "runtime/RuntimeDescriptorABI.h"

_Static_assert(sizeof(LunaNativeProofV1) == 504,
               "Native proof ABI must remain usable from C");
_Static_assert(LUNA_RUNTIME_DECLARATION_SLOT_V1 == 8,
               "Runtime Slot declaration kind must remain stable");
_Static_assert(LUNA_NATIVE_DECLARATION_SLOT_V1 == 8,
               "Native Slot declaration kind must remain stable");

// Build-only C translation unit: the public host ABI must not require C++.
size_t luna_runtime_abi_c_layout_probe(void) {
    void* (*allocate)(size_t, size_t) = rt_alloc;
    void (*deallocate)(void*, size_t, size_t) = rt_dealloc;
    int (*snapshot_error)(uint32_t, LunaRuntimeErrorSnapshotV1*, char*, size_t) =
        rt_runtime_error_snapshot_v1;
    LunaFileHandleV1 handle = LUNA_INVALID_FILE_HANDLE_V1;
    LunaNativeLibraryDescriptorFnV1 native_descriptor = 0;
    LunaRuntimeDescriptorRegistryV1 runtime_registry = {0};
    (void)allocate;
    (void)deallocate;
    (void)snapshot_error;
    (void)handle;
    (void)native_descriptor;
    (void)runtime_registry;
    (void)&rt_console_read_v1;
    (void)&rt_file_open_v1;
    (void)&rt_file_close_v1;
    (void)&rt_checked_array_layout_v1;
    (void)&rt_try_alloc_v1;
    (void)&rt_try_realloc_v1;
    return sizeof(LunaAllocatorV1) + sizeof(LunaHostServicesV1) +
           sizeof(LunaFileSystemV1) + sizeof(LunaIoErrorV1) +
           sizeof(LunaFileMetadataV1) + sizeof(LunaAllocErrorV1) +
           sizeof(LunaOwnedForeignMemoryV1) + sizeof(LunaRuntimeModuleContextV1) +
           sizeof(LunaRuntimeErrorSnapshotV1) + sizeof(LunaNativeProofV1) +
           sizeof(LunaNativeExportDescriptorV1) +
           sizeof(LunaNativeLibraryDescriptorV1) +
           sizeof(LunaRuntimeMetadataValueV1) +
           sizeof(LunaRuntimeMetadataInstanceV1) +
           sizeof(LunaRuntimeDeclarationDescriptorV1) +
           sizeof(LunaRuntimeDescriptorRegistryV1);
}
