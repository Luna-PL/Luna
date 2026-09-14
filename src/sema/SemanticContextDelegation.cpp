#include "SemanticContext.h"
#include "../core/TypeRelations.h"
#include "../core/TypeLayout.h"
#include "../parser/AST.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"
#include <sstream>
#include <functional>
#include <cmath>
#include <set>
#include <iomanip>

#include "SemanticAnalysisSupport.h"
#include "PredefinedTypes.h"

void SemanticContext::declareFunction(FunctionDecl* decl) {
    mDeclarationAnalysis->declareFunction(decl);
}

void SemanticContext::declareMeta(MetaDecl* decl) {
    mDeclarationAnalysis->declareMeta(decl);
}

void SemanticContext::declareConstraint(ConstraintDecl* decl) {
    mDeclarationAnalysis->declareConstraint(decl);
}

void SemanticContext::analyzeConstraint(ConstraintDecl* decl) {
    mDeclarationAnalysis->analyzeConstraint(decl);
}

void SemanticContext::analyzeMeta(MetaDecl* decl) {
    mDeclarationAnalysis->analyzeMeta(decl);
}

void SemanticContext::validateMetadata(Decl* decl) {
    mDeclarationAnalysis->validateMetadata(decl);
}

void SemanticContext::declareFragment(FragmentDecl* decl) {
    mDeclarationAnalysis->declareFragment(decl);
}

bool SemanticContext::isFFIType(
    const TypePtr& type, const std::string& context) {
    return mDeclarationAnalysis->isFFIType(type, context);
}

void SemanticContext::validateFFIFunction(FunctionDecl* decl) {
    mDeclarationAnalysis->validateFFIFunction(decl);
}

void SemanticContext::declareStruct(StructDecl* decl) {
    mDeclarationAnalysis->declareStruct(decl);
}

void SemanticContext::declareEnum(EnumDecl* decl) {
    mDeclarationAnalysis->declareEnum(decl);
}

void SemanticContext::declareTrait(TraitDecl* decl) {
    mDeclarationAnalysis->declareTrait(decl);
}

void SemanticContext::declareImpl(ImplDecl* decl) {
    mDeclarationAnalysis->declareImpl(decl);
}

void SemanticContext::analyzeFunction(FunctionDecl* decl) {
    mBodyAnalysis->analyzeFunction(decl);
}
void SemanticContext::analyzeStruct(StructDecl* decl) {
    mBodyAnalysis->analyzeStruct(decl);
}
void SemanticContext::analyzeEnum(EnumDecl* decl) {
    mBodyAnalysis->analyzeEnum(decl);
}
void SemanticContext::analyzeTrait(TraitDecl* decl) {
    mBodyAnalysis->analyzeTrait(decl);
}
void SemanticContext::analyzeImpl(ImplDecl* decl) {
    mBodyAnalysis->analyzeImpl(decl);
}
void SemanticContext::analyzeSlotDecl(SlotDeclStmt* stmt) {
    mControlAnalysis->analyzeSlotDecl(stmt);
}

void SemanticContext::declareSlot(SlotDecl* decl) {
    mControlAnalysis->declareSlot(decl);
}

void SemanticContext::finalizeSlot(SlotDecl* decl) {
    mControlAnalysis->finalizeSlot(decl);
}

void SemanticContext::analyzeSlotInvoke(
    SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    mControlAnalysis->analyzeSlotInvoke(stmt, std::move(expectedReturn));
}

void SemanticContext::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    mControlAnalysis->analyzeApply(stmt, std::move(expectedReturn));
}

void SemanticContext::analyzeFragmentForSlot(
    FragmentDecl* fragment, const std::string& slotName,
    const TypeVec& parameterTypes,
    const std::vector<luna::ownership::Contract>& parameterContracts,
    const std::unordered_map<std::string, SymbolInfo>& captures) {
    mControlAnalysis->analyzeFragmentForSlot(
        fragment, slotName, parameterTypes, parameterContracts, captures);
}

