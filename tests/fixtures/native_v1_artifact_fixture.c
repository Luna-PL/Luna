#include "runtime/NativeArtifactABI.h"

#if defined(_WIN32)
#define LUNA_TEST_EXPORT __declspec(dllexport)
#define LUNA_TEST_PROOF_SECTION ".luna$proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna$desc"
#elif defined(__APPLE__)
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION "__DATA,__luna_proof"
#define LUNA_TEST_DESCRIPTOR_SECTION "__DATA,__luna_desc"
#else
#define LUNA_TEST_EXPORT __attribute__((visibility("default")))
#define LUNA_TEST_PROOF_SECTION ".luna.native.proof"
#define LUNA_TEST_DESCRIPTOR_SECTION ".luna.native.descriptor"
#endif

#define LUNA_TEST_SECTION(name) __attribute__((section(name), used))

static int32_t legacy_answer(void) { return 7; }

// The test helper replaces this bounded field with the process target triple
// before sealing. The producer and descriptor source are otherwise v1-only.
static const char target_abi[128] = "LUNA_TEST_TARGET_ABI_PLACEHOLDER";

static const LunaNativeExportDescriptorV1 exports[] = {{
    LUNA_NATIVE_DESCRIPTOR_ABI_V1,
    sizeof(LunaNativeExportDescriptorV1),
    LUNA_NATIVE_DECLARATION_FUNCTION_V1,
    LUNA_NATIVE_EXPORT_CALLABLE_V1,
    "symbol:legacy-answer",
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

const LunaNativeProofV1 luna_native_proof_v1
    LUNA_TEST_SECTION(LUNA_TEST_PROOF_SECTION) = {
        {'L', 'U', 'N', 'A', 'N', 'P', '1', 0},
        LUNA_NATIVE_PROOF_ABI_V1,
        sizeof(LunaNativeProofV1),
        LUNA_NATIVE_PROOF_DIGEST_SHA256,
        0,
    };
