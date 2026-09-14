#pragma once

#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "driver/CompilerPipeline.h"
#include "package/Package.h"
#include "tooling/AnalysisSnapshot.h"

#include <optional>
#include <string>
#include <vector>

namespace luna::driver::driver_detail {

using SourceOverlays = std::vector<PackageRequest::SourceOverlay>;

void printErrors(
    const std::vector<diagnostic::Diagnostic>& errors,
    const char* stage = nullptr);
int buildMoonContainer(
    const CompilerPipeline& pipeline, const std::string& inputPath,
    const std::string& outputOverride);
std::string packageArtifactName(const std::string& packageId);
bool collectCffiExports(
    const CompilerPipeline& pipeline,
    std::vector<const moon::FunctionDecl*>& exports,
    std::string& error);
int buildCffiLibrary(
    const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
    const std::string& inputPath,
    const std::vector<std::string>& linkLibraries,
    const std::string& runtimeLibrary, const std::string& aotCompiler,
    const std::string& outputOverride,
    LunaOptimizationLevel optimizationLevel);
int buildNativeLibrary(
    const CompilerPipeline& pipeline, CodeGenerator& codeGenerator,
    const std::string& inputPath,
    const std::vector<std::string>& linkLibraries,
    const std::string& runtimeLibrary, const std::string& aotCompiler,
    const std::string& outputOverride,
    LunaOptimizationLevel optimizationLevel);

void printJsonHello();
void printJsonDiagnostics(
    const std::vector<diagnostic::Diagnostic>& diagnostics);
void printJsonSummary(size_t errors);
void printAnalysisHello();
std::optional<SourceOverlays> parseAnalysisOverlays(
    const std::string& input, std::string& error);
bool printAnalysisSymbol(
    const tooling::IndexedSymbol& symbol, const SourceOverlays& overlays);
bool printAnalysisReference(
    const tooling::IndexedReference& reference,
    const SourceOverlays& overlays);
void printAnalysisSummary(
    size_t symbols, size_t references, bool complete);
bool requestsJsonOutput(int argc, char* argv[]);
void printUsage();
bool loadJITLibraries(const std::vector<std::string>& libraries);

} // namespace luna::driver::driver_detail