void SemanticContext::enterSlotScope() {
    mControlAnalysis->enterSlotScope();
}

void SemanticContext::exitSlotScope() {
    mControlAnalysis->exitSlotScope();
}

FragmentDecl* SemanticContext::selectFragment(
    const std::string& name, const ASTNode* useSite) {
    return mControlAnalysis->selectFragment(name, useSite);
}

std::string SemanticContext::traitIdentity(const TraitDecl* trait) const {
    if (!trait) return "";
    const auto symbol = trait->generatedSymbolName.empty()
        ? trait->name : trait->generatedSymbolName;
    return nominalDeclarationIdentity(mProgram, "trait", symbol, trait);
}

std::string SemanticContext::typeIdentity(const TypePtr& type) const {
    if (!type) return "?";
    return luna::types::typeId(type).value;
}

bool SemanticContext::satisfiesTrait(const std::string& traitId, const TypePtr& type) const {
    const auto trait = mImpls.find(traitId);
    if (trait == mImpls.end()) return false;
    const auto implementation = trait->second.find(typeIdentity(type));
    if (implementation == trait->second.end()) return false;
    const auto signatures = mTraitMethods.find(traitId);
    if (signatures == mTraitMethods.end()) return false;
    for (const auto& [methodName, _] : signatures->second) {
        if (!implementation->second.count(methodName)) return false;
    }
    return true;
}

std::string SemanticContext::resolveTraitRef(TraitRef& trait, const ASTNode* useSite) {
    if (!trait.resolvedTraitId.empty()) return trait.resolvedTraitId;
    if (trait.name == "Drop") {
        if (!trait.typeArgs.empty())
            error("Drop does not accept type arguments",
                  trait.line, trait.col);
        trait.resolvedTraitId = luna::sysmeta::DropTraitId;
        return trait.resolvedTraitId;
    }
    if (trait.name == "From") {
        trait.resolvedTraitId = luna::sysmeta::FromTraitId;
        return trait.resolvedTraitId;
    }
    const ASTNode* diagnosticSite = trait.line > 0 ? static_cast<const ASTNode*>(&trait) : useSite;
    TraitDecl* selected = nullptr;
    auto declared = mTraits.find(sourceDeclarationKey(trait.name));
    if (declared != mTraits.end()) selected = declared->second;
    else {
        error("unknown trait '" + trait.name + "'",
              diagnosticSite->line, diagnosticSite->col);
        return "";
    }
    if (!selected) return "";
    trait.resolvedTraitId = traitIdentity(selected);
    recordDeclarationReference(&trait, trait.name.size(), selected);
    return trait.resolvedTraitId;
}

// ─── Statement analysis ────────────────────────────────────────────

TypePtr SemanticContext::analyzeStmt(
    Stmt* stmt, TypePtr expectedReturn) {
    return mBodyAnalysis->analyzeStmt(stmt, std::move(expectedReturn));
}
TypePtr SemanticContext::analyzeBlock(
    BlockStmt* block, TypePtr expectedReturn) {
    return mBodyAnalysis->analyzeBlock(block, std::move(expectedReturn));
}
bool SemanticContext::statementAlwaysReturns(const Stmt* stmt) const {
    return mBodyAnalysis->statementAlwaysReturns(stmt);
}
bool SemanticContext::blockAlwaysReturns(const BlockStmt* block) const {
    return mBodyAnalysis->blockAlwaysReturns(block);
}
TypePtr SemanticContext::analyzeExpr(Expr* expr) {
    return mBodyAnalysis->analyzeExpr(expr);
}
TypePtr SemanticContext::analyzeSelect(SelectExpr* selection) {
    return mBodyAnalysis->analyzeSelect(selection);
}
TypePtr SemanticContext::analyzeCall(CallExpr* call) {
    return mBodyAnalysis->analyzeCall(call);
}
void SemanticContext::recordDeclarationReference(
    const ASTNode* source, size_t byteLength,
    const Decl* declaration) {
    if (!source || !declaration || source->sourcePath.empty() ||
        source->line <= 0 || source->col <= 0 || byteLength == 0)
        return;
    std::string linkageName = declaration->generatedSymbolName;
    if (linkageName.empty()) {
        std::string name;
        if (const auto* function = dynamic_cast<const FunctionDecl*>(declaration))
            name = function->name;
        else if (const auto* structure = dynamic_cast<const StructDecl*>(declaration))
            name = structure->name;
        else if (const auto* enumeration = dynamic_cast<const EnumDecl*>(declaration))
            name = enumeration->name;
        else if (const auto* trait = dynamic_cast<const TraitDecl*>(declaration))
            name = trait->name;
        else if (const auto* metadata = dynamic_cast<const MetaDecl*>(declaration))
            name = metadata->name;
        else
            return;
        linkageName = qualifiedDeclarationKey(
            declaration->packageId.empty() ? std::string("main")
                                           : declaration->packageId,
            declaration->modulePath, name);
    }
    recordResolvedReference(source->sourcePath, source->line, source->col,
                            byteLength, std::move(linkageName));
}

