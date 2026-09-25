#include "driver/CompilerPipeline.h"
#include "diagnostics/Diagnostic.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "moonir_canonical_test_support.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace canonical_test {

int runCrossPackageRuntimeTest() {
    const auto showcasePath = std::filesystem::path(LUNA_TEST_SOURCE_DIR) /
        "examples/full_showcase";
    const auto effectsPath = showcasePath / "foundation/src/effects.luna";
    std::ifstream effectsInput(effectsPath, std::ios::binary);
    if (!effectsInput)
        return fail("dependency host source could not be opened");
    std::ostringstream effectsBuffer;
    effectsBuffer << effectsInput.rdbuf();
    luna::driver::CompilerPipeline hostPipeline;
    if (!hostPipeline.compileSourceToMoonIR(
            effectsBuffer.str(), effectsPath.string())) {
        for (const auto& diagnostic : hostPipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return fail("dependency host did not compile to MoonIR");
    }
    luna::driver::CompilerPipeline pluginPipeline;
    luna::driver::CompilerPipelineOptions pluginOptions;
    pluginOptions.inputPath = (showcasePath / "app").string();
    if (!pluginPipeline.compileToMoonIR(pluginOptions)) {
        for (const auto& diagnostic : pluginPipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return fail("application plugin did not compile to MoonIR");
    }

    const auto& hostModule = hostPipeline.moonModule();
    const auto& pluginModule = pluginPipeline.moonModule();
    const moon::DeclarationRecord* slot = nullptr;
    const moon::DeclarationRecord* fragment = nullptr;
    const moon::DeclarationRecord* entry = nullptr;
    for (const auto& record : hostModule.declarationTable) {
        if (record.kind == moon::DeclarationKind::Slot &&
            record.sourceName == "audit_slot")
            slot = &record;
        else if (record.kind == moon::DeclarationKind::Function &&
                 record.sourceName == "host_probe")
            entry = &record;
    }
    for (const auto& record : pluginModule.declarationTable) {
        if (record.kind == moon::DeclarationKind::Fragment &&
            record.sourceName == "app_audit")
            fragment = &record;
    }
    if (!slot || !fragment || !entry ||
        fragment->controlTarget.symbol != slot->symbolId ||
        fragment->controlTarget.contract != slot->contractId)
        return fail("cross-package Fragment lost its exact dependency Slot target");

    if (!hostPipeline.generateCode({})) {
        for (const auto& diagnostic : hostPipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return fail("dependency host did not generate code");
    }
    if (!pluginPipeline.generateCode({})) {
        for (const auto& diagnostic : pluginPipeline.errors())
            std::cerr << diagnostic::render(diagnostic) << '\n';
        return fail("application plugin did not generate code");
    }
    std::string error;
    auto hostLease = hostPipeline.codeGenerator().materializeJitModule(error);
    if (!hostLease) {
        std::cerr << error << '\n';
        return fail("dependency host did not materialize in JIT");
    }
    const void* hostRegistryAddress = hostLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(hostModule.name), error);
    luna::runtime::RuntimeDescriptorRegistryView hostRegistry;
    if (!hostRegistryAddress || !hostRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                hostRegistryAddress), error))
        return fail("dependency host Runtime registry did not bind");

    const auto* entryDescriptor = hostRegistry.find(
        entry->symbolId.value, entry->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!entryDescriptor || !entryDescriptor->entry)
        return fail("dependency host Runtime entry was not published");

    using ContextEntry = int32_t (*)(const void*);
    const auto call = reinterpret_cast<ContextEntry>(
        const_cast<void*>(entryDescriptor->entry));
    luna::runtime::RuntimeFragmentBindingSet emptyBindings;
    std::vector<luna::runtime::RuntimeFragmentRef> none;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(none), emptyBindings, error))
        return fail("cross-package empty BindingSet was rejected");
    luna::runtime::RuntimeFragmentExecutionContext emptyContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            emptyBindings, emptyContext, error) ||
        call(emptyContext.opaque()) != 42)
        return fail("cross-package Slot did not run its unbound continuation");

    const luna::runtime::RuntimeSlotRequirement slotRequirement{
        slot->symbolId.value, slot->contractId.value};
    luna::runtime::RuntimeFragmentExecutionContext selectedContext;
    std::weak_ptr<LunaJitModule> pluginWeak;
    {
        auto pluginLease = pluginPipeline.codeGenerator().materializeJitModule(error);
        if (!pluginLease) {
            std::cerr << error << '\n';
            return fail("application plugin did not materialize in JIT");
        }
        pluginWeak = pluginLease;
        const void* pluginRegistryAddress = pluginLease->lookup(
            luna::runtime::runtimeDescriptorRegistrySymbol(
                pluginModule.name), error);
        luna::runtime::RuntimeDescriptorRegistryView pluginRegistry;
        if (!pluginRegistryAddress || !pluginRegistry.bind(
                static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                    pluginRegistryAddress), error))
            return fail("application plugin Runtime registry did not bind");
        const auto* fragmentDescriptor = pluginRegistry.find(
            fragment->symbolId.value, fragment->contractId.value,
            LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1 |
                LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1);
        if (!fragmentDescriptor || !fragmentDescriptor->entry)
            return fail("application plugin candidate was not published");

        luna::runtime::MoonRuntime runtime;
        luna::runtime::GenerationStagingRequest hostRequest{
            hostModule.name, std::string(64, 'a'), hostLease};
        luna::runtime::MoonRuntime::StagedGeneration stagedHost;
        if (!runtime.stage(
                hostRequest,
                [](const auto&, std::string&) { return true; },
                [&](const auto&, auto& bindings, std::string&) {
                    bindings.push_back({
                        entryDescriptor->symbol_id,
                        entryDescriptor->contract_id,
                        entryDescriptor->entry,
                        entryDescriptor->declaration_kind,
                        luna::runtime::GenerationBindingCallable |
                            luna::runtime::GenerationBindingFragmentContext});
                    return true;
                },
                {}, stagedHost, error))
            return fail("dependency host generation did not stage");
        luna::runtime::MoonRuntime::PinnedGeneration hostGeneration;
        if (!runtime.loadOnce(stagedHost, hostGeneration, error))
            return fail("dependency host generation did not load");

        luna::runtime::GenerationStagingRequest pluginRequest{
            pluginModule.name, std::string(64, 'b'), pluginLease};
        luna::runtime::MoonRuntime::StagedGeneration stagedPlugin;
        if (!runtime.stage(
                pluginRequest,
                [](const auto&, std::string&) { return true; },
                [&](const auto&, auto& bindings, std::string&) {
                    bindings.push_back({
                        fragmentDescriptor->symbol_id,
                        fragmentDescriptor->contract_id,
                        fragmentDescriptor->entry,
                        fragmentDescriptor->declaration_kind,
                        luna::runtime::GenerationBindingFragmentExecutable |
                            luna::runtime::GenerationBindingPublicControl});
                    return true;
                },
                {}, stagedPlugin, error))
            return fail("application plugin generation did not stage");
        luna::runtime::MoonRuntime::PinnedGeneration pluginGeneration;
        if (!runtime.loadOnce(stagedPlugin, pluginGeneration, error) ||
            pluginGeneration.generationId() == hostGeneration.generationId())
            return fail("host and plugin did not load as distinct generations");

        luna::runtime::RuntimeFragmentCandidateSnapshot candidates;
        if (!luna::runtime::snapshotRuntimeFragmentCandidates(
                pluginGeneration, slotRequirement, candidates, error) ||
            candidates.size() != 1)
            return fail("plugin candidate was not indexed by host Slot");
        luna::runtime::RuntimeFragmentRef selected;
        const luna::runtime::RuntimeFragmentFactoryArguments noFactory{
            "", nullptr};
        if (!luna::runtime::makeOwnedRuntimeFragmentRef(
                *candidates.at(0), slotRequirement, noFactory, selected,
                error))
            return fail("host could not select the independent plugin candidate");
        std::vector<luna::runtime::RuntimeFragmentRef> selectedFragments;
        selectedFragments.push_back(std::move(selected));
        luna::runtime::RuntimeFragmentBindingSet bindings;
        if (!luna::runtime::makeRuntimeFragmentBindingSet(
                std::move(selectedFragments), bindings, error))
            return fail("plugin selection did not become a BindingSet");
        auto safePoint = runtime.safePoint();
        if (!runtime.activateFragmentBindings(bindings, safePoint, error))
            return fail("plugin BindingSet did not activate at a safe point");
        const auto active = runtime.pinFragmentBindings();
        if (!active || active.chainSize(slotRequirement) != 1 ||
            !luna::runtime::makeRuntimeFragmentExecutionContext(
                active, selectedContext, error))
            return fail("active cross-generation BindingSet lost the chosen Slot");
        pluginLease.reset();
    }
    if (pluginWeak.expired() || call(selectedContext.opaque()) != 40)
        return fail("selected plugin did not remain executable after Runtime teardown");
    selectedContext = {};
    if (!pluginWeak.expired())
        return fail("released execution context retained the plugin generation");
    return 0;
}

} // namespace canonical_test
