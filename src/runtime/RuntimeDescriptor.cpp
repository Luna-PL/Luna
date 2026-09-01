#include "RuntimeDescriptor.h"

#include <algorithm>
#include <cstring>

namespace luna::runtime {
namespace {

constexpr uint64_t MaxDescriptorCount = 1u << 20;
constexpr uint64_t MaxMetadataCount = 1u << 20;
constexpr size_t MaxIdentityBytes = 4096;

bool validText(const char* text, bool allowEmpty = false) {
    if (!text) return false;
    size_t length = 0;
    while (length < MaxIdentityBytes && text[length] != '\0') {
        if (text[length] == '\r' || text[length] == '\n' ||
            text[length] == '\t')
            return false;
        ++length;
    }
    return length != MaxIdentityBytes && (allowEmpty || length != 0);
}

bool validateMetadata(const LunaRuntimeMetadataInstanceV1& metadata) {
    if (metadata.abi_version != LUNA_RUNTIME_DESCRIPTOR_ABI_V1 ||
        metadata.struct_size < sizeof(LunaRuntimeMetadataInstanceV1) ||
        metadata.reserved_zero != 0 ||
        metadata.retention != LUNA_RUNTIME_RETENTION_RUNTIME_V1 ||
        !validText(metadata.schema_id) ||
        metadata.value_count > MaxMetadataCount ||
        (metadata.value_count != 0 && !metadata.values))
        return false;
    for (uint64_t index = 0; index < metadata.value_count; ++index) {
        const auto& value = metadata.values[index];
        if (value.reserved_zero != 0 ||
            value.kind > LUNA_RUNTIME_METADATA_STRING_V1)
            return false;
        if (value.kind == LUNA_RUNTIME_METADATA_BOOLEAN_V1 && value.payload > 1)
            return false;
        if (value.kind == LUNA_RUNTIME_METADATA_STRING_V1) {
            if (!validText(value.string_value, true)) return false;
        } else if (value.string_value) {
            return false;
        }
    }
    return true;
}

bool validateDescriptor(const LunaRuntimeDeclarationDescriptorV1& descriptor) {
    if (descriptor.magic != LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1 ||
        descriptor.abi_version != LUNA_RUNTIME_DESCRIPTOR_ABI_V1 ||
        descriptor.struct_size < sizeof(LunaRuntimeDeclarationDescriptorV1) ||
        descriptor.declaration_kind < LUNA_RUNTIME_DECLARATION_FUNCTION_V1 ||
        descriptor.declaration_kind >
            LUNA_RUNTIME_DECLARATION_SLOT_V1 ||
        (descriptor.flags & ~LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) != 0 ||
        descriptor.retention != LUNA_RUNTIME_RETENTION_RUNTIME_V1 ||
        descriptor.reserved_zero_0 != 0 || descriptor.reserved_zero_1 != 0 ||
        !validText(descriptor.symbol_id) ||
        !validText(descriptor.contract_id) || !validText(descriptor.type_id) ||
        !validText(descriptor.linkage_name, true) ||
        descriptor.metadata_count > MaxMetadataCount ||
        (descriptor.metadata_count != 0 && !descriptor.metadata) ||
        (descriptor.retention == LUNA_RUNTIME_RETENTION_COMPILE_TIME_V1 &&
         descriptor.metadata_count == 0))
        return false;
    const bool callable =
        (descriptor.flags & LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) != 0;
    if (callable != (descriptor.entry != nullptr) ||
        (callable && descriptor.declaration_kind !=
            LUNA_RUNTIME_DECLARATION_FUNCTION_V1))
        return false;
    for (uint64_t index = 0; index < descriptor.metadata_count; ++index) {
        if (!validateMetadata(descriptor.metadata[index])) return false;
    }
    return true;
}

} // namespace

bool RuntimeDescriptorRegistryView::bind(
    const LunaRuntimeDescriptorRegistryV1* registry, std::string& error) {
    error.clear();
    if (registry_) {
        error = "Runtime descriptor view is already bound";
        return false;
    }
    if (!registry || registry->magic != LUNA_RUNTIME_REGISTRY_MAGIC_V1 ||
        registry->abi_version != LUNA_RUNTIME_DESCRIPTOR_ABI_V1 ||
        registry->struct_size < sizeof(LunaRuntimeDescriptorRegistryV1) ||
        registry->reserved_zero != 0 || !validText(registry->module_id) ||
        registry->descriptor_count > MaxDescriptorCount ||
        (registry->descriptor_count != 0 && !registry->descriptors)) {
        error = "Runtime descriptor registry header is invalid";
        return false;
    }
    const char* previousSymbol = nullptr;
    for (uint64_t index = 0; index < registry->descriptor_count; ++index) {
        const auto* descriptor = registry->descriptors[index];
        if (!descriptor || !validateDescriptor(*descriptor)) {
            error = "Runtime descriptor registry contains an invalid row";
            return false;
        }
        if (previousSymbol &&
            std::strcmp(previousSymbol, descriptor->symbol_id) >= 0) {
            error = "Runtime descriptor registry is not strictly SymbolId ordered";
            return false;
        }
        previousSymbol = descriptor->symbol_id;
    }
    registry_ = registry;
    moduleId_ = registry->module_id;
    return true;
}

uint64_t RuntimeDescriptorRegistryView::size() const {
    return registry_ ? registry_->descriptor_count : 0;
}

const LunaRuntimeDeclarationDescriptorV1*
RuntimeDescriptorRegistryView::at(uint64_t index) const {
    return registry_ && index < registry_->descriptor_count
        ? registry_->descriptors[index] : nullptr;
}

const LunaRuntimeDeclarationDescriptorV1*
RuntimeDescriptorRegistryView::find(
    const std::string& symbolId, const std::string& contractId,
    uint32_t declarationKind, uint32_t requiredFlags) const {
    if (!registry_ || symbolId.empty() || contractId.empty() ||
        declarationKind == 0 ||
        (requiredFlags & ~LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) != 0)
        return nullptr;
    if (registry_->descriptor_count == 0) return nullptr;
    const auto* begin = registry_->descriptors;
    const auto* end = begin + registry_->descriptor_count;
    const auto found = std::lower_bound(
        begin, end, symbolId,
        [](const LunaRuntimeDeclarationDescriptorV1* descriptor,
           const std::string& identity) {
            return std::strcmp(descriptor->symbol_id, identity.c_str()) < 0;
        });
    if (found == end || std::strcmp((*found)->symbol_id, symbolId.c_str()) != 0 ||
        std::strcmp((*found)->contract_id, contractId.c_str()) != 0 ||
        (*found)->declaration_kind != declarationKind ||
        ((*found)->flags & requiredFlags) != requiredFlags)
        return nullptr;
    return *found;
}

} // namespace luna::runtime
