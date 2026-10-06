#include "runtime/NativeArtifactABI.h"

// Compile this file with a freestanding 32-bit target to check the candidate
// C record layout without requiring 32-bit runtime libraries or a linker.
// This does not establish 32-bit loader or artifact support.
#if UINTPTR_MAX == UINT32_MAX
_Static_assert(sizeof(LunaNativeExportDescriptorV1) == 32,
               "32-bit Native v1 export layout changed");
#if defined(_WIN32)
_Static_assert(sizeof(LunaNativeLibraryDescriptorV1) == 48,
               "32-bit Windows Native v1 library layout changed");
#else
_Static_assert(sizeof(LunaNativeLibraryDescriptorV1) == 44,
               "32-bit Native v1 library layout changed");
#endif
_Static_assert(sizeof(LunaNativeExportDescriptorV2) == 40 &&
                   offsetof(LunaNativeExportDescriptorV2, entry_abi) == 16 &&
                   offsetof(LunaNativeExportDescriptorV2, symbol_id) == 24 &&
                   offsetof(LunaNativeExportDescriptorV2, entry) == 36,
               "32-bit Native v2 export layout changed");
_Static_assert(offsetof(LunaNativeLibraryDescriptorV2, export_count) == 32 &&
                   offsetof(LunaNativeLibraryDescriptorV2, exports) == 40 &&
                   offsetof(LunaNativeLibraryDescriptorV2,
                            export_descriptor_digest) == 44,
               "32-bit Native v2 library field offsets changed");
#if defined(_WIN32)
_Static_assert(sizeof(LunaNativeLibraryDescriptorV2) == 80,
               "32-bit Windows Native v2 library size changed");
#else
_Static_assert(sizeof(LunaNativeLibraryDescriptorV2) == 76,
               "32-bit Native v2 library size changed");
#endif
#endif
