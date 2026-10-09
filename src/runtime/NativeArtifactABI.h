#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// In-memory declaration registry exposed only after an artifact's pointer-free
// proof has been verified. V1 records declaration kind and identity, but no
// callable signature or entry ABI profile. These pointers are process-local
// and are never hashed as binary bytes; the canonical field values are bound
// by export_descriptor_digest in LunaNativeProofV1.
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

// Optional parallel registry. V1 stays byte-for-byte compatible; V2 rows
// describe only entries for which the producer can prove a concrete C ABI.
// Loaders require exact abi_version, struct_size, and zero reserved fields;
// a tail extension or new entry profile needs a new parallel query version.
#define LUNA_NATIVE_DESCRIPTOR_MAGIC_V2 0x4c4e4432u /* "LND2" */
#define LUNA_NATIVE_DESCRIPTOR_ABI_V2 2u
#define LUNA_NATIVE_ENTRY_ABI_C_I32_NOARGS_V1 1u
#define LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V2 32u

typedef struct LunaNativeExportDescriptorV2 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t declaration_kind;
    uint32_t flags;
    uint32_t entry_abi;
    uint32_t reserved_zero;
    const char* symbol_id;
    const char* contract_id;
    const char* linkage_name;
    const void* entry;
} LunaNativeExportDescriptorV2;

typedef struct LunaNativeLibraryDescriptorV2 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    const char* package_id;
    const char* package_version;
    const char* target_abi;
    const char* compiler_identity;
    uint64_t export_count;
    const LunaNativeExportDescriptorV2* exports;
    uint8_t export_descriptor_digest[LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V2];
} LunaNativeLibraryDescriptorV2;

typedef const LunaNativeLibraryDescriptorV2*
    (*LunaNativeLibraryDescriptorFnV2)(void);

// Optional parallel candidate for one Ref ingress and owned Result return.
// This metadata is validation-only: no public host entry or owner-handle
// invocation is enabled by its presence. V1/V2 layouts remain unchanged.
#define LUNA_NATIVE_DESCRIPTOR_MAGIC_V3 0x4c4e4433u /* "LND3" */
#define LUNA_NATIVE_DESCRIPTOR_ABI_V3 3u
#define LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1 2u
#define LUNA_NATIVE_REF_SHARED_BORROW_V1 1u
#define LUNA_NATIVE_RESULT_I32_OWNED_ERROR_V1 1u
#define LUNA_NATIVE_STATUS_DOMAIN_REF_RESULT_OWNER_V1 1u
#define LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V3 32u

// Candidate callable contract for the v3 metadata profile. The host supplies
// a Runtime-issued live code-lease token and an empty, stable owner cell.
// This typedef does not make v3 rows callable through the current loader.
typedef int32_t (*LunaNativeRefResultOwnerEntryV1)(
    const void* parent_context, const void* borrowed_ref,
    uint32_t* tag_output, int32_t* scalar_output,
    void** owner_cell, const void* code_lease);

enum LunaNativeRefResultOwnerStatusV1 {
    LUNA_NATIVE_REF_RESULT_OWNER_SUCCESS_V1 = 0,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_OUTPUT_V1 = 1,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_RESOURCE_V1 = 2,
    LUNA_NATIVE_REF_RESULT_OWNER_EXECUTION_FAILURE_V1 = 3,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_CONTEXT_V1 = 4,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_HANDLE_V1 = 5,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_TARGET_V1 = 6,
    LUNA_NATIVE_REF_RESULT_OWNER_UNEXPECTED_CHECK_V1 = 7,
    LUNA_NATIVE_REF_RESULT_OWNER_ADOPTION_FAILURE_V1 = 8,
    LUNA_NATIVE_REF_RESULT_OWNER_INVALID_LEASE_V1 = 9,
};

typedef struct LunaNativeExportDescriptorV3 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t declaration_kind;
    uint32_t flags;
    uint32_t entry_abi;
    uint32_t ref_mode;
    uint32_t result_mode;
    uint32_t status_domain;
    uint64_t error_value_size;
    uint64_t error_value_alignment;
    const char* symbol_id;
    const char* contract_id;
    const char* linkage_name;
    const char* ref_slot_symbol_id;
    const char* ref_slot_contract_id;
    const char* result_type_id;
    const char* error_type_id;
    const char* error_abi_layout_id;
    const char* error_drop_symbol_id;
    const char* error_drop_contract_id;
    const void* entry;
} LunaNativeExportDescriptorV3;

typedef struct LunaNativeLibraryDescriptorV3 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    const char* package_id;
    const char* package_version;
    const char* target_abi;
    const char* compiler_identity;
    uint64_t export_count;
    const LunaNativeExportDescriptorV3* exports;
    uint8_t export_descriptor_digest[LUNA_NATIVE_DESCRIPTOR_DIGEST_SIZE_V3];
} LunaNativeLibraryDescriptorV3;

typedef const LunaNativeLibraryDescriptorV3*
    (*LunaNativeLibraryDescriptorFnV3)(void);

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
#define LUNA_NATIVE_LAYOUT_ASSERT static_assert
#else
_Static_assert(sizeof(LunaNativeProofV1) == 504,
               "Luna Native proof v1 layout changed");
#define LUNA_NATIVE_LAYOUT_ASSERT _Static_assert
#endif

// The current Native producer/loader targets use natural 64-bit C layout.
// Keep these values stable for old v1 readers and the parallel v2 query.
#if UINTPTR_MAX == UINT64_MAX
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeExportDescriptorV1) == 48,
                          "Native v1 export layout changed");
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeLibraryDescriptorV1) == 64,
                          "Native v1 library layout changed");
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeExportDescriptorV2) == 56 &&
                          offsetof(LunaNativeExportDescriptorV2, entry_abi) == 16 &&
                          offsetof(LunaNativeExportDescriptorV2, symbol_id) == 24 &&
                          offsetof(LunaNativeExportDescriptorV2, entry) == 48,
                          "Native v2 export layout changed");
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeLibraryDescriptorV2) == 96 &&
                          offsetof(LunaNativeLibraryDescriptorV2, export_count) == 48 &&
                          offsetof(LunaNativeLibraryDescriptorV2, exports) == 56 &&
                          offsetof(LunaNativeLibraryDescriptorV2,
                                   export_descriptor_digest) == 64,
                          "Native v2 library layout changed");
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeExportDescriptorV3) == 136 &&
                          offsetof(LunaNativeExportDescriptorV3, entry_abi) == 16 &&
                          offsetof(LunaNativeExportDescriptorV3, error_value_size) == 32 &&
                          offsetof(LunaNativeExportDescriptorV3, symbol_id) == 48 &&
                          offsetof(LunaNativeExportDescriptorV3, entry) == 128,
                          "Native v3 export layout changed");
LUNA_NATIVE_LAYOUT_ASSERT(sizeof(LunaNativeLibraryDescriptorV3) == 96 &&
                          offsetof(LunaNativeLibraryDescriptorV3,
                                   export_descriptor_digest) == 64,
                          "Native v3 library layout changed");
#endif
#undef LUNA_NATIVE_LAYOUT_ASSERT
