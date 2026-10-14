#include "driver/Driver.h"
#include "Version.h"
#include "diagnostics/Diagnostic.h"
#include "driver/AotLinker.h"
#include "driver/CommandLine.h"
#include "driver/CompilerPipeline.h"
#include "driver/NativeArtifact.h"
#include "driver/NativeGeneration.h"
#include "codegen/NativeRefUnitFacts.h"
#include "driver/Repl.h"
#include "moonir/ContainerModel.h"
#include "moonir/Printer.h"
#include "package/Package.h"
#include "runtime/Runtime.h"
#include "runtime/RuntimeFragment.h"
#include "runtime/RuntimeDescriptorABI.h"
#include "tooling/AnalysisSnapshot.h"
#include <llvm/ADT/SmallString.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/TargetParser/Host.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "driver/DriverInternal.h"

namespace luna::driver::driver_detail {

#ifndef LUNA_COMPILER_COMMIT
#define LUNA_COMPILER_COMMIT "unknown"
#endif

void printErrors(const std::vector<diagnostic::Diagnostic>& errors,
                 const char* stage) {
    for (auto& e : errors) {
        if (stage) std::cerr << "error[" << stage << "]: ";
        std::cerr << e << "\n";
    }
}

int buildMoonContainer(const CompilerPipeline& pipeline, const std::string& inputPath,
                              const std::string& outputOverride) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::path(inputPath))) {
        std::cerr << diagnostic::format("driver", "-t moon requires a package directory", inputPath,
                                        0, 0, "pass the directory containing luna.package")
                  << "\n";
        return 1;
    }
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    if (package.id.empty() || package.version.empty() || package.kind == PackageKind::Unspecified) {
        std::cerr << diagnostic::format(
                         "driver", "-t moon requires an explicit package manifest kind", inputPath,
                         0, 0, "set kind = \"application\" or kind = \"library\" in luna.package")
                  << "\n";
        return 1;
    }

    moon::DeclarationRef entrypoint;
    size_t mainCount = 0;
    for (const auto& declaration : pipeline.moonModule().declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(declaration.get());
        if (!function || function->packageId != package.id || function->name != "main") continue;
        ++mainCount;
        entrypoint = {function->symbolId, function->contractId};
    }
    if ((package.kind == PackageKind::Application && mainCount != 1) ||
        (package.kind == PackageKind::Library && mainCount != 0)) {
        std::cerr << diagnostic::format(
                         "driver",
                         package.kind == PackageKind::Application
                             ? "Moon application must contain exactly one package main"
                             : "Moon library must not contain a package main",
                         inputPath, 0, 0,
                         package.kind == PackageKind::Application
                             ? "define exactly one `fn main() -> i32` in the root package"
                             : "remove main or set kind = \"application\"")
                  << "\n";
        return 1;
    }

    llvm::InitializeNativeTarget();
    llvm::InitializeNativeTargetAsmPrinter();
    auto targetBuilder = llvm::orc::JITTargetMachineBuilder::detectHost();
    if (!targetBuilder) {
        std::cerr << "error[driver]: cannot detect Moon host target: "
                  << llvm::toString(targetBuilder.takeError()) << "\n";
        return 1;
    }
    auto dataLayout = targetBuilder->getDefaultDataLayoutForTarget();
    if (!dataLayout) {
        std::cerr << "error[driver]: cannot derive Moon host data layout: "
                  << llvm::toString(dataLayout.takeError()) << "\n";
        return 1;
    }

    moon::ContainerManifest manifest;
    manifest.packageId = package.id;
    manifest.packageVersion = package.version;
    manifest.packageKind = package.kind == PackageKind::Application
                               ? moon::ContainerPackageKind::Application
                               : moon::ContainerPackageKind::Library;
    manifest.targetTriple = targetBuilder->getTargetTriple().str();
    manifest.dataLayout = dataLayout->getStringRepresentation();
    manifest.entrypoint = entrypoint;
    manifest.features = pipeline.moonModule().features;

    std::vector<uint8_t> encoded;
    std::string error;
    if (!moon::ContainerModelCodec::encodeContainer(manifest, pipeline.moonModule(), encoded,
                                                    error)) {
        std::cerr << diagnostic::format(
                         "moon-container", error, inputPath, 0, 0,
                         "the package must lower to closed, verified canonical MoonIR")
                  << "\n";
        return 1;
    }
    // encodeContainer performs the complete target-bound roundtrip before
    // returning bytes, using source-verified dependency facts only internally.
    // Re-decoding here without explicit owner evidence would reject plugins.

    std::string artifactName = package.id;
    if (const auto separator = artifactName.rfind('.'); separator != std::string::npos)
        artifactName = artifactName.substr(separator + 1);
    fs::path outputPath;
    if (!outputOverride.empty()) {
        outputPath = outputOverride;
    } else {
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) / "build" / "moon" /
                     (artifactName + ".moon");
    }
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
        fs::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
                         "driver",
                         "cannot create Moon output directory: " + filesystemError.message(),
                         outputPath.string(), 0, 0, "check the output path permissions")
                  << "\n";
        return 1;
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) {
        std::cerr << diagnostic::format("driver", "cannot write Moon Container",
                                        outputPath.string(), 0, 0,
                                        "check the output path permissions")
                  << "\n";
        return 1;
    }
    std::cout << "Built Moon Container: " << outputPath.string() << "\n";
    return 0;
}

std::string packageArtifactName(const std::string& packageId) {
    const auto separator = packageId.rfind('.');
    return separator == std::string::npos ? packageId : packageId.substr(separator + 1);
}

bool isCIdentifier(const std::string& value) {
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) || value.front() == '_'))
        return false;
    for (const char character : value) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) || character == '_'))
            return false;
    }
    return true;
}

