#include "tooling/AnalysisSnapshot.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/OwnershipChecker.h"
#include "sema/SemanticAnalyzer.h"
#include "sema/TraitChecker.h"

#include <utility>

namespace luna::tooling {
namespace {

void assignSourceOwnership(Program& program) {
    for (auto& declaration : program.declarations) {
        if (declaration->packageId.empty())
            declaration->packageId = program.packageName;
        if (auto* implementation =
                dynamic_cast<ImplDecl*>(declaration.get())) {
            for (auto& method : implementation->methods) {
                method->packageId = declaration->packageId;
                method->modulePath = declaration->modulePath;
            }
        }
    }
    for (auto& use : program.packageUses) {
        if (use.ownerPackageId.empty())
            use.ownerPackageId = program.packageName;
    }
}

} // namespace

AnalysisSnapshot::AnalysisSnapshot() = default;
AnalysisSnapshot::~AnalysisSnapshot() = default;
AnalysisSnapshot::AnalysisSnapshot(AnalysisSnapshot&&) noexcept = default;
AnalysisSnapshot& AnalysisSnapshot::operator=(AnalysisSnapshot&&) noexcept = default;

AnalysisSnapshot AnalysisSnapshot::analyzePath(const std::string& inputPath) {
    AnalysisSnapshot snapshot;
    PackageManager manager;
    LoadedPackage loaded;
    std::vector<diagnostic::Diagnostic> errors;
    PackageRequest request;
    request.inputPath = inputPath;
    const bool loadedSuccessfully = manager.load(
        request, loaded, snapshot.mPackageGraph, errors);
    snapshot.mPackageManifest = std::move(loaded.manifest);
    snapshot.mPackageRootPath = std::move(loaded.rootPath);
    snapshot.mProgram = std::move(loaded.program);
    if (!loadedSuccessfully) {
        snapshot.fail(errors);
        return snapshot;
    }
    snapshot.analyzeProgram();
    return snapshot;
}

AnalysisSnapshot AnalysisSnapshot::analyzePathWithOverlay(
    const std::string& inputPath, const std::string& documentPath,
    const std::string& source) {
    return analyzePathWithOverlays(
        inputPath, {{documentPath, source}});
}

AnalysisSnapshot AnalysisSnapshot::analyzePathWithOverlays(
    const std::string& inputPath,
    const std::vector<PackageRequest::SourceOverlay>& overlays) {
    AnalysisSnapshot snapshot;
    PackageManager manager;
    LoadedPackage loaded;
    std::vector<diagnostic::Diagnostic> errors;
    PackageRequest request;
    request.inputPath = inputPath;
    request.overlays = overlays;
    const bool loadedSuccessfully = manager.load(
        request, loaded, snapshot.mPackageGraph, errors);
    snapshot.mPackageManifest = std::move(loaded.manifest);
    snapshot.mPackageRootPath = std::move(loaded.rootPath);
    snapshot.mProgram = std::move(loaded.program);
    if (!loadedSuccessfully) {
        snapshot.fail(errors);
        return snapshot;
    }
    snapshot.analyzeProgram();
    return snapshot;
}

AnalysisSnapshot AnalysisSnapshot::analyzeSource(
    const std::string& source, const std::string& documentId) {
    AnalysisSnapshot snapshot;
    snapshot.mPackageGraph.sourceUnits.push_back(documentId);

    Lexer lexer(source, documentId);
    auto tokens = lexer.tokenize();
    if (!lexer.errors().empty()) {
        snapshot.fail(lexer.errors(), "lexer");
        return snapshot;
    }

    Parser parser(std::move(tokens), documentId, source);
    snapshot.mProgram = parser.parse();
    if (snapshot.mProgram) assignSourceOwnership(*snapshot.mProgram);
    if (!parser.errors().empty()) {
        snapshot.fail(parser.errors(), "parser");
        return snapshot;
    }
    snapshot.analyzeProgram();
    return snapshot;
}

bool AnalysisSnapshot::analyzeProgram() {
    if (!mProgram) return fail({}, "frontend");

    mSemanticAnalyzer = std::make_unique<SemanticAnalyzer>();
    if (!mSemanticAnalyzer->analyze(mProgram.get()))
        return fail(mSemanticAnalyzer->errors());

    TraitChecker traits;
    if (!traits.check(mProgram.get())) return fail(traits.errors());

    OwnershipChecker owner;
    if (!owner.check(mProgram.get(), mSemanticAnalyzer->symTable()))
        return fail(owner.errors());

    mSymbolIndex = SymbolIndex::build(*mProgram);
    mReferenceIndex = ReferenceIndex::build(
        *mSemanticAnalyzer, mSymbolIndex);
    mSuccess = true;
    return true;
}

const SymbolTable* AnalysisSnapshot::symbolTable() const {
    return mSemanticAnalyzer ? &mSemanticAnalyzer->symTable() : nullptr;
}

const luna::selector::SymbolCatalog* AnalysisSnapshot::symbolCatalog() const {
    return mSemanticAnalyzer ? mSemanticAnalyzer->symbolCatalog() : nullptr;
}

bool AnalysisSnapshot::fail(
    const std::vector<diagnostic::Diagnostic>& errors, std::string stage) {
    if (mProgram) {
        mSymbolIndex = SymbolIndex::build(*mProgram);
        if (mSemanticAnalyzer)
            mReferenceIndex = ReferenceIndex::build(
                *mSemanticAnalyzer, mSymbolIndex);
    }
    mErrors = errors;
    mErrorStage = std::move(stage);
    mSuccess = false;
    return false;
}

} // namespace luna::tooling
