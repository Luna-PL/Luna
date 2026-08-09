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

SemanticContext::SemanticContext() {
    // Register built-in types
    mSymTable.defineType("i32", TyI32);
    mSymTable.defineType("i64", TyI64);
    mSymTable.defineType("f32", TyF32);
    mSymTable.defineType("f64", TyF64);
    mSymTable.defineType("bool", TyBool);
    mSymTable.defineType("string", TyString);

    // Register built-in print function (generic, handled by codegen)
    SymbolInfo printInfo;
    printInfo.kind = SymbolKind::Function;
    printInfo.typeParams = {"T"};
    printInfo.returnType = TyUnit;
    printInfo.paramTypes = {Type::makeTypeParam("T")};
    mSymTable.defineAtRoot("print", printInfo);
}

bool SemanticContext::analyze(Program* program) {
    mProgram = program;
    mConstScopes.clear();
    enterConstScope();
    mConstexprFunctions.clear();
    mSlotScopes.clear();
    mApplyScopes.clear();
    mDynamicApplyScopes.clear();
    enterSlotScope();
    mFragments.clear();
    mMetadataSchemas.clear();
    mConcepts.clear();
    mFunctionFamilies.clear();
    mQualifiedDeclarations.clear();
    mPackageAliases.clear();
    mGeneratedInstances.clear();
    mInstantiator.reset();
    mInstantiatedFunctions.clear();
    mInferenceRoots.clear();
    mDeclarationReferences.clear();
    mIteratorStateCounter = 0;
    mTraits.clear();
    mTraitTypeParams.clear();
    mTraitMethods.clear();
    mTraitOwners.clear();
    // Drop is a compiler-known resource contract. Programs provide impls,
    // but cannot replace its identity or signature with a source declaration.
    mTraitMethods[luna::sysmeta::DropTraitId] = {
        {luna::sysmeta::DropMethodName, nullptr}
    };
    mTraitOwners[luna::sysmeta::DropTraitId] = "luna.compiler";
    mTraitMethods[luna::sysmeta::FromTraitId] = {
        {luna::sysmeta::FromMethodName, nullptr}
    };
    mTraitOwners[luna::sysmeta::FromTraitId] = "luna.compiler";
    mImpls.clear();
    mFromConversions.clear();
    mFromIteratorImplementations.clear();
    mCurrentPackageId = program->packageName.empty() ? "main" : program->packageName;
    mCurrentModulePath.clear();
    const size_t sourceDeclarationCount = program->declarations.size();

    for (const auto& use : program->packageUses) {
        const std::string owner = use.ownerPackageId.empty()
            ? mCurrentPackageId : use.ownerPackageId;
        mPackageAliases[owner][use.alias] = use.packageId;
    }

    // A package has one shared namespace even when its declarations come from
    // different files. Diagnose collisions here instead of allowing LLVM to
    // silently create suffixed symbols during code generation.
    std::unordered_map<std::string, Decl*> declaredNames;
    std::unordered_map<std::string, size_t> linkageNameCounts;
    size_t rootEntryCount = 0;
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto* declaration = program->declarations[i].get();
        std::string name;
        if (auto* f = dynamic_cast<FunctionDecl*>(declaration)) name = f->name;
        else if (auto* f = dynamic_cast<FragmentDecl*>(declaration)) name = f->name;
        else if (auto* s = dynamic_cast<StructDecl*>(declaration)) name = s->name;
        else if (auto* e = dynamic_cast<EnumDecl*>(declaration)) name = e->name;
        else if (auto* t = dynamic_cast<TraitDecl*>(declaration)) name = t->name;
        else if (auto* m = dynamic_cast<MetaDecl*>(declaration)) name = m->name;
        else if (auto* c = dynamic_cast<ConstraintDecl*>(declaration)) name = c->name;
        if (!name.empty()) ++linkageNameCounts[metadataDeclarationName(name, declaration)];
    }
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto* declaration = program->declarations[i].get();
        setDeclarationContext(declaration);
        setDiagnosticLocation(declaration);
        std::string name;
        if (auto* f = dynamic_cast<FunctionDecl*>(declaration)) name = f->name;
        else if (auto* f = dynamic_cast<FragmentDecl*>(declaration)) name = f->name;
        else if (auto* s = dynamic_cast<StructDecl*>(declaration)) name = s->name;
        else if (auto* e = dynamic_cast<EnumDecl*>(declaration)) name = e->name;
        else if (auto* t = dynamic_cast<TraitDecl*>(declaration)) name = t->name;
        else if (auto* m = dynamic_cast<MetaDecl*>(declaration)) name = m->name;
        else if (auto* c = dynamic_cast<ConstraintDecl*>(declaration)) name = c->name;
        if (!name.empty()) {
            const std::string familyKey = qualifiedDeclarationKey(
                mCurrentPackageId, mCurrentModulePath, name);
            const std::string sourceLinkage = metadataDeclarationName(name, declaration);
            const bool isRootEntry = name == "main" &&
                mCurrentPackageId == (program->packageName.empty()
                    ? std::string("main") : program->packageName);
            if (isRootEntry && ++rootEntryCount > 1)
                error("Package has more than one 'main' entry declaration");
            declaration->generatedSymbolName = isRootEntry
                ? "main"
                : (linkageNameCounts[sourceLinkage] > 1 || sourceLinkage == "main")
                ? isolatedLinkageName(familyKey + "::" + sourceLinkage, sourceLinkage)
                : sourceLinkage;
            const std::string identity = familyKey + "::" + sourceLinkage;
            if (!declaredNames.emplace(identity, declaration).second)
                error("Duplicate package declaration '" + name + "' in module '" +
                      (mCurrentModulePath.empty() ? std::string("<root>")
                                                  : mCurrentModulePath) + "'");
            mQualifiedDeclarations.emplace(familyKey, declaration);
        }
        if (declaration->isExported && dynamic_cast<ImplDecl*>(declaration))
            error("Only functions, types, and traits can be exported; 'impl' is an internal declaration");
    }
    // Metadata schemas are ordinary declarations, but attachments on every
    // other declaration need their types during the declaration pass.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* metadata = dynamic_cast<MetaDecl*>(program->declarations[i].get())) {
            setDeclarationContext(metadata);
            setDiagnosticLocation(metadata);
            declareMeta(metadata);
        }
    }
    // Constraints are named compile-time predicates. Register all names
    // before functions so where clauses and constraint composition are
    // declaration-order independent.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* constraintDecl =
                dynamic_cast<ConstraintDecl*>(program->declarations[i].get())) {
            setDeclarationContext(constraintDecl);
            setDiagnosticLocation(constraintDecl);
            declareConstraint(constraintDecl);
        }
    }
    // Bind every nominal name before resolving any field. This permits
    // forward references and makes the declaration identity independent from
    // the order in which source declarations appear.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* s = dynamic_cast<StructDecl*>(program->declarations[i].get())) {
            setDeclarationContext(s);
            const std::string identity = s->generatedSymbolName.empty()
                ? s->name : s->generatedSymbolName;
            const std::string sourceKey = qualifiedDeclarationKey(
                mCurrentPackageId, mCurrentModulePath, s->name);
            if (!mDeclaredTypes.count(sourceKey)) {
                auto type = Type::makeStruct(
                    s->name, {},
                    nominalDeclarationIdentity(program, "struct", identity, s));
                type->declarationLinkageName = identity;
                type->typeParams = s->typeParams;
                mDeclaredTypes[identity] = type;
                mDeclaredTypes[sourceKey] = type;
                mSymTable.defineType(identity, type);
                mSymTable.defineType(sourceKey, type);
            }
        } else if (auto* e = dynamic_cast<EnumDecl*>(program->declarations[i].get())) {
            setDeclarationContext(e);
            const std::string identity = e->generatedSymbolName.empty()
                ? e->name : e->generatedSymbolName;
            const std::string sourceKey = qualifiedDeclarationKey(
                mCurrentPackageId, mCurrentModulePath, e->name);
            if (!mDeclaredTypes.count(sourceKey)) {
                auto type = Type::makeEnum(
                    e->name, {},
                    nominalDeclarationIdentity(program, "enum", identity, e));
                type->declarationLinkageName = identity;
                type->typeParams = e->typeParams;
                mDeclaredTypes[identity] = type;
                mDeclaredTypes[sourceKey] = type;
                mSymTable.defineType(identity, type);
                mSymTable.defineType(sourceKey, type);
            }
        }
    }
    // Pass 1a: register every trait identity before impls or generic
    // constraints are touched. This makes metadata-qualified declaration
    // families order-independent, just as nominal type binding above is.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        if (auto* t = dynamic_cast<TraitDecl*>(decl.get())) {
            setDeclarationContext(t);
            setDiagnosticLocation(t);
            declareTrait(t);
        }
    }
    // Pass 1b: populate every product/sum shape before any function or impl
    // can instantiate a generic nominal type.  Merely prebinding the nominal
    // name above is insufficient: an early Option<i32> instantiation would
    // otherwise copy an empty variant set and become declaration-order
    // dependent across packages.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        setDeclarationContext(decl.get());
        setDiagnosticLocation(decl.get());
        validateMetadata(decl.get());
        if (auto* s = dynamic_cast<StructDecl*>(decl.get())) declareStruct(s);
        else if (auto* e = dynamic_cast<EnumDecl*>(decl.get())) declareEnum(e);
    }
    // Pass 1c: declarations that may resolve or instantiate those complete
    // nominal shapes.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        setDeclarationContext(decl.get());
        setDiagnosticLocation(decl.get());
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get()))
            declareFunction(f);
        else if (auto* f =
                     dynamic_cast<FragmentDecl*>(decl.get()))
            declareFragment(f);
        else if (auto* implementation =
                     dynamic_cast<ImplDecl*>(decl.get()))
            declareImpl(implementation);
    }
    // Pass 2a: materialize all trait method sets before checking any impl or
    // generic call. This also supports trait declarations after their uses.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* t = dynamic_cast<TraitDecl*>(program->declarations[i].get())) {
            setDeclarationContext(t);
            setDiagnosticLocation(t);
            analyzeTrait(t);
        }
    }
    // Pass 2b: analyze all ordinary bodies.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        setDeclarationContext(decl.get());
        setDiagnosticLocation(decl.get());
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) analyzeFunction(f);
        else if (auto* s = dynamic_cast<StructDecl*>(decl.get())) analyzeStruct(s);
        else if (auto* e = dynamic_cast<EnumDecl*>(decl.get())) analyzeEnum(e);
        else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) analyzeImpl(i);
        else if (auto* m = dynamic_cast<MetaDecl*>(decl.get())) analyzeMeta(m);
        else if (auto* c = dynamic_cast<ConstraintDecl*>(decl.get()))
            analyzeConstraint(c);
    }
    // Numeric constraints have a useful, deterministic default. Other
    // unresolved variables are diagnosed because silently turning them into
    // a machine type would make inference depend on codegen details.
    mConstraints.defaultUnconstrainedNumeric();
    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            if (f->isExtern) continue;
            for (auto& p : f->params)
                checkUnresolved(p.inferredType, "parameter '" + p.name + "' of '" + f->name + "'");
            checkUnresolved(f->inferredReturnType, "return type of '" + f->name + "'");
        } else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& f : i->methods) {
                for (auto& p : f->params)
                    checkUnresolved(p.inferredType, "parameter '" + p.name + "' of '" + f->name + "'");
                checkUnresolved(f->inferredReturnType, "return type of '" + f->name + "'");
            }
        }
    }
    for (auto& root : mInferenceRoots)
        checkUnresolved(root.first, root.second);
    materializeInferredTypes(program);
    return mErrors.empty();
}

// ─── Declaration pass ──────────────────────────────────────────────

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
    FunctionDecl* function, const luna::selector::DeclarationView& view,
    const std::vector<ConstValue>& arguments, std::string& failure) {
    return mCompileTimeAnalysis->evaluateSelectorFunction(
        function, view, arguments, failure);
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
