#pragma once

#include "diagnostics/Diagnostic.h"
#include "package/PackageManager.h"
#include "tooling/SymbolIndex.h"
#include "tooling/ReferenceIndex.h"

#include <memory>
#include <string>
#include <vector>

class SemanticAnalyzer;
class SymbolTable;
namespace luna::selector {
class SymbolCatalog;
}

namespace luna::tooling {

// Owns one immutable-after-analysis frontend result. Keeping Program and
// SemanticAnalyzer alive together preserves typed AST nodes, generated
// instances, type objects, and symbol witnesses for read-only tooling.
class AnalysisSnapshot {
public:
    AnalysisSnapshot();
    ~AnalysisSnapshot();
    AnalysisSnapshot(AnalysisSnapshot&&) noexcept;
    AnalysisSnapshot& operator=(AnalysisSnapshot&&) noexcept;
    AnalysisSnapshot(const AnalysisSnapshot&) = delete;
    AnalysisSnapshot& operator=(const AnalysisSnapshot&) = delete;

    static AnalysisSnapshot analyzePath(const std::string& inputPath);
    static AnalysisSnapshot analyzePathWithOverlay(
        const std::string& inputPath, const std::string& documentPath,
        const std::string& source);
    static AnalysisSnapshot analyzePathWithOverlays(
        const std::string& inputPath,
        const std::vector<PackageRequest::SourceOverlay>& overlays);
    static AnalysisSnapshot analyzeSource(
        const std::string& source, const std::string& documentId);

    bool success() const { return mSuccess; }
    const Program* program() const { return mProgram.get(); }
    const SymbolTable* symbolTable() const;
    const luna::selector::SymbolCatalog* symbolCatalog() const;
    const PackageGraph& packageGraph() const { return mPackageGraph; }
    const PackageManifest& packageManifest() const { return mPackageManifest; }
    const std::string& packageRootPath() const { return mPackageRootPath; }
    const SymbolIndex& symbolIndex() const { return mSymbolIndex; }
    const ReferenceIndex& referenceIndex() const { return mReferenceIndex; }
    const std::vector<diagnostic::Diagnostic>& errors() const {
        return mErrors;
    }
    const std::string& errorStage() const { return mErrorStage; }

private:
    bool analyzeProgram();
    bool fail(const std::vector<diagnostic::Diagnostic>& errors,
              std::string stage = {});

    std::unique_ptr<Program> mProgram;
    std::unique_ptr<SemanticAnalyzer> mSemanticAnalyzer;
    PackageGraph mPackageGraph;
    PackageManifest mPackageManifest;
    std::string mPackageRootPath;
    SymbolIndex mSymbolIndex;
    ReferenceIndex mReferenceIndex;
    std::vector<diagnostic::Diagnostic> mErrors;
    std::string mErrorStage;
    bool mSuccess = false;
};

} // namespace luna::tooling