std::string cTypeName(const moon::Module& module, const moon::TypeRef& reference,
                             std::string& error) {
    const auto* type = module.findType(reference);
    if (!type) {
        error = "C export references a missing sealed MoonIR type '" + reference.value + "'";
        return {};
    }
    switch (type->kind) {
    case TypeKind::I8:
        return "int8_t";
    case TypeKind::I16:
        return "int16_t";
    case TypeKind::I32:
        return "int32_t";
    case TypeKind::I64:
        return "int64_t";
    case TypeKind::U8:
        return "uint8_t";
    case TypeKind::U16:
        return "uint16_t";
    case TypeKind::U32:
        return "uint32_t";
    case TypeKind::U64:
        return "uint64_t";
    case TypeKind::USize:
        return "size_t";
    case TypeKind::ISize:
        return "ptrdiff_t";
    case TypeKind::F32:
        return "float";
    case TypeKind::F64:
        return "double";
    case TypeKind::CStr:
        return "const char *";
    case TypeKind::RawPointer:
        return "void *";
    case TypeKind::Unit:
        return "void";
    case TypeKind::Reference: {
        std::string inner = cTypeName(module, type->innerTypeId, error);
        if (inner.empty()) return {};
        return inner + (type->isMutable ? " *" : " const *");
    }
    default:
        error = "C export type '" + type->displayName +
                "' is not representable in a generated C header";
        return {};
    }
}

bool collectCffiExports(const CompilerPipeline& pipeline,
                               std::vector<const moon::FunctionDecl*>& exports,
                               std::string& error) {
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    if (package.id.empty() || package.version.empty() || package.kind == PackageKind::Unspecified) {
        error = "-t cffi requires an explicit package manifest";
        return false;
    }
    if (package.kind != PackageKind::Library) {
        error = "-t cffi requires kind = \"library\"";
        return false;
    }

    size_t mainCount = 0;
    for (const auto& declaration : pipeline.moonModule().declarations) {
        if (!declaration || declaration->packageId != package.id) continue;
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main") ++mainCount;
        if (!declaration->isExported) continue;
        if (!function || function->abi != "C" || function->isExtern) {
            error = "CFFI public surface contains non-C export '" + declaration->name +
                    "'; use `export \"C\" fn` only";
            return false;
        }
        const std::string symbol =
            function->linkName.empty() ? function->generatedSymbolName : function->linkName;
        if (!isCIdentifier(symbol)) {
            error = "C export '" + function->name +
                    "' has a link symbol that is not a portable C identifier: '" + symbol + "'";
            return false;
        }
        exports.push_back(function);
    }
    if (mainCount != 0) {
        error = "CFFI library must not contain a package main";
        return false;
    }
    if (exports.empty()) {
        error = "CFFI library must export at least one `export \"C\" fn`";
        return false;
    }
    return true;
}

int buildCffiLibrary(const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
                            const std::string& inputPath,
                            const std::vector<std::string>& linkLibraries,
                            const std::string& runtimeLibrary, const std::string& aotCompiler,
                            const std::string& outputOverride,
                            LunaOptimizationLevel optimizationLevel) {
    namespace fs = std::filesystem;
    std::vector<const moon::FunctionDecl*> exports;
    std::string error;
    if (!collectCffiExports(pipeline, exports, error)) {
        std::cerr << diagnostic::format(
                         "driver", error, inputPath, 0, 0,
                         "CFFI artifacts are library packages with a closed explicit C ABI")
                  << "\n";
        return 1;
    }

    const auto& package = pipeline.analysisSnapshot().packageManifest();
    const std::string artifactName = packageArtifactName(package.id);
    fs::path outputPath;
    if (!outputOverride.empty()) {
        outputPath = outputOverride;
    } else {
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) / "build" / "cffi";
#ifdef _WIN32
        outputPath /= artifactName + ".dll";
#elif defined(__APPLE__)
        outputPath /= "lib" + artifactName + ".dylib";
#else
        outputPath /= "lib" + artifactName + ".so";
#endif
    }
    const fs::path headerPath = outputPath.parent_path() / (artifactName + ".h");
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
        fs::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
                         "driver",
                         "cannot create CFFI output directory: " + filesystemError.message(),
                         outputPath.string(), 0, 0, "check the output path permissions")
                  << "\n";
        return 1;
    }

    std::ostringstream header;
    std::string guard = "LUNA_CFFI_" + artifactName + "_H";
    for (char& character : guard) {
        if (std::isalnum(static_cast<unsigned char>(character)))
            character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
        else
            character = '_';
    }
    header << "#ifndef " << guard << "\n#define " << guard
           << "\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
           << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
    for (const auto* function : exports) {
        std::string typeError;
        const std::string returnType =
            cTypeName(pipeline.moonModule(), function->returnType, typeError);
        if (returnType.empty()) {
            std::cerr << diagnostic::format(
                             "driver", typeError, function->location.path, function->location.line,
                             function->location.column,
                             "use a C-ABI-safe scalar, pointer, cstr, reference, or unit")
                      << "\n";
            return 1;
        }
        const std::string symbol =
            function->linkName.empty() ? function->generatedSymbolName : function->linkName;
        header << returnType << ' ' << symbol << '(';
        if (function->params.empty()) {
            header << "void";
        } else {
            for (size_t index = 0; index < function->params.size(); ++index) {
                if (index) header << ", ";
                const std::string parameterType =
                    cTypeName(pipeline.moonModule(), function->params[index].type, typeError);
                if (parameterType.empty()) {
                    std::cerr << diagnostic::format(
                                     "driver", typeError, function->location.path,
                                     function->location.line, function->location.column,
                                     "use a C-ABI-safe scalar, pointer, cstr, or reference")
                              << "\n";
                    return 1;
                }
                header << parameterType << " arg" << index;
            }
        }
        header << ");\n";
    }
    header << "\n#ifdef __cplusplus\n}\n#endif\n\n#endif\n";

    if (AotLinker::build(codeGenerator, {
                                            inputPath,
                                            pipeline.declaredPackageName(),
                                            linkLibraries,
                                            runtimeLibrary,
                                            aotCompiler,
                                            outputPath.string(),
                                            optimizationLevel,
                                            AotArtifactKind::SharedLibrary,
                                            std::nullopt,
                                            {},
                                        }) != 0)
        return 1;

    std::ofstream output(headerPath, std::ios::binary | std::ios::trunc);
    output << header.str();
    if (!output) {
        std::cerr << diagnostic::format("driver", "cannot write generated CFFI header",
                                        headerPath.string(), 0, 0,
                                        "check the output path permissions")
                  << "\n";
        return 1;
    }
    std::cout << "Generated C header: " << headerPath.string() << "\n";
    return 0;
}