void SemanticContext::recordResolvedReference(
    const std::string& sourcePath, int line, int column, size_t byteLength,
    std::string targetLinkageName) {
    if (sourcePath.empty() || line <= 0 || column <= 0 || byteLength == 0 ||
        targetLinkageName.empty())
        return;
    mDeclarationReferences.push_back({
        sourcePath, line, column, byteLength, std::move(targetLinkageName),
    });
}

TypePtr SemanticContext::analyzeMemberCall(
    CallExpr* call, FieldAccessExpr* member) {
    return mBodyAnalysis->analyzeMemberCall(call, member);
}
TypePtr SemanticContext::analyzeIteratorCall(
    CallExpr* call, FieldAccessExpr* member) {
    return mBodyAnalysis->analyzeIteratorCall(call, member);
}
TypePtr SemanticContext::analyzeLaunch(LaunchExpr* launch) {
    return mBodyAnalysis->analyzeLaunch(launch);
}
TypePtr SemanticContext::analyzeReflectionCall(
    CallExpr* call, const std::string& name) {
    return mCompileTimeAnalysis->analyzeReflectionCall(call, name);
}

TypePtr SemanticContext::analyzeDeclarationReflectionCall(
    CallExpr* call, const std::string& name) {
    return mCompileTimeAnalysis->analyzeDeclarationReflectionCall(call, name);
}

