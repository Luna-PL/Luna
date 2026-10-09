#include "runtime/NativeArtifactABI.h"

#if defined(_WIN32)
#define LUNA_TEST_EXPORT __declspec(dllexport)
#define LUNA_TEST_PROOF_SECTION ".luna$proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna$desc"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION ".luna$desc2"
#define LUNA_TEST_DESCRIPTOR_V3_SECTION ".luna$desc3"
#elif defined(__APPLE__)
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION "__DATA,__luna_proof"
#define LUNA_TEST_DESCRIPTOR_SECTION "__DATA,__luna_desc"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION "__DATA,__luna_desc2"
#define LUNA_TEST_DESCRIPTOR_V3_SECTION "__DATA,__luna_desc3"
#else
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION ".luna.native.proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna.native.descriptor"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION ".luna.native.descriptor.v2"
#define LUNA_TEST_DESCRIPTOR_V3_SECTION ".luna.native.descriptor.v3"
#endif

#define LUNA_TEST_SECTION(name) __attribute__((section(name), used))

#ifdef LUNA_TEST_V3_DESCRIPTOR
typedef struct OwnedError { uint64_t marker; } OwnedError;
static int32_t legacy_answer(const void* parent, const void* ref,
                             uint32_t* tag, int32_t* scalar, void** owner) {
    (void)parent; (void)ref; (void)tag; (void)scalar; (void)owner;
    return 1; // The candidate entry is deliberately never invoked by the host.
}
#else
static int32_t legacy_answer(void) { return 7; }
#endif

// The test helper replaces this bounded field with the process target triple
// before sealing. The producer and descriptor source are otherwise v1-only.
static const char target_abi[128] = "LUNA_TEST_TARGET_ABI_PLACEHOLDER";

#ifdef LUNA_TEST_INVALID_UTF8
#define LUNA_TEST_SYMBOL_ID "symbol:\xc0\xaf"
#else
#define LUNA_TEST_SYMBOL_ID "symbol:legacy-answer"
#endif

#ifdef LUNA_TEST_V3_DESCRIPTOR
#ifdef LUNA_TEST_V3_BAD_ROW_SIZE
#define LUNA_TEST_V3_ROW_SIZE (sizeof(LunaNativeExportDescriptorV3) + 8)
#else
#define LUNA_TEST_V3_ROW_SIZE sizeof(LunaNativeExportDescriptorV3)
#endif
#ifdef LUNA_TEST_V3_BAD_PROFILE
#define LUNA_TEST_V3_ENTRY_ABI 99u
#else
#define LUNA_TEST_V3_ENTRY_ABI LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1
#endif
#ifdef LUNA_TEST_V3_BAD_SLOT_DIGEST
#define LUNA_TEST_V3_SLOT_CONTRACT "contract:other-slot"
#else
#define LUNA_TEST_V3_SLOT_CONTRACT "contract:slot-checkpoint"
#endif
#ifdef LUNA_TEST_V3_BAD_IDENTITY
#define LUNA_TEST_V3_CONTRACT "contract:other-function"
#else
#define LUNA_TEST_V3_CONTRACT "contract:legacy-v1"
#endif

static const LunaNativeExportDescriptorV3 owned_exports[] = {{
    LUNA_NATIVE_DESCRIPTOR_ABI_V3,
    LUNA_TEST_V3_ROW_SIZE,
    LUNA_NATIVE_DECLARATION_FUNCTION_V1,
    LUNA_NATIVE_EXPORT_CALLABLE_V1,
    LUNA_TEST_V3_ENTRY_ABI,
    LUNA_NATIVE_REF_SHARED_BORROW_V1,
    LUNA_NATIVE_RESULT_I32_OWNED_ERROR_V1,
    LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1,
    sizeof(OwnedError),
    _Alignof(OwnedError),
    LUNA_TEST_SYMBOL_ID,
    LUNA_TEST_V3_CONTRACT,
    "legacy_answer",
    "symbol:slot-checkpoint",
    LUNA_TEST_V3_SLOT_CONTRACT,
    "type:result-i32-owned",
    "type:owned-error",
    "layout:owned-error-v1",
    "symbol:drop-owned-error",
    "contract:drop-owned-error",
    (const void*)&legacy_answer,
}};