bool prepareNativeLibraryProofSurface(
    const CompilerPipeline& pipeline, NativeLibraryProofSurface& surface,
    diagnostic::Diagnostic& error) {
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    NativeLibraryProofSurface prepared;
    prepared.proof.packageId = package.id;
    prepared.proof.packageVersion = package.version;
    prepared.proof.targetAbi = llvm::sys::getProcessTriple();
    prepared.proof.compilerIdentity =
        std::string("luna/") + LUNA_VERSION_STRING + "@" + LUNA_COMPILER_COMMIT;

    for (const auto& declaration : pipeline.moonModule().declarations) {
        if (!declaration || !declaration->isExported ||
            declaration->packageId != package.id)
            continue;
        const auto* function =
            dynamic_cast<const moon::FunctionDecl*>(declaration.get());
        if (function && (function->isExtern || function->abi == "C")) {
            error = diagnostic::format(
                "native-proof",
                "Native public surface contains foreign C export '" +
                    function->name + "'",
                function->location.path, function->location.line,
                function->location.column,
                "use ordinary `export fn` for trusted Native ABI or build `-t cffi`");
            return false;
        }
        const auto* record = pipeline.moonModule().findDeclaration(
            {declaration->symbolId, declaration->contractId});
        if (!record || record->id != declaration->declarationId) {
            error = diagnostic::format(
                "native-proof", "Native export has no exact sealed declaration record",
                declaration->location.path, declaration->location.line,
                declaration->location.column,
                "report this compiler defect with the package");
            return false;
        }
        if (function) {
            const auto hasRef = [&](const moon::TypeRef& typeId) {
                const auto* type = pipeline.moonModule().findType(typeId);
                return type && type->kind == TypeKind::RuntimeFragmentRef;
            };
            const bool usesRef = hasRef(function->returnType) ||
                std::any_of(function->params.begin(), function->params.end(),
                    [&](const moon::Param& param) {
                        return hasRef(param.type);
                    });
            if (usesRef) {
                luna::codegen::NativeRefUnitSourceFacts facts;
                std::string sourceError;
                if (prepared.refUnitSource ||
                    !luna::codegen::deriveNativeRefUnitSourceFacts(
                        pipeline.moonModule(), *function, facts, sourceError)) {
                    error = diagnostic::format(
                        "native-proof",
                        prepared.refUnitSource
                            ? "Native proof surface has another Ref callable"
                            : sourceError,
                        function->location.path, function->location.line,
                        function->location.column,
                        "check the Native Ref/unit source and public Slot");
                    return false;
                }
                prepared.refUnitSource = function;
                continue;
            }
        }
        NativeExportSpec nativeExport;
        nativeExport.declarationKind = static_cast<uint32_t>(record->kind) + 1;
        nativeExport.flags = function ? LUNA_NATIVE_EXPORT_CALLABLE_V1 : 0;
        nativeExport.symbolId = record->symbolId.value;
        nativeExport.contractId = record->contractId.value;
        nativeExport.linkageName = record->linkageName;
        prepared.proof.exportedDescriptors.push_back(
            canonicalNativeExport(nativeExport));
        prepared.v1Exports.push_back(std::move(nativeExport));
    }
    surface = std::move(prepared);
    error = {};
    return true;
}

