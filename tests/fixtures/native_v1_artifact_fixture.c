#include "runtime/NativeArtifactABI.h"

#if defined(_WIN32)
#define LUNA_TEST_EXPORT __declspec(dllexport)
#define LUNA_TEST_PROOF_SECTION ".luna$proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna$desc"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION ".luna$desc2"
#elif defined(__APPLE__)
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION "__DATA,__luna_proof"
#define LUNA_TEST_DESCRIPTOR_SECTION "__DATA,__luna_desc"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION "__DATA,__luna_desc2"
#else
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION ".luna.native.proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna.native.descriptor"
#define LUNA_TEST_DESCRIPTOR_V2_SECTION ".luna.native.descriptor.v2"
#endif

#define LUNA_TEST_SECTION(name) __attribute__((section(name), used))

static int32_t legacy_answer(void) { return 7; }

// The test helper replaces this bounded field with the process target triple
// before sealing. The producer and descriptor source are otherwise v1-only.
static const char target_abi[128] = "LUNA_TEST_TARGET_ABI_PLACEHOLDER";

#ifdef LUNA_TEST_INVALID_UTF8
#define LUNA_TEST_SYMBOL_ID "symbol:\xc0\xaf"
#else
#define LUNA_TEST_SYMBOL_ID "symbol:legacy-answer"
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
