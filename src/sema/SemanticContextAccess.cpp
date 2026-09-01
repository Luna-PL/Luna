#include "SemanticContextAccess.h"

#include <utility>

BodyContextAccess SemanticContext::bodyAccess() {
    return BodyContextAccess(*this);
}

CompileTimeContextAccess SemanticContext::compileTimeAccess() {
    return CompileTimeContextAccess(*this);
}

ControlContextAccess SemanticContext::controlAccess() {
    return ControlContextAccess(*this);
}

DeclarationContextAccess SemanticContext::declarationAccess() {
    return DeclarationContextAccess(*this);
}

TypeContextAccess SemanticContext::typeAccess() {
    return TypeContextAccess(*this);
}

BodyContextAccess::BodyContextAccess(SemanticContext& context)
    : mOwner(context),
      mConcepts(context.mConcepts),
      mConstexprFunctions(context.mConstexprFunctions),
      mConstraints(context.mConstraints),
      mCurrentFragmentDecl(context.mCurrentFragmentDecl),
      mCurrentFunctionReturnUsage(context.mCurrentFunctionReturnUsage),
      mCurrentFunctionReturnsLinear(context.mCurrentFunctionReturnsLinear),
      mCurrentModulePath(context.mCurrentModulePath),
      mCurrentPackageId(context.mCurrentPackageId),
      mCurrentReturnType(context.mCurrentReturnType),
      mDeclaredTypes(context.mDeclaredTypes),
      mFromConversions(context.mFromConversions),
      mFromIteratorImplementations(context.mFromIteratorImplementations),
      mFunctionFamilies(context.mFunctionFamilies),
      mGeneratedInstances(context.mGeneratedInstances),
      mImpls(context.mImpls),
      mInFunction(context.mInFunction),
      mInKernel(context.mInKernel),
      mInferenceRoots(context.mInferenceRoots),
      mIteratorStateCounter(context.mIteratorStateCounter),
      mMetadataSchemas(context.mMetadataSchemas),
      mProgram(context.mProgram),
      mQualifiedDeclarations(context.mQualifiedDeclarations),
      mSawReturn(context.mSawReturn),
      mSymbolCatalog(context.mSymbolCatalog),
      mSymTable(context.mSymTable),
      mTraitMethods(context.mTraitMethods),
      mTraits(context.mTraits) {}

void BodyContextAccess::analyzeApply(
    ApplyStmt* stmt, TypePtr expectedReturn) {
    mOwner.analyzeApply(stmt, std::move(expectedReturn));
}

TypePtr BodyContextAccess::analyzeDeclarationReflectionCall(
    CallExpr* call, const std::string& name) {
    return mOwner.analyzeDeclarationReflectionCall(call, name);
}

TypePtr BodyContextAccess::analyzeReflectionCall(
    CallExpr* call, const std::string& name) {
    return mOwner.analyzeReflectionCall(call, name);
}

TypePtr BodyContextAccess::lookupDeclaredType(const std::string& name) {
    return mOwner.lookupDeclaredType(name);
}

void BodyContextAccess::analyzeSlotDecl(SlotDeclStmt* stmt) {
    mOwner.analyzeSlotDecl(stmt);
}

void BodyContextAccess::analyzeSlotInvoke(
    SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    mOwner.analyzeSlotInvoke(stmt, std::move(expectedReturn));
}

bool BodyContextAccess::constrain(
    const TypePtr& actual, const TypePtr& expected,
    const std::string& context) {
    return mOwner.constrain(actual, expected, context);
}

void BodyContextAccess::declareFunction(FunctionDecl* decl) {
    mOwner.declareFunction(decl);
}

TypePtr BodyContextAccess::declaredType(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.declaredType(ast, bindings);
}

void BodyContextAccess::defineConst(
    const std::string& name, const ConstValue& value) {
    mOwner.defineConst(name, value);
}

void BodyContextAccess::enterConstScope() {
    mOwner.enterConstScope();
}

void BodyContextAccess::enterSlotScope() {
    mOwner.enterSlotScope();
}

void BodyContextAccess::error(
    const std::string& message, int line, int column) {
    mOwner.error(message, line, column);
}

std::optional<BodyContextAccess::ConstValue>
BodyContextAccess::evaluateConstExpr(
    Expr* expr,
    const std::unordered_map<std::string, ConstValue>& locals) {
    return mOwner.evaluateConstExpr(expr, locals);
}

std::optional<bool> BodyContextAccess::evaluateConstraint(
    const std::string& name, const TypeVec& arguments,
    std::vector<std::string>& active) {
    return mOwner.evaluateConstraint(name, arguments, active);
}

std::optional<BodyContextAccess::ConstValue>
BodyContextAccess::evaluateConstraintExpr(
    Expr* expr,
    const std::unordered_map<std::string, TypePtr>& bindings,
    std::vector<std::string>& active) {
    return mOwner.evaluateConstraintExpr(expr, bindings, active);
}