namespace {

struct NativeRefPreviewProbe {
    const char* slotId = nullptr;
    const char* slotContractId = nullptr;
    const char* layoutId = nullptr;
    uint64_t argumentsSize = 0;
    uint64_t argumentsAlignment = 0;
    unsigned calls = 0;
    bool sawExpectedValue = false;
    bool repeatResume = false;
    int32_t resumeStatus = LUNA_RUNTIME_FRAGMENT_CONTINUATION_ESCAPED_V1;
    int32_t repeatedResumeStatus = 0;
};

thread_local NativeRefPreviewProbe* activeNativeRefPreviewProbe = nullptr;

void executeNativeRefPreviewFragment(void*, void* activation) {
    auto* probe = activeNativeRefPreviewProbe;
    if (!probe) return;
    ++probe->calls;
    const void* arguments = luna_runtime_fragment_activation_arguments_v1(
        activation, probe->slotId, probe->slotContractId,
        probe->layoutId, probe->argumentsSize, probe->argumentsAlignment);
    if (arguments && probe->argumentsSize >= sizeof(int32_t)) {
        int32_t value = 0;
        std::memcpy(&value, arguments, sizeof(value));
        probe->sawExpectedValue = value == 1;
    }
    probe->resumeStatus =
        luna_runtime_fragment_activation_resume_v1(activation);
    if (probe->repeatResume)
        probe->repeatedResumeStatus =
            luna_runtime_fragment_activation_resume_v1(activation);
}

bool exerciseNativeRefPreviewForTest(
    const moon::Module& module,
    const luna::codegen::NativeRefUnitSourceFacts& source,
    const luna::runtime::MoonRuntime::PinnedRefUnitCandidate& candidate,
    std::string& error) {
    const moon::SlotDecl* slot = nullptr;
    for (const auto& declaration : module.declarations) {
        const auto* current =
            dynamic_cast<const moon::SlotDecl*>(declaration.get());
        if (current && current->symbolId.value == source.slotSymbolId &&
            current->contractId.value == source.slotContractId) {
            slot = current;
            break;
        }
    }
    const auto* arguments = slot ? module.findType(slot->argumentsType) : nullptr;
    if (!slot || !arguments) {
        error = "Native Ref/unit preview lacks the sealed Slot argument layout";
        return false;
    }
    const LunaRuntimeFragmentDescriptorV1 fragment = {
        LUNA_RUNTIME_FRAGMENT_MAGIC_V1,
        LUNA_RUNTIME_FRAGMENT_ABI_V1,
        sizeof(LunaRuntimeFragmentDescriptorV1),
        LUNA_RUNTIME_FRAGMENT_CAPTURE_FREE_V1,
        0, 0, 0, 0,
        "fragment:native-ref-preview", "contract:native-ref-preview",
        source.slotSymbolId.c_str(), source.slotContractId.c_str(),
        arguments->abiLayoutId.value.c_str(),
        arguments->valueSize, arguments->valueAlignment,
        "", "layout:unit", 0, 1,
        nullptr, nullptr, executeNativeRefPreviewFragment};
    const std::string foreignSlotId = source.slotSymbolId + ".foreign";
    const std::string foreignSlotContract =
        source.slotContractId + ".foreign";
    auto foreignFragment = fragment;
    foreignFragment.fragment_id = "fragment:native-ref-preview-foreign";
    foreignFragment.slot_id = foreignSlotId.c_str();
    foreignFragment.slot_contract_id = foreignSlotContract.c_str();
    if (!luna::runtime::validateRuntimeFragmentDescriptor(fragment, error) ||
        !luna::runtime::validateRuntimeFragmentDescriptor(
            foreignFragment, error))
        return false;
    luna::runtime::MoonRuntime hostRuntime;
    const luna::runtime::GenerationStagingRequest hostRequest{
        "org.luna.native.ref-preview", std::string(64, 'a'),
        std::make_shared<int>(1)};
    luna::runtime::MoonRuntime::StagedGeneration staged;
    constexpr uint32_t fragmentFlags =
        luna::runtime::GenerationBindingFragmentExecutable |
        luna::runtime::GenerationBindingPublicControl;
    if (!hostRuntime.stage(
            hostRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                const LunaRuntimeFragmentDescriptorV1* descriptors[] = {
                    &fragment, &foreignFragment};
                for (const auto* descriptor : descriptors)
                    bindings.push_back({descriptor->fragment_id,
                        descriptor->fragment_contract_id, descriptor,
                        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, fragmentFlags});
                return true;
            }, {}, staged, error))
        return false;
    luna::runtime::MoonRuntime::PinnedGeneration hostPinned;
    if (!hostRuntime.loadOnce(staged, hostPinned, error)) return false;
    const luna::runtime::GenerationBindingRequirement requirement{
        fragment.fragment_id, fragment.fragment_contract_id,
        LUNA_RUNTIME_DECLARATION_FRAGMENT_V1, fragmentFlags};
    const auto binding = hostPinned.find(requirement);
    auto foreignRequirement = requirement;
    foreignRequirement.symbolId = foreignFragment.fragment_id;
    const auto foreignBinding = hostPinned.find(foreignRequirement);
    const luna::runtime::RuntimeSlotRequirement target{
        source.slotSymbolId, source.slotContractId};
    const luna::runtime::RuntimeSlotRequirement foreignTarget{
        foreignSlotId, foreignSlotContract};
    luna::runtime::RuntimeFragmentRef reference;
    luna::runtime::RuntimeFragmentRef foreignReference;
    luna::runtime::RuntimeFragmentRefHandle handle;
    luna::runtime::RuntimeFragmentRefHandle foreignHandle;
    if (!binding || !foreignBinding ||
        !luna::runtime::makeOwnedRuntimeFragmentRef(
            binding, target, {}, reference, error) ||
        !luna::runtime::makeRuntimeFragmentRefHandle(
            reference, target, handle, error) ||
        !luna::runtime::makeOwnedRuntimeFragmentRef(
            foreignBinding, foreignTarget, {}, foreignReference, error) ||
        !luna::runtime::makeRuntimeFragmentRefHandle(
            foreignReference, foreignTarget, foreignHandle, error))
        return false;
    luna::runtime::RuntimeFragmentBindingSet empty;
    luna::runtime::RuntimeFragmentExecutionContext parent;
    if (!luna::runtime::makeRuntimeFragmentBindingSet({}, empty, error) ||
        !luna::runtime::makeRuntimeFragmentExecutionContext(
            empty, parent, error))
        return false;
    luna::runtime::RuntimeFragmentExecutionContext missingParent;
    luna::runtime::RuntimeFragmentRefHandle missingHandle;
    NativeRefPreviewProbe probe{
        source.slotSymbolId.c_str(), source.slotContractId.c_str(),
        arguments->abiLayoutId.value.c_str(),
        arguments->valueSize, arguments->valueAlignment};
    struct ScopedProbe {
        NativeRefPreviewProbe* previous;
        explicit ScopedProbe(NativeRefPreviewProbe& current)
            : previous(activeNativeRefPreviewProbe) {
            activeNativeRefPreviewProbe = &current;
        }
        ~ScopedProbe() { activeNativeRefPreviewProbe = previous; }
    } active(probe);
    int32_t status = -1;
    const auto rejectsBeforeEntry = [&](const auto& testParent,
                                        const auto& testHandle) {
        error.clear();
        status = -1;
        return !candidate.call(testParent, testHandle, status, error) &&
            !error.empty() && status == -1 && probe.calls == 0;
    };
    if (!rejectsBeforeEntry(missingParent, handle) ||
        !rejectsBeforeEntry(parent, missingHandle) ||
        !rejectsBeforeEntry(parent, foreignHandle)) {
        error = "Native Ref/unit linked preview admitted an invalid capability";
        return false;
    }
    error.clear();
    status = -1;
    const bool called = candidate.call(parent, handle, status, error);
    if (!called || status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 ||
        probe.calls != 1 || !probe.sawExpectedValue || probe.resumeStatus !=
            LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
        if (error.empty())
            error = "Native Ref/unit linked preview did not complete one Apply";
        return false;
    }
    probe.calls = 0;
    probe.sawExpectedValue = false;
    probe.repeatResume = true;
    probe.repeatedResumeStatus = 0;
    error.clear();
    status = -1;
    if (!candidate.call(parent, handle, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_EXECUTION_FAILURE_V1 ||
        probe.calls != 1 || !probe.sawExpectedValue ||
        probe.resumeStatus != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1 ||
        probe.repeatedResumeStatus !=
            LUNA_RUNTIME_FRAGMENT_DISPATCH_EXECUTION_FAILED_V1) {
        if (error.empty())
            error = "Native Ref/unit linked preview lost its execution failure";
        return false;
    }
    probe.calls = 0;
    probe.sawExpectedValue = false;
    probe.repeatResume = false;
    error.clear();
    status = -1;
    if (!candidate.call(parent, handle, status, error) ||
        status != LUNA_NATIVE_REF_UNIT_SUCCESS_V1 ||
        probe.calls != 1 || !probe.sawExpectedValue ||
        probe.resumeStatus != LUNA_RUNTIME_FRAGMENT_CONTINUATION_COMPLETED_V1) {
        if (error.empty())
            error = "Native Ref/unit linked preview did not recover after failure";
        return false;
    }
    return true;
}

} // namespace

