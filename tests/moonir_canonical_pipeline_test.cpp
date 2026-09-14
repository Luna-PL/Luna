#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "core/TypeLayout.h"
#include "driver/MoonGeneration.h"
#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "selector/Selector.h"
#include "sema/SemanticAnalysisSupport.h"
#include "sema/SymbolTable.h"
#include "tooling/AnalysisSnapshot.h"

#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/Compiler.h>
#include <llvm/Support/TargetSelect.h>

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <type_traits>

static_assert(std::is_same_v<decltype(moon::MetadataField::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::DeclarationRecord::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Param::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::Expr::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::LetStmt::type), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::intrinsicType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::CallExpr::calleeRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::ForStmt::protocolNext), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::LaunchExpr::kernelRef), moon::DeclarationRef>);
static_assert(std::is_same_v<decltype(moon::TryExpr::errorConversion), moon::DeclarationRef>);
static_assert(!std::is_same_v<moon::BlockId, moon::ScopeId>);
static_assert(!std::is_same_v<moon::LocalId, moon::CleanupId>);
static_assert(std::is_same_v<decltype(moon::FunctionDecl::returnType), moon::TypeRef>);
static_assert(std::is_same_v<decltype(moon::StructDecl::type), moon::TypeRef>);
#include "moonir_canonical_test_support.h"

namespace canonical_test {

int runCatalogProjectionTests() {
    // The semantic Symbol Catalog and sealed MoonIR must derive strong
    // identity from the same declaration projection. This fixture includes
    // every source declaration kind represented in canonical MoonIR, plus a
    // Drop-bound nominal type whose ContractId depends on another row.
    {
        const std::string catalogProjectionSource = R"luna(
meta revision { value: i32; }

@revision(1)
struct Resource { value: i32; }

enum Choice { None, Some(i32) }

trait Tagged {
    fn validate(value: i32) -> i32;
}

impl Drop for Resource {
    fn drop(resource: &mut Resource) -> unit { }
}

impl Tagged for Resource {
    fn validate(value: i32) -> i32 { return value; }
}

slot context trace_slot(value: i32);
context trace(value: i32) for trace_slot { resume(); }

constraint Same<T> = type_same::<T, T>();

fn main() -> i32 { return 0; }
)luna";
        luna::driver::CompilerPipeline catalogPipeline;
        if (!catalogPipeline.compileSourceToMoonIR(
                catalogProjectionSource, "<catalog-projection>")) {
            for (const auto& diagnostic : catalogPipeline.errors())
                std::cerr << diagnostic::render(diagnostic) << '\n';
            return fail("catalog projection fixture did not compile");
        }
        const auto* catalog =
            catalogPipeline.analysisSnapshot().symbolCatalog();
        if (!catalog || !catalog->valid())
            return fail("semantic analysis did not publish a valid Symbol Catalog");

        const std::pair<moon::DeclarationKind, size_t> expectedKinds[] = {
            {moon::DeclarationKind::Function, 3},
            {moon::DeclarationKind::Fragment, 1},
            {moon::DeclarationKind::Struct, 1},
            {moon::DeclarationKind::Enum, 1},
            {moon::DeclarationKind::Trait, 1},
            {moon::DeclarationKind::Implementation, 2},
            {moon::DeclarationKind::MetadataSchema, 1},
            {moon::DeclarationKind::Slot, 1},
        };
        size_t projectedCount = 0;
        for (const auto& [kind, expectedCount] : expectedKinds) {
            luna::selector::SymbolQuery query;
            query.kind = kind;
            auto symbols = catalog->query(query);
            if (!symbols.valid() || symbols.size() != expectedCount)
                return fail("Symbol Catalog projected the wrong declaration-kind cardinality");
            projectedCount += symbols.size();
            for (const auto* symbol : symbols.orderedSymbols()) {
                const auto* record =
                    catalogPipeline.moonModule().findDeclaration(
                        symbol->symbolId);
                if (!record || record->id != symbol->declarationId ||
                    record->familyId != symbol->familyDeclarationId ||
                    record->kind != symbol->kind ||
                    record->type != symbol->typeId ||
                    record->contractId != symbol->contractId) {
                    std::cerr << "catalog mismatch for "
                              << symbol->declarationId << '\n';
                    if (record) {
                        std::cerr << "  catalog family/type/contract: "
                                  << symbol->familyDeclarationId << " / "
                                  << symbol->typeId.value << " / "
                                  << symbol->contractId.value << '\n'
                                  << "  MoonIR family/type/contract: "
                                  << record->familyId << " / "
                                  << record->type.value << " / "
                                  << record->contractId.value << '\n'
                                  << "  catalog canonical: "
                                  << symbol->canonicalContract << '\n'
                                  << "  MoonIR canonical: "
                                  << record->canonicalContract << '\n';
                    }
                    return fail("Symbol Catalog identity disagrees with sealed MoonIR");
                }
            }
        }
        if (projectedCount != catalog->size())
            return fail("compiler-only constraint leaked into the Symbol Catalog");
    }
    return 0;
}

