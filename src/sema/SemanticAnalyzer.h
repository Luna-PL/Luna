#pragma once

#include "diagnostics/Diagnostic.h"

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct ResolvedDeclarationReference {
    std::string sourcePath;
    int line = 0;
    int column = 0;
    size_t byteLength = 0;
    std::string targetLinkageName;
};

struct Program;
class SymbolTable;

class SemanticContext;
class BodyAnalyzer;
class CompileTimeEvaluator;
class ControlAnalyzer;
class DeclarationCollector;
class TypeResolver;

class SemanticAnalyzer {
public:
    SemanticAnalyzer();
    ~SemanticAnalyzer();

    SemanticAnalyzer(const SemanticAnalyzer&) = delete;
    SemanticAnalyzer& operator=(const SemanticAnalyzer&) = delete;

    bool analyze(Program* program);
    const std::vector<diagnostic::Diagnostic>& errors() const;
    SymbolTable& symTable();
    const SymbolTable& symTable() const;
    const std::vector<ResolvedDeclarationReference>& declarationReferences() const;

private:
    std::unique_ptr<SemanticContext> mContext;
    std::unique_ptr<BodyAnalyzer> mBodyAnalyzer;
    std::unique_ptr<TypeResolver> mTypeResolver;
    std::unique_ptr<CompileTimeEvaluator> mCompileTimeEvaluator;
    std::unique_ptr<DeclarationCollector> mDeclarationCollector;
    std::unique_ptr<ControlAnalyzer> mControlAnalyzer;
};