static const LunaNativeLibraryDescriptorV3 owned_library
    LUNA_TEST_SECTION(LUNA_TEST_DESCRIPTOR_V3_SECTION) = {
        LUNA_NATIVE_DESCRIPTOR_MAGIC_V3,
        LUNA_NATIVE_DESCRIPTOR_ABI_V3,
        sizeof(LunaNativeLibraryDescriptorV3),
        0,
        "org.luna.fixture.native_v1",
        "1.0.0",
        target_abi,
        "luna-v1-compat-fixture",
        1,
        owned_exports,
        "V3_DIGEST_PLACEHOLDER_0123456789",
    };

LUNA_TEST_EXPORT const LunaNativeLibraryDescriptorV3*
luna_native_library_descriptor_v3(void) {
    return &owned_library;
}
#endif

static const LunaNativeExportDescriptorV1 exports[] = {{
    LUNA_NATIVE_DESCRIPTOR_ABI_V1,
    sizeof(LunaNativeExportDescriptorV1),
    LUNA_NATIVE_DECLARATION_FUNCTION_V1,
    LUNA_NATIVE_EXPORT_CALLABLE_V1,
    LUNA_TEST_SYMBOL_ID,
    "contract:legacy-v1",
    "legacy_answer",
    (const void*)&legacy_answer,
}};

static const LunaNativeLibraryDescriptorV1 library
    LUNA_TEST_SECTION(LUNA_TEST_DESCRIPTOR_SECTION) = {
        LUNA_NATIVE_DESCRIPTOR_MAGIC_V1,
        LUNA_NATIVE_DESCRIPTOR_ABI_V1,
        sizeof(LunaNativeLibraryDescriptorV1),
        0,
        "org.luna.fixture.native_v1",
        "1.0.0",
        target_abi,
        "luna-v1-compat-fixture",
        1,
        exports,
    };

LUNA_TEST_EXPORT const LunaNativeLibraryDescriptorV1*
luna_native_library_descriptor_v1(void) {
    return &library;
}

#ifdef LUNA_TEST_V2_DESCRIPTOR
#ifdef LUNA_TEST_V2_BAD_ROW_SIZE
#define LUNA_TEST_V2_ROW_SIZE (sizeof(LunaNativeExportDescriptorV2) + 8)
#else
#define LUNA_TEST_V2_ROW_SIZE sizeof(LunaNativeExportDescriptorV2)
#endif

static const LunaNativeExportDescriptorV2 typed_exports[] = {{
    LUNA_NATIVE_DESCRIPTOR_ABI_V2,
    LUNA_TEST_V2_ROW_SIZE,
    LUNA_NATIVE_DECLARATION_FUNCTION_V1,
    LUNA_NATIVE_EXPORT_CALLABLE_V1,
    LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1,
    0,
    LUNA_TEST_SYMBOL_ID,
    "contract:legacy-v1",
    "legacy_answer",
    (const void*)&legacy_answer,
}};

static const LunaNativeLibraryDescriptorV2 typed_library
    LUNA_TEST_SECTION(LUNA_TEST_DESCRIPTOR_V2_SECTION) = {
        LUNA_NATIVE_DESCRIPTOR_MAGIC_V2,
        LUNA_NATIVE_DESCRIPTOR_ABI_V2,
        sizeof(LunaNativeLibraryDescriptorV2),
        0,
        "org.luna.fixture.native_v1",
        "1.0.0",
        target_abi,
        "luna-v1-compat-fixture",
        1,
        typed_exports,
        "V2_DIGEST_PLACEHOLDER_0123456789",
    };

LUNA_TEST_EXPORT const LunaNativeLibraryDescriptorV2*
luna_native_library_descriptor_v2(void) {
    return &typed_library;
}
#endif

const LunaNativeProofV1 luna_native_proof_v1
    LUNA_TEST_SECTION(LUNA_TEST_PROOF_SECTION) = {
        {'L', 'U', 'N', 'A', 'N', 'P', '1', 0},
        LUNA_NATIVE_PROOF_ABI_V1,
        sizeof(LunaNativeProofV1),
        LUNA_NATIVE_PROOF_DIGEST_SHA256,
        0,
    };