int runPipelineContainerTests() {
    // CompilerPipeline always seals production function bodies into canonical
    // CFGs. Verify that a simple program cannot retain a structured body.
    {
        const std::string pipelineSource = R"luna(
meta revision {
    major: i32;
}

runtime@revision(1)
fn retained_answer(value: i32) -> i32 {
    return value + 1;
}

fn identity<T>(value: T) -> T {
    return value;
}

fn forward(value: i32) -> i32 {
    return identity(value);
}

fn dead_concrete() -> i32 {
    return -1;
}

fn main() -> i32 {
    let values = [10, 20, 30];
    let total = 0;
    for v in values { total = total + v; }
    if total > 0 { return forward(total); }
    return 0;
}
)luna";
        luna::driver::CompilerPipeline pipeline;
        luna::driver::CompilerPipelineOptions options;
        bool compiled = pipeline.compileSourceToMoonIR(
            pipelineSource, "<pipeline-seal>", options);
        if (!compiled) {
            for (const auto& diag : pipeline.errors())
                std::cerr << diagnostic::render(diag) << '\n';
            return fail("production canonical sealing rejected a simple for-each program");
        }
        // The sealed module should have at least one function with a CFG.
        bool hasCgf = false;
        for (const auto& decl : pipeline.moonModule().declarations) {
            const auto* fn = dynamic_cast<const moon::FunctionDecl*>(decl.get());
            if (fn && fn->controlFlow) { hasCgf = true; break; }
        }
        if (!hasCgf)
            return fail("production pipeline did not produce a canonical CFG");

        const moon::DeclarationRecord* mainRecord = nullptr;
        for (const auto& record : pipeline.moonModule().declarationTable) {
            if (record.kind == moon::DeclarationKind::Function &&
                record.sourceName == "main") {
                mainRecord = &record;
                break;
            }
        }
        if (!mainRecord)
            return fail("production pipeline lost the main declaration row");
        bool hasGenericRecipe = false;
        bool hasConcreteInstance = false;
        bool hasDeadConcrete = false;
        for (const auto& declaration : pipeline.moonModule().declarations) {
            const auto* function = dynamic_cast<const moon::FunctionDecl*>(
                declaration.get());
            if (!function) continue;
            hasGenericRecipe |= !function->typeParams.empty() &&
                !function->isTemplateInstance;
            hasConcreteInstance |= function->isTemplateInstance;
            hasDeadConcrete |= function->name == "dead_concrete";
        }
        if (!hasGenericRecipe || !hasConcreteInstance || !hasDeadConcrete)
            return fail("production projection fixture lost recipe, instance, or dead function input");
        moon::ContainerManifest manifest;
        manifest.packageId = pipeline.moonModule().name;
        manifest.packageVersion = "0.3.0";
        manifest.packageKind = moon::ContainerPackageKind::Application;
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        auto targetBuilder = llvm::orc::JITTargetMachineBuilder::detectHost();
        if (!targetBuilder)
            return fail("container JIT test could not detect its host target");
        auto dataLayout = targetBuilder->getDefaultDataLayoutForTarget();
        if (!dataLayout)
            return fail("container JIT test could not derive its host data layout");
        manifest.targetTriple = targetBuilder->getTargetTriple().str();
        manifest.dataLayout = dataLayout->getStringRepresentation();
        manifest.entrypoint = {
            mainRecord->symbolId, mainRecord->contractId};
        manifest.features = pipeline.moonModule().features;
        std::vector<uint8_t> encodedContainer;
        std::string containerError;
        if (!moon::ContainerModelCodec::encodeContainer(
                manifest, pipeline.moonModule(),
                encodedContainer, containerError)) {
            std::cerr << containerError << '\n';
            return fail("production MoonIR could not enter a Moon Container");
        }
        moon::ContainerManifest loadedManifest;
        moon::Module loadedModule;
        if (!moon::ContainerModelCodec::decodeContainerForTarget(
                encodedContainer, manifest.targetTriple, manifest.dataLayout,
                loadedManifest,
                loadedModule, containerError)) {
            std::cerr << containerError << '\n';
            return fail("production Moon Container failed verified loading");
        }
        for (const auto& type : loadedModule.typeTable) {
            if (type.kind == TypeKind::TypeParam ||
                type.kind == TypeKind::InferenceVar ||
                type.kind == TypeKind::Unknown)
                return fail("Moon Container retained an unresolved generic type");
        }
        const bool hasRuntimeDescriptorInput = std::any_of(
            loadedModule.declarationTable.begin(),
            loadedModule.declarationTable.end(),
            [](const moon::DeclarationRecord& record) {
                return record.retention != moon::Retention::CompileTime ||
                    std::any_of(
                        record.metadata.begin(), record.metadata.end(),
                        [](const moon::MetadataInstance& metadata) {
                            return metadata.retention !=
                                moon::Retention::CompileTime;
                        });
            });
        if (!hasRuntimeDescriptorInput)
            return fail("Moon loader fixture lost its Runtime descriptor input");
        luna::runtime::MoonRuntime evolutionRuntime;
        luna::runtime::MoonRuntime::PinnedGeneration pinnedMoon;
        if (!luna::driver::loadVerifiedMoonGenerationOnce(
                evolutionRuntime, encodedContainer, manifest.targetTriple,
                manifest.dataLayout, pinnedMoon, containerError)) {
            std::cerr << containerError << '\n';
            return fail("verified Moon Container did not complete load-once");
        }
        const moon::DeclarationRecord* retainedAnswer = nullptr;
        for (const auto& record : loadedModule.declarationTable) {
            if (record.sourceName == "retained_answer") {
                retainedAnswer = &record;
                break;
            }
        }
        if (!retainedAnswer)
            return fail("Moon loader fixture lost its retained function row");
        const luna::runtime::GenerationBindingRequirement retainedRequirement{
            retainedAnswer->symbolId.value,
            retainedAnswer->contractId.value,
            static_cast<uint32_t>(retainedAnswer->kind) + 1,
            luna::runtime::GenerationBindingCallable};
        const auto retainedBinding = pinnedMoon.find(retainedRequirement);
        if (!retainedBinding ||
            invokeUnaryGenerationEntry(retainedBinding.implementation(), 41) != 42)
            return fail("Moon descriptor-backed typed reference was not callable");
        const uint64_t stagedMoonId = pinnedMoon.generationId();
        if (stagedMoonId == 0 ||
            pinnedMoon.moduleId() != manifest.packageId ||
            pinnedMoon.contentDigest().size() != 64 ||
            evolutionRuntime.activeGenerationId(manifest.packageId) !=
                stagedMoonId ||
            evolutionRuntime.retainedGenerationCount(manifest.packageId) != 1)
            return fail("Moon load-once lost content identity or publication");
        luna::runtime::MoonRuntime::PinnedGeneration duplicateMoon;
        if (!luna::driver::loadVerifiedMoonGenerationOnce(
                evolutionRuntime, encodedContainer, manifest.targetTriple,
                manifest.dataLayout, duplicateMoon, containerError) ||
            duplicateMoon.generationId() != stagedMoonId ||
            evolutionRuntime.retainedGenerationCount(manifest.packageId) != 1)
            return fail("Moon same-content load did not reuse its first generation");
        for (const auto& exported : loadedModule.exports) {
            const auto* descriptor = loadedModule.findDeclaration(
                exported.declaration);
            if (!descriptor)
                return fail("Moon generation lost an exported descriptor");
            luna::runtime::GenerationBindingRequirement requirement;
            requirement.symbolId = exported.declaration.symbol.value;
            requirement.contractId = exported.declaration.contract.value;
            requirement.declarationKind =
                static_cast<uint32_t>(descriptor->kind) + 1;
            if (descriptor->kind == moon::DeclarationKind::Function)
                requirement.requiredFlags =
                    luna::runtime::GenerationBindingCallable;
            const auto binding = pinnedMoon.find(requirement);
            if (!binding)
                return fail("Moon generation lost an exported binding");
            if (descriptor->kind == moon::DeclarationKind::Function) {
                if (!binding.implementation() ||
                    (binding.flags() &
                     luna::runtime::GenerationBindingCallable) == 0)
                    return fail("Moon generation lost an executable export");
            } else if (binding.implementation() == nullptr ||
                       binding.flags() != 0) {
                return fail("Moon generation lost a descriptor-backed export");
            }
        }
        const auto entryBinding = pinnedMoon.find(
            manifest.entrypoint.symbol.value,
            manifest.entrypoint.contract.value);
        if (!entryBinding ||
            (entryBinding.flags() &
             luna::runtime::GenerationBindingCallable) == 0 ||
            invokeGenerationEntry(entryBinding.implementation()) != 60)
            return fail("retained Moon JIT lease changed entrypoint behavior");

        luna::runtime::MoonRuntime::SwitchableBinding switchableEntry;
        if (!evolutionRuntime.makeSwitchable(
                manifest.packageId, manifest.entrypoint.symbol.value,
                manifest.entrypoint.contract.value, switchableEntry,
                containerError))
            return fail("active Moon entrypoint did not become switchable");
        const std::string replacementSource = R"luna(
fn main() -> i32 {
    return 13;
}
)luna";
        luna::driver::CompilerPipeline replacementPipeline;
        if (!replacementPipeline.compileSourceToMoonIR(
                replacementSource, "<pipeline-seal>", options))
            return fail("replacement Moon generation failed canonical lowering");
        const moon::DeclarationRecord* replacementMain = nullptr;
        for (const auto& record :
             replacementPipeline.moonModule().declarationTable) {
            if (record.kind == moon::DeclarationKind::Function &&
                record.sourceName == "main") {
                replacementMain = &record;
                break;
            }
        }
        if (!replacementMain ||
            replacementMain->symbolId != manifest.entrypoint.symbol ||
            replacementMain->contractId != manifest.entrypoint.contract)
            return fail("replacement Moon generation changed entrypoint identity");
        moon::ContainerManifest replacementManifest = manifest;
        replacementManifest.features = replacementPipeline.moonModule().features;
        replacementManifest.entrypoint = {
            replacementMain->symbolId, replacementMain->contractId};
        std::vector<uint8_t> replacementContainer;
        if (!moon::ContainerModelCodec::encodeContainer(
                replacementManifest, replacementPipeline.moonModule(),
                replacementContainer, containerError))
            return fail("replacement Moon generation failed container encoding");
        luna::runtime::MoonRuntime::PinnedGeneration changedMoon;
        if (luna::driver::loadVerifiedMoonGenerationOnce(
                evolutionRuntime, replacementContainer,
                replacementManifest.targetTriple,
                replacementManifest.dataLayout, changedMoon,
                containerError) ||
            containerError.find("different content") == std::string::npos ||
            evolutionRuntime.activeGenerationId(manifest.packageId) !=
                stagedMoonId ||
            invokeGenerationEntry(entryBinding.implementation()) != 60)
            return fail("Moon load-once replaced a module with different content");
        luna::runtime::MoonRuntime::StagedGeneration replacementStaged;
        bool replacementInitializerRan = false;
        if (!luna::driver::stageVerifiedMoonGeneration(
                evolutionRuntime, replacementContainer,
                replacementManifest.targetTriple,
                replacementManifest.dataLayout,
                [&](const auto&, const auto& bindings,
                    std::string& initializerError) {
                    if (evolutionRuntime.activeGenerationId(
                            manifest.packageId) != stagedMoonId) {
                        initializerError =
                            "replacement initializer observed early publication";
                        return false;
                    }
                    const auto binding = std::find_if(
                        bindings.begin(), bindings.end(), [&](const auto& item) {
                            return item.symbolId ==
                                    replacementMain->symbolId.value &&
                                item.contractId ==
                                    replacementMain->contractId.value;
                        });
                    if (binding == bindings.end() ||
                        (binding->flags &
                         luna::runtime::GenerationBindingCallable) == 0 ||
                        invokeGenerationEntry(binding->implementation) != 13) {
                        initializerError =
                            "replacement initializer saw an unresolved entry";
                        return false;
                    }
                    replacementInitializerRan = true;
                    return true;
                }, replacementStaged,
                containerError)) {
            std::cerr << containerError << '\n';
            return fail("replacement Moon generation failed staging");
        }
        if (!replacementInitializerRan)
            return fail("replacement Moon initializer did not run during staging");
        const uint64_t replacementId = replacementStaged.generationId();
        if (replacementId != stagedMoonId + 1)
            return fail("Moon load-once materialized a discarded generation");
        auto replacementSafePoint = evolutionRuntime.safePoint();
        if (!evolutionRuntime.activate(
                replacementStaged, replacementSafePoint, containerError) ||
            replacementId == stagedMoonId ||
            invokeGenerationEntry(entryBinding.implementation()) != 60 ||
            invokeGenerationEntry(
                switchableEntry.pin().implementation()) != 13)
            return fail("Moon generation switch violated pinned/switchable behavior");
        auto rollbackSafePoint = evolutionRuntime.safePoint();
        if (!evolutionRuntime.rollback(
                manifest.packageId, stagedMoonId, rollbackSafePoint,
                containerError) ||
            invokeGenerationEntry(
                switchableEntry.pin().implementation()) != 60 ||
            evolutionRuntime.retainedGenerationCount(manifest.packageId) != 2)
            return fail("Moon JIT generation rollback lost retained code");

        bool rejectingInitializerRan = false;
        luna::runtime::MoonRuntime::StagedGeneration initializerRejected;
        if (luna::driver::stageVerifiedMoonGeneration(
                evolutionRuntime, replacementContainer,
                replacementManifest.targetTriple,
                replacementManifest.dataLayout,
                [&](const auto&, const auto& bindings,
                    std::string& initializerError) {
                    rejectingInitializerRan = !bindings.empty();
                    initializerError = "fixture rejected Moon initializer";
                    return false;
                }, initializerRejected, containerError) ||
            !rejectingInitializerRan || initializerRejected ||
            containerError != "fixture rejected Moon initializer" ||
            evolutionRuntime.activeGenerationId(manifest.packageId) !=
                stagedMoonId ||
            evolutionRuntime.retainedGenerationCount(manifest.packageId) != 2 ||
            invokeGenerationEntry(
                switchableEntry.pin().implementation()) != 60)
            return fail("real Moon initializer failure changed active state");
        auto corruptedContainer = encodedContainer;
        corruptedContainer.back() ^= 1;
        luna::runtime::MoonRuntime::StagedGeneration rejectedMoon;
        if (luna::driver::stageVerifiedMoonGeneration(
                evolutionRuntime, corruptedContainer, manifest.targetTriple,
                manifest.dataLayout, {}, rejectedMoon, containerError) ||
            rejectedMoon ||
            evolutionRuntime.activeGenerationId(manifest.packageId) !=
                stagedMoonId)
            return fail("corrupt Moon generation changed active state");
        bool loadedConcreteInstance = false;
        bool loadedForward = false;
        for (const auto& declaration : loadedModule.declarations) {
            const auto* function = dynamic_cast<const moon::FunctionDecl*>(
                declaration.get());
            if (!function) continue;
            if (!function->typeParams.empty() && !function->isTemplateInstance)
                return fail("Moon Container retained a generic function recipe");
            if (function->name == "dead_concrete")
                return fail("Moon Container retained an unreachable concrete function");
            loadedConcreteInstance |= function->isTemplateInstance;
            loadedForward |= function->name == "forward";
        }
        if (!loadedConcreteInstance || !loadedForward)
            return fail("Moon Container projection dropped a transitive concrete callee");
        CodeGenerator loadedCodegen("container-roundtrip");
        if (!loadedCodegen.generate(&loadedModule)) {
            for (const auto& diagnostic : loadedCodegen.errors())
                std::cerr << diagnostic::render(diagnostic) << '\n';
            return fail("verified Moon Container failed LLVM generation");
        }
        const auto loadedExecution = loadedCodegen.jitRun();
        if (!loadedExecution.executed || loadedExecution.exitCode != 60)
            return fail("verified Moon Container changed JIT behavior");
    }
    return 0;
}

int runRegisteredTests() {
    using TestCase = int (*)();
    const TestCase tests[] = {
        &runCatalogProjectionTests,
        &runPipelineContainerTests,
    };
    for (const auto test : tests)
        if (const int result = test()) return result;
    return 0;
}

} // namespace canonical_test
