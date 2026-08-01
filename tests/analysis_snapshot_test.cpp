#include "tooling/AnalysisSnapshot.h"

#include "sema/SymbolTable.h"

#include <algorithm>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (condition) return true;
    std::cerr << message << '\n';
    return false;
}

} // namespace

int main(int argc, char* argv[]) {
    if (!expect(argc == 2, "expected the Luna source directory")) return 1;

    const std::string validSource =
        "package org.luna.test;\n"
        "module api;\n"
        "export fn add(value: i32) -> i32 { return value + 1; }\n"
        "fn main() -> i32 { return add(41); }\n";
    auto valid = luna::tooling::AnalysisSnapshot::analyzeSource(
        validSource,
        "file:///workspace/main.luna");
    if (!expect(valid.success(), "valid in-memory analysis failed") ||
        !expect(valid.program() != nullptr, "valid typed AST was not retained") ||
        !expect(valid.symbolTable() != nullptr,
                "valid symbol table was not retained") ||
        !expect(valid.program()->declarations.size() == 2,
                "valid declaration inventory is incorrect") ||
        !expect(!valid.symbolTable()->visibleSymbols().empty(),
                "valid visible symbol inventory is empty") ||
        !expect(valid.symbolIndex().declarations().size() == 2,
                "valid indexed declaration count is incorrect") ||
        !expect(valid.symbolIndex().findByName("add").size() == 1,
                "function name lookup is incorrect") ||
        !expect(valid.symbolIndex().findByName("add").front()->signature ==
                    "fn add(value: i32) -> i32",
                "typed function signature is incorrect") ||
        !expect(valid.symbolIndex().findByName("add").front()->packageId ==
                    "org.luna.test" &&
                    valid.symbolIndex().findByName("add").front()->modulePath ==
                    "api" &&
                    valid.symbolIndex().findByName("add").front()->exported,
                "package/module/export symbol metadata is incorrect") ||
        !expect(valid.symbolIndex().findByName("add").front()->selection.line ==
                    3 &&
                    valid.symbolIndex().findByName("add").front()->selection.column ==
                    11 &&
                    valid.symbolIndex().findByName("add").front()->selection.byteLength ==
                    3,
                "source-name selection is incorrect") ||
        !expect(valid.symbolIndex().findById(
                    valid.symbolIndex().findByName("add").front()->id) != nullptr,
                "stable symbol ID lookup failed") ||
        !expect(valid.symbolIndex().inDocument(
                    "file:///workspace/main.luna").size() == 2,
                "document symbol lookup is incorrect") ||
        !expect(valid.referenceIndex().references().size() == 1,
                "resolved reference inventory is incorrect") ||
        !expect(valid.referenceIndex().references().front().targetId ==
                    valid.symbolIndex().findByName("add").front()->id &&
                    valid.referenceIndex().references().front().source.line == 4 &&
                    valid.referenceIndex().references().front().source.column == 27 &&
                    valid.referenceIndex().references().front().source.byteLength == 3,
                "direct call did not resolve to the add Symbol ID") ||
        !expect(valid.referenceIndex().inDocument(
                    "file:///workspace/main.luna").size() == 1,
                "document reference lookup is incorrect") ||
        !expect(valid.packageGraph().sourceUnits.size() == 1,
                "in-memory source unit was not recorded"))
        return 2;

    auto repeated = luna::tooling::AnalysisSnapshot::analyzeSource(
        validSource, "file:///workspace/main.luna");
    if (!expect(repeated.success(), "repeated in-memory analysis failed") ||
        !expect(repeated.symbolIndex().findByName("add").size() == 1 &&
                    repeated.symbolIndex().findByName("add").front()->id ==
                    valid.symbolIndex().findByName("add").front()->id,
                "Symbol ID changed across equivalent snapshots"))
        return 3;

    const std::string namedTypeSource =
        "package org.luna.test;\n"
        "module api;\n"
        "nominal struct Box { value: i32; }\n"
        "fn keep(affine value: Box) -> affine Box { return value; }\n";
    auto namedTypes = luna::tooling::AnalysisSnapshot::analyzeSource(
        namedTypeSource, "file:///workspace/types.luna");
    const auto boxes = namedTypes.symbolIndex().findByName("Box");
    if (!expect(namedTypes.success(), "named type analysis failed") ||
        !expect(boxes.size() == 1 &&
                    boxes.front()->kind ==
                        luna::tooling::IndexedSymbolKind::Struct,
                "named struct was not indexed") ||
        !expect(namedTypes.referenceIndex().references().size() == 2,
                "named type reference inventory is incorrect") ||
        !expect(std::all_of(
                    namedTypes.referenceIndex().references().begin(),
                    namedTypes.referenceIndex().references().end(),
                    [&](const auto& reference) {
                        return reference.targetId == boxes.front()->id &&
                            reference.source.line == 4 &&
                            reference.source.byteLength == 3;
                    }),
                "named type did not resolve to the struct Symbol ID") ||
        !expect(namedTypes.referenceIndex().references()[0].source.column == 23 &&
                    namedTypes.referenceIndex().references()[1].source.column == 38,
                "named type source spans are not exact"))
        return 12;

    const std::string methodSource =
        "package org.luna.test;\n"
        "module api;\n"
        "trait Transform {\n"
        "    fn transform(value: i32) -> i32;\n"
        "}\n"
        "impl Transform for i32 {\n"
        "    fn transform(value: i32) -> i32 { return value + 2; }\n"
        "}\n"
        "fn main() -> i32 {\n"
        "    return 8.transform();\n"
        "}\n";
    auto methodCall = luna::tooling::AnalysisSnapshot::analyzeSource(
        methodSource, "file:///workspace/method.luna");
    const auto methods = methodCall.symbolIndex().findByName("transform");
    const auto traits = methodCall.symbolIndex().findByName("Transform");
    const auto methodReference = std::find_if(
        methodCall.referenceIndex().references().begin(),
        methodCall.referenceIndex().references().end(),
        [&](const auto& reference) {
            return !methods.empty() && reference.targetId == methods.front()->id;
        });
    const auto traitReference = std::find_if(
        methodCall.referenceIndex().references().begin(),
        methodCall.referenceIndex().references().end(),
        [&](const auto& reference) {
            return !traits.empty() && reference.targetId == traits.front()->id;
        });
    if (!expect(methodCall.success(), "trait method analysis failed") ||
        !expect(methods.size() == 1 &&
                    methods.front()->kind ==
                        luna::tooling::IndexedSymbolKind::Method,
                "implemented method was not indexed") ||
        !expect(traits.size() == 1,
                "trait declaration was not indexed") ||
        !expect(methodReference != methodCall.referenceIndex().references().end() &&
                    methodReference->source.line == 10 &&
                    methodReference->source.column == 14 &&
                    methodReference->source.byteLength == 9,
                "member call did not resolve to the method Symbol ID") ||
        !expect(traitReference != methodCall.referenceIndex().references().end() &&
                    traitReference->source.line == 6 &&
                    traitReference->source.column == 6 &&
                    traitReference->source.byteLength == 9,
                "impl trait did not resolve to the trait Symbol ID"))
        return 10;

    auto semanticFailure = luna::tooling::AnalysisSnapshot::analyzeSource(
        "fn main() -> i32 { return missing; }\n",
        "file:///workspace/semantic_error.luna");
    if (!expect(!semanticFailure.success(),
                "semantic failure was accepted") ||
        !expect(semanticFailure.program() != nullptr,
                "typed AST was discarded after semantic failure") ||
        !expect(semanticFailure.symbolTable() != nullptr,
                "partial symbol table was discarded after semantic failure") ||
        !expect(semanticFailure.symbolIndex().findByName("main").size() == 1,
                "semantic failure discarded its declaration index") ||
        !expect(!semanticFailure.errors().empty() &&
                    semanticFailure.errors().front().code == "SEM0001",
                "semantic diagnostic was not retained"))
        return 4;

    auto shadowed = luna::tooling::AnalysisSnapshot::analyzeSource(
        "fn target() -> i32 { return 1; }\n"
        "fn main() -> i32 {\n"
        "  let target = fn() -> i32 { return 2; };\n"
        "  return target();\n"
        "}\n",
        "file:///workspace/shadowed.luna");
    if (!expect(shadowed.success(), "shadowing analysis failed") ||
        !expect(shadowed.referenceIndex().references().empty(),
                "local closure call was attributed to a shadowed declaration"))
        return 5;

    auto parseFailure = luna::tooling::AnalysisSnapshot::analyzeSource(
        "fn main( -> i32 { return 0; }\n",
        "file:///workspace/parse_error.luna");
    if (!expect(!parseFailure.success(), "parse failure was accepted") ||
        !expect(parseFailure.program() != nullptr,
                "recoverable parse AST was not retained") ||
        !expect(parseFailure.symbolTable() == nullptr,
                "parse failure unexpectedly exposed semantic state") ||
        !expect(parseFailure.errorStage() == "parser",
                "parse failure stage is incorrect"))
        return 6;

    const std::string minimalPath =
        std::string(argv[1]) + "/examples/minimal.luna";
    auto pathSnapshot =
        luna::tooling::AnalysisSnapshot::analyzePath(minimalPath);
    if (!expect(pathSnapshot.success(), "path analysis failed") ||
        !expect(pathSnapshot.packageGraph().sourceUnits.size() == 1,
                "path package graph is missing its source unit") ||
        !expect(pathSnapshot.program() != nullptr &&
                    !pathSnapshot.program()->sourceFiles.empty(),
                "path analysis did not retain source-file provenance"))
        return 7;

    const std::string multiModulePath =
        std::string(argv[1]) + "/tests/fixtures/packages/module_headers";
    auto multiModule =
        luna::tooling::AnalysisSnapshot::analyzePath(multiModulePath);
    const auto answers = multiModule.symbolIndex().findByName("answer");
    if (!expect(multiModule.success(), "multi-module path analysis failed") ||
        !expect(answers.size() == 1,
                "multi-module declaration index is incorrect") ||
        !expect(multiModule.referenceIndex().references().size() == 1 &&
                    multiModule.referenceIndex().references().front().targetId ==
                        answers.front()->id &&
                    multiModule.referenceIndex().references().front().source.path.find(
                        "02_main.luna") != std::string::npos,
                "cross-file call did not resolve to its declaration Symbol ID"))
        return 8;

    const std::string methodPackagePath =
        std::string(argv[1]) + "/tests/fixtures/packages/method_references";
    auto methodPackage =
        luna::tooling::AnalysisSnapshot::analyzePath(methodPackagePath);
    const auto packageMethods =
        methodPackage.symbolIndex().findByName("increment");
    const auto counters = methodPackage.symbolIndex().findByName("Counter");
    const luna::tooling::IndexedSymbol* implementedMethod = nullptr;
    for (const auto* symbol : packageMethods) {
        if (symbol->kind == luna::tooling::IndexedSymbolKind::Method)
            implementedMethod = symbol;
    }
    const auto packageMethodReference = std::find_if(
        methodPackage.referenceIndex().references().begin(),
        methodPackage.referenceIndex().references().end(),
        [&](const auto& reference) {
            return implementedMethod && reference.targetId == implementedMethod->id;
        });
    const auto crossFileTypeReferences = std::count_if(
        methodPackage.referenceIndex().references().begin(),
        methodPackage.referenceIndex().references().end(),
        [&](const auto& reference) {
            return counters.size() == 1 &&
                reference.targetId == counters.front()->id &&
                reference.source.path.find("02_main.luna") != std::string::npos;
        });
    if (!expect(methodPackage.success(),
                "cross-file trait method analysis failed") ||
        !expect(packageMethods.size() == 2 && implementedMethod != nullptr,
                "method/function name collision was not indexed") ||
        !expect(packageMethodReference !=
                    methodPackage.referenceIndex().references().end() &&
                    packageMethodReference->source.path.find(
                        "02_main.luna") != std::string::npos,
                "cross-file member call resolved by name instead of method identity") ||
        !expect(counters.size() == 1 && crossFileTypeReferences == 2,
                "qualified cross-file type did not resolve to its Symbol ID"))
        return 11;

    const std::string overlaidDocument = multiModulePath + "/02_main.luna";
    const std::string overlaySource =
        "package org.luna.module_headers;\n"
        "module application;\n"
        "using org.luna.std as std;\n"
        "fn unsaved_value() -> i32 { return 7; }\n"
        "fn main() -> i32 { return unsaved_value(); }\n";
    auto overlaid = luna::tooling::AnalysisSnapshot::analyzePathWithOverlay(
        multiModulePath, overlaidDocument, overlaySource);
    const auto unsavedValues =
        overlaid.symbolIndex().findByName("unsaved_value");
    if (!expect(overlaid.success(), "package overlay analysis failed") ||
        !expect(unsavedValues.size() == 1 &&
                    unsavedValues.front()->modulePath == "application",
                "overlay declaration lost its package/module identity") ||
        !expect(overlaid.referenceIndex().references().size() == 1 &&
                    overlaid.referenceIndex().references().front().targetId ==
                        unsavedValues.front()->id,
                "overlay call did not resolve against in-memory source"))
        return 9;

    return 0;
}