void SemanticContext::enterConstScope() {
    mCompileTimeAnalysis->enterConstScope();
}
void SemanticContext::exitConstScope() {
    mCompileTimeAnalysis->exitConstScope();
}
void SemanticContext::defineConst(
    const std::string& name, const ConstValue& value) {
    mCompileTimeAnalysis->defineConst(name, value);
}
const SemanticContext::ConstValue*
SemanticContext::lookupConst(const std::string& name) const {
    return mCompileTimeAnalysis->lookupConst(name);
}
std::optional<SemanticContext::ConstValue>
SemanticContext::evaluateConstExpr(
    Expr* expr,
    const std::unordered_map<std::string, ConstValue>& locals) {
    return mCompileTimeAnalysis->evaluateConstExpr(expr, locals);
}
std::optional<SemanticContext::ConstValue>
SemanticContext::evaluateConstFunction(
    FunctionDecl* function, const std::vector<ConstValue>& arguments) {
    return mCompileTimeAnalysis->evaluateConstFunction(function, arguments);
}
bool SemanticContext::evaluateConstBlock(
    BlockStmt* block, std::unordered_map<std::string, ConstValue>& locals,
    std::optional<ConstValue>& result) {
    return mCompileTimeAnalysis->evaluateConstBlock(block, locals, result);
}
std::optional<SemanticContext::ConstValue>
SemanticContext::evaluateConstraintExpr(
    Expr* expr, const std::unordered_map<std::string, TypePtr>& bindings,
    std::vector<std::string>& active) {
    return mCompileTimeAnalysis->evaluateConstraintExpr(
        expr, bindings, active);
}
std::optional<bool> SemanticContext::evaluateConstraint(
    const std::string& name, const TypeVec& arguments,
    std::vector<std::string>& active) {
    return mCompileTimeAnalysis->evaluateConstraint(name, arguments, active);
}
std::optional<SemanticContext::SelectorValue>
SemanticContext::evaluateSelectorExpr(
    Expr* expr, std::unordered_map<std::string, SelectorValue>& locals) {
    return mCompileTimeAnalysis->evaluateSelectorExpr(expr, locals);
}
bool SemanticContext::evaluateSelectorBlock(
    BlockStmt* block,
    std::unordered_map<std::string, SelectorValue>& locals,
    std::optional<SelectorValue>& result, bool& returned) {
    return mCompileTimeAnalysis->evaluateSelectorBlock(
        block, locals, result, returned);
}
std::optional<std::string> SemanticContext::evaluateSelectorFunction(
    FunctionDecl* function, const luna::selector::SymbolSet& symbols,
    const std::vector<ConstValue>& arguments, std::string& failure) {
    return mCompileTimeAnalysis->evaluateSelectorFunction(
        function, symbols, arguments, failure);
}
FunctionDecl* SemanticContext::findMatchingImpl(
    const std::string& traitName, const std::string& typeName,
    const std::string& methodName) {
    return mTypeAnalysis->findMatchingImpl(traitName, typeName, methodName);
}

FunctionDecl* SemanticContext::monomorphize(
    FunctionDecl* generic, const TypeVec& concreteTypes) {
    return mTypeAnalysis->monomorphize(generic, concreteTypes);
}

TypePtr SemanticContext::resolveTypeAST(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mTypeAnalysis->resolveTypeAST(ast, bindings);
}

TypePtr SemanticContext::instantiateNominal(
    const TypePtr& type, const std::vector<TypePtr>& args) {
    return mTypeAnalysis->instantiateNominal(type, args);
}

TypePtr SemanticContext::declaredType(
    const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    return mTypeAnalysis->declaredType(ast, bindings);
}

TypePtr SemanticContext::resolved(const TypePtr& type) {
    return mTypeAnalysis->resolved(type);
}

bool SemanticContext::constrain(
    const TypePtr& actual, const TypePtr& expected,
    const std::string& context) {
    return mTypeAnalysis->constrain(actual, expected, context);
}

void SemanticContext::requireBool(
    const TypePtr& type, const std::string& context) {
    mTypeAnalysis->requireBool(type, context);
}

void SemanticContext::requireNumeric(
    const TypePtr& type, const std::string& context) {
    mTypeAnalysis->requireNumeric(type, context);
}

void SemanticContext::requireInteger(
    const TypePtr& type, const std::string& context) {
    mTypeAnalysis->requireInteger(type, context);
}

void SemanticContext::checkUnresolved(
    const TypePtr& type, const std::string& context) {
    mTypeAnalysis->checkUnresolved(type, context);
}

std::unique_ptr<TypeAST> SemanticContext::typeToAST(const TypePtr& type) {
    return mTypeAnalysis->typeToAST(type);
}

void SemanticContext::materializeInferredTypes(Program* program) {
    mTypeAnalysis->materializeInferredTypes(program);
}