int buildNativeLibrary(const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
                              const NativeLibraryProofSurface& surface,
                              const std::string& inputPath,
                              const std::vector<std::string>& linkLibraries,
                              const std::string& runtimeLibrary, const std::string& aotCompiler,
                              const std::string& outputOverride,
                              LunaOptimizationLevel optimizationLevel) {
    namespace fs = std::filesystem;
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    auto spec = surface.proof;
    const auto& nativeExports = surface.v1Exports;
    if (surface.refUnitSource) {
        if (!codeGenerator.nativeRefUnitV4Claim()) {
            std::cerr << diagnostic::format(
                "native-proof", "Native Ref/unit module has no v4 proof claim",
                inputPath, 0, 0, "recheck Native Ref/unit code generation")
                      << "\n";
            return 1;
        }
        spec.exportedDescriptors.push_back(
            *codeGenerator.nativeRefUnitV4Claim());
    }

    std::vector<uint8_t> proofRecord;
    std::string proofError;
    if (!makeNativeProofPlaceholder(spec, proofRecord, proofError) ||
        !codeGenerator.emitNativeProofPlaceholder(proofRecord) ||
        !codeGenerator.emitNativeLibraryDescriptor(spec.packageId, spec.packageVersion,
                                                   spec.targetAbi, spec.compilerIdentity,
                                                   nativeExports)) {
        if (!proofError.empty())
            std::cerr << diagnostic::format(
                             "native-proof", proofError, inputPath, 0, 0,
                             "shorten manifest identity fields or report a compiler defect")
                      << "\n";
        else
            printErrors(codeGenerator.errors(), "native-proof");
        return 1;
    }

    const std::string artifactName = packageArtifactName(package.id);
    fs::path outputPath;
    if (!outputOverride.empty()) {
        outputPath = outputOverride;
    } else {
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) / "build" / "native";
#ifdef _WIN32
        outputPath /= artifactName + ".dll";
#elif defined(__APPLE__)
        outputPath /= "lib" + artifactName + ".dylib";
#else
        outputPath /= "lib" + artifactName + ".so";
#endif
    }
    auto buildAt = [&](const fs::path& path) {
        return AotLinker::build(codeGenerator, {
                                            inputPath,
                                            pipeline.declaredPackageName(),
                                            linkLibraries,
                                            runtimeLibrary,
                                            aotCompiler,
                                            path.string(),
                                            optimizationLevel,
                                            AotArtifactKind::SharedLibrary,
                                            std::nullopt,
                                            {},
                                        });
    };
    if (surface.refUnitSource) {
        std::error_code tempError;
        const fs::path tempRoot = fs::temp_directory_path(tempError);
        if (tempError) {
            std::cerr << diagnostic::format(
                "native-proof", "cannot locate Native Ref/unit proof preview directory: " +
                    tempError.message(), inputPath, 0, 0,
                "check the temporary directory") << "\n";
            return 1;
        }
        llvm::SmallString<256> previewDirectoryText;
        std::error_code directoryError = llvm::sys::fs::createUniqueDirectory(
            (tempRoot / "luna-native-ref-proof").string(),
            previewDirectoryText);
        if (directoryError) {
            std::cerr << diagnostic::format(
                "native-proof", "cannot create Native Ref/unit proof preview: " +
                    directoryError.message(), inputPath, 0, 0,
                "check the temporary directory") << "\n";
            return 1;
        }
        struct PreviewCleanup {
            fs::path directory;
            ~PreviewCleanup() {
                if (directory.empty()) return;
                std::error_code ignored;
                fs::remove_all(directory, ignored);
            }
        } cleanup{fs::path(previewDirectoryText.str().str())};
        fs::path previewArtifact = cleanup.directory;
#ifdef _WIN32
        previewArtifact /= "luna_ref_unit_preview.dll";
#elif defined(__APPLE__)
        previewArtifact /= "libluna_ref_unit_preview.dylib";
#else
        previewArtifact /= "libluna_ref_unit_preview.so";
#endif
        if (buildAt(previewArtifact) != 0) return 1;
        const fs::path previewTrust = previewArtifact.string() + ".trust";
        NativeProofInfo previewProof;
        if (!sealNativeArtifact(previewArtifact.string(), previewTrust.string(),
                                previewProof, proofError)) {
            std::cerr << diagnostic::format(
                "native-proof", proofError, previewArtifact.string(), 0, 0,
                "inspect the Native Ref/unit proof preview") << "\n";
            return 1;
        }
        VerifiedNativeLibrary previewLoaded;
        if (!loadVerifiedNativeLibrary(previewArtifact.string(),
                                       previewTrust.string(), previewLoaded,
                                       proofError) ||
            !previewLoaded.hasRefUnitCandidate(
                surface.refUnitSource->symbolId.value,
                surface.refUnitSource->contractId.value)) {
            std::cerr << diagnostic::format(
                "native-proof",
                proofError.empty()
                    ? "Native Ref/unit entry is absent from the verified preview"
                    : proofError,
                previewArtifact.string(), 0, 0,
                "keep the Native Ref/unit artifact unpublished") << "\n";
            return 1;
        }
        luna::runtime::MoonRuntime::PinnedRefUnitCandidate candidate;
        luna::codegen::NativeRefUnitSourceFacts source;
        {
            luna::runtime::MoonRuntime previewRuntime;
            luna::runtime::MoonRuntime::PinnedGeneration pinned;
            if (!loadVerifiedNativeGenerationOnce(
                    previewRuntime, previewArtifact.string(),
                    previewTrust.string(), pinned, proofError)) {
                std::cerr << diagnostic::format(
                    "native-proof", proofError, previewArtifact.string(), 0, 0,
                    "keep the Native Ref/unit artifact unpublished") << "\n";
                return 1;
            }
            candidate = pinned.findRefUnitCandidate(
                surface.refUnitSource->symbolId.value,
                surface.refUnitSource->contractId.value);
            const auto facts = candidate.facts();
            if (!luna::codegen::deriveNativeRefUnitSourceFacts(
                    pipeline.moonModule(), *surface.refUnitSource,
                    source, proofError) ||
                !facts || candidate.generationId() != pinned.generationId() ||
                facts->functionTypeId != source.functionTypeId ||
                facts->refTypeId != source.refTypeId ||
                facts->slotSymbolId != source.slotSymbolId ||
                facts->slotContractId != source.slotContractId ||
                facts->slotTypeId != source.slotTypeId ||
                facts->linkageName !=
                    "__luna_native_ref_unit_entry_v1_" +
                        source.functionSymbolId) {
                std::cerr << diagnostic::format(
                    "native-proof",
                    "Native Ref/unit pinned facts differ from the sealed source",
                    previewArtifact.string(), 0, 0,
                    "keep the Native Ref/unit artifact unpublished") << "\n";
                return 1;
            }
        }
        previewLoaded = VerifiedNativeLibrary{};
        const char* previewCall =
            std::getenv("LUNA_NATIVE_REF_UNIT_PREVIEW_CALL_TEST");
        if (previewCall && std::string(previewCall) == "1") {
            if (!exerciseNativeRefPreviewForTest(
                    pipeline.moonModule(), source, candidate,
                    proofError)) {
                std::cerr << diagnostic::format(
                    "native-proof", proofError, previewArtifact.string(),
                    0, 0, "keep the Native Ref/unit artifact unpublished")
                          << "\n";
                return 1;
            }
            std::cerr << diagnostic::format(
                "native-proof",
                "Native Ref/unit private linked preview proved ingress, failure and recovery",
                previewArtifact.string(), 0, 0,
                "keep the Native Ref/unit artifact unpublished")
                      << "\n";
        }
        candidate = {};
        std::error_code cleanupError;
        fs::remove_all(cleanup.directory, cleanupError);
        if (cleanupError) {
            std::cerr << diagnostic::format(
                "native-proof", "cannot remove Native Ref/unit proof preview: " +
                    cleanupError.message(), cleanup.directory.string(), 0, 0,
                "remove the private preview before building again") << "\n";
            return 1;
        }
        cleanup.directory.clear();
        const char* outputTest =
            std::getenv("LUNA_NATIVE_REF_UNIT_OUTPUT_TEST");
        if (outputTest && std::string(outputTest) == "1") {
            if (buildAt(outputPath) != 0) return 1;
            const fs::path trustPath = outputPath.string() + ".trust";
            NativeProofInfo outputProof;
            if (!sealNativeArtifact(outputPath.string(), trustPath.string(),
                                    outputProof, proofError)) {
                std::cerr << diagnostic::format(
                    "native-proof", proofError, outputPath.string(), 0, 0,
                    "discard the test-only Native Ref/unit output") << "\n";
                return 1;
            }
            luna::runtime::MoonRuntime outputRuntime;
            luna::runtime::MoonRuntime::PinnedGeneration outputGeneration;
            if (!loadVerifiedNativeGenerationOnce(
                    outputRuntime, outputPath.string(), trustPath.string(),
                    outputGeneration, proofError)) {
                std::cerr << diagnostic::format(
                    "native-proof", proofError, outputPath.string(), 0, 0,
                    "discard the test-only Native Ref/unit output") << "\n";
                return 1;
            }
            const auto outputCandidate =
                outputGeneration.findRefUnitCandidate(
                    surface.refUnitSource->symbolId.value,
                    surface.refUnitSource->contractId.value);
            if (!outputCandidate ||
                !exerciseNativeRefPreviewForTest(
                    pipeline.moonModule(), source, outputCandidate,
                    proofError)) {
                std::cerr << diagnostic::format(
                    "native-proof",
                    proofError.empty()
                        ? "test-only Native Ref/unit output lost its typed entry"
                        : proofError,
                    outputPath.string(), 0, 0,
                    "discard the test-only Native Ref/unit output") << "\n";
                return 1;
            }
            std::cout << "Sealed test-only Native Ref/unit output: sha256="
                      << nativeDigestHex(outputProof.artifactDigest) << "\n"
                      << "Generated trust candidate: " << trustPath.string()
                      << "\n";
            return 0;
        }
        std::cerr << diagnostic::format(
            "native-proof",
            "Native Ref/unit publication remains gated after pinned typed preview validation",
            inputPath, 0, 0,
            "review the public Ref/unit ABI contract and validate the enabled publication path")
                  << "\n";
        return 1;
    }
    if (buildAt(outputPath) != 0)
        return 1;

    NativeProofInfo proofInfo;
    const fs::path trustRecordPath = outputPath.string() + ".trust";
    if (!sealNativeArtifact(outputPath.string(), trustRecordPath.string(), proofInfo, proofError)) {
        std::cerr << diagnostic::format("native-proof", proofError, outputPath.string(), 0, 0,
                                        "do not distribute an unsealed Native library")
                  << "\n";
        return 1;
    }
    std::cout << "Sealed Native proof: sha256=" << nativeDigestHex(proofInfo.artifactDigest) << "\n"
              << "Generated trust candidate: " << trustRecordPath.string() << "\n";
    return 0;
}