std::optional<std::string> BodyContextAccess::evaluateSelectorFunction(
    FunctionDecl* function,
    const luna::selector::SymbolSet& symbols,
    const std::vector<ConstValue>& arguments,
    std::string& failure) {
    return mOwner.evaluateSelectorFunction(
        function, symbols, arguments, failure);
}

void BodyContextAccess::exitConstScope() {
    mOwner.exitConstScope();
}

void BodyContextAccess::exitSlotScope() {
    mOwner.exitSlotScope();
}

TypePtr BodyContextAccess::instantiateNominal(
    const TypePtr& type, const std::vector<TypePtr>& arguments) {
    return mOwner.instantiateNominal(type, arguments);
}

SymbolInfo* BodyContextAccess::lookupSymbol(const std::string& name) {
    return mOwner.lookupSymbol(name);
}

FunctionDecl* BodyContextAccess::monomorphize(
    FunctionDecl* generic, const TypeVec& concreteTypes) {
    return mOwner.monomorphize(generic, concreteTypes);
}

void BodyContextAccess::recordDeclarationReference(
    const ASTNode* source, size_t byteLength, const Decl* declaration) {
    mOwner.recordDeclarationReference(source, byteLength, declaration);
}

void BodyContextAccess::recordResolvedReference(
    const std::string& sourcePath, int line, int column,
    size_t byteLength, std::string targetLinkageName) {
    mOwner.recordResolvedReference(
        sourcePath, line, column, byteLength,
        std::move(targetLinkageName));
}

void BodyContextAccess::requireBool(
    const TypePtr& type, const std::string& context) {
    mOwner.requireBool(type, context);
}

void BodyContextAccess::requireInteger(
    const TypePtr& type, const std::string& context) {
    mOwner.requireInteger(type, context);
}

void BodyContextAccess::requireNumeric(
    const TypePtr& type, const std::string& context) {
    mOwner.requireNumeric(type, context);
}

TypePtr BodyContextAccess::resolveTypeAST(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.resolveTypeAST(ast, bindings);
}

TypePtr BodyContextAccess::resolved(const TypePtr& type) {
    return mOwner.resolved(type);
}

bool BodyContextAccess::satisfiesTrait(
    const std::string& traitId, const TypePtr& type) const {
    return mOwner.satisfiesTrait(traitId, type);
}

void BodyContextAccess::setDeclarationContext(const Decl* declaration) {
    mOwner.setDeclarationContext(declaration);
}

void BodyContextAccess::setDiagnosticLocation(const ASTNode* node) {
    mOwner.setDiagnosticLocation(node);
}

std::string BodyContextAccess::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    return mOwner.sourceDeclarationKey(name, diagnoseVisibility);
}

std::string BodyContextAccess::traitIdentity(const TraitDecl* trait) const {
    return mOwner.traitIdentity(trait);
}

std::string BodyContextAccess::typeIdentity(const TypePtr& type) const {
    return mOwner.typeIdentity(type);
}

std::unique_ptr<TypeAST> BodyContextAccess::typeToAST(
    const TypePtr& type) {
    return mOwner.typeToAST(type);
}

DeclarationContextAccess::DeclarationContextAccess(SemanticContext& context)
    : mOwner(context),
      mConcepts(context.mConcepts),
      mConstexprFunctions(context.mConstexprFunctions),
      mCurrentModulePath(context.mCurrentModulePath),
      mCurrentPackageId(context.mCurrentPackageId),
      mDeclaredTypes(context.mDeclaredTypes),
      mFragments(context.mFragments),
      mFromConversions(context.mFromConversions),
      mFromIteratorImplementations(context.mFromIteratorImplementations),
      mFunctionFamilies(context.mFunctionFamilies),
      mImpls(context.mImpls),
      mMetadataSchemas(context.mMetadataSchemas),
      mProgram(context.mProgram),
      mSlotScopes(context.mSlotScopes),
      mSymTable(context.mSymTable),
      mTraitOwners(context.mTraitOwners),
      mTraitTypeParams(context.mTraitTypeParams),
      mTraits(context.mTraits) {}

TypePtr DeclarationContextAccess::analyzeExpr(Expr* expr) {
    return mOwner.analyzeExpr(expr);
}

void DeclarationContextAccess::checkUnresolved(
    const TypePtr& type, const std::string& context) {
    mOwner.checkUnresolved(type, context);
}

bool DeclarationContextAccess::constrain(
    const TypePtr& actual, const TypePtr& expected,
    const std::string& context) {
    return mOwner.constrain(actual, expected, context);
}

TypePtr DeclarationContextAccess::declaredType(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.declaredType(ast, bindings);
}

void DeclarationContextAccess::error(
    const std::string& message, int line, int column) {
    mOwner.error(message, line, column);
}

std::optional<SemanticConstValue>
DeclarationContextAccess::evaluateConstExpr(
    Expr* expr,
    const std::unordered_map<std::string, SemanticConstValue>& locals) {
    return mOwner.evaluateConstExpr(expr, locals);
}

