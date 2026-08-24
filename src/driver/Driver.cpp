#include "driver/Driver.h"
#include "driver/AotLinker.h"
#include "driver/CommandLine.h"
#include "driver/CompilerPipeline.h"
#include "driver/NativeArtifact.h"
#include "driver/Repl.h"
#include "moonir/ContainerModel.h"
#include "moonir/Printer.h"
#include "runtime/Runtime.h"
#include "tooling/AnalysisSnapshot.h"
#include "Version.h"
#include "package/Package.h"
#include "diagnostics/Diagnostic.h"
#include <llvm/Support/DynamicLibrary.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/TargetSelect.h>
#include <llvm/ExecutionEngine/Orc/JITTargetMachineBuilder.h>
#include <llvm/TargetParser/Host.h>

#include <iostream>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace luna::driver {

using SourceOverlays = std::vector<PackageRequest::SourceOverlay>;

#ifndef LUNA_COMPILER_COMMIT
#define LUNA_COMPILER_COMMIT "unknown"
#endif

static void printErrors(const std::vector<diagnostic::Diagnostic>& errors,
                        const char* stage = nullptr) {
    for (auto& e : errors) {
        if (stage) std::cerr << "error[" << stage << "]: ";
        std::cerr << e << "\n";
    }
}

static int buildMoonContainer(
    const CompilerPipeline& pipeline, const std::string& inputPath,
    const std::string& outputOverride) {
    namespace fs = std::filesystem;
    if (!fs::is_directory(fs::path(inputPath))) {
        std::cerr << diagnostic::format(
            "driver", "-t moon requires a package directory",
            inputPath, 0, 0,
            "pass the directory containing luna.package") << "\n";
        return 1;
    }
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    if (package.id.empty() || package.version.empty() ||
        package.kind == PackageKind::Unspecified) {
        std::cerr << diagnostic::format(
            "driver", "-t moon requires an explicit package manifest kind",
            inputPath, 0, 0,
            "set kind = \"application\" or kind = \"library\" in luna.package")
                  << "\n";
        return 1;
    }

    moon::DeclarationRef entrypoint;
    size_t mainCount = 0;
    for (const auto& declaration : pipeline.moonModule().declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (!function || function->packageId != package.id ||
            function->name != "main") continue;
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
                : "remove main or set kind = \"application\"") << "\n";
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
    if (!moon::ContainerModelCodec::encodeContainer(
            manifest, pipeline.moonModule(), encoded, error)) {
        std::cerr << diagnostic::format(
            "moon-container", error, inputPath, 0, 0,
            "the package must lower to closed, verified canonical MoonIR") << "\n";
        return 1;
    }
    moon::ContainerManifest verifiedManifest;
    moon::Module verifiedModule;
    if (!moon::ContainerModelCodec::decodeContainerForTarget(
            encoded, manifest.targetTriple, manifest.dataLayout,
            verifiedManifest, verifiedModule, error)) {
        std::cerr << diagnostic::format(
            "moon-container", "generated container failed self-verification: " + error,
            inputPath, 0, 0,
            "report this compiler defect with the input package") << "\n";
        return 1;
    }

    std::string artifactName = package.id;
    if (const auto separator = artifactName.rfind('.');
        separator != std::string::npos)
        artifactName = artifactName.substr(separator + 1);
    fs::path outputPath;
    if (!outputOverride.empty()) {
        outputPath = outputOverride;
    } else {
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) /
            "build" / "moon" / (artifactName + ".moon");
    }
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
        fs::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
            "driver", "cannot create Moon output directory: " +
                filesystemError.message(), outputPath.string(), 0, 0,
            "check the output path permissions") << "\n";
        return 1;
    }
    std::ofstream output(outputPath, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(encoded.data()),
                 static_cast<std::streamsize>(encoded.size()));
    if (!output) {
        std::cerr << diagnostic::format(
            "driver", "cannot write Moon Container", outputPath.string(),
            0, 0, "check the output path permissions") << "\n";
        return 1;
    }
    std::cout << "Built Moon Container: " << outputPath.string() << "\n";
    return 0;
}

static std::string packageArtifactName(const std::string& packageId) {
    const auto separator = packageId.rfind('.');
    return separator == std::string::npos
        ? packageId : packageId.substr(separator + 1);
}