void printJsonHello() {
    std::cout << "{\"protocol\":\"luna.diagnostic\",\"version\":1,"
              << "\"kind\":\"hello\",\"language_version\":\""
              << diagnostic::jsonEscape(LUNA_VERSION_STRING) << "\",\"compiler_commit\":\""
              << diagnostic::jsonEscape(LUNA_COMPILER_COMMIT) << "\",\"build_target\":\""
              << diagnostic::jsonEscape(llvm::sys::getDefaultTargetTriple())
              << "\",\"capabilities\":[\"byte-spans\"]}\n";
}

void printJsonDiagnostics(const std::vector<diagnostic::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cout << diagnostic::toJson(diagnostic) << '\n';
}

void printJsonSummary(size_t errors) {
    std::cout << "{\"protocol\":\"luna.diagnostic\",\"version\":1,"
              << "\"kind\":\"summary\",\"errors\":" << errors
              << ",\"warnings\":0,\"success\":" << (errors == 0 ? "true" : "false") << "}\n";
}

void printAnalysisHello() {
    std::cout << "{\"protocol\":\"luna.analysis\",\"version\":1,"
              << "\"kind\":\"hello\",\"language_version\":\""
              << diagnostic::jsonEscape(LUNA_VERSION_STRING) << "\",\"compiler_commit\":\""
              << diagnostic::jsonEscape(LUNA_COMPILER_COMMIT) << "\",\"build_target\":\""
              << diagnostic::jsonEscape(llvm::sys::getDefaultTargetTriple())
              << "\",\"capabilities\":[\"declarations\",\"call-references\","
              << "\"method-references\",\"type-references\","
              << "\"trait-references\",\"package-references\","
              << "\"field-references\",\"enum-variant-references\","
              << "\"single-document-overlay\","
              << "\"multi-document-overlay\"]}\n";
}