std::string DeclarationContextAccess::resolveTraitRef(
    TraitRef& trait, const ASTNode* useSite) {
    return mOwner.resolveTraitRef(trait, useSite);
}

TypePtr DeclarationContextAccess::resolveTypeAST(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.resolveTypeAST(ast, bindings);
}

TypePtr DeclarationContextAccess::resolved(const TypePtr& type) {
    return mOwner.resolved(type);
}

std::string DeclarationContextAccess::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    return mOwner.sourceDeclarationKey(name, diagnoseVisibility);
}

std::string DeclarationContextAccess::traitIdentity(
    const TraitDecl* trait) const {
    return mOwner.traitIdentity(trait);
}

std::string DeclarationContextAccess::typeIdentity(
    const TypePtr& type) const {
    return mOwner.typeIdentity(type);
}

ControlContextAccess::ControlContextAccess(SemanticContext& context)
    : mOwner(context),
      mApplyScopes(context.mApplyScopes),
      mCurrentFragmentDecl(context.mCurrentFragmentDecl),
      mCurrentFragmentSlot(context.mCurrentFragmentSlot),
      mCurrentReturnType(context.mCurrentReturnType),
      mFragments(context.mFragments),
      mProgram(context.mProgram),
      mSlotScopes(context.mSlotScopes),
      mSymTable(context.mSymTable) {}

TypePtr ControlContextAccess::analyzeBlock(
    BlockStmt* block, TypePtr expectedReturn) {
    return mOwner.analyzeBlock(block, std::move(expectedReturn));
}

TypePtr ControlContextAccess::analyzeExpr(Expr* expr) {
    return mOwner.analyzeExpr(expr);
}

bool ControlContextAccess::constrain(
    const TypePtr& actual, const TypePtr& expected,
    const std::string& context) {
    return mOwner.constrain(actual, expected, context);
}

TypePtr ControlContextAccess::declaredType(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.declaredType(ast, bindings);
}

void ControlContextAccess::error(
    const std::string& message, int line, int column) {
    mOwner.error(message, line, column);
}

std::string ControlContextAccess::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    return mOwner.sourceDeclarationKey(name, diagnoseVisibility);
}

TypeContextAccess::TypeContextAccess(SemanticContext& context)
    : mOwner(context),
      mConstraints(context.mConstraints),
      mDeclaredTypes(context.mDeclaredTypes),
      mDiagnosticCol(context.mDiagnosticCol),
      mDiagnosticFile(context.mDiagnosticFile),
      mDiagnosticLine(context.mDiagnosticLine),
      mGeneratedInstances(context.mGeneratedInstances),
      mImpls(context.mImpls),
      mInstantiatedFunctions(context.mInstantiatedFunctions),
      mInstantiator(context.mInstantiator),
      mProgram(context.mProgram),
      mQualifiedDeclarations(context.mQualifiedDeclarations),
      mSymTable(context.mSymTable) {}

void TypeContextAccess::error(
    const std::string& message, int line, int column) {
    mOwner.error(message, line, column);
}

TypePtr TypeContextAccess::lookupDeclaredType(const std::string& name) {
    return mOwner.lookupDeclaredType(name);
}

void TypeContextAccess::recordDeclarationReference(
    const ASTNode* source, size_t byteLength, const Decl* declaration) {
    mOwner.recordDeclarationReference(source, byteLength, declaration);
}

std::string TypeContextAccess::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    return mOwner.sourceDeclarationKey(name, diagnoseVisibility);
}

std::string TypeContextAccess::typeIdentity(const TypePtr& type) const {
    return mOwner.typeIdentity(type);
}

CompileTimeContextAccess::CompileTimeContextAccess(
    SemanticContext& context)
    : mOwner(context),
      mActiveSelectorSet(context.mActiveSelectorSet),
      mConcepts(context.mConcepts),
      mConstEvaluationDepth(context.mConstEvaluationDepth),
      mConstScopes(context.mConstScopes),
      mConstexprFunctions(context.mConstexprFunctions),
      mCurrentModulePath(context.mCurrentModulePath),
      mCurrentPackageId(context.mCurrentPackageId),
      mFunctionFamilies(context.mFunctionFamilies),
      mMetadataSchemas(context.mMetadataSchemas),
      mProgram(context.mProgram),
      mSymTable(context.mSymTable) {}

TypePtr CompileTimeContextAccess::analyzeExpr(Expr* expr) {
    return mOwner.analyzeExpr(expr);
}

void CompileTimeContextAccess::error(
    const std::string& message, int line, int column) {
    mOwner.error(message, line, column);
}

TypePtr CompileTimeContextAccess::resolveTypeAST(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mOwner.resolveTypeAST(ast, bindings);
}

TypePtr CompileTimeContextAccess::resolved(const TypePtr& type) {
    return mOwner.resolved(type);
}

void CompileTimeContextAccess::setDeclarationContext(
    const Decl* declaration) {
    mOwner.setDeclarationContext(declaration);
}

std::string CompileTimeContextAccess::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    return mOwner.sourceDeclarationKey(name, diagnoseVisibility);
}
