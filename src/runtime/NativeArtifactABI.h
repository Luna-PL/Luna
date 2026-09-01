#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// In-memory typed registry exposed only after an artifact's pointer-free proof
// has been verified. These pointers are process-local and are never hashed as
// binary bytes; the canonical field values are bound by
// export_descriptor_digest in LunaNativeProofV1.
#define LUNA_NATIVE_DESCRIPTOR_MAGIC_V1 0x4c4e4431u /* "LND1" */
#define LUNA_NATIVE_DESCRIPTOR_ABI_V1 1u

enum LunaNativeDeclarationKindV1 {
    LUNA_NATIVE_DECLARATION_FUNCTION_V1 = 1,
    LUNA_NATIVE_DECLARATION_FRAGMENT_V1 = 2,
    LUNA_NATIVE_DECLARATION_STRUCT_V1 = 3,
    LUNA_NATIVE_DECLARATION_ENUM_V1 = 4,
    LUNA_NATIVE_DECLARATION_TRAIT_V1 = 5,
    LUNA_NATIVE_DECLARATION_IMPLEMENTATION_V1 = 6,
    LUNA_NATIVE_DECLARATION_METADATA_SCHEMA_V1 = 7,
    LUNA_NATIVE_DECLARATION_SLOT_V1 = 8,
};

enum LunaNativeExportFlagV1 {
    LUNA_NATIVE_EXPORT_CALLABLE_V1 = 1u << 0,
};

typedef struct LunaNativeExportDescriptorV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t declaration_kind;
    uint32_t flags;
    const char* symbol_id;
    const char* contract_id;
    const char* linkage_name;
    const void* entry;
} LunaNativeExportDescriptorV1;

typedef struct LunaNativeLibraryDescriptorV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    const char* package_id;
    const char* package_version;
    const char* target_abi;
    const char* compiler_identity;
    uint64_t export_count;
    const LunaNativeExportDescriptorV1* exports;
} LunaNativeLibraryDescriptorV1;

typedef const LunaNativeLibraryDescriptorV1*
    (*LunaNativeLibraryDescriptorFnV1)(void);

// Pointer-free proof record embedded in a platform-native section. The
// artifact digest is SHA-256 over the complete file with this entire record
// replaced by zero bytes. This removes the proof section's self-reference
// while binding every other byte of the shared library.
#define LUNA_NATIVE_PROOF_MAGIC_V1 "LUNANP1"
#define LUNA_NATIVE_PROOF_ABI_V1 1u
#define LUNA_NATIVE_PROOF_DIGEST_SHA256 1u
#define LUNA_NATIVE_PROOF_DIGEST_SIZE 32u
#define LUNA_NATIVE_PROOF_PACKAGE_ID_SIZE 128u
#define LUNA_NATIVE_PROOF_PACKAGE_VERSION_SIZE 32u
#define LUNA_NATIVE_PROOF_TARGET_ABI_SIZE 128u
#define LUNA_NATIVE_PROOF_COMPILER_ID_SIZE 96u

#if defined(_MSC_VER)
#pragma pack(push, 1)
#define LUNA_NATIVE_PACKED
#else
#define LUNA_NATIVE_PACKED __attribute__((packed))
#endif

typedef struct LUNA_NATIVE_PACKED LunaNativeProofV1 {
    uint8_t magic[8];
    uint32_t abi_version;
    uint32_t record_size;
    uint32_t digest_algorithm;
    uint32_t reserved_zero;
    uint8_t artifact_digest[LUNA_NATIVE_PROOF_DIGEST_SIZE];
    uint8_t export_descriptor_digest[LUNA_NATIVE_PROOF_DIGEST_SIZE];
    uint8_t foreign_dependency_digest[LUNA_NATIVE_PROOF_DIGEST_SIZE];
    char package_id[LUNA_NATIVE_PROOF_PACKAGE_ID_SIZE];
    char package_version[LUNA_NATIVE_PROOF_PACKAGE_VERSION_SIZE];
    char target_abi[LUNA_NATIVE_PROOF_TARGET_ABI_SIZE];
    char compiler_identity[LUNA_NATIVE_PROOF_COMPILER_ID_SIZE];
} LunaNativeProofV1;

#if defined(_MSC_VER)
#pragma pack(pop)
#endif
#undef LUNA_NATIVE_PACKED

#define LUNA_NATIVE_PROOF_ARTIFACT_DIGEST_OFFSET \
    offsetof(LunaNativeProofV1, artifact_digest)

#ifdef __cplusplus
}
static_assert(sizeof(LunaNativeProofV1) == 504,
              "Luna Native proof v1 layout changed");
#else
_Static_assert(sizeof(LunaNativeProofV1) == 504,
               "Luna Native proof v1 layout changed");
#endif