static bool isCIdentifier(const std::string& value) {
    if (value.empty() ||
        !(std::isalpha(static_cast<unsigned char>(value.front())) ||
          value.front() == '_'))
        return false;
    for (const char character : value) {
        if (!(std::isalnum(static_cast<unsigned char>(character)) ||
              character == '_'))
            return false;
    }
    return true;
}

static std::string cTypeName(const moon::Module& module,
                             const moon::TypeRef& reference,
                             std::string& error) {
    const auto* type = module.findType(reference);
    if (!type) {
        error = "C export references a missing sealed MoonIR type '" +
            reference.value + "'";
        return {};
    }
    switch (type->kind) {
        case TypeKind::I8: return "int8_t";
        case TypeKind::I16: return "int16_t";
        case TypeKind::I32: return "int32_t";
        case TypeKind::I64: return "int64_t";
        case TypeKind::U8: return "uint8_t";
        case TypeKind::U16: return "uint16_t";
        case TypeKind::U32: return "uint32_t";
        case TypeKind::U64: return "uint64_t";
        case TypeKind::USize: return "size_t";
        case TypeKind::ISize: return "ptrdiff_t";
        case TypeKind::F32: return "float";
        case TypeKind::F64: return "double";
        case TypeKind::CStr: return "const char *";
        case TypeKind::RawPointer: return "void *";
        case TypeKind::Unit: return "void";
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

static bool collectCffiExports(
    const CompilerPipeline& pipeline,
    std::vector<const moon::FunctionDecl*>& exports,
    std::string& error) {
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    if (package.id.empty() || package.version.empty() ||
        package.kind == PackageKind::Unspecified) {
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
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "main") ++mainCount;
        if (!declaration->isExported) continue;
        if (!function || function->abi != "C" || function->isExtern) {
            error = "CFFI public surface contains non-C export '" +
                declaration->name + "'; use `export \"C\" fn` only";
            return false;
        }
        const std::string symbol = function->linkName.empty()
            ? function->generatedSymbolName : function->linkName;
        if (!isCIdentifier(symbol)) {
            error = "C export '" + function->name +
                "' has a link symbol that is not a portable C identifier: '" +
                symbol + "'";
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

static int buildCffiLibrary(
    const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
    const std::string& inputPath, const std::vector<std::string>& linkLibraries,
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
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) /
            "build" / "cffi";
#ifdef _WIN32
        outputPath /= artifactName + ".dll";
#elif defined(__APPLE__)
        outputPath /= "lib" + artifactName + ".dylib";
#else
        outputPath /= "lib" + artifactName + ".so";
#endif
    }
    const fs::path headerPath = outputPath.parent_path() /
        (artifactName + ".h");
    std::error_code filesystemError;
    if (!outputPath.parent_path().empty())
        fs::create_directories(outputPath.parent_path(), filesystemError);
    if (filesystemError) {
        std::cerr << diagnostic::format(
            "driver", "cannot create CFFI output directory: " +
                filesystemError.message(), outputPath.string(), 0, 0,
            "check the output path permissions") << "\n";
        return 1;
    }

    std::ostringstream header;
    std::string guard = "LUNA_CFFI_" + artifactName + "_H";
    for (char& character : guard) {
        if (std::isalnum(static_cast<unsigned char>(character)))
            character = static_cast<char>(
                std::toupper(static_cast<unsigned char>(character)));
        else
            character = '_';
    }
    header << "#ifndef " << guard << "\n#define " << guard
           << "\n\n#include <stddef.h>\n#include <stdint.h>\n\n"
           << "#ifdef __cplusplus\nextern \"C\" {\n#endif\n\n";
    for (const auto* function : exports) {
        std::string typeError;
        const std::string returnType = cTypeName(
            pipeline.moonModule(), function->returnType, typeError);
        if (returnType.empty()) {
            std::cerr << diagnostic::format(
                "driver", typeError, function->location.path,
                function->location.line, function->location.column,
                "use a C-ABI-safe scalar, pointer, cstr, reference, or unit")
                      << "\n";
            return 1;
        }
        const std::string symbol = function->linkName.empty()
            ? function->generatedSymbolName : function->linkName;
        header << returnType << ' ' << symbol << '(';
        if (function->params.empty()) {
            header << "void";
        } else {
            for (size_t index = 0; index < function->params.size(); ++index) {
                if (index) header << ", ";
                const std::string parameterType = cTypeName(
                    pipeline.moonModule(), function->params[index].type,
                    typeError);
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
        }) != 0)
        return 1;

    std::ofstream output(headerPath, std::ios::binary | std::ios::trunc);
    output << header.str();
    if (!output) {
        std::cerr << diagnostic::format(
            "driver", "cannot write generated CFFI header",
            headerPath.string(), 0, 0,
            "check the output path permissions") << "\n";
        return 1;
    }
    std::cout << "Generated C header: " << headerPath.string() << "\n";
    return 0;
}

static int buildNativeLibrary(
    const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
    const std::string& inputPath, const std::vector<std::string>& linkLibraries,
    const std::string& runtimeLibrary, const std::string& aotCompiler,
    const std::string& outputOverride,
    LunaOptimizationLevel optimizationLevel) {
    namespace fs = std::filesystem;
    const auto& package = pipeline.analysisSnapshot().packageManifest();
    NativeProofSpec spec;
    spec.packageId = package.id;
    spec.packageVersion = package.version;
    spec.targetAbi = llvm::sys::getProcessTriple();
    spec.compilerIdentity = std::string("luna/") + LUNA_VERSION_STRING + "@" +
        LUNA_COMPILER_COMMIT;
    std::vector<NativeExportSpec> nativeExports;

    for (const auto& declaration : pipeline.moonModule().declarations) {
        if (!declaration) continue;
        if (declaration->isExported && declaration->packageId == package.id) {
            if (const auto* function =
                    dynamic_cast<const moon::FunctionDecl*>(declaration.get());
                function && (function->isExtern || function->abi == "C")) {
                std::cerr << diagnostic::format(
                    "native-proof",
                    "Native public surface contains foreign C export '" +
                        function->name + "'",
                    function->location.path, function->location.line,
                    function->location.column,
                    "use ordinary `export fn` for trusted Native ABI or build `-t cffi`")
                          << "\n";
                return 1;
            }
            const moon::DeclarationRecord* record = nullptr;
            for (const auto& candidate :
                 pipeline.moonModule().declarationTable) {
                if (candidate.symbolId == declaration->symbolId) {
                    record = &candidate;
                    break;
                }
            }
            if (!record) {
                std::cerr << diagnostic::format(
                    "native-proof",
                    "Native export has no sealed declaration record",
                    declaration->location.path, declaration->location.line,
                    declaration->location.column,
                    "report this compiler defect with the package") << "\n";
                return 1;
            }
            NativeExportSpec nativeExport;
            nativeExport.declarationKind =
                static_cast<uint32_t>(record->kind) + 1;
            nativeExport.flags =
                dynamic_cast<const moon::FunctionDecl*>(declaration.get())
                ? LUNA_NATIVE_EXPORT_CALLABLE_V1 : 0;
            nativeExport.symbolId = record->symbolId.value;
            nativeExport.contractId = record->contractId.value;
            nativeExport.linkageName = record->linkageName;
            spec.exportedDescriptors.push_back(
                canonicalNativeExport(nativeExport));
            nativeExports.push_back(std::move(nativeExport));
        }
    }

    std::vector<uint8_t> proofRecord;
    std::string proofError;
    if (!makeNativeProofPlaceholder(spec, proofRecord, proofError) ||
        !codeGenerator.emitNativeProofPlaceholder(proofRecord) ||
        !codeGenerator.emitNativeLibraryDescriptor(
            spec.packageId, spec.packageVersion, spec.targetAbi,
            spec.compilerIdentity, nativeExports)) {
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
        outputPath = fs::path(pipeline.analysisSnapshot().packageRootPath()) /
            "build" / "native";
#ifdef _WIN32
        outputPath /= artifactName + ".dll";
#elif defined(__APPLE__)
        outputPath /= "lib" + artifactName + ".dylib";
#else
        outputPath /= "lib" + artifactName + ".so";
#endif
    }
    if (AotLinker::build(codeGenerator, {
            inputPath,
            pipeline.declaredPackageName(),
            linkLibraries,
            runtimeLibrary,
            aotCompiler,
            outputPath.string(),
            optimizationLevel,
            AotArtifactKind::SharedLibrary,
        }) != 0)
        return 1;

    NativeProofInfo proofInfo;
    const fs::path trustRecordPath = outputPath.string() + ".trust";
    if (!sealNativeArtifact(outputPath.string(), trustRecordPath.string(),
                            proofInfo, proofError)) {
        std::cerr << diagnostic::format(
            "native-proof", proofError, outputPath.string(), 0, 0,
            "do not distribute an unsealed Native library") << "\n";
        return 1;
    }
    std::cout << "Sealed Native proof: sha256="
              << nativeDigestHex(proofInfo.artifactDigest) << "\n"
              << "Generated trust candidate: " << trustRecordPath.string()
              << "\n";
    return 0;
}

static void printJsonHello() {
    std::cout
        << "{\"protocol\":\"luna.diagnostic\",\"version\":1,"
        << "\"kind\":\"hello\",\"language_version\":\""
        << diagnostic::jsonEscape(LUNA_VERSION_STRING)
        << "\",\"compiler_commit\":\""
        << diagnostic::jsonEscape(LUNA_COMPILER_COMMIT)
        << "\",\"build_target\":\""
        << diagnostic::jsonEscape(llvm::sys::getDefaultTargetTriple())
        << "\",\"capabilities\":[\"byte-spans\"]}\n";
}

static void printJsonDiagnostics(
    const std::vector<diagnostic::Diagnostic>& diagnostics) {
    for (const auto& diagnostic : diagnostics)
        std::cout << diagnostic::toJson(diagnostic) << '\n';
}

static void printJsonSummary(size_t errors) {
    std::cout << "{\"protocol\":\"luna.diagnostic\",\"version\":1,"
              << "\"kind\":\"summary\",\"errors\":" << errors
              << ",\"warnings\":0,\"success\":"
              << (errors == 0 ? "true" : "false") << "}\n";
}

static void printAnalysisHello() {
    std::cout
        << "{\"protocol\":\"luna.analysis\",\"version\":1,"
        << "\"kind\":\"hello\",\"language_version\":\""
        << diagnostic::jsonEscape(LUNA_VERSION_STRING)
        << "\",\"compiler_commit\":\""
        << diagnostic::jsonEscape(LUNA_COMPILER_COMMIT)
        << "\",\"build_target\":\""
        << diagnostic::jsonEscape(llvm::sys::getDefaultTargetTriple())
        << "\",\"capabilities\":[\"declarations\",\"call-references\","
        << "\"method-references\",\"type-references\","
        << "\"trait-references\",\"package-references\","
        << "\"field-references\",\"enum-variant-references\","
        << "\"single-document-overlay\","
        << "\"multi-document-overlay\"]}\n";
}

static std::optional<SourceOverlays> parseAnalysisOverlays(
    const std::string& input, std::string& error) {
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

static std::optional<size_t> byteOffsetFromSource(
    const std::string& source, int line, int column) {
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

static std::optional<size_t> analysisByteOffset(
    const tooling::SymbolSourceLocation& location,
    const SourceOverlays& overlays) {
    const std::string normalizedLocation =
        diagnostic::normalizedPath(location.path);
    for (const auto& overlay : overlays) {
        if (normalizedLocation == diagnostic::normalizedPath(overlay.path))
            return byteOffsetFromSource(
                overlay.source, location.line, location.column);
    }
    return diagnostic::byteOffsetFromFile(
        location.path, location.line, location.column);
}

static bool printAnalysisSymbol(
    const tooling::IndexedSymbol& symbol, const SourceOverlays& overlays) {
    const auto startByte = analysisByteOffset(symbol.selection, overlays);
    if (!startByte) return false;
    const size_t endByte = *startByte + symbol.selection.byteLength;
    std::cout
        << "{\"protocol\":\"luna.analysis\",\"version\":1,"
        << "\"kind\":\"symbol\",\"id\":\""
        << diagnostic::jsonEscape(symbol.id) << "\",\"name\":\""
        << diagnostic::jsonEscape(symbol.name) << "\",\"qualified_name\":\""
        << diagnostic::jsonEscape(symbol.qualifiedName) << "\",\"package_id\":\""
        << diagnostic::jsonEscape(symbol.packageId) << "\",\"module_path\":\""
        << diagnostic::jsonEscape(symbol.modulePath) << "\",\"linkage_name\":\""
        << diagnostic::jsonEscape(symbol.linkageName) << "\",\"symbol_kind\":\""
        << tooling::indexedSymbolKindName(symbol.kind) << "\",\"signature\":\""
        << diagnostic::jsonEscape(symbol.signature) << "\",\"selection\":{"
        << "\"path\":\""
        << diagnostic::jsonEscape(diagnostic::normalizedPath(symbol.selection.path))
        << "\",\"start\":{\"byte\":" << *startByte
        << ",\"line\":" << symbol.selection.line
        << ",\"column\":" << symbol.selection.column
        << "},\"end\":{\"byte\":" << endByte
        << ",\"line\":" << symbol.selection.line
        << ",\"column\":"
        << symbol.selection.column + static_cast<int>(symbol.selection.byteLength)
        << "}},\"exported\":" << (symbol.exported ? "true" : "false")
        << ",\"external\":" << (symbol.external ? "true" : "false") << "}\n";
    return true;
}

static bool printAnalysisReference(
    const tooling::IndexedReference& reference,
    const SourceOverlays& overlays) {
    const auto startByte = analysisByteOffset(reference.source, overlays);
    if (!startByte) return false;
    const size_t endByte = *startByte + reference.source.byteLength;
    std::cout
        << "{\"protocol\":\"luna.analysis\",\"version\":1,"
        << "\"kind\":\"reference\",\"target_id\":\""
        << diagnostic::jsonEscape(reference.targetId) << "\",\"source\":{"
        << "\"path\":\""
        << diagnostic::jsonEscape(diagnostic::normalizedPath(reference.source.path))
        << "\",\"start\":{\"byte\":" << *startByte
        << ",\"line\":" << reference.source.line
        << ",\"column\":" << reference.source.column
        << "},\"end\":{\"byte\":" << endByte
        << ",\"line\":" << reference.source.line
        << ",\"column\":"
        << reference.source.column + static_cast<int>(reference.source.byteLength)
        << "}}}\n";
    return true;
}

static void printAnalysisSummary(size_t symbols, size_t references,
                                 bool complete) {
    std::cout << "{\"protocol\":\"luna.analysis\",\"version\":1,"
              << "\"kind\":\"summary\",\"symbols\":" << symbols
              << ",\"references\":" << references
              << ",\"complete\":" << (complete ? "true" : "false")
              << "}\n";
}

static bool requestsJsonOutput(int argc, char* argv[]) {
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--message-format=json") return true;
        if (argument == "--message-format" && index + 1 < argc &&
            std::string(argv[index + 1]) == "json")
            return true;
    }
    return false;
}

static void printUsage() {
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

static bool loadJITLibraries(const std::vector<std::string>& libraries) {
    for (const auto& library : libraries) {
        std::string error;
        if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(library.c_str(), &error)) {
            std::cerr << "Cannot load JIT library '" << library << "': " << error << "\n";
            return false;
        }
    }
    return true;
}

int run(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage();
        return 0;
    }

    std::string cmd = argv[1];

    if (cmd == "--version" || cmd == "-V" || cmd == "version") {
        std::cout << "Luna " << LUNA_VERSION_STRING << "\n";
        return 0;
    }

    if (cmd == "repl") {
        return runRepl(std::cin, std::cout, std::cerr);
    }

    auto parseResult = parseCommandLine(argc, argv);
    if (!parseResult.options) {
        if (requestsJsonOutput(argc, argv)) {
            if (argc > 1 && std::string(argv[1]) == "analyze") {
                printAnalysisHello();
                printAnalysisSummary(0, 0, false);
            } else {
                printJsonHello();
                const auto invocationError = diagnostic::format(
                    "driver", parseResult.error, "", 0, 0,
                    "run `luna` without arguments to view supported commands");
                printJsonDiagnostics({invocationError});
                printJsonSummary(1);
            }
            return 2;
        }
        std::cerr << parseResult.error << "\n";
        if (parseResult.showUsage) printUsage();
        return 1;
    }
    auto options = std::move(*parseResult.options);
    auto& filePath = options.inputPath;
    auto& linkLibraries = options.linkLibraries;
    auto& runtimeLibrary = options.runtimeLibrary;
    auto& aotCompiler = options.aotCompiler;
    auto& outputPath = options.outputPath;
    auto& moonIrOutput = options.moonIrOutput;
    auto& overlayPath = options.overlayPath;
    const bool overlaysFromStdin = options.overlaysFromStdin;
    auto& gpuTargets = options.gpuTargets;
    auto& printMoonCostReport = options.printMoonCostReport;
    auto& reserveKernelRuntime = options.reserveKernelRuntime;
    auto& optimizationLevel = options.optimizationLevel;
    const auto artifactTarget = options.artifactTarget;
    const bool jsonDiagnostics =
        options.messageFormat == MessageFormat::Json && cmd == "check";

    if (jsonDiagnostics) printJsonHello();

    if (cmd == "analyze") {
        printAnalysisHello();
        SourceOverlays overlays;
        if (!overlayPath.empty() || overlaysFromStdin) {
            std::ostringstream input;
            input << std::cin.rdbuf();
            if (std::cin.bad()) {
                std::cerr << "failed to read analysis overlay stdin\n";
                printAnalysisSummary(0, 0, false);
                return 2;
            }
            if (overlaysFromStdin) {
                std::string overlayError;
                auto parsed = parseAnalysisOverlays(input.str(), overlayError);
                if (!parsed) {
                    std::cerr << overlayError << '\n';
                    printAnalysisSummary(0, 0, false);
                    return 2;
                }
                overlays = std::move(*parsed);
            } else {
                overlays.push_back({overlayPath, input.str()});
            }
        }
        auto snapshot = overlays.empty()
            ? tooling::AnalysisSnapshot::analyzePath(filePath)
            : tooling::AnalysisSnapshot::analyzePathWithOverlays(
                filePath, overlays);
        size_t emitted = 0;
        size_t emittedReferences = 0;
        bool locationsComplete = true;
        for (const auto& symbol : snapshot.symbolIndex().declarations()) {
            if (printAnalysisSymbol(symbol, overlays))
                ++emitted;
            else
                locationsComplete = false;
        }
        for (const auto& reference : snapshot.referenceIndex().references()) {
            if (printAnalysisReference(reference, overlays))
                ++emittedReferences;
            else
                locationsComplete = false;
        }
        printAnalysisSummary(
            emitted, emittedReferences,
            snapshot.success() && locationsComplete);
        return snapshot.success() && locationsComplete ? 0 : 1;
    }

    if (cmd != "build" && (!runtimeLibrary.empty() || !aotCompiler.empty())) {
        const auto invocationError = diagnostic::format(
            "driver", "AOT linker options are only valid with `build`",
            "", 0, 0,
            "use `luna build ... --runtime-lib <path> --cc <compiler>");
        if (jsonDiagnostics) {
            printJsonDiagnostics({invocationError});
            printJsonSummary(1);
            return 2;
        }
        std::cerr << invocationError << "\n";
        return 1;
    }

    if (cmd == "build") {
        namespace fs = std::filesystem;
        const fs::path packagePath(filePath);
        std::error_code filesystemError;
        if (!fs::is_directory(packagePath, filesystemError) ||
            !fs::is_regular_file(
                packagePath / "luna.package", filesystemError)) {
            std::cerr << diagnostic::format(
                "driver",
                "formal artifact builds require a package directory with luna.package",
                filePath, 0, 0,
                "use standalone files with `check`, `run`, or `analyze`; pass a package directory to `build`")
                      << "\n";
            return 1;
        }
    }

    CompilerPipeline pipeline;
    if (!pipeline.compileToMoonIR({
            filePath,
            optimizationLevel,
            reserveKernelRuntime,
            cmd == "build"})) {
        if (jsonDiagnostics) {
            printJsonDiagnostics(pipeline.errors());
            printJsonSummary(pipeline.errors().size());
        } else {
            printErrors(
                pipeline.errors(),
                pipeline.errorStage().empty()
                    ? nullptr : pipeline.errorStage().c_str());
        }
        return 1;
    }

    moon::Printer moonPrinter;
    if (!moonIrOutput.empty()) {
        std::ofstream output(moonIrOutput);
        if (!output) {
            const auto outputError = diagnostic::format(
                "driver", "cannot write MoonIR file '" + moonIrOutput + "'",
                moonIrOutput, 0, 0,
                "check the output directory and permissions");
            if (jsonDiagnostics) {
                printJsonDiagnostics({outputError});
                printJsonSummary(1);
            } else {
                std::cerr << outputError << "\n";
            }
            return 1;
        }
        moonPrinter.print(pipeline.moonModule(), output);
    }
    if (printMoonCostReport)
        moonPrinter.printCostReport(pipeline.moonModule(), std::cout);

    // Library packages deliberately have no main function. `check` validates
    // the complete frontend -> MoonIR boundary without manufacturing an
    // executable entry point or paying LLVM code-generation costs.
    if (cmd == "check") {
        if (jsonDiagnostics) printJsonSummary(0);
        return 0;
    }

    if (cmd == "build" && artifactTarget == ArtifactTarget::Moon)
        return buildMoonContainer(pipeline, filePath, outputPath);

    if (cmd == "build") {
        const auto& package = pipeline.analysisSnapshot().packageManifest();
        if (package.kind == PackageKind::Unspecified) {
            std::cerr << diagnostic::format(
                "driver", "formal artifact builds require an explicit package kind",
                filePath, 0, 0,
                "set kind = \"application\" or kind = \"library\" in luna.package")
                      << "\n";
            return 1;
        }
        if (artifactTarget == ArtifactTarget::Cffi) {
            std::vector<const moon::FunctionDecl*> exports;
            std::string cffiError;
            if (!std::filesystem::is_directory(std::filesystem::path(filePath)) ||
                !collectCffiExports(pipeline, exports, cffiError)) {
                if (cffiError.empty())
                    cffiError = "-t cffi requires a package directory";
                std::cerr << diagnostic::format(
                    "driver", cffiError, filePath, 0, 0,
                    "pass the directory containing a library luna.package")
                          << "\n";
                return 1;
            }
        } else if (artifactTarget == ArtifactTarget::Native) {
            if (package.kind == PackageKind::Library) {
                size_t mainCount = 0;
                for (const auto& declaration : pipeline.moonModule().declarations) {
                    const auto* function =
                        dynamic_cast<const moon::FunctionDecl*>(declaration.get());
                    if (function && function->packageId == package.id &&
                        function->name == "main")
                        ++mainCount;
                }
                if (mainCount != 0) {
                    std::cerr << diagnostic::format(
                        "driver", "Native library must not contain a package main",
                        filePath, 0, 0,
                        "remove main or set kind = \"application\"") << "\n";
                    return 1;
                }
            } else {
                size_t mainCount = 0;
                for (const auto& declaration :
                     pipeline.moonModule().declarations) {
                    const auto* function =
                        dynamic_cast<const moon::FunctionDecl*>(
                            declaration.get());
                    if (function && function->packageId == package.id &&
                        function->name == "main")
                        ++mainCount;
                }
                if (mainCount != 1) {
                    std::cerr << diagnostic::format(
                        "driver",
                        "Native application must contain exactly one package main",
                        filePath, 0, 0,
                        "define exactly one `fn main() -> i32` in the root package")
                              << "\n";
                    return 1;
                }
            }
        }
    }

    if (!pipeline.generateCode(std::move(gpuTargets))) {
        printErrors(pipeline.errors());
        return 1;
    }
    auto& cg = pipeline.codeGenerator();

    if (cmd == "run") {
        if (!loadJITLibraries(linkLibraries)) return 1;
        int result = cg.jitRun();
        // Keep the CLI marker after all observable program output on every
        // CRT, including MinGW/UCRT under redirected GitHub Actions stdout.
        std::fflush(stdout);
        std::cout << "Program exited with code: " << result << std::endl;
        return result;
    }

    if (cmd == "build" && artifactTarget == ArtifactTarget::Cffi)
        return buildCffiLibrary(
            pipeline, cg, filePath, linkLibraries, runtimeLibrary,
            aotCompiler, outputPath, optimizationLevel);

    if (cmd == "build" && artifactTarget == ArtifactTarget::Native &&
        pipeline.analysisSnapshot().packageManifest().kind ==
            PackageKind::Library)
        return buildNativeLibrary(
            pipeline, cg, filePath, linkLibraries, runtimeLibrary,
            aotCompiler, outputPath, optimizationLevel);

    if (cmd == "build") {
        std::string nativeOutputPath = outputPath;
        if (nativeOutputPath.empty()) {
            const auto& package = pipeline.analysisSnapshot().packageManifest();
            std::filesystem::path artifactPath =
                std::filesystem::path(
                    pipeline.analysisSnapshot().packageRootPath()) /
                "build" / "native" / packageArtifactName(package.id);
#ifdef _WIN32
            artifactPath += ".exe";
#endif
            nativeOutputPath = artifactPath.string();
        }
        return AotLinker::build(cg, {
            filePath,
            pipeline.declaredPackageName(),
            linkLibraries,
            runtimeLibrary,
            aotCompiler,
            nativeOutputPath,
            optimizationLevel,
            AotArtifactKind::Executable,
        });
    }

    std::cerr << "Unknown command: " << cmd << "\n";
    printUsage();
    return 1;
}

} // namespace luna::driver
