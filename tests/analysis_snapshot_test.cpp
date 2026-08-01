#include "tooling/AnalysisSnapshot.h"

#include "sema/SymbolTable.h"

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

    return 0;
}