std::optional<SourceOverlays> parseAnalysisOverlays(const std::string& input,
                                                           std::string& error) {
    auto parsed = llvm::json::parse(input);
    if (!parsed) {
        error = "invalid overlay JSON: " + llvm::toString(parsed.takeError());
        return std::nullopt;
    }
    const auto* envelope = parsed->getAsObject();
    if (!envelope || envelope->getString("protocol") != "luna.overlay" ||
        envelope->getInteger("version") != 1) {
        error = "overlay stdin must be a luna.overlay version 1 object";
        return std::nullopt;
    }
    const auto* records = envelope->getArray("overlays");
    if (!records || records->empty()) {
        error = "overlay stdin must contain a non-empty overlays array";
        return std::nullopt;
    }
    SourceOverlays overlays;
    overlays.reserve(records->size());
    for (const auto& record : *records) {
        const auto* object = record.getAsObject();
        const auto path = object ? object->getString("path") : std::nullopt;
        const auto text = object ? object->getString("text") : std::nullopt;
        if (!path || path->empty() || !text) {
            error = "each overlay must contain non-empty path and string text fields";
            return std::nullopt;
        }
        overlays.push_back({path->str(), text->str()});
    }
    return overlays;
}

std::optional<size_t> byteOffsetFromSource(const std::string& source, int line, int column) {
    if (line <= 0 || column <= 0) return std::nullopt;
    size_t offset = 0;
    int currentLine = 1;
    while (currentLine < line && offset < source.size()) {
        if (source[offset++] == '\n') ++currentLine;
    }
    if (currentLine != line) return std::nullopt;
    const size_t columnOffset = static_cast<size_t>(column - 1);
    if (columnOffset > source.size() - offset) return std::nullopt;
    for (size_t index = 0; index < columnOffset; ++index)
        if (source[offset + index] == '\n') return std::nullopt;
    return offset + columnOffset;
}

std::optional<size_t> analysisByteOffset(const tooling::SymbolSourceLocation& location,
                                                const SourceOverlays& overlays) {
    const std::string normalizedLocation = diagnostic::normalizedPath(location.path);
    for (const auto& overlay : overlays) {
        if (normalizedLocation == diagnostic::normalizedPath(overlay.path))
            return byteOffsetFromSource(overlay.source, location.line, location.column);
    }
    return diagnostic::byteOffsetFromFile(location.path, location.line, location.column);
}

