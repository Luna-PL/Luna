#include "SemanticAnalyzer.h"

#include "BodyAnalyzer.h"
#include "CompileTimeEvaluator.h"
#include "ControlAnalyzer.h"
#include "DeclarationCollector.h"
#include "SemanticContext.h"
#include "TypeResolver.h"

SemanticAnalyzer::SemanticAnalyzer()
    : mContext(std::make_unique<SemanticContext>()),
      mBodyAnalyzer(std::make_unique<BodyAnalyzer>(mContext->bodyAccess())),
      mTypeResolver(std::make_unique<TypeResolver>(mContext->typeAccess())),
      mCompileTimeEvaluator(std::make_unique<CompileTimeEvaluator>(
          mContext->compileTimeAccess())),
      mDeclarationCollector(std::make_unique<DeclarationCollector>(
          mContext->declarationAccess())),
      mControlAnalyzer(std::make_unique<ControlAnalyzer>(
          mContext->controlAccess())) {
    mContext->bindBodyAnalysis(*mBodyAnalyzer);
    mContext->bindTypeAnalysis(*mTypeResolver);
    mContext->bindCompileTimeAnalysis(*mCompileTimeEvaluator);
    mContext->bindDeclarationAnalysis(*mDeclarationCollector);
    mContext->bindControlAnalysis(*mControlAnalyzer);
}

SemanticAnalyzer::~SemanticAnalyzer() = default;

bool SemanticAnalyzer::analyze(Program* program) {
    return mContext->analyze(program);
}

const std::vector<diagnostic::Diagnostic>& SemanticAnalyzer::errors() const {
    return mContext->errors();
}

SymbolTable& SemanticAnalyzer::symTable() {
    return mContext->symTable();
}

const SymbolTable& SemanticAnalyzer::symTable() const {
    return mContext->symTable();
}

const luna::selector::SymbolCatalog* SemanticAnalyzer::symbolCatalog() const {
    return mContext->symbolCatalog();
}

const std::vector<ResolvedDeclarationReference>&
SemanticAnalyzer::declarationReferences() const {
    return mContext->declarationReferences();
}
