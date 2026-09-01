#include "runtime/RuntimeDescriptor.h"

#include <iostream>
#include <string>

namespace {

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

} // namespace

int main() {
    const LunaRuntimeMetadataValueV1 values[] = {
        {LUNA_RUNTIME_METADATA_INTEGER_V1, 0, 7, nullptr},
        {LUNA_RUNTIME_METADATA_STRING_V1, 0, 0, "stable"},
    };
    const LunaRuntimeMetadataInstanceV1 metadata[] = {{
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeMetadataInstanceV1),
        LUNA_RUNTIME_RETENTION_RUNTIME_V1,
        0,
        "meta:revision",
        2,
        values,
    }};
    LunaRuntimeDeclarationDescriptorV1 function = {
        LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDeclarationDescriptorV1),
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1,
        LUNA_RUNTIME_RETENTION_RUNTIME_V1,
        0,
        0,
        "symbol:a",
        "contract:fn-i32",
        "type:fn-i32",
        "fixture_answer",
        1,
        metadata,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(1)),
    };
    LunaRuntimeDeclarationDescriptorV1 structure = {
        LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDeclarationDescriptorV1),
        LUNA_RUNTIME_DECLARATION_STRUCT_V1,
        0,
        LUNA_RUNTIME_RETENTION_RUNTIME_V1,
        0,
        0,
        "symbol:b",
        "contract:record",
        "type:record",
        "",
        0,
        nullptr,
        nullptr,
    };
    const LunaRuntimeDeclarationDescriptorV1* rows[] = {
        &function, &structure};
    LunaRuntimeDescriptorRegistryV1 registry = {
        LUNA_RUNTIME_REGISTRY_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDescriptorRegistryV1),
        0,
        "org.luna.fixture.runtime-descriptor",
        2,
        rows,
    };

    luna::runtime::RuntimeDescriptorRegistryView view;
    std::string error;
    if (!view.bind(&registry, error) || view.size() != 2 ||
        view.moduleId() != registry.module_id || view.at(1) != &structure)
        return fail("valid Runtime descriptor registry did not bind");
    if (view.find(
            function.symbol_id, function.contract_id,
            LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
            LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) != &function)
        return fail("exact typed Runtime descriptor lookup failed");
    if (view.find(
            function.symbol_id, "contract:wrong",
            LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
            LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) ||
        view.find(
            function.symbol_id, function.contract_id,
            LUNA_RUNTIME_DECLARATION_STRUCT_V1,
            LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1) ||
        view.find(
            function.symbol_id, function.contract_id,
            LUNA_RUNTIME_DECLARATION_FUNCTION_V1, 1u << 8))
        return fail("typed Runtime descriptor lookup accepted a mismatch");

    const LunaRuntimeDeclarationDescriptorV1* reversedRows[] = {
        &structure, &function};
    auto reversed = registry;
    reversed.descriptors = reversedRows;
    luna::runtime::RuntimeDescriptorRegistryView reversedView;
    if (reversedView.bind(&reversed, error) ||
        error.find("SymbolId ordered") == std::string::npos)
        return fail("Runtime descriptor registry accepted unstable ordering");

    auto nonFunctionCallable = structure;
    nonFunctionCallable.flags = LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1;
    nonFunctionCallable.entry = function.entry;
    const LunaRuntimeDeclarationDescriptorV1* invalidRows[] = {
        &function, &nonFunctionCallable};
    auto invalid = registry;
    invalid.descriptors = invalidRows;
    luna::runtime::RuntimeDescriptorRegistryView invalidView;
    if (invalidView.bind(&invalid, error) ||
        error.find("invalid row") == std::string::npos)
        return fail("Runtime descriptor registry accepted a callable non-function");

    auto badValue = values[0];
    badValue.kind = LUNA_RUNTIME_METADATA_BOOLEAN_V1;
    badValue.payload = 2;
    auto badMetadata = metadata[0];
    badMetadata.values = &badValue;
    badMetadata.value_count = 1;
    auto badFunction = function;
    badFunction.metadata = &badMetadata;
    const LunaRuntimeDeclarationDescriptorV1* badMetadataRows[] = {
        &badFunction, &structure};
    auto badMetadataRegistry = registry;
    badMetadataRegistry.descriptors = badMetadataRows;
    luna::runtime::RuntimeDescriptorRegistryView badMetadataView;
    if (badMetadataView.bind(&badMetadataRegistry, error))
        return fail("Runtime descriptor registry accepted malformed metadata");

    auto wrongAbi = registry;
    wrongAbi.abi_version += 1;
    luna::runtime::RuntimeDescriptorRegistryView wrongAbiView;
    if (wrongAbiView.bind(&wrongAbi, error))
        return fail("Runtime descriptor registry accepted an unknown ABI");

    LunaRuntimeDescriptorRegistryV1 emptyRegistry = {
        LUNA_RUNTIME_REGISTRY_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDescriptorRegistryV1),
        0,
        "org.luna.fixture.empty-runtime-descriptor",
        0,
        nullptr,
    };
    luna::runtime::RuntimeDescriptorRegistryView emptyView;
    if (!emptyView.bind(&emptyRegistry, error) || emptyView.size() != 0 ||
        emptyView.find("symbol:none", "contract:none",
                       LUNA_RUNTIME_DECLARATION_FUNCTION_V1, 0))
        return fail("empty Runtime descriptor registry is not a valid empty view");
    return 0;
}