bool printAnalysisSymbol(const tooling::IndexedSymbol& symbol,
                                const SourceOverlays& overlays) {
    const auto startByte = analysisByteOffset(symbol.selection, overlays);
    if (!startByte) return false;
    const size_t endByte = *startByte + symbol.selection.byteLength;
    std::cout << "{\"protocol\":\"luna.analysis\",\"version\":1,"
              << "\"kind\":\"symbol\",\"id\":\"" << diagnostic::jsonEscape(symbol.id)
              << "\",\"name\":\"" << diagnostic::jsonEscape(symbol.name)
              << "\",\"qualified_name\":\"" << diagnostic::jsonEscape(symbol.qualifiedName)
              << "\",\"package_id\":\"" << diagnostic::jsonEscape(symbol.packageId)
              << "\",\"module_path\":\"" << diagnostic::jsonEscape(symbol.modulePath)
              << "\",\"linkage_name\":\"" << diagnostic::jsonEscape(symbol.linkageName)
              << "\",\"symbol_kind\":\"" << tooling::indexedSymbolKindName(symbol.kind)
              << "\",\"signature\":\"" << diagnostic::jsonEscape(symbol.signature)
              << "\",\"selection\":{"
              << "\"path\":\""
              << diagnostic::jsonEscape(diagnostic::normalizedPath(symbol.selection.path))
              << "\",\"start\":{\"byte\":" << *startByte << ",\"line\":" << symbol.selection.line
              << ",\"column\":" << symbol.selection.column << "},\"end\":{\"byte\":" << endByte
              << ",\"line\":" << symbol.selection.line << ",\"column\":"
              << symbol.selection.column + static_cast<int>(symbol.selection.byteLength)
              << "}},\"exported\":" << (symbol.exported ? "true" : "false")
              << ",\"external\":" << (symbol.external ? "true" : "false") << "}\n";
    return true;
}

bool printAnalysisReference(const tooling::IndexedReference& reference,
                                   const SourceOverlays& overlays) {
    const auto startByte = analysisByteOffset(reference.source, overlays);
    if (!startByte) return false;
    const size_t endByte = *startByte + reference.source.byteLength;
    std::cout << "{\"protocol\":\"luna.analysis\",\"version\":1,"
              << "\"kind\":\"reference\",\"target_id\":\""
              << diagnostic::jsonEscape(reference.targetId) << "\",\"source\":{"
              << "\"path\":\""
              << diagnostic::jsonEscape(diagnostic::normalizedPath(reference.source.path))
              << "\",\"start\":{\"byte\":" << *startByte << ",\"line\":" << reference.source.line
              << ",\"column\":" << reference.source.column << "},\"end\":{\"byte\":" << endByte
              << ",\"line\":" << reference.source.line << ",\"column\":"
              << reference.source.column + static_cast<int>(reference.source.byteLength) << "}}}\n";
    return true;
}

void printAnalysisSummary(size_t symbols, size_t references, bool complete) {
    std::cout << "{\"protocol\":\"luna.analysis\",\"version\":1,"
              << "\"kind\":\"summary\",\"symbols\":" << symbols << ",\"references\":" << references
              << ",\"complete\":" << (complete ? "true" : "false") << "}\n";
}

bool requestsJsonOutput(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--message-format=json") return true;
        if (argument == "--message-format" && index + 1 < argc &&
            std::string(argv[index + 1]) == "json")
            return true;
    }
    return false;
}

void printUsage() {
    std::cout << R"(
╔══════════════════════════════════════════════╗
║           Luna — Luna Language          ║
║   A programming language built with LLVM    ║
╚══════════════════════════════════════════════╝

Usage:
  luna --version            Print the compiler version
  luna check  <file-or-package> [--emit-moonir <path>]
                   [--message-format=json]  Verify through MoonIR
  luna analyze <file-or-package> --message-format=json
                   [--overlay <document> | --overlays-from-stdin]
                                           Emit a semantic snapshot;
                                           overlay source is read from stdin
  luna run    <file-or-package> [-O0|-O2|-O3] [--link <shared-library>]
                   [--gpu-target <target[,target...]>]  JIT-compile and execute
  luna build  <package> [-O0|-O2|-O3] [-t native|moon|cffi] [-o <path>]
                   [--link <library-or-name>]
                   [--runtime-lib <path>] [--cc <compiler>]
                   [--gpu-target <target[,target...]>]
                   [--reserve-kernel-runtime] [--emit-moonir <path>]
                   [--moon-cost-report]  AOT-compile to executable
  luna repl                  Interactive REPL (JIT)

Features:
  • Stack allocation (let x: i32 = 42)
  • Heap allocation (let p = new i32(100))
  • Auto type inference (let z = x + 1)
  • Templates / Generics (fn id<T>(x: T) -> T { x })
  • Trait constraints (fn sum<T: Addable>(a: T, b: T) -> T)
  • Where clauses (where T: Clone)
  • Named compile-time constraints (constraint Small<T> = ...)
  • Iterable static selector views and declaration reflection
  • Algebraic data types (struct Point { x: i32; }, enum Option<T> { ... })
  • Reverse-DNS packages with module/submodule source identities and explicit exports
  • Nominal ADT binding with structural fields and layouts
  • Ownership system (move, borrow, auto-free)
  • Linear values and strict shared/mutable borrow checking (linear, borrow mut, &mut)
  • Heterogeneous compute surface (kernel fn, device_buffer<T>, launch, await)
  • GPU code targets via --gpu-target=sim|cuda[:sm_*]|rocm[:gfx*]
  • Runtime backend via LUNA_GPU_BACKEND=sim|cuda|rocm (default: sim)

Examples: see examples/*.luna
)";
}

bool loadJITLibraries(const std::vector<std::string>& libraries) {
    for (const auto& library : libraries) {
        std::string error;
        if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(library.c_str(), &error)) {
            std::cerr << "Cannot load JIT library '" << library << "': " << error << "\n";
            return false;
        }
    }
    return true;
}

} // namespace luna::driver::driver_detail
