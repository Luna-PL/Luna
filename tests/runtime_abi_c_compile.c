#include "runtime/NativeArtifactABI.h"
#include "runtime/RuntimeABI.h"
#include "runtime/RuntimeDescriptorABI.h"
#include "runtime/RuntimeFragmentABI.h"

_Static_assert(sizeof(LunaNativeProofV1) == 504,
               "Native proof ABI must remain usable from C");
_Static_assert(LUNA_RUNTIME_DECLARATION_SLOT_V1 == 8,
               "Runtime Slot declaration kind must remain stable");
_Static_assert(LUNA_NATIVE_DECLARATION_SLOT_V1 == 8,
               "Native Slot declaration kind must remain stable");
_Static_assert(LUNA_RUNTIME_FRAGMENT_ABI_V1 == 1,
               "Runtime Fragment ABI version must remain stable");
_Static_assert(LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1 == 0,
               "Runtime Fragment dispatch success must remain zero");
_Static_assert(LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1 == 0,
               "Native Ref bridge check success must remain zero");
_Static_assert(LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1 == (1u << 3),
               "Runtime Fragment-context callable flag must remain stable");
_Static_assert(sizeof(LunaDeviceBufferI32V1) == sizeof(void*) + sizeof(size_t),
               "Device-buffer carrier must remain two contiguous ABI words");
_Static_assert(offsetof(LunaDeviceBufferI32V1, data) == 0,
               "Device-buffer data must be the first ABI field");
_Static_assert(offsetof(LunaDeviceBufferI32V1, length) == sizeof(void*),
               "Device-buffer length must follow the data pointer");

// Build-only C translation unit: the public host ABI must not require C++.
size_t luna_runtime_abi_c_layout_probe(void) {
    void* (*allocate)(size_t, size_t) = rt_alloc;
    void (*deallocate)(void*, size_t, size_t) = rt_dealloc;
    int (*snapshot_error)(uint32_t, LunaRuntimeErrorSnapshotV1*, char*, size_t) =
        rt_runtime_error_snapshot_v1;
    LunaFileHandleV1 handle = LUNA_INVALID_FILE_HANDLE_V1;
    LunaNativeLibraryDescriptorFnV1 native_descriptor = 0;
    LunaRuntimeDescriptorRegistryV1 runtime_registry = {0};
    LunaRuntimeFragmentDescriptorV1 runtime_fragment = {0};
    int32_t (*ref_check)(const void*, const char*, const char*) =
        luna_runtime_fragment_ref_check_v1;
    void (*ref_drop)(void**) = luna_runtime_fragment_ref_drop_v1;
    (void)allocate;
    (void)deallocate;
    (void)snapshot_error;
    (void)handle;
    (void)native_descriptor;
    (void)runtime_registry;
    (void)runtime_fragment;
    (void)ref_check;
    (void)ref_drop;
    (void)&luna_runtime_fragment_dispatch_v1;
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
           sizeof(LunaDeviceBufferI32V1) +
           sizeof(LunaNativeExportDescriptorV1) +
           sizeof(LunaNativeLibraryDescriptorV1) +
           sizeof(LunaRuntimeMetadataValueV1) +
           sizeof(LunaRuntimeMetadataInstanceV1) +
           sizeof(LunaRuntimeDeclarationDescriptorV1) +
           sizeof(LunaRuntimeDescriptorRegistryV1) +
           sizeof(LunaRuntimeFragmentDescriptorV1);
}