void SemanticContext::error(const std::string& msg, int line, int col) {
    if (line <= 0) line = mDiagnosticLine;
    if (col <= 0) col = mDiagnosticCol;
    std::string hint;
    if (msg.find("undefined name") != std::string::npos)
        hint = "declare it with `let`, make sure its spelling matches, or export/import it from its package";
    else if (msg.find("FFI") != std::string::npos || msg.find("ABI") != std::string::npos)
        hint = "C FFI signatures may use only C-compatible scalar, `cstr`, `raw<T>`, or supported reference types";
    else if (msg.find("Argument count mismatch") != std::string::npos)
        hint = "adjust the call arguments to match the function declaration";
    else if (msg.find("declaration family") != std::string::npos ||
             msg.find("selector") != std::string::npos)
        hint = "use `select target with selector(arguments)` or `@selector(arguments) target`; a selector must return exactly one candidate";
    else if (msg.find("not callable") != std::string::npos)
        hint = "call a function value, not an ordinary value";
    else if (msg.find("must be an integer") != std::string::npos)
        hint = "bitwise operators (`&`, `|`, `^`, `~`) and shifts (`<<`, `>>`) only accept integer operands";
    else if (msg.find("const binding") != std::string::npos)
        hint = "use literals, other const bindings, reflection queries, or a `constexpr fn` call with compile-time arguments";
    else if (msg.find("constraint '") != std::string::npos)
        hint = "constraint predicates must be compile-time bool expressions and must hold for every concrete generic instantiation";
    else if (msg.find("type_") != std::string::npos && msg.find("requires") != std::string::npos)
        hint = "pass a valid type with `<Type>()`; metadata indexes must be non-negative compile-time integers";
    mErrors.push_back(diagnostic::format(
        "semantic", msg, mDiagnosticFile, line, col, hint,
        diagnostic::sourceLineFromFile(mDiagnosticFile, line)));
}

void SemanticContext::setDiagnosticLocation(const ASTNode* node) {
    if (!node) return;
    if (!node->sourcePath.empty()) mDiagnosticFile = node->sourcePath;
    if (node->line > 0) mDiagnosticLine = node->line;
    if (node->col > 0) mDiagnosticCol = node->col;
}

void SemanticContext::setDeclarationContext(const Decl* declaration) {
    mCurrentPackageId = effectivePackageId(mProgram, declaration);
    mCurrentModulePath = declaration ? declaration->modulePath : std::string{};
}

std::string SemanticContext::sourceDeclarationKey(
    const std::string& name, bool diagnoseVisibility) {
    const auto parts = splitQualifiedName(name);
    std::string packageId = mCurrentPackageId;
    std::string modulePath = mCurrentModulePath;
    std::string symbol = name;

    if (parts.size() > 1) {
        size_t moduleBegin = 0;
        auto ownerAliases = mPackageAliases.find(mCurrentPackageId);
        if (ownerAliases != mPackageAliases.end()) {
            auto alias = ownerAliases->second.find(parts.front());
            if (alias != ownerAliases->second.end()) {
                packageId = alias->second;
                moduleBegin = 1;
            }
        }
        symbol = parts.back();
        modulePath.clear();
        for (size_t index = moduleBegin; index + 1 < parts.size(); ++index) {
            if (!modulePath.empty()) modulePath += "::";
            modulePath += parts[index];
        }
    }

    const std::string key = qualifiedDeclarationKey(packageId, modulePath, symbol);
    auto declaration = mQualifiedDeclarations.find(key);
    if (declaration == mQualifiedDeclarations.end())
        return parts.size() == 1 ? name : key;
    if (diagnoseVisibility && packageId != mCurrentPackageId &&
        !declaration->second->isExported) {
        error("declaration '" + name + "' is private to package '" + packageId + "'");
    }
    return key;
}

SymbolInfo* SemanticContext::lookupSymbol(const std::string& name) {
    // Lexical bindings and compiler built-ins deliberately shadow package
    // declarations for an unqualified name.
    if (name.find("::") == std::string::npos) {
        if (auto* direct = mSymTable.lookup(name)) return direct;
    }
    return mSymTable.lookup(sourceDeclarationKey(name));
}

TypePtr SemanticContext::lookupDeclaredType(const std::string& name) {
    if (auto direct = mSymTable.lookupType(name)) return direct;
    return mSymTable.lookupType(sourceDeclarationKey(name));
}
