#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Stable in-memory ABI for descriptor-backed Runtime discovery. These records
// live inside a verified Moon/Native module image and are never accepted as a
// substitute for container/proof verification.
#define LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1 0x4c524431u /* "LRD1" */
#define LUNA_RUNTIME_REGISTRY_MAGIC_V1 0x4c525231u   /* "LRR1" */
#define LUNA_RUNTIME_DESCRIPTOR_ABI_V1 1u

enum LunaRuntimeDeclarationKindV1 {
    LUNA_RUNTIME_DECLARATION_FUNCTION_V1 = 1,
    LUNA_RUNTIME_DECLARATION_FRAGMENT_V1 = 2,
    LUNA_RUNTIME_DECLARATION_STRUCT_V1 = 3,
    LUNA_RUNTIME_DECLARATION_ENUM_V1 = 4,
    LUNA_RUNTIME_DECLARATION_TRAIT_V1 = 5,
    LUNA_RUNTIME_DECLARATION_IMPLEMENTATION_V1 = 6,
    LUNA_RUNTIME_DECLARATION_METADATA_SCHEMA_V1 = 7,
    LUNA_RUNTIME_DECLARATION_SLOT_V1 = 8,
};

enum LunaRuntimeDescriptorFlagV1 {
    LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 = 1u << 0,
};

enum LunaRuntimeRetentionV1 {
    LUNA_RUNTIME_RETENTION_COMPILE_TIME_V1 = 0,
    LUNA_RUNTIME_RETENTION_RUNTIME_V1 = 1,
};

enum LunaRuntimeMetadataValueKindV1 {
    LUNA_RUNTIME_METADATA_INTEGER_V1 = 0,
    LUNA_RUNTIME_METADATA_FLOAT_V1 = 1,
    LUNA_RUNTIME_METADATA_BOOLEAN_V1 = 2,
    LUNA_RUNTIME_METADATA_STRING_V1 = 3,
};

typedef struct LunaRuntimeMetadataValueV1 {
    uint32_t kind;
    uint32_t reserved_zero;
    uint64_t payload;
    const char* string_value;
} LunaRuntimeMetadataValueV1;

typedef struct LunaRuntimeMetadataInstanceV1 {
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t retention;
    uint32_t reserved_zero;
    const char* schema_id;
    uint64_t value_count;
    const LunaRuntimeMetadataValueV1* values;
} LunaRuntimeMetadataInstanceV1;

typedef struct LunaRuntimeDeclarationDescriptorV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t declaration_kind;
    uint32_t flags;
    uint32_t retention;
    uint32_t reserved_zero_0;
    uint32_t reserved_zero_1;
    const char* symbol_id;
    const char* contract_id;
    const char* type_id;
    const char* linkage_name;
    uint64_t metadata_count;
    const LunaRuntimeMetadataInstanceV1* metadata;
    const void* entry;
} LunaRuntimeDeclarationDescriptorV1;

typedef struct LunaRuntimeDescriptorRegistryV1 {
    uint32_t magic;
    uint32_t abi_version;
    uint32_t struct_size;
    uint32_t reserved_zero;
    const char* module_id;
    uint64_t descriptor_count;
    const LunaRuntimeDeclarationDescriptorV1* const* descriptors;
} LunaRuntimeDescriptorRegistryV1;

#ifdef __cplusplus
}
#endif
