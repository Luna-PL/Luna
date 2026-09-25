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
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1,
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
        LUNA_RUNTIME_RETENTION_COMPILE_TIME_V1,
        0,
        0,
        "symbol:c",
        "contract:record",
        "type:record",
        "",
        1,
        metadata,
        nullptr,
    };
    LunaRuntimeDeclarationDescriptorV1 fragment = {
        LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDeclarationDescriptorV1),
        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
        LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1,
        LUNA_RUNTIME_RETENTION_RUNTIME_V1,
        0,
        0,
        "symbol:b",
        "contract:fragment",
        "type:fragment",
        "fixture_fragment_descriptor",
        0,
        nullptr,
        reinterpret_cast<const void*>(static_cast<uintptr_t>(2)),
    };
    LunaRuntimeDeclarationDescriptorV1 slot = {
        LUNA_RUNTIME_DESCRIPTOR_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDeclarationDescriptorV1),
        LUNA_RUNTIME_DECLARATION_SLOT_V1,
        LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1,
        LUNA_RUNTIME_RETENTION_COMPILE_TIME_V1,
        0,
        0,
        "symbol:d",
        "contract:slot",
        "type:slot",
        "published_slot",
        0,
        nullptr,
        nullptr,
    };
    const LunaRuntimeDeclarationDescriptorV1* rows[] = {
        &function, &fragment, &structure, &slot};
    LunaRuntimeDescriptorRegistryV1 registry = {
        LUNA_RUNTIME_REGISTRY_MAGIC_V1,
        LUNA_RUNTIME_DESCRIPTOR_ABI_V1,
        sizeof(LunaRuntimeDescriptorRegistryV1),
        0,
        "org.luna.fixture.runtime-descriptor",
        4,
        rows,
    };

    luna::runtime::RuntimeDescriptorRegistryView view;
    std::string error;
    if (!view.bind(&registry, error) || view.size() != 4 ||
        view.moduleId() != registry.module_id || view.at(2) != &structure)
        return fail("valid Runtime descriptor registry did not bind");
    if (view.find(
            function.symbol_id, function.contract_id,
            LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
            LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
                LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1) != &function)
        return fail("exact typed Runtime descriptor lookup failed");
    if (view.find(
            fragment.symbol_id, fragment.contract_id,
            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1) != &fragment)
        return fail("executable Fragment descriptor lookup failed");
    if (view.find(
            slot.symbol_id, slot.contract_id,
            LUNA_RUNTIME_DECLARATION_SLOT_V1,
            LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1) != &slot)
        return fail("public compile-time Slot descriptor lookup failed");
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
        &structure, &fragment, &function};
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
    invalid.descriptor_count = 2;
    luna::runtime::RuntimeDescriptorRegistryView invalidView;
    if (invalidView.bind(&invalid, error) ||
        error.find("invalid row") == std::string::npos)
        return fail("Runtime descriptor registry accepted a callable non-function");

    auto contextWithoutCallable = function;
    contextWithoutCallable.flags =
        LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1;
    contextWithoutCallable.entry = nullptr;
    const LunaRuntimeDeclarationDescriptorV1* invalidContextRows[] = {
        &contextWithoutCallable, &structure};
    auto invalidContext = registry;
    invalidContext.descriptors = invalidContextRows;
    invalidContext.descriptor_count = 2;
    luna::runtime::RuntimeDescriptorRegistryView invalidContextView;
    if (invalidContextView.bind(&invalidContext, error) ||
        error.find("invalid row") == std::string::npos)
        return fail("Runtime descriptor accepted context ABI without callable entry");

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
    badMetadataRegistry.descriptor_count = 2;
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
