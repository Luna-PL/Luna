#include "SemanticAnalyzer.h"
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

static std::string displayTraitRef(const TraitRef& trait) {
    return trait.name;
}

static std::string nominalDeclarationIdentity(
    const Program* program, const char* kind, const std::string& symbol,
    const Decl* declaration = nullptr) {
    std::string owner = declaration && !declaration->packageId.empty()
        ? declaration->packageId
        : (program && !program->packageName.empty() ? program->packageName : "main");
    if (declaration && !declaration->modulePath.empty())
        owner += "::" + declaration->modulePath;
    return owner + "::" + kind + "::" + symbol;
}

static uint64_t stableMetadataHash(const std::string& value) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : value) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

static std::string metadataExpressionKey(const Expr* expression) {
    if (auto* value = dynamic_cast<const IntLiteralExpr*>(expression))
        return "i:" + std::to_string(value->value);
    if (auto* value = dynamic_cast<const FloatLiteralExpr*>(expression)) {
        std::ostringstream output;
        output << "f:" << std::setprecision(17) << value->value;
        return output.str();
    }
    if (auto* value = dynamic_cast<const BoolLiteralExpr*>(expression))
        return value->value ? "b:1" : "b:0";
    if (auto* value = dynamic_cast<const StringLiteralExpr*>(expression))
        return "s:" + std::to_string(value->value.size()) + ":" + value->value;
    if (auto* value = dynamic_cast<const IdentifierExpr*>(expression))
        return "id:" + value->name;
    return "expr@" + expression->sourcePath + ":" +
           std::to_string(expression->line) + ":" + std::to_string(expression->col);
}

static std::string metadataDeclarationName(const std::string& base,
                                           const Decl* declaration) {
    if (!declaration || declaration->metadata.empty()) return base;
    std::string key;
    for (const auto& attachment : declaration->metadata) {
        key += attachment.schemaName + "(";
        for (const auto& argument : attachment.arguments)
            key += metadataExpressionKey(argument.get()) + ";";
        key += ")";
    }
    std::ostringstream output;
    output << base << "__meta_" << std::hex << std::setw(16)
           << std::setfill('0') << stableMetadataHash(key);
    return output.str();
}

static std::string effectivePackageId(const Program* program, const Decl* declaration) {
    if (declaration && !declaration->packageId.empty()) return declaration->packageId;
    if (program && !program->packageName.empty()) return program->packageName;
    return "main";
}

static std::string nominalTypeOwner(const TypePtr& type) {
    if (!type || type->nominalId.empty()) return {};
    const size_t separator = type->nominalId.find("::");
    return separator == std::string::npos
        ? std::string{} : type->nominalId.substr(0, separator);
}

static TypePtr substituteNominalType(
    const TypePtr& type,
    const std::unordered_map<std::string, TypePtr>& bindings);

static std::string qualifiedDeclarationKey(const std::string& packageId,
                                           const std::string& modulePath,
                                           const std::string& name) {
    return packageId + "::" + (modulePath.empty() ? "" : modulePath + "::") + name;
}

static std::vector<std::string> splitQualifiedName(const std::string& name) {
    std::vector<std::string> parts;
    size_t begin = 0;
    while (begin <= name.size()) {
        const size_t separator = name.find("::", begin);
        parts.push_back(name.substr(begin, separator == std::string::npos
            ? std::string::npos : separator - begin));
        if (separator == std::string::npos) break;
        begin = separator + 2;
    }
    return parts;
}

static bool reachesInlineType(
    const TypePtr& current, const Type* target,
    std::unordered_set<const Type*>& active) {
    if (!current) return false;
    if (current.get() == target) return true;
    if (!active.insert(current.get()).second) return false;
    bool reaches = false;
    switch (current->kind) {
        case TypeKind::Array:
            reaches = reachesInlineType(
                current->inner, target, active);
            break;
        case TypeKind::Result:
            for (const auto& argument : current->typeArgs)
                reaches = reaches ||
                    reachesInlineType(argument, target, active);
            break;
        case TypeKind::Enum:
            for (const auto& variant : current->variants)
                for (const auto& field : variant.fields)
                    reaches = reaches ||
                        reachesInlineType(field, target, active);
            break;
        default:
            // Product values and explicit pointer/reference/shared wrappers
            // are representation barriers in the current ABI.
            break;
    }
    active.erase(current.get());
    return reaches;
}

static std::string isolatedLinkageName(const std::string& key,
                                       const std::string& sourceName) {
    std::ostringstream output;
    output << "__luna_" << std::hex << std::setw(16) << std::setfill('0')
           << stableMetadataHash(key) << "_" << sourceName;
    return output.str();
}

SemanticAnalyzer::SemanticAnalyzer() {
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

bool SemanticAnalyzer::analyze(Program* program) {
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
                auto type = Type::makeStruct(s->name, {}, s->isNominal
                    ? nominalDeclarationIdentity(program, "struct", identity, s) : "");
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
                auto type = Type::makeEnum(e->name, {}, e->isNominal
                    ? nominalDeclarationIdentity(program, "enum", identity, e) : "");
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

void SemanticAnalyzer::declareFunction(FunctionDecl* decl) {
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    mFunctionFamilies[sourceKey].push_back(decl);
    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) {
        bindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& clause : decl->whereClauses) {
        if (clause.kind == WhereClause::Kind::TraitBound) {
            resolveTraitRef(clause.trait, decl);
            continue;
        }
        const std::string key = sourceDeclarationKey(clause.constraintName);
        auto constraintIt = mConcepts.find(key);
        if (constraintIt == mConcepts.end()) {
            error("unknown constraint '" + clause.constraintName + "'",
                  decl->line, decl->col);
            continue;
        }
        clause.constraintName = key;
        if (clause.constraintTypeArgs.size() != constraintIt->second->typeParams.size())
            error("constraint '" + constraintIt->second->name + "' expects " +
                  std::to_string(constraintIt->second->typeParams.size()) +
                  " type arguments", decl->line, decl->col);
    }
    SymbolInfo info;
    info.kind = SymbolKind::Function;
    info.isExported = decl->isExported;
    info.isExtern = decl->isExtern;
    info.returnsLinear = decl->returnsLinear;
    info.returnType = declaredType(decl->returnType.get(), bindings);
    if ((decl->isExtern || !decl->abi.empty()) && !decl->returnType)
        info.returnType = TyUnit;
    decl->inferredReturnType = info.returnType;
    const bool explicitReturnUsage = decl->returnsLinear ||
        dynamic_cast<AffineTypeAST*>(decl->returnType.get()) != nullptr;
    decl->returnUsage = explicitReturnUsage
        ? (decl->returnsLinear ? luna::ownership::Usage::Linear
                               : luna::ownership::Usage::Affine)
        : defaultUsageForType(info.returnType);
    info.returnUsage = decl->returnUsage;
    info.typeParams = decl->typeParams;
    info.genericDecl = decl;
    for (auto& p : decl->params) {
        TypePtr pt = declaredType(p.type.get(), bindings);
        p.inferredType = pt;
        const bool explicitUsage = p.hasExplicitUsage || p.isLinear ||
            dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr ||
            dynamic_cast<AffineTypeAST*>(p.type.get()) != nullptr;
        const auto usage = p.isLinear || dynamic_cast<LinearTypeAST*>(p.type.get())
            ? luna::ownership::Usage::Linear
            : (explicitUsage ? luna::ownership::Usage::Affine : defaultUsageForType(pt));
        const auto contract = parameterContractFor(pt, usage, explicitUsage);
        p.usage = contract.usage;
        p.relation = contract.relation;
        info.paramContracts.push_back(contract);
        info.paramTypes.push_back(pt);
    }
    if (mFunctionFamilies[sourceKey].size() == 1) {
        mSymTable.defineAtRoot(sourceKey, info);
    }
    mSymTable.defineLinkage(decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName, info);
    if (decl->isConstexpr)
        mConstexprFunctions[decl->generatedSymbolName.empty()
            ? decl->name : decl->generatedSymbolName] = decl;
    if (decl->isExtern || (decl->isExported && !decl->abi.empty()))
        validateFFIFunction(decl);
}

void SemanticAnalyzer::declareMeta(MetaDecl* decl) {
    if (!decl) return;
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    if (mMetadataSchemas.count(sourceKey)) {
        error("duplicate metadata schema '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    std::set<std::string> fieldNames;
    std::vector<TypeField> fields;
    for (auto& field : decl->fields) {
        if (!fieldNames.insert(field.name).second)
            error("duplicate metadata field '" + field.name + "' in '" +
                  decl->name + "'", decl->line, decl->col);
        field.inferredType = declaredType(field.type.get(), {});
        fields.push_back({field.name, field.inferredType});
    }
    const auto schemaSymbol = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto metadataType = Type::makeMetadata(
        nominalDeclarationIdentity(mProgram, "meta", schemaSymbol, decl),
        std::move(fields));
    metadataType->name = decl->name;
    mMetadataSchemas[sourceKey] = decl;
    mSymTable.defineType(sourceKey, metadataType);
    mSymTable.defineType(schemaSymbol, metadataType);

    SymbolInfo constructor;
    constructor.kind = SymbolKind::Metadata;
    constructor.type = metadataType;
    constructor.returnType = metadataType;
    for (const auto& field : decl->fields)
        constructor.paramTypes.push_back(field.inferredType);
    if (!mSymTable.defineAtRoot(sourceKey, std::move(constructor)))
        error("metadata schema name '" + decl->name +
              "' conflicts with an existing declaration", decl->line, decl->col);
}

void SemanticAnalyzer::declareConstraint(ConstraintDecl* decl) {
    if (!decl) return;
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    if (!mConcepts.emplace(sourceKey, decl).second) {
        error("duplicate constraint '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    std::set<std::string> parameters;
    for (const auto& parameter : decl->typeParams) {
        if (!parameters.insert(parameter).second)
            error("duplicate type parameter '" + parameter +
                  "' in constraint '" + decl->name + "'", decl->line, decl->col);
    }
}

void SemanticAnalyzer::analyzeConstraint(ConstraintDecl* decl) {
    if (!decl || !decl->predicate) return;
    mSymTable.enterScope();
    for (const auto& parameter : decl->typeParams) {
        SymbolInfo info;
        info.kind = SymbolKind::TypeParam;
        info.type = Type::makeTypeParam(parameter);
        mSymTable.define(parameter, info);
    }
    TypePtr predicate = resolved(analyzeExpr(decl->predicate.get()));
    if (predicate->kind != TypeKind::Bool &&
        predicate->kind != TypeKind::InferenceVar)
        error("constraint '" + decl->name +
              "' predicate must have type bool", decl->line, decl->col);
    mSymTable.exitScope();
}

void SemanticAnalyzer::analyzeMeta(MetaDecl* decl) {
    if (!decl) return;
    for (auto& field : decl->fields)
        checkUnresolved(field.inferredType,
                        "metadata field '" + decl->name + "." + field.name + "'");
}

void SemanticAnalyzer::validateMetadata(Decl* decl) {
    if (!decl) return;
    for (auto& attachment : decl->metadata) {
        auto schema = mMetadataSchemas.find(sourceDeclarationKey(attachment.schemaName));
        if (schema == mMetadataSchemas.end()) {
            error("unknown metadata schema '" + attachment.schemaName + "'",
                  decl->line, decl->col);
            continue;
        }
        const auto schemaSymbol = schema->second->generatedSymbolName.empty()
            ? schema->second->name : schema->second->generatedSymbolName;
        attachment.resolvedSchemaId = nominalDeclarationIdentity(
            mProgram, "meta", schemaSymbol, schema->second);
        if (attachment.arguments.size() != schema->second->fields.size()) {
            error("metadata '" + attachment.schemaName + "' expects " +
                  std::to_string(schema->second->fields.size()) + " arguments, got " +
                  std::to_string(attachment.arguments.size()), decl->line, decl->col);
            continue;
        }
        attachment.evaluatedArguments.clear();
        for (size_t index = 0; index < attachment.arguments.size(); ++index) {
            auto value = evaluateConstExpr(attachment.arguments[index].get());
            if (!value) {
                error("metadata argument " + std::to_string(index + 1) + " of '" +
                      attachment.schemaName + "' is not a compile-time value",
                      decl->line, decl->col);
                continue;
            }
            TypePtr actual = TyUnknown;
            if (std::holds_alternative<int64_t>(*value)) actual = TyI32;
            else if (std::holds_alternative<double>(*value)) actual = TyF64;
            else if (std::holds_alternative<bool>(*value)) actual = TyBool;
            else if (std::holds_alternative<std::string>(*value)) actual = TyString;
            constrain(actual, schema->second->fields[index].inferredType,
                      "metadata field '" + attachment.schemaName + "." +
                          schema->second->fields[index].name + "'");
            attachment.evaluatedArguments.push_back(*value);
        }
        if (attachment.retention == RetentionKind::Runtime &&
            decl->retention == RetentionKind::CompileTime)
            decl->retention = RetentionKind::Runtime;
        if (attachment.retention == RetentionKind::Dynamic)
            decl->retention = RetentionKind::Dynamic;
    }
}

void SemanticAnalyzer::declareFragment(FragmentDecl* decl) {
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    if (!mFragments.emplace(sourceKey, decl).second) {
        error("duplicate fragment declaration '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    SymbolInfo info;
    info.kind = SymbolKind::Fragment;
    TypeVec parameterTypes;
    std::vector<luna::ownership::Contract> parameterContracts;
    for (auto& parameter : decl->params) {
        parameter.inferredType = declaredType(parameter.type.get(), {});
        parameterTypes.push_back(parameter.inferredType);
        const bool explicitUsage = parameter.hasExplicitUsage || parameter.isLinear ||
            dynamic_cast<LinearTypeAST*>(parameter.type.get()) != nullptr ||
            dynamic_cast<AffineTypeAST*>(parameter.type.get()) != nullptr;
        const auto requestedUsage = parameter.isLinear
            ? luna::ownership::Usage::Linear
            : (explicitUsage ? parameter.usage
                             : defaultUsageForType(parameter.inferredType));
        const auto contract = parameterContractFor(
            parameter.inferredType, requestedUsage, explicitUsage);
        parameter.relation = contract.relation;
        parameter.usage = contract.usage;
        parameterContracts.push_back(contract);
    }
    info.type = Type::makeFragment(
        std::move(parameterTypes), TyUnit,
        decl->cardinality == FragmentCardinality::Many,
        decl->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        std::move(parameterContracts));
    decl->structuralType = info.type;
    mSymTable.defineAtRoot(sourceKey, info);
}

bool SemanticAnalyzer::isFFIType(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (!t) return false;
    switch (t->kind) {
        case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
        case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
        case TypeKind::USize: case TypeKind::ISize:
        case TypeKind::F32: case TypeKind::F64:
        case TypeKind::CStr: case TypeKind::RawPointer: case TypeKind::Unit:
            return true;
        case TypeKind::Reference:
            if (t->inner && t->inner->kind != TypeKind::Struct &&
                t->inner->kind != TypeKind::Record && t->inner->kind != TypeKind::Enum &&
                t->inner->kind != TypeKind::RawPointer)
                return isFFIType(t->inner, context);
            break;
        default:
            break;
    }
    error("Unsupported FFI type " + t->toString() + " in " + context);
    return false;
}

void SemanticAnalyzer::validateFFIFunction(FunctionDecl* decl) {
    if (!decl->abi.empty() && decl->abi != "C")
        error("Unsupported ABI '" + decl->abi + "' for function '" + decl->name + "'");
    if (decl->isExtern && decl->isExported)
        error("An extern function cannot also be exported: '" + decl->name + "'");
    if (decl->isExtern && decl->isConstexpr)
        error("An extern function cannot be constexpr: '" + decl->name + "'");
    if (!decl->typeParams.empty())
        error("C ABI function cannot be generic: '" + decl->name + "'");
    if (decl->returnsLinear &&
        (!decl->inferredReturnType || resolved(decl->inferredReturnType)->kind != TypeKind::RawPointer)) {
        error("owning FFI return of '" + decl->name + "' must use `linear raw<T>`");
    }
    for (auto& param : decl->params) {
        if (!param.type) {
            error("C ABI parameter '" + param.name + "' of '" + decl->name +
                  "' requires an explicit type");
            continue;
        }
        isFFIType(param.inferredType, "parameter '" + param.name + "' of '" + decl->name + "'");
    }
    isFFIType(decl->inferredReturnType,
              "return type of FFI function '" + decl->name + "'");
}

void SemanticAnalyzer::declareStruct(StructDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto declared = mDeclaredTypes[identity];
    if (!declared) {
        declared = Type::makeStruct(decl->name, {}, decl->isNominal
            ? nominalDeclarationIdentity(mProgram, "struct", identity, decl) : "");
        mDeclaredTypes[identity] = declared;
    }
    declared->typeParams = decl->typeParams;
    declared->fields.clear();
    mDeclaredTypes[identity] = declared;

    SymbolInfo info;
    info.kind = SymbolKind::Struct;
    info.isExported = decl->isExported;
    info.type = declared;
    info.typeParams = decl->typeParams;
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    mSymTable.defineAtRoot(sourceKey, info);
    mSymTable.defineType(sourceKey, declared);
    mSymTable.defineType(identity, declared);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& field : decl->fields) {
        TypeField typedField;
        typedField.name = field.name;
        typedField.type = resolveTypeAST(field.type.get(), bindings);
        field.inferredType = typedField.type;
        declared->fields.push_back(std::move(typedField));
    }
    if (!decl->isNominal && luna::types::isRecursiveShape(declared))
        error("recursive structural type '" + decl->name + "' requires `nominal`",
              decl->line, decl->col);
}

void SemanticAnalyzer::declareEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto declared = mDeclaredTypes[identity];
    if (!declared) {
        declared = Type::makeEnum(decl->name, {}, decl->isNominal
            ? nominalDeclarationIdentity(mProgram, "enum", identity, decl) : "");
        mDeclaredTypes[identity] = declared;
    }
    declared->typeParams = decl->typeParams;
    declared->variants.clear();
    mDeclaredTypes[identity] = declared;
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    mSymTable.defineType(sourceKey, declared);
    mSymTable.defineType(identity, declared);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& variant : decl->variants) {
        TypeVariant typedVariant;
        typedVariant.name = variant.name;
        for (auto& field : variant.fields)
            typedVariant.fields.push_back(resolveTypeAST(field.get(), bindings));
        declared->variants.push_back(std::move(typedVariant));
    }
    if (!decl->isNominal && luna::types::isRecursiveShape(declared))
        error("recursive structural type '" + decl->name + "' requires `nominal`",
              decl->line, decl->col);
}

void SemanticAnalyzer::declareTrait(TraitDecl* decl) {
    if (decl->name == "Drop" || decl->name == "From") {
        error("trait name '" + decl->name +
              "' is reserved for a compiler-known resource/error contract",
              decl->line, decl->col);
        return;
    }
    const std::string identity = traitIdentity(decl);
    decl->resolvedTraitId = identity;
    const std::string sourceKey = qualifiedDeclarationKey(
        mCurrentPackageId, mCurrentModulePath, decl->name);
    if (!mTraits.emplace(sourceKey, decl).second)
        error("duplicate trait declaration '" + decl->name + "'", decl->line, decl->col);
    mTraitTypeParams[identity] = decl->typeParams;
    mTraitOwners[identity] = effectivePackageId(mProgram, decl);
    // Traits are compile-time interfaces with stable declaration identity.
    mSymTable.defineType(identity, Type::makeTrait(identity));
    mSymTable.defineType(sourceKey, Type::makeTrait(identity));
}

void SemanticAnalyzer::declareImpl(ImplDecl* decl) {
    const std::string traitId = resolveTraitRef(decl->trait, decl);
    if (traitId.empty()) return;
    const auto coreFromIterator =
        mTraits.find(luna::sysmeta::FromIteratorTraitId);
    const bool isCoreFromIteratorTrait =
        coreFromIterator != mTraits.end() &&
        traitId == traitIdentity(coreFromIterator->second);
    const TypePtr targetType = resolveTypeAST(decl->targetType.get(), {});
    const std::string targetId = typeIdentity(targetType);
    decl->resolvedTargetTypeId = targetId;
    const std::string implPackage =
        effectivePackageId(mProgram, decl);
    const std::string targetOwner =
        nominalTypeOwner(resolved(targetType));
    const auto traitOwnerEntry = mTraitOwners.find(traitId);
    const std::string traitOwner =
        traitOwnerEntry == mTraitOwners.end()
            ? std::string{} : traitOwnerEntry->second;

    auto registerMethod = [&](FunctionDecl* method,
                              const std::string& symbol,
                              bool exposeUnqualified = true) {
        method->generatedSymbolName = symbol;
        SymbolInfo info;
        info.kind = SymbolKind::Function;
        info.returnType = declaredType(method->returnType.get(), {});
        method->inferredReturnType = info.returnType;
        info.returnsLinear = method->returnsLinear;
        method->returnUsage = method->returnsLinear
            ? luna::ownership::Usage::Linear
            : defaultUsageForType(info.returnType);
        info.returnUsage = method->returnUsage;
        for (auto& parameter : method->params) {
            parameter.inferredType = declaredType(parameter.type.get(), {});
            const bool explicitUsage =
                parameter.hasExplicitUsage || parameter.isLinear ||
                dynamic_cast<LinearTypeAST*>(parameter.type.get()) ||
                dynamic_cast<AffineTypeAST*>(parameter.type.get());
            auto requestedUsage = parameter.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage ? parameter.usage
                                 : defaultUsageForType(parameter.inferredType));
            auto contract = parameterContractFor(
                parameter.inferredType, requestedUsage, explicitUsage);
            parameter.usage = contract.usage;
            parameter.relation = contract.relation;
            info.paramContracts.push_back(contract);
            info.paramTypes.push_back(parameter.inferredType);
        }
        if (exposeUnqualified)
            mSymTable.defineAtRoot(method->name, info);
        mSymTable.defineLinkage(symbol, info);
    };

    if (traitId == luna::sysmeta::FromTraitId) {
        if (!decl->typeParams.empty()) {
            error("generic `impl From<Source>` is not yet supported; use concrete error types",
                  decl->line, decl->col);
            return;
        }
        if (decl->trait.typeArgs.size() != 1) {
            error("From expects exactly one source type argument",
                  decl->line, decl->col);
            return;
        }
        TypePtr sourceType = resolveTypeAST(
            decl->trait.typeArgs.front().get(), {});
        decl->trait.resolvedTypeArgs = {sourceType};
        const std::string sourceOwner =
            nominalTypeOwner(resolved(sourceType));
        if (targetOwner != implPackage &&
            sourceOwner != implPackage) {
            error("orphan impl of compiler trait `From`: package '" +
                  implPackage +
                  "' owns neither target type '" +
                  targetType->toString() + "' nor source type '" +
                  sourceType->toString() + "'",
                  decl->line, decl->col);
            return;
        }
        const std::string sourceId = typeIdentity(sourceType);
        auto& conversions = mFromConversions[targetId];
        if (conversions.count(sourceId)) {
            error("duplicate `From<" + sourceType->toString() +
                  "> for " + targetType->toString() + "` implementation",
                  decl->line, decl->col);
            return;
        }
        FunctionDecl* conversion = nullptr;
        for (auto& method : decl->methods) {
            if (method->name == luna::sysmeta::FromMethodName && !conversion)
                conversion = method.get();
            const std::string symbol =
                std::string(luna::sysmeta::FromTraitId) + "__" + sourceId +
                "__for__" + targetId + "__" + method->name;
            registerMethod(method.get(), symbol, false);
        }
        decl->generatedSymbolName =
            std::string(luna::sysmeta::FromTraitId) + "__" + sourceId +
            "__for__" + targetId;
        conversions.emplace(sourceId, FromConversion{
            sourceType, targetType, conversion,
            conversion ? conversion->generatedSymbolName : std::string{}
        });
        return;
    }

    const auto traitParameters = mTraitTypeParams.find(traitId);
    const size_t expectedTraitArgumentCount =
        traitParameters == mTraitTypeParams.end()
            ? 0 : traitParameters->second.size();
    if (decl->trait.typeArgs.size() !=
        expectedTraitArgumentCount) {
        error("trait '" + displayTraitRef(decl->trait) +
              "' expects " +
              std::to_string(expectedTraitArgumentCount) +
              " type argument(s)", decl->line, decl->col);
        return;
    }
    decl->trait.resolvedTypeArgs.clear();
    for (auto& argument : decl->trait.typeArgs)
        decl->trait.resolvedTypeArgs.push_back(
            resolveTypeAST(argument.get(), {}));

    if (isCoreFromIteratorTrait &&
        !decl->typeParams.empty()) {
        error("generic `FromIterator` impls are reserved until impl "
              "specialization participates in coherence; use a concrete "
              "collection, item, and builder type",
              decl->line, decl->col);
        return;
    }

    if (traitId == luna::sysmeta::DropTraitId) {
        if (targetOwner != implPackage) {
            error("orphan impl of `Drop`: package '" + implPackage +
                  "' does not own nominal target type '" +
                  targetType->toString() + "'",
                  decl->line, decl->col);
            return;
        }
    } else if (traitOwner != implPackage &&
               targetOwner != implPackage) {
        error("orphan impl of trait '" + displayTraitRef(decl->trait) +
              "': package '" + implPackage +
              "' owns neither the trait nor nominal target type '" +
              targetType->toString() + "'",
              decl->line, decl->col);
        return;
    }

    auto& implementations = mImpls[traitId];
    if (implementations.count(targetId)) {
        error("duplicate impl of trait '" + traitId + "' for type '" + targetId + "'",
              decl->line, decl->col);
        return;
    }
    implementations.emplace(targetId, std::unordered_map<std::string, FunctionDecl*>{});
    auto& methods = implementations.at(targetId);
    for (auto& method : decl->methods) {
        if (methods.count(method->name)) {
            error("duplicate method '" + method->name + "' in impl of trait '" + traitId + "'",
                  method->line, method->col);
            continue;
        }
        methods[method->name] = method.get();
        registerMethod(
            method.get(),
            traitId + "__for__" + targetId + "__" + method->name);
    }
    if (isCoreFromIteratorTrait &&
        decl->trait.resolvedTypeArgs.size() == 2) {
        FromIteratorImplementation protocol;
        protocol.item = decl->trait.resolvedTypeArgs[0];
        protocol.builder = decl->trait.resolvedTypeArgs[1];
        protocol.target = targetType;
        auto begin = methods.find(
            luna::sysmeta::FromIteratorBeginMethodName);
        auto push = methods.find(
            luna::sysmeta::FromIteratorPushMethodName);
        auto finish = methods.find(
            luna::sysmeta::FromIteratorFinishMethodName);
        if (begin != methods.end()) protocol.begin = begin->second;
        if (push != methods.end()) protocol.push = push->second;
        if (finish != methods.end()) protocol.finish = finish->second;
        mFromIteratorImplementations[targetId] =
            std::move(protocol);
    }
}

// ─── Analysis pass ─────────────────────────────────────────────────

void SemanticAnalyzer::analyzeFunction(FunctionDecl* decl) {
    TypePtr savedReturn = mCurrentReturnType;
    bool savedInFunction = mInFunction;
    bool savedInKernel = mInKernel;
    bool savedReturnsLinear = mCurrentFunctionReturnsLinear;
    auto savedReturnUsage = mCurrentFunctionReturnUsage;
    bool savedSawReturn = mSawReturn;
    mInFunction = true;
    mInKernel = decl->isKernel;
    mCurrentFunctionReturnsLinear = decl->returnsLinear;
    if (decl->returnUsage == luna::ownership::Usage::Copy && decl->inferredReturnType)
        decl->returnUsage = defaultUsageForType(resolved(decl->inferredReturnType));
    mCurrentFunctionReturnUsage = decl->returnUsage;
    mSawReturn = false;
    mSymTable.enterScope();
    enterConstScope();
    enterSlotScope();

    // Build type param bindings FIRST
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        TypePtr tpType = Type::makeTypeParam(tp);
        SymbolInfo info;
        info.kind = SymbolKind::TypeParam;
        info.type = tpType;
        mSymTable.define(tp, info);
        typeBindings[tp] = tpType;
    }

    mCurrentReturnType = decl->inferredReturnType
        ? decl->inferredReturnType
        : declaredType(decl->returnType.get(), typeBindings);

    if (decl->isKernel) {
        if (decl->isExtern || decl->isConstexpr || !decl->abi.empty())
            error("kernel '" + decl->name + "' cannot be extern, constexpr, or ABI-exported",
                  decl->line, decl->col);
        if (!decl->typeParams.empty())
            error("kernel '" + decl->name + "' cannot be generic in the initial device ABI",
                  decl->line, decl->col);
        if (decl->params.empty()) {
            error("kernel '" + decl->name + "' requires an explicit first parameter `index: i32`",
                  decl->line, decl->col);
        } else if (!decl->params.front().type ||
                   !luna::types::sameType(
                       resolved(decl->params.front().inferredType), TyI32)) {
            error("kernel '" + decl->name + "' must declare its first parameter as `index: i32`",
                  decl->line, decl->col);
        }
    }

    for (auto& p : decl->params) {
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = p.inferredType
            ? p.inferredType
            : declaredType(p.type.get(), typeBindings);
        const bool explicitUsage = p.hasExplicitUsage || p.isLinear ||
            dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr ||
            dynamic_cast<AffineTypeAST*>(p.type.get()) != nullptr;
        info.usage = p.isLinear || dynamic_cast<LinearTypeAST*>(p.type.get())
            ? luna::ownership::Usage::Linear
            : (explicitUsage ? p.usage : defaultUsageForType(resolved(info.type)));
        const auto contract = parameterContractFor(
            resolved(info.type), info.usage, explicitUsage);
        info.relation = contract.relation;
        info.usage = contract.usage;
        info.isLinear = info.usage == luna::ownership::Usage::Linear;
        p.usage = info.usage;
        p.relation = info.relation;
        p.inferredType = info.type;
        info.isHeapAllocated = false;
        mSymTable.define(p.name, info);
    }

    if (decl->isKernel) {
        for (size_t i = 1; i < decl->params.size(); ++i) {
            const auto& parameter = decl->params[i];
            const TypePtr type = resolved(parameter.inferredType);
            if (!parameter.type)
                error("kernel parameter '" + parameter.name + "' requires an explicit ABI type",
                      decl->line, decl->col);
            if (type->kind == TypeKind::DeviceBuffer)
                error("kernel parameter '" + parameter.name +
                      "' must use `&device_buffer<T>` or `&mut device_buffer<T>`; device buffers are never passed by value",
                      decl->line, decl->col);
            if (type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind != TypeKind::DeviceBuffer)
                error("kernel parameter '" + parameter.name +
                      "' may only use references to device_buffer<T> in the initial device ABI",
                      decl->line, decl->col);
        }
    }

    if (decl->body) {
        analyzeBlock(decl->body.get(), mCurrentReturnType);
    }

    // A function without an explicit return value is a unit-producing
    // declaration. Explicit return statements have already constrained this
    // variable through analyzeStmt.
    if (!decl->isExtern && !mSawReturn &&
        !(decl->body && blockAlwaysReturns(decl->body.get())))
        constrain(mCurrentReturnType, TyUnit, "empty function body");

    // A single return statement is not enough for a non-unit function: every
    // reachable path must return a value.  Keeping this in semantic analysis
    // gives a source-level diagnostic instead of letting host IR verification
    // discover an unterminated LLVM basic block later in the pipeline.
    if (!decl->isExtern && decl->body &&
        !luna::types::sameType(resolved(mCurrentReturnType), TyUnit) &&
        !blockAlwaysReturns(decl->body.get())) {
        error("function '" + decl->name + "' may finish without returning '" +
              resolved(mCurrentReturnType)->toString() + "'",
              decl->line, decl->col);
    }

    if (decl->isKernel &&
        !luna::types::sameType(resolved(mCurrentReturnType), TyUnit))
        error("kernel '" + decl->name + "' must return unit", decl->line, decl->col);

    mSymTable.exitScope();
    exitConstScope();
    exitSlotScope();
    mCurrentReturnType = savedReturn;
    mInFunction = savedInFunction;
    mInKernel = savedInKernel;
    mCurrentFunctionReturnsLinear = savedReturnsLinear;
    mCurrentFunctionReturnUsage = savedReturnUsage;
    mSawReturn = savedSawReturn;
}

void SemanticAnalyzer::analyzeStruct(StructDecl* decl) {
    // Just validate fields don't have errors
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        typeBindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& field : decl->fields) {
        resolveTypeAST(field.type.get(), typeBindings);
    }
}

void SemanticAnalyzer::analyzeEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto it = mDeclaredTypes.find(identity);
    if (it == mDeclaredTypes.end()) return;
    std::unordered_map<std::string, bool> names;
    for (auto& variant : it->second->variants) {
        if (names[variant.name])
            error("Duplicate enum variant '" + variant.name + "' in '" + decl->name + "'");
        names[variant.name] = true;
    }
    for (const auto& variant : it->second->variants) {
        for (const auto& field : variant.fields) {
            std::unordered_set<const Type*> active;
            if (reachesInlineType(
                    field, it->second.get(), active)) {
                error("enum '" + decl->name +
                      "' has an infinite inline layout through variant '" +
                      variant.name +
                      "'; place the recursive value behind a nominal "
                      "struct, raw pointer, rc, arc, or reference",
                      decl->line, decl->col);
            }
        }
    }
}

void SemanticAnalyzer::analyzeTrait(TraitDecl* decl) {
    // Store method signatures for constraint checking
    auto& sigs = mTraitMethods[traitIdentity(decl)];
    std::unordered_map<std::string, TypePtr> bindings;
    bindings["Self"] = Type::makeTypeParam("Self");
    for (const auto& typeParameter : decl->typeParams)
        bindings[typeParameter] =
            Type::makeTypeParam(typeParameter);
    for (auto& method : decl->methods) {
        for (auto& parameter : method.params) {
            parameter.inferredType =
                resolveTypeAST(parameter.type.get(), bindings);
            const bool explicitUsage =
                parameter.hasExplicitUsage ||
                parameter.isLinear ||
                dynamic_cast<LinearTypeAST*>(
                    parameter.type.get()) ||
                dynamic_cast<AffineTypeAST*>(
                    parameter.type.get());
            const auto requestedUsage = parameter.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage
                    ? parameter.usage
                    : defaultUsageForType(
                        parameter.inferredType));
            const auto contract = parameterContractFor(
                parameter.inferredType, requestedUsage,
                explicitUsage);
            parameter.relation = contract.relation;
            parameter.usage = contract.usage;
        }
        method.inferredReturnType =
            resolveTypeAST(method.returnType.get(), bindings);
        // Create a FunctionDecl-like entry
        auto fd = std::make_unique<FunctionDecl>();
        fd->name = method.name;
        fd->typeParams = decl->typeParams;
        for (auto& p : method.params) {
            Param paramClone;
            paramClone.name = p.name;
            paramClone.type = std::make_unique<NamedTypeAST>(
                p.type ? (dynamic_cast<NamedTypeAST*>(p.type.get()) ?
                         dynamic_cast<NamedTypeAST*>(p.type.get())->name : "?") : "?");
            fd->params.push_back(std::move(paramClone));
        }
        if (method.returnType) {
            fd->returnType = std::make_unique<NamedTypeAST>(
                dynamic_cast<NamedTypeAST*>(method.returnType.get()) ?
                dynamic_cast<NamedTypeAST*>(method.returnType.get())->name : "?");
        }
        sigs.emplace_back(method.name, fd.get());
        mGeneratedInstances.push_back(std::move(fd));
    }
}

void SemanticAnalyzer::analyzeImpl(ImplDecl* decl) {
    const std::string traitId = decl->trait.resolvedTraitId;
    if (traitId.empty()) return;
    const auto coreFromIterator =
        mTraits.find(luna::sysmeta::FromIteratorTraitId);
    const bool isCoreFromIteratorTrait =
        coreFromIterator != mTraits.end() &&
        traitId == traitIdentity(coreFromIterator->second);
    if (traitId == luna::sysmeta::FromTraitId) {
        const TypePtr source = decl->trait.resolvedTypeArgs.size() == 1
            ? resolved(decl->trait.resolvedTypeArgs.front()) : TyUnknown;
        const TypePtr target = resolved(
            resolveTypeAST(decl->targetType.get(), {}));
        FunctionDecl* conversion = nullptr;
        for (auto& method : decl->methods) {
            if (method->name != luna::sysmeta::FromMethodName) {
                error("impl of `From` defines unknown method '" +
                      method->name + "'", method->line, method->col);
            } else if (conversion) {
                error("impl of `From` defines `from` more than once",
                      method->line, method->col);
            } else {
                conversion = method.get();
            }
            analyzeFunction(method.get());
        }
        bool valid = conversion != nullptr;
        if (!conversion) {
            error("impl of `From<" + source->toString() +
                  "> for " + target->toString() +
                  "` is missing method `from`", decl->line, decl->col);
            return;
        }
        if (!conversion->typeParams.empty()) {
            error("From::from may not be generic",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (conversion->params.size() != 1 ||
            !luna::types::sameType(
                resolved(conversion->params.front().inferredType), source)) {
            error("From::from requires exactly one parameter of type '" +
                  source->toString() + "'", conversion->line, conversion->col);
            valid = false;
        } else if (luna::ownership::isMoveOnly(
                       defaultUsageForType(source)) &&
                   (conversion->params.front().relation !=
                        luna::ownership::Relation::Owned ||
                    conversion->params.front().usage !=
                        defaultUsageForType(source))) {
            error("From::from must take ownership of move-only source '" +
                  source->toString() +
                  "' with an explicit affine or linear parameter",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (!luna::types::sameType(
                resolved(conversion->inferredReturnType), target)) {
            error("From::from must return '" + target->toString() + "'",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (!valid) {
            auto targetIt = mFromConversions.find(typeIdentity(target));
            if (targetIt != mFromConversions.end())
                targetIt->second.erase(typeIdentity(source));
        }
        return;
    }
    // Verify that each impl method matches a trait method
    auto traitIt = mTraitMethods.find(traitId);
    if (traitIt == mTraitMethods.end()) {
        error("Impl for unknown trait '" + traitId + "'");
        return;
    }

    auto implementations = mImpls.find(traitId);
    auto implementation = implementations == mImpls.end()
        ? std::unordered_map<std::string, FunctionDecl*>{}
        : implementations->second[decl->resolvedTargetTypeId];
    std::unordered_map<std::string, bool> declaredMethods;
    for (const auto& [methodName, _] : traitIt->second) declaredMethods[methodName] = true;
    for (const auto& [methodName, _] : implementation) {
        if (!declaredMethods.count(methodName)) {
            error("impl of trait '" + displayTraitRef(decl->trait) +
                  "' defines unknown method '" + methodName + "'",
                  decl->line, decl->col);
        }
    }
    for (const auto& [methodName, _] : traitIt->second) {
        if (!implementation.count(methodName)) {
            error("impl of trait '" + displayTraitRef(decl->trait) +
                  "' for type '" + decl->resolvedTargetTypeId +
                  "' is missing method '" + methodName + "'", decl->line, decl->col);
        }
    }

    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        typeBindings[tp] = Type::makeTypeParam(tp);
    }

    for (auto& method : decl->methods) {
        // Analyze the method body
        analyzeFunction(method.get());
    }

    TraitDecl* traitDeclaration = nullptr;
    for (const auto& [_, candidate] : mTraits) {
        if (candidate &&
            traitIdentity(candidate) == traitId) {
            traitDeclaration = candidate;
            break;
        }
    }
    if (traitDeclaration) {
        std::unordered_map<std::string, TypePtr>
            signatureBindings;
        signatureBindings["Self"] = resolved(
            resolveTypeAST(decl->targetType.get(),
                           typeBindings));
        for (size_t index = 0;
             index < traitDeclaration->typeParams.size() &&
             index < decl->trait.resolvedTypeArgs.size();
             ++index)
            signatureBindings[
                traitDeclaration->typeParams[index]] =
                resolved(decl->trait.resolvedTypeArgs[index]);

        for (const auto& signature :
             traitDeclaration->methods) {
            auto implementationMethod = std::find_if(
                decl->methods.begin(), decl->methods.end(),
                [&](const std::unique_ptr<FunctionDecl>& method) {
                    return method &&
                           method->name == signature.name;
                });
            if (implementationMethod == decl->methods.end())
                continue;
            auto* method = implementationMethod->get();
            if (method->params.size() !=
                signature.params.size()) {
                error("method '" + signature.name +
                      "' has the wrong parameter count for trait '" +
                      displayTraitRef(decl->trait) + "'",
                      method->line, method->col);
                continue;
            }
            for (size_t index = 0;
                 index < method->params.size(); ++index) {
                TypePtr expected = substituteNominalType(
                    signature.params[index].inferredType,
                    signatureBindings);
                if (!luna::types::sameType(
                        resolved(method->params[index].inferredType),
                        resolved(expected)))
                    error("parameter " +
                          std::to_string(index + 1) +
                          " of method '" + signature.name +
                          "' must be '" + expected->toString() + "'",
                          method->line, method->col);
                if (method->params[index].relation !=
                        signature.params[index].relation ||
                    method->params[index].usage !=
                        signature.params[index].usage)
                    error("ownership contract of parameter " +
                          std::to_string(index + 1) +
                          " in method '" + signature.name +
                          "' does not match trait '" +
                          displayTraitRef(decl->trait) + "'",
                          method->line, method->col);
            }
            TypePtr expectedReturn = substituteNominalType(
                signature.inferredReturnType,
                signatureBindings);
            if (!luna::types::sameType(
                    resolved(method->inferredReturnType),
                    resolved(expectedReturn)))
                error("method '" + signature.name +
                      "' must return '" +
                      expectedReturn->toString() + "'",
                      method->line, method->col);
        }
    }

    if (isCoreFromIteratorTrait) {
        const TypePtr target = resolved(
            resolveTypeAST(decl->targetType.get(), typeBindings));
        auto protocolIt =
            mFromIteratorImplementations.find(typeIdentity(target));
        bool valid =
            protocolIt != mFromIteratorImplementations.end();
        FromIteratorImplementation* protocol =
            valid ? &protocolIt->second : nullptr;
        const TypePtr item =
            protocol ? resolved(protocol->item) : TyUnknown;
        const TypePtr builder =
            protocol ? resolved(protocol->builder) : TyUnknown;

        const auto rejectGeneric = [&](FunctionDecl* method) {
            if (!method || method->typeParams.empty()) return;
            error("FromIterator::" + method->name +
                  " may not be generic",
                  method->line, method->col);
            valid = false;
        };
        if (!protocol || !protocol->begin ||
            !protocol->push || !protocol->finish) {
            valid = false;
        } else {
            rejectGeneric(protocol->begin);
            rejectGeneric(protocol->push);
            rejectGeneric(protocol->finish);
            if (!protocol->begin->params.empty() ||
                !luna::types::sameType(
                    resolved(protocol->begin->inferredReturnType),
                    builder) ||
                protocol->begin->returnUsage !=
                    luna::ownership::Usage::Affine) {
                error("FromIterator::begin must take no parameters and "
                      "return `affine " + builder->toString() + "`",
                      protocol->begin->line, protocol->begin->col);
                valid = false;
            }
            if (protocol->push->params.size() != 2) {
                error("FromIterator::push must take `&mut " +
                      builder->toString() + "` and `affine " +
                      item->toString() + "`",
                      protocol->push->line, protocol->push->col);
                valid = false;
            } else {
                const TypePtr builderParameter = resolved(
                    protocol->push->params[0].inferredType);
                if (!builderParameter ||
                    builderParameter->kind != TypeKind::Reference ||
                    !builderParameter->isMutable ||
                    !luna::types::sameType(
                        resolved(builderParameter->inner), builder) ||
                    !luna::types::sameType(
                        resolved(protocol->push->params[1].inferredType),
                        item) ||
                    protocol->push->params[1].relation !=
                        luna::ownership::Relation::Owned ||
                    protocol->push->params[1].usage !=
                        luna::ownership::Usage::Affine) {
                    error("FromIterator::push must take `&mut " +
                          builder->toString() + "` and `affine " +
                          item->toString() + "` (got `" +
                          (builderParameter
                              ? builderParameter->toString()
                              : std::string("?")) + "`, `" +
                          resolved(protocol->push->params[1].inferredType)->
                              toString() + "` with " +
                          std::string(luna::ownership::relationName(
                              protocol->push->params[1].relation)) + "/" +
                          std::string(luna::ownership::usageName(
                              protocol->push->params[1].usage)) + ")",
                          protocol->push->line, protocol->push->col);
                    valid = false;
                }
            }
            if (!luna::types::sameType(
                    resolved(protocol->push->inferredReturnType),
                    TyUnit)) {
                error("FromIterator::push must return unit",
                      protocol->push->line, protocol->push->col);
                valid = false;
            }
            if (protocol->finish->params.size() != 1 ||
                !luna::types::sameType(
                    resolved(protocol->finish->params.front().inferredType),
                    builder) ||
                protocol->finish->params.front().relation !=
                    luna::ownership::Relation::Owned ||
                protocol->finish->params.front().usage !=
                    luna::ownership::Usage::Affine ||
                !luna::types::sameType(
                    resolved(protocol->finish->inferredReturnType),
                    target) ||
                protocol->finish->returnUsage !=
                    luna::ownership::Usage::Affine) {
                error("FromIterator::finish must take `affine " +
                      builder->toString() + "` and return `affine " +
                      target->toString() + "` (got parameter `" +
                      (protocol->finish->params.empty()
                          ? std::string("?")
                          : resolved(protocol->finish->params.front().
                                inferredType)->toString()) +
                      "` and return `" +
                      resolved(protocol->finish->inferredReturnType)->
                          toString() + "`)",
                      protocol->finish->line, protocol->finish->col);
                valid = false;
            }
            if (luna::ownership::mustConsume(
                    defaultUsageForType(item)) ||
                luna::ownership::mustConsume(
                    defaultUsageForType(builder)) ||
                luna::ownership::mustConsume(
                    defaultUsageForType(target))) {
                error("FromIterator currently requires affine-or-copy item, "
                      "builder, and target types; linear collection state "
                      "needs an explicit hidden obligation",
                      decl->line, decl->col);
                valid = false;
            }
        }
        if (!valid)
            mFromIteratorImplementations.erase(
                typeIdentity(target));
    }

    if (traitId == luna::sysmeta::DropTraitId) {
        FunctionDecl* drop = nullptr;
        auto dropIt = implementation.find(luna::sysmeta::DropMethodName);
        if (dropIt != implementation.end()) drop = dropIt->second;
        const TypePtr target = resolveTypeAST(decl->targetType.get(), typeBindings);
        bool valid = drop != nullptr;
        if (drop && !drop->typeParams.empty()) {
            error("Drop::drop may not be generic", drop->line, drop->col);
            valid = false;
        }
        if (drop && drop->params.size() != 1) {
            error("Drop::drop requires exactly one parameter of type '&mut " +
                  target->toString() + "'", drop->line, drop->col);
            valid = false;
        } else if (drop) {
            const TypePtr parameter = resolved(drop->params.front().inferredType);
            if (!parameter || parameter->kind != TypeKind::Reference ||
                !parameter->isMutable ||
                !luna::types::sameType(resolved(parameter->inner), resolved(target))) {
                error("Drop::drop parameter must be '&mut " + target->toString() + "'",
                      drop->line, drop->col);
                valid = false;
            }
        }
        if (drop && !luna::types::sameType(
                resolved(drop->inferredReturnType), TyUnit)) {
            error("Drop::drop must return unit", drop->line, drop->col);
            valid = false;
        }
        if (valid) {
            const std::string targetId = typeIdentity(target);
            for (auto& [_, declared] : mDeclaredTypes) {
                if (declared && typeIdentity(declared) == targetId) {
                    declared->sysmeta.resource.needsDrop = true;
                    declared->sysmeta.abi.dropGlueSymbol =
                        drop->generatedSymbolName;
                }
            }
            target->sysmeta.resource.needsDrop = true;
            target->sysmeta.abi.dropGlueSymbol = drop->generatedSymbolName;
        }
    }
}

void SemanticAnalyzer::analyzeSlotDecl(SlotDeclStmt* stmt) {
    if (mSlotScopes.back().count(stmt->name)) {
        error("duplicate slot declaration '" + stmt->name + "'", stmt->line, stmt->col);
        return;
    }
    TypeVec params;
    std::vector<luna::ownership::Contract> contracts;
    for (auto& param : stmt->params) {
        if (!param.type) {
            error("explicit slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
            params.push_back(TyUnknown);
            contracts.push_back({});
        } else {
            param.inferredType = declaredType(param.type.get(), {});
            params.push_back(param.inferredType);
            const bool explicitUsage = param.hasExplicitUsage || param.isLinear ||
                dynamic_cast<LinearTypeAST*>(param.type.get()) != nullptr ||
                dynamic_cast<AffineTypeAST*>(param.type.get()) != nullptr;
            const auto requestedUsage = param.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage ? param.usage
                                 : defaultUsageForType(param.inferredType));
            const auto contract = parameterContractFor(
                param.inferredType, requestedUsage, explicitUsage);
            param.relation = contract.relation;
            param.usage = contract.usage;
            contracts.push_back(contract);
        }
    }
    SlotInfo info;
    info.name = stmt->name;
    info.acceptedKind = stmt->acceptedKind;
    info.acceptedCardinality = stmt->acceptedCardinality;
    info.isDynamic = stmt->isDynamic;
    info.paramTypes = params;
    info.paramContracts = contracts;
    for (const auto& param : stmt->params) info.paramNames.push_back(param.name);
    info.defaultFragment = stmt->defaultFragment;
    if (!info.defaultFragment.empty()) {
        if (auto* fragment = selectFragment(info.defaultFragment, stmt)) {
            if (fragment->kind != info.acceptedKind ||
                fragment->cardinality != info.acceptedCardinality)
                error("default binding for slot '" + stmt->name +
                      "' has the wrong interceptor/context or once/many contract", stmt->line, stmt->col);
            info.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            stmt->resolvedDefaultFragmentName = info.resolvedDefaultFragmentName;
        }
    }
    info.structuralType = Type::makeSlot(
        params, TyUnit, info.acceptedCardinality == FragmentCardinality::Many,
        info.acceptedKind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        contracts);
    stmt->structuralType = info.structuralType;
    mSlotScopes.back().emplace(stmt->name, info);

    SymbolInfo symbol;
    symbol.kind = SymbolKind::Slot;
    symbol.type = stmt->structuralType;
    if (!mSymTable.define(stmt->name, symbol))
        error("slot name '" + stmt->name + "' conflicts with an existing binding", stmt->line, stmt->col);
}

void SemanticAnalyzer::analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    if (stmt->isDynamic) {
        error("dynamic slot must be a separate declaration with an explicit interface; write `dynamic slot name(value: Type);`",
              stmt->line, stmt->col);
        return;
    }
    auto lookupSlot = [this](const std::string& name) -> SlotInfo* {
        for (auto it = mSlotScopes.rbegin(); it != mSlotScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };
    auto lookupApplied = [this](const std::string& name) -> FragmentDecl* {
        for (auto it = mApplyScopes.rbegin(); it != mApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    };
    auto lookupDynamicApplied = [this](const std::string& name) -> const std::vector<FragmentDecl*>* {
        for (auto it = mDynamicApplyScopes.rbegin(); it != mDynamicApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };

    SlotInfo active;
    if (stmt->isImplicitCapture) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.isImplicitCapture = true;
        active.defaultFragment = stmt->defaultFragment;
        active.structuralType = Type::makeSlot(
            {}, TyUnit, active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context);
        if (!mSlotScopes.back().emplace(stmt->name, active).second) {
            error("duplicate implicit slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = mSlotScopes.back().at(stmt->name);
    } else if (!stmt->interfaceParams.empty()) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.defaultFragment = stmt->defaultFragment;
        for (auto& param : stmt->interfaceParams) {
            if (!param.type) {
                error("inline slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
                active.paramTypes.push_back(TyUnknown);
                continue;
            }
            auto parameterType = declaredType(param.type.get(), {});
            param.inferredType = parameterType;
            active.paramTypes.push_back(parameterType);
            const bool explicitUsage = param.hasExplicitUsage || param.isLinear ||
                dynamic_cast<LinearTypeAST*>(param.type.get()) != nullptr ||
                dynamic_cast<AffineTypeAST*>(param.type.get()) != nullptr;
            const auto requestedUsage = param.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage ? param.usage : defaultUsageForType(parameterType));
            const auto contract = parameterContractFor(
                parameterType, requestedUsage, explicitUsage);
            param.relation = contract.relation;
            param.usage = contract.usage;
            active.paramContracts.push_back(contract);
            active.paramNames.push_back(param.name);
            auto* captured = mSymTable.lookup(param.name);
            if (!captured) error("inline slot parameter '" + param.name + "' has no matching local binding");
            else constrain(captured->type, parameterType,
                           "inline slot parameter '" + param.name + "'");
        }
        active.structuralType = Type::makeSlot(
            active.paramTypes, TyUnit,
            active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context,
            active.paramContracts);
        mSlotScopes.back()[stmt->name] = active;
    } else {
        auto* declared = lookupSlot(stmt->name);
        if (!declared) {
            error("unknown slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = *declared;
        stmt->acceptedKind = active.acceptedKind;
        stmt->acceptedCardinality = active.acceptedCardinality;
        if (stmt->args.size() != active.paramTypes.size()) {
            error("slot '" + stmt->name + "' expects " +
                  std::to_string(active.paramTypes.size()) + " arguments, got " +
                  std::to_string(stmt->args.size()), stmt->line, stmt->col);
        }
        const size_t count = std::min(stmt->args.size(), active.paramTypes.size());
        for (size_t i = 0; i < count; ++i)
            constrain(analyzeExpr(stmt->args[i].get()), active.paramTypes[i],
                      "argument " + std::to_string(i + 1) + " of slot '" + stmt->name + "'");
    }

    stmt->structuralType = active.structuralType;
    stmt->resolvedParamNames = active.paramNames;
    const auto captures = mSymTable.visibleSymbols();
    analyzeBlock(stmt->continuation.get(), expectedReturn);

    FragmentDecl* fragment = lookupApplied(stmt->name);
    const std::vector<FragmentDecl*>* dynamicFragments = lookupDynamicApplied(stmt->name);
    if (dynamicFragments && !dynamicFragments->empty()) {
        stmt->usesDynamicDispatch = true;
        stmt->resolvedDynamicFragmentNames.clear();
        FragmentDecl* contract = nullptr;
        for (auto* candidate : *dynamicFragments) {
            if (!candidate) continue;
            if (candidate->cardinality == FragmentCardinality::Many)
                error("dynamic apply currently rejects explicitly multi-shot context '" +
                      candidate->name + "'", stmt->line, stmt->col);
            if (candidate->kind != active.acceptedKind ||
                candidate->cardinality != active.acceptedCardinality)
                error("dynamic candidate '" + candidate->name +
                      "' does not match slot '" + stmt->name + "' contract",
                      stmt->line, stmt->col);
            if (!contract) contract = candidate;
            else if (candidate->kind != contract->kind ||
                     candidate->cardinality != contract->cardinality) {
                error("all dynamic candidates must declare the same interceptor/context and once/many contract",
                      stmt->line, stmt->col);
            }
            stmt->resolvedDynamicFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
            analyzeFragmentForSlot(candidate, stmt->name, active.paramTypes,
                                   active.paramContracts, captures);
        }
        return;
    }
    if (!fragment && !active.defaultFragment.empty()) {
        fragment = selectFragment(active.defaultFragment, stmt);
    }
    if (fragment) {
        active.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
        stmt->resolvedDefaultFragmentName = active.resolvedDefaultFragmentName;
    }
    if (!fragment) return; // no binding is an identity fragment: resume once
    if (fragment->kind != active.acceptedKind ||
        fragment->cardinality != active.acceptedCardinality)
        error("fragment '" + fragment->name + "' does not match slot '" + stmt->name +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    analyzeFragmentForSlot(fragment, stmt->name, active.paramTypes,
                           active.paramContracts, captures);
}

void SemanticAnalyzer::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    auto* fragment = selectFragment(stmt->fragmentName, stmt);
    if (!fragment) return;
    stmt->resolvedFragmentName = fragment->generatedSymbolName.empty()
        ? fragment->name : fragment->generatedSymbolName;
    SlotInfo* knownSlot = nullptr;
    for (auto it = mSlotScopes.rbegin(); it != mSlotScopes.rend(); ++it) {
        auto found = it->find(stmt->slotName);
        if (found != it->end()) { knownSlot = &found->second; break; }
    }
    auto matchesContract = [](const FragmentDecl* candidate, const SlotInfo* slot) {
        return !slot || (candidate->kind == slot->acceptedKind &&
                         candidate->cardinality == slot->acceptedCardinality);
    };
    if (!matchesContract(fragment, knownSlot))
        error("fragment '" + fragment->name + "' does not match slot '" + stmt->slotName +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    if (stmt->isDynamic) {
        SlotInfo* slot = knownSlot;
        if (!slot) {
            error("dynamic apply requires a previously declared dynamic slot '" + stmt->slotName + "'",
                  stmt->line, stmt->col);
            return;
        }
        if (!slot->isDynamic) {
            error("slot '" + stmt->slotName + "' is static; declare it with `dynamic slot` before dynamic apply",
                  stmt->line, stmt->col);
            return;
        }
        auto requireRuntimeCandidate = [this, stmt](const FragmentDecl* candidate) {
            if (candidate && candidate->retention == RetentionKind::CompileTime)
                error("dynamic apply candidate '" + candidate->name +
                      "' must be declared `runtime` or `dynamic`",
                      stmt->line, stmt->col);
        };
        requireRuntimeCandidate(fragment);
        std::vector<FragmentDecl*> candidates{fragment};
        for (const auto& name : stmt->alternativeFragmentNames) {
            auto* candidate = selectFragment(name, stmt);
            if (!candidate) continue;
            requireRuntimeCandidate(candidate);
            if (!matchesContract(candidate, slot))
                error("dynamic candidate '" + candidate->name + "' does not match slot '" +
                      stmt->slotName + "' contract", stmt->line, stmt->col);
            candidates.push_back(candidate);
            stmt->resolvedAlternativeFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
        }
        if (stmt->body) {
            enterSlotScope();
            mApplyScopes.back()[stmt->slotName] = fragment;
            mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
            analyzeBlock(stmt->body.get(), expectedReturn);
            exitSlotScope();
        } else {
            mApplyScopes.back()[stmt->slotName] = fragment;
            mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
        }
        return;
    }
    if (stmt->body) {
        enterSlotScope();
        mApplyScopes.back()[stmt->slotName] = fragment;
        analyzeBlock(stmt->body.get(), expectedReturn);
        exitSlotScope();
    } else {
        mApplyScopes.back()[stmt->slotName] = fragment;
    }
}

void SemanticAnalyzer::analyzeFragmentForSlot(
    FragmentDecl* fragment, const std::string& slotName, const TypeVec& parameterTypes,
    const std::vector<luna::ownership::Contract>& parameterContracts,
    const std::unordered_map<std::string, SymbolInfo>& captures) {
    if (fragment->params.size() > parameterTypes.size()) {
        error("fragment '" + fragment->name + "' requires " +
              std::to_string(fragment->params.size()) + " parameters, but slot '" + slotName +
              " provides " + std::to_string(parameterTypes.size()));
        return;
    }

    SlotInfo context;
    context.name = slotName;
    context.paramTypes = parameterTypes;
    context.paramContracts = parameterContracts;
    context.structuralType = Type::makeSlot(
        parameterTypes, TyUnit,
        fragment->cardinality == FragmentCardinality::Many,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        parameterContracts);
    const SlotInfo* savedSlot = mCurrentFragmentSlot;
    FragmentDecl* savedFragment = mCurrentFragmentDecl;
    TypePtr savedReturnType = mCurrentReturnType;
    mCurrentFragmentSlot = &context;
    mCurrentFragmentDecl = fragment;
    mCurrentReturnType = TyUnit;

    mSymTable.enterScope();
    for (size_t i = 0; i < fragment->params.size(); ++i) {
        auto& param = fragment->params[i];
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = parameterTypes[i];
        if (param.type) constrain(declaredType(param.type.get(), {}), info.type,
                                  "parameter '" + param.name + "' of fragment '" + fragment->name + "'");
        if (i < parameterContracts.size()) {
            const luna::ownership::Contract fragmentContract{
                param.relation, param.usage};
            if (fragmentContract != parameterContracts[i]) {
                error("parameter '" + param.name + "' of fragment '" +
                      fragment->name + "' has ownership contract " +
                      std::string(luna::ownership::relationName(fragmentContract.relation)) +
                      "/" + std::string(luna::ownership::usageName(fragmentContract.usage)) +
                      ", but slot '" + slotName + "' requires " +
                      std::string(luna::ownership::relationName(
                          parameterContracts[i].relation)) +
                      "/" + std::string(luna::ownership::usageName(
                          parameterContracts[i].usage)));
            }
        }
        param.inferredType = info.type;
        mSymTable.define(param.name, info);
    }
    for (const auto& [name, info] : captures) {
        if (!mSymTable.hasInCurrentScope(name)) mSymTable.define(name, info);
    }
    analyzeBlock(fragment->body.get(), TyUnit);
    mSymTable.exitScope();
    mCurrentReturnType = savedReturnType;

    struct ControlPaths {
        std::set<int> active{0};
        bool aborted = false;
        bool returned = false;
        bool abortAfterResume = false;
    };
    std::function<ControlPaths(const BlockStmt*, const std::set<int>&)> analyzePaths;
    std::function<ControlPaths(const Stmt*, const std::set<int>&)> analyzeStmtPaths;
    auto mergePaths = [](ControlPaths left, const ControlPaths& right) {
        left.active.insert(right.active.begin(), right.active.end());
        left.aborted = left.aborted || right.aborted;
        left.returned = left.returned || right.returned;
        left.abortAfterResume = left.abortAfterResume || right.abortAfterResume;
        return left;
    };
    analyzeStmtPaths = [&](const Stmt* stmt, const std::set<int>& incoming) -> ControlPaths {
        ControlPaths out; out.active = incoming;
        if (!stmt) return out;
        if (dynamic_cast<const ResumeStmt*>(stmt)) {
            out.active.clear();
            for (int count : incoming) out.active.insert(std::min(count + 1, 2));
            return out;
        }
        if (dynamic_cast<const AbortStmt*>(stmt)) {
            for (int count : incoming) if (count > 0) out.abortAfterResume = true;
            out.active.clear(); out.aborted = true; return out;
        }
        if (dynamic_cast<const ReturnStmt*>(stmt)) {
            out.active.clear(); out.returned = true; return out;
        }
        if (auto* block = dynamic_cast<const BlockStmt*>(stmt))
            return analyzePaths(block, incoming);
        if (auto* branch = dynamic_cast<const IfStmt*>(stmt)) {
            ControlPaths thenPaths = analyzePaths(branch->thenBlock.get(), incoming);
            ControlPaths elsePaths;
            elsePaths.active = incoming;
            if (branch->elseBranch) elsePaths = analyzeStmtPaths(branch->elseBranch.get(), incoming);
            return mergePaths(std::move(thenPaths), elsePaths);
        }
        if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
            ControlPaths paths;
            paths.active.clear();
            for (const auto& arm : match->arms)
                paths = mergePaths(
                    std::move(paths),
                    analyzePaths(arm.body.get(), incoming));
            return paths;
        }
        if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            // A loop may execute zero times or repeat. Any resume in its body
            // therefore makes a once-context path potentially multi-shot.
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        return out;
    };
    analyzePaths = [&](const BlockStmt* block, const std::set<int>& incoming) {
        ControlPaths paths; paths.active = incoming;
        if (!block) return paths;
        for (const auto& statement : block->stmts) {
            if (paths.active.empty()) break;
            ControlPaths next = analyzeStmtPaths(statement.get(), paths.active);
            paths.active = std::move(next.active);
            paths.aborted = paths.aborted || next.aborted;
            paths.returned = paths.returned || next.returned;
            paths.abortAfterResume = paths.abortAfterResume || next.abortAfterResume;
        }
        return paths;
    };
    const ControlPaths control = analyzePaths(fragment->body.get(), {0});
    const bool isMany = fragment->cardinality == FragmentCardinality::Many;
    if (!isMany && control.abortAfterResume)
        error("single-shot context '" + fragment->name +
              "' cannot abort after resume(); the continuation has already been consumed");
    if (fragment->kind == FragmentKind::Context) {
        for (int resumes : control.active) {
            if (!isMany && resumes > 1) {
                error("single-shot context '" + fragment->name +
                      "' may resume its continuation at most once; a path with no `resume()` is an implicit abort");
                break;
            }
        }
    }
    // A fragment may leave parameter types implicit until it is bound to a
    // typed slot. Rebuild its own declaration shape from the now-resolved
    // prefix rather than copying the entire slot interface into the fragment.
    TypeVec fragmentParameterTypes;
    std::vector<luna::ownership::Contract> fragmentParameterContracts;
    for (const auto& parameter : fragment->params) {
        fragmentParameterTypes.push_back(parameter.inferredType);
        fragmentParameterContracts.push_back(
            {parameter.relation, parameter.usage});
    }
    fragment->structuralType = Type::makeFragment(
        std::move(fragmentParameterTypes), TyUnit, isMany,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        std::move(fragmentParameterContracts));
    if (isMany) {
        for (const auto& [name, info] : captures) {
            if (info.isLinear) {
                error("fragment '" + fragment->name + "' may resume slot '" + slotName +
                      "' more than once, but captured linear value '" + name + "' is not replayable");
            }
        }
    }

    mCurrentFragmentSlot = savedSlot;
    mCurrentFragmentDecl = savedFragment;
}

void SemanticAnalyzer::enterSlotScope() {
    mSlotScopes.emplace_back();
    mApplyScopes.emplace_back();
    mDynamicApplyScopes.emplace_back();
}

void SemanticAnalyzer::exitSlotScope() {
    if (mSlotScopes.size() > 1) mSlotScopes.pop_back();
    if (mApplyScopes.size() > 1) mApplyScopes.pop_back();
    if (mDynamicApplyScopes.size() > 1) mDynamicApplyScopes.pop_back();
}

FragmentDecl* SemanticAnalyzer::selectFragment(
    const std::string& name, const ASTNode* useSite) {
    auto fragment = mFragments.find(sourceDeclarationKey(name));
    if (fragment != mFragments.end()) return fragment->second;
    error("unknown fragment '" + name + "'", useSite->line, useSite->col);
    return nullptr;
}

std::string SemanticAnalyzer::traitIdentity(const TraitDecl* trait) const {
    if (!trait) return "";
    const auto symbol = trait->generatedSymbolName.empty()
        ? trait->name : trait->generatedSymbolName;
    return nominalDeclarationIdentity(mProgram, "trait", symbol, trait);
}

std::string SemanticAnalyzer::typeIdentity(const TypePtr& type) const {
    if (!type) return "?";
    return luna::types::typeId(type).value;
}

bool SemanticAnalyzer::satisfiesTrait(const std::string& traitId, const TypePtr& type) const {
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

std::string SemanticAnalyzer::resolveTraitRef(TraitRef& trait, const ASTNode* useSite) {
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
    return trait.resolvedTraitId;
}

// ─── Statement analysis ────────────────────────────────────────────

TypePtr SemanticAnalyzer::analyzeStmt(Stmt* stmt, TypePtr expectedReturn) {
    setDiagnosticLocation(stmt);
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) return analyzeBlock(bs, expectedReturn);

    // A device kernel is deliberately a small DeviceMemory-only sublanguage.
    // The source-level continuation constructs are host control flow: lowering
    // them into SIMT code would require a per-lane continuation runtime and
    // could not preserve their resume semantics. Likewise, awaiting, freeing,
    // and host allocation are host-side synchronization/resource effects.
    // Keep this boundary in semantic analysis, before code generation can
    // accidentally inline a fragment or emit a host runtime call into HSACO.
    if (mInKernel) {
        const char* construct = nullptr;
        if (dynamic_cast<SlotDeclStmt*>(stmt)) construct = "slot declaration";
        else if (dynamic_cast<SlotInvokeStmt*>(stmt)) construct = "slot invocation";
        else if (dynamic_cast<ApplyStmt*>(stmt)) construct = "apply binding";
        else if (dynamic_cast<ResumeStmt*>(stmt)) construct = "resume()";
        else if (dynamic_cast<AbortStmt*>(stmt)) construct = "abort()";
        else if (dynamic_cast<AwaitStmt*>(stmt)) construct = "await";
        else if (dynamic_cast<FreeStmt*>(stmt)) construct = "free";
        if (construct) {
            error("kernel body may not use " + std::string(construct) +
                  "; device kernels support only DeviceMemory operations and structured scalar control flow",
                  stmt->line, stmt->col);
            return TyUnit;
        }
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        analyzeSlotDecl(slot);
        return TyUnit;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        analyzeSlotInvoke(slot, expectedReturn);
        return TyUnit;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        analyzeApply(apply, expectedReturn);
        return TyUnit;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mCurrentFragmentDecl)
            error("`resume()` may only appear inside a fragment", stmt->line, stmt->col);
        else if (mCurrentFragmentDecl &&
                 mCurrentFragmentDecl->kind == FragmentKind::Interceptor)
            error("`resume()` is not allowed in an interceptor; normal completion forwards automatically",
                  stmt->line, stmt->col);
        return TyUnit;
    }
    if (dynamic_cast<AbortStmt*>(stmt)) {
        if (!mCurrentFragmentDecl)
            error("`abort()` may only appear inside an interceptor or context", stmt->line, stmt->col);
        return TyUnit;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        TypePtr eventType = resolved(analyzeExpr(await->event.get()));
        if (eventType->kind != TypeKind::Event)
            error("`await` requires a launch event, got " + eventType->toString(),
                  await->line, await->col);
        return TyUnit;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt)) {
        TypePtr rhsType = analyzeExpr(ls->initializer.get());

        // Check if rhs is a HeapAllocExpr — mark as heap allocated
        bool isHeap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()) != nullptr;

        TypePtr declaredType;
        if (ls->typeAnnotation) {
            std::unordered_map<std::string, TypePtr> bindings;
            declaredType = this->declaredType(ls->typeAnnotation.get(), bindings);
            if (!(dynamic_cast<StringLiteralExpr*>(ls->initializer.get()) &&
                  resolved(declaredType)->kind == TypeKind::CStr))
                constrain(rhsType, declaredType, "let binding '" + ls->name + "'");
        } else {
            declaredType = rhsType; // auto inference
        }

        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = declaredType;
        info.isConst = ls->isConst;
        info.isHeapAllocated = isHeap || declaredType->isHeapType();
        if (auto* reflected =
                dynamic_cast<CallExpr*>(ls->initializer.get());
            reflected && !reflected->compileTimeDeclarationId.empty())
            info.compileTimeDeclarationId =
                reflected->compileTimeDeclarationId;
        auto finalType = resolved(declaredType);
        ls->inferredType = finalType;
        ls->materializesIteratorRecipe = false;
        ls->materializedIteratorOwnsSource = false;
        ls->materializedIteratorSourceType.reset();
        if (finalType->kind == TypeKind::Iterator) {
            CallExpr* base = nullptr;
            std::function<void(Expr*)> findBase =
                [&](Expr* expression) {
                    auto* call =
                        dynamic_cast<CallExpr*>(
                            expression);
                    if (!call) return;
                    if (call->iteratorOp ==
                            IteratorOp::Range ||
                        call->iteratorOp ==
                            IteratorOp::Iter ||
                        call->iteratorOp ==
                            IteratorOp::IterMut ||
                        call->iteratorOp ==
                            IteratorOp::IntoIter) {
                        base = call;
                        return;
                    }
                    auto* member =
                        dynamic_cast<FieldAccessExpr*>(
                            call->callee.get());
                    if (member)
                        findBase(member->object.get());
                };
            findBase(ls->initializer.get());
            bool supported = base != nullptr;
            if (base && base->iteratorOp !=
                            IteratorOp::Range) {
                auto* member =
                    dynamic_cast<FieldAccessExpr*>(
                        base->callee.get());
                auto* source = member
                    ? dynamic_cast<IdentifierExpr*>(
                          member->object.get())
                    : nullptr;
                if (!source) {
                    error("materialized iterator recipe "
                          "requires a local array or slice "
                          "source",
                          ls->line, ls->col);
                    supported = false;
                }
                TypePtr sourceType;
                if (base->resultType &&
                    !base->resultType->typeArgs.empty())
                    sourceType = resolved(
                        base->resultType->
                            typeArgs.front());
                if (base->iteratorOp ==
                        IteratorOp::IntoIter &&
                    sourceType &&
                    sourceType->kind == TypeKind::Array &&
                    sourceType->inner &&
                    defaultUsageForType(
                        sourceType->inner) !=
                        luna::ownership::Usage::Copy) {
                    if (luna::ownership::mustConsume(
                            defaultUsageForType(sourceType))) {
                        error("materialized iterator recipe cannot hide a "
                              "linear source obligation",
                              ls->line, ls->col);
                        supported = false;
                    } else {
                        ls->materializedIteratorOwnsSource = true;
                        ls->materializedIteratorSourceType =
                            sourceType;
                    }
                }
            }
            if (!base) {
                error("an iterator binding cannot be "
                      "re-materialized from another recipe; "
                      "consume the existing binding directly",
                      ls->line, ls->col);
            }
            ls->materializesIteratorRecipe = supported;
        }
        if (ls->isLinear || dynamic_cast<LinearTypeAST*>(ls->typeAnnotation.get()))
            ls->usage = luna::ownership::Usage::Linear;
        else if (dynamic_cast<AffineTypeAST*>(ls->typeAnnotation.get()))
            ls->usage = luna::ownership::Usage::Affine;
        else if (ls->usage == luna::ownership::Usage::Copy)
            ls->usage = defaultUsageForType(finalType);
        info.usage = ls->usage;
        info.isLinear = info.usage == luna::ownership::Usage::Linear;
        mSymTable.define(ls->name, info);
        // Device resources and completion events carry ownership semantics.
        // Preserve their inferred type on the AST so the ownership and codegen
        // passes observe the same ABI even without an explicit annotation.
        if (!ls->typeAnnotation &&
            (finalType->kind == TypeKind::DeviceBuffer || finalType->kind == TypeKind::Event ||
             finalType->kind == TypeKind::Array || finalType->kind == TypeKind::Slice))
            ls->typeAnnotation = typeToAST(finalType);
        if (ls->isConst) {
            auto value = evaluateConstExpr(ls->initializer.get());
            if (!value) {
                error("const binding '" + ls->name + "' is not a compile-time expression");
            } else {
                defineConst(ls->name, *value);
            }
        }
        return TyUnit;
    }
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        mSawReturn = true;
        if (rs->value) {
            TypePtr valueType = analyzeExpr(rs->value.get());
            if (!(dynamic_cast<StringLiteralExpr*>(rs->value.get()) &&
                  resolved(mCurrentReturnType)->kind == TypeKind::CStr))
                constrain(valueType, mCurrentReturnType, "return statement");
            luna::ownership::Usage returningUsage = luna::ownership::Usage::Copy;
            if (auto* call = dynamic_cast<CallExpr*>(rs->value.get())) {
                returningUsage = call->returnsLinear
                    ? luna::ownership::Usage::Linear : call->returnUsage;
            } else if (auto* id = dynamic_cast<IdentifierExpr*>(rs->value.get())) {
                if (auto* symbol = mSymTable.lookup(id->name))
                    returningUsage = symbol->isLinear
                        ? luna::ownership::Usage::Linear : symbol->usage;
            }
            if (returningUsage == luna::ownership::Usage::Linear &&
                mCurrentFunctionReturnUsage != luna::ownership::Usage::Linear) {
                error("returning a linear value requires a linear function return contract",
                      rs->line, rs->col);
            } else if (returningUsage != luna::ownership::Usage::Linear &&
                       mCurrentFunctionReturnUsage == luna::ownership::Usage::Linear) {
                error("function declared with `-> linear raw<T>` must return an owning value",
                      rs->line, rs->col);
            } else if (returningUsage == luna::ownership::Usage::Affine &&
                       mCurrentFunctionReturnUsage == luna::ownership::Usage::Copy) {
                error("returning an affine value requires an affine function return contract",
                      rs->line, rs->col);
            }
        } else {
            constrain(TyUnit, mCurrentReturnType, "unit return statement");
        }
        return TyUnit;
    }
    if (auto* is = dynamic_cast<IfStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(is->cond.get());
        requireBool(condType, "if condition");
        analyzeBlock(is->thenBlock.get(), expectedReturn);
        if (is->elseBranch) analyzeStmt(is->elseBranch.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt)) {
        TypePtr matched = resolved(analyzeExpr(match->scrutinee.get()));
        match->matchedType = matched;

        struct VariantView {
            std::string name;
            size_t physicalIndex = 0;
            TypeVec fields;
        };
        std::vector<VariantView> variants;
        if (matched && matched->kind == TypeKind::Enum) {
            for (size_t index = 0; index < matched->variants.size(); ++index)
                variants.push_back({
                    matched->variants[index].name, index,
                    matched->variants[index].fields});
        } else if (matched && matched->kind == TypeKind::Result &&
                   matched->typeArgs.size() == 2) {
            // Result's frozen ABI uses false/0 for Err and true/1 for Ok.
            variants.push_back({"Err", 0, {matched->typeArgs[1]}});
            variants.push_back({"Ok", 1, {matched->typeArgs[0]}});
        } else {
            error("match requires an enum or Result value",
                  match->line, match->col);
            return TyUnit;
        }

        std::unordered_set<std::string> seenVariants;
        for (auto& arm : match->arms) {
            const auto selected = std::find_if(
                variants.begin(), variants.end(),
                [&](const VariantView& variant) {
                    return variant.name == arm.variantName;
                });
            if (selected == variants.end()) {
                error("unknown variant '" + arm.variantName +
                      "' in match on '" + matched->toString() + "'",
                      arm.line, arm.col);
                continue;
            }
            std::string qualifierName = arm.typeQualifier;
            const size_t qualifierSeparator =
                qualifierName.rfind("::");
            if (qualifierSeparator != std::string::npos)
                qualifierName =
                    qualifierName.substr(qualifierSeparator + 2);
            if (!arm.typeQualifier.empty() &&
                qualifierName != matched->name &&
                arm.typeQualifier != matched->toString()) {
                error("match pattern qualifier '" + arm.typeQualifier +
                      "' does not name matched type '" +
                      matched->toString() + "'", arm.line, arm.col);
            }
            if (!seenVariants.insert(arm.variantName).second)
                error("duplicate match arm for variant '" +
                      arm.variantName + "'", arm.line, arm.col);
            if (arm.bindings.size() != selected->fields.size()) {
                error("variant '" + arm.variantName + "' expects " +
                      std::to_string(selected->fields.size()) +
                      " payload binding(s), got " +
                      std::to_string(arm.bindings.size()),
                      arm.line, arm.col);
            }
            arm.variantIndex = selected->physicalIndex;
            arm.bindingTypes = selected->fields;

            mSymTable.enterScope();
            std::unordered_set<std::string> seenBindings;
            const size_t count =
                std::min(arm.bindings.size(), arm.bindingTypes.size());
            for (size_t index = 0; index < count; ++index) {
                if (!seenBindings.insert(arm.bindings[index]).second) {
                    error("duplicate payload binding '" +
                          arm.bindings[index] + "'", arm.line, arm.col);
                    continue;
                }
                SymbolInfo binding;
                binding.kind = SymbolKind::Variable;
                binding.type = arm.bindingTypes[index];
                binding.usage =
                    defaultUsageForType(arm.bindingTypes[index]);
                binding.isLinear =
                    binding.usage == luna::ownership::Usage::Linear;
                mSymTable.define(arm.bindings[index], binding);
            }
            analyzeBlock(arm.body.get(), expectedReturn);
            mSymTable.exitScope();
        }

        if (seenVariants.size() != variants.size()) {
            std::string missing;
            for (const auto& variant : variants) {
                if (seenVariants.count(variant.name)) continue;
                if (!missing.empty()) missing += ", ";
                missing += variant.name;
            }
            if (matched->kind == TypeKind::Result) {
                error("Result match must contain exactly one `Ok` arm "
                      "and one `Err` arm", match->line, match->col);
            } else {
                error("match on '" + matched->toString() +
                      "' is not exhaustive; missing variant(s): " +
                      missing, match->line, match->col);
            }
        }
        return TyUnit;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(ws->cond.get());
        requireBool(condType, "while condition");
        analyzeBlock(ws->body.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt)) {
        TypePtr iterable = resolved(analyzeExpr(fs->iterable.get()));
        TypePtr element = TyI32;
        fs->protocolNextSymbol.clear();
        fs->protocolIteratorType.reset();
        fs->protocolOptionType.reset();
        fs->protocolIntoSymbol.clear();
        fs->protocolInputType.reset();
        fs->protocolStateName.clear();
        fs->protocolStateNeedsCleanup = false;
        fs->recipeStateName.clear();
        fs->recipeSourceType.reset();
        const auto markMoveOnlyRecipe =
            [&](Expr* source,
                const TypePtr& sourceType) {
                if (!sourceType ||
                    sourceType->kind !=
                        TypeKind::Array ||
                    defaultUsageForType(
                        sourceType->inner) ==
                        luna::ownership::Usage::Copy)
                    return;
                if (!dynamic_cast<IdentifierExpr*>(
                        source)) {
                    error("move-only consuming array iteration "
                          "currently requires a local source binding",
                          fs->line, fs->col);
                    return;
                }
                fs->recipeSourceType = sourceType;
                fs->recipeStateName =
                    "$for.recipe." +
                    std::to_string(fs->line) + "." +
                    std::to_string(fs->col) + "." +
                    fs->varName;
            };
        if (iterable->kind == TypeKind::DeclarationView)
            element = Type::makeDeclarationRef(iterable->inner);
        else if (iterable->kind == TypeKind::MetadataView)
            element = iterable->inner;
        else if (iterable->kind == TypeKind::Iterator) {
            element = iterable->inner;
            std::function<void(Expr*)> findConsumingArray =
                [&](Expr* expression) {
                    auto* call =
                        dynamic_cast<CallExpr*>(
                            expression);
                    if (!call) return;
                    auto* member =
                        dynamic_cast<FieldAccessExpr*>(
                            call->callee.get());
                    if (!member) return;
                    if (call->iteratorOp ==
                        IteratorOp::IntoIter) {
                        TypePtr sourceType;
                        if (call->resultType &&
                            !call->resultType->
                                typeArgs.empty())
                            sourceType = resolved(
                                call->resultType->
                                    typeArgs.front());
                        markMoveOnlyRecipe(
                            member->object.get(),
                            sourceType);
                        return;
                    }
                    findConsumingArray(
                        member->object.get());
                };
            findConsumingArray(
                fs->iterable.get());
        }
        else if (iterable->kind == TypeKind::Slice)
            element = Type::makeReference(iterable->inner);
        else if (iterable->kind == TypeKind::Array) {
            element = iterable->inner;
            markMoveOnlyRecipe(
                fs->iterable.get(), iterable);
        }
        else {
            // User-defined loops are a closed Core protocol, not structural
            // "has a next method" duck typing.  This preserves coherence and
            // leaves compiler iterator recipes free to use their fused path.
            TraitDecl* iteratorTrait = nullptr;
            auto coreIterator =
                mTraits.find(luna::sysmeta::IteratorTraitId);
            if (coreIterator != mTraits.end())
                iteratorTrait = coreIterator->second;
            const std::string iteratorTraitId =
                traitIdentity(iteratorTrait);
            FunctionDecl* next = nullptr;
            FunctionDecl* into = nullptr;
            TypePtr iteratorStateType = iterable;
            TypePtr declaredIntoItem;
            if (!iteratorTraitId.empty()) {
                auto traitImpls = mImpls.find(iteratorTraitId);
                if (traitImpls != mImpls.end()) {
                    auto implementation = traitImpls->second.find(
                        typeIdentity(iterable));
                    if (implementation != traitImpls->second.end()) {
                        auto method =
                            implementation->second.find("next");
                        if (method !=
                            implementation->second.end())
                            next = method->second;
                    }
                }
            }

            if (!next) {
                TraitDecl* intoIteratorTrait = nullptr;
                auto coreIntoIterator = mTraits.find(
                    luna::sysmeta::IntoIteratorTraitId);
                if (coreIntoIterator != mTraits.end())
                    intoIteratorTrait =
                        coreIntoIterator->second;
                const std::string intoTraitId =
                    traitIdentity(intoIteratorTrait);
                if (!intoTraitId.empty()) {
                    auto traitImpls =
                        mImpls.find(intoTraitId);
                    if (traitImpls != mImpls.end()) {
                        auto implementation =
                            traitImpls->second.find(
                                typeIdentity(iterable));
                        if (implementation !=
                            traitImpls->second.end()) {
                            auto method =
                                implementation->second.find(
                                    "into_iter");
                            if (method !=
                                implementation->second.end())
                                into = method->second;
                        }
                    }
                }
                if (into) {
                    if (into->params.size() != 1 ||
                        !luna::types::sameType(
                            resolved(into->params.front().
                                inferredType),
                            iterable) ||
                        into->params.front().relation !=
                            luna::ownership::Relation::Owned) {
                        error("Core IntoIterator::into_iter must "
                              "take ownership of exactly one '" +
                              iterable->toString() +
                              "' value", into->line, into->col);
                    }
                    iteratorStateType = resolved(
                        into->inferredReturnType);

                    // Recover the declared Item/Iter association from the
                    // exact coherent impl.  Method return type alone carries
                    // Iter but not the associated Item witness.
                    for (const auto& declaration :
                         mProgram->declarations) {
                        auto* implementation =
                            dynamic_cast<ImplDecl*>(
                                declaration.get());
                        if (!implementation ||
                            implementation->trait.
                                resolvedTraitId !=
                                intoTraitId ||
                            implementation->
                                resolvedTargetTypeId !=
                                typeIdentity(iterable))
                            continue;
                        if (implementation->trait.
                                resolvedTypeArgs.size() ==
                            2) {
                            declaredIntoItem = resolved(
                                implementation->trait.
                                    resolvedTypeArgs[0]);
                            TypePtr declaredIterator =
                                resolved(
                                    implementation->trait.
                                        resolvedTypeArgs[1]);
                            if (!luna::types::sameType(
                                    declaredIterator,
                                    iteratorStateType))
                                error("Core IntoIterator::into_iter "
                                      "return type disagrees with "
                                      "its Iter argument",
                                      into->line, into->col);
                        }
                        break;
                    }

                    if (!iteratorTraitId.empty()) {
                        auto traitImpls =
                            mImpls.find(iteratorTraitId);
                        if (traitImpls != mImpls.end()) {
                            auto implementation =
                                traitImpls->second.find(
                                    typeIdentity(
                                        iteratorStateType));
                            if (implementation !=
                                traitImpls->second.end()) {
                                auto method =
                                    implementation->second.find(
                                        "next");
                                if (method !=
                                    implementation->second.end())
                                    next = method->second;
                            }
                        }
                    }
                    if (!next)
                        error("Core IntoIterator for type '" +
                              iterable->toString() +
                              "' returns '" +
                              iteratorStateType->toString() +
                              "', which does not implement "
                              "core::iter::Iterator",
                              fs->line, fs->col);
                }
            }

            bool validProtocol = next != nullptr;
            if (!next) {
                if (!into)
                    error("for-loop type '" +
                          iterable->toString() +
                          "' implements neither "
                          "core::iter::Iterator nor "
                          "core::iter::IntoIterator",
                          fs->line, fs->col);
            } else if (!dynamic_cast<IdentifierExpr*>(
                           fs->iterable.get())) {
                error("Core Iterator/IntoIterator for-loop source must "
                      "currently be a local binding",
                      fs->line, fs->col);
                validProtocol = false;
            }

            if (next) {
                if (next->params.size() != 1) {
                    error("Core Iterator::next must have exactly one "
                          "receiver parameter", next->line, next->col);
                    validProtocol = false;
                } else {
                    TypePtr receiver = resolved(
                        next->params.front().inferredType);
                    if (!receiver ||
                        receiver->kind != TypeKind::Reference ||
                        !receiver->isMutable ||
                        !luna::types::sameType(
                            resolved(receiver->inner),
                            iteratorStateType)) {
                        error("Core Iterator::next receiver must be '&mut " +
                              iteratorStateType->toString() + "'",
                              next->line, next->col);
                        validProtocol = false;
                    }
                }

                TypePtr option = resolved(
                    next->inferredReturnType);
                TypePtr coreOption;
                auto optionDeclaration =
                    mDeclaredTypes.find(
                        luna::sysmeta::OptionTypeId);
                if (optionDeclaration !=
                    mDeclaredTypes.end())
                    coreOption =
                        resolved(optionDeclaration->second);
                const bool isCoreOption =
                    option && coreOption &&
                    option->kind == TypeKind::Enum &&
                    !option->nominalId.empty() &&
                    option->nominalId ==
                        coreOption->nominalId;
                size_t noneIndex = 0;
                size_t someIndex = 0;
                bool foundNone = false;
                bool foundSome = false;
                if (isCoreOption) {
                    for (size_t index = 0;
                         index < option->variants.size();
                         ++index) {
                        const auto& variant =
                            option->variants[index];
                        if (variant.name == "None" &&
                            variant.fields.empty()) {
                            noneIndex = index;
                            foundNone = true;
                        } else if (
                            variant.name == "Some" &&
                            variant.fields.size() == 1) {
                            someIndex = index;
                            element =
                                resolved(variant.fields.front());
                            foundSome = true;
                        }
                    }
                }
                if (foundSome && declaredIntoItem &&
                    !luna::types::sameType(
                        element, declaredIntoItem)) {
                    error("Core IntoIterator Item type '" +
                          declaredIntoItem->toString() +
                          "' disagrees with Iterator item type '" +
                          element->toString() + "'",
                          fs->line, fs->col);
                    validProtocol = false;
                }
                if (!isCoreOption || !foundNone ||
                    !foundSome) {
                    error("Core Iterator::next must return "
                          "core::option::Option<Item> (resolved '" +
                          (option
                              ? option->toString()
                              : std::string("?")) +
                          "' with nominal identity '" +
                          (option
                              ? option->nominalId
                              : std::string{}) +
                          "', expected '" +
                          (coreOption
                              ? coreOption->nominalId
                              : std::string{}) + "')",
                          next->line, next->col);
                    validProtocol = false;
                }
                if (validProtocol) {
                    fs->protocolNextSymbol =
                        next->generatedSymbolName.empty()
                            ? next->name
                            : next->generatedSymbolName;
                    fs->protocolIteratorType =
                        iteratorStateType;
                    fs->protocolOptionType = option;
                    fs->protocolNoneVariant = noneIndex;
                    fs->protocolSomeVariant = someIndex;
                    if (into) {
                        fs->protocolIntoSymbol =
                            into->generatedSymbolName.empty()
                                ? into->name
                                : into->generatedSymbolName;
                        fs->protocolInputType = iterable;
                        fs->protocolStateName =
                            "$for.iterator." +
                            std::to_string(fs->line) + "." +
                            std::to_string(fs->col) + "." +
                            fs->varName;
                    }
                }
            }
        }
        fs->elementType = element;
        mSymTable.enterScope();
        SymbolInfo vi;
        vi.kind = SymbolKind::Variable;
        vi.type = element;
        vi.usage = defaultUsageForType(element);
        vi.isLinear = vi.usage == luna::ownership::Usage::Linear;
        mSymTable.define(fs->varName, vi);
        analyzeBlock(fs->body.get(), expectedReturn);
        mSymTable.exitScope();
        return TyUnit;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) {
        return analyzeExpr(es->expr.get());
    }
    if (auto* fs = dynamic_cast<FreeStmt*>(stmt)) {
        analyzeExpr(fs->operand.get());
        return TyUnit;
    }
    return TyUnit;
}

TypePtr SemanticAnalyzer::analyzeBlock(BlockStmt* block, TypePtr expectedReturn) {
    mSymTable.enterScope();
    enterConstScope();
    enterSlotScope();
    for (auto& stmt : block->stmts) {
        analyzeStmt(stmt.get(), expectedReturn);
    }
    mSymTable.exitScope();
    exitConstScope();
    exitSlotScope();
    return TyUnit;
}

bool SemanticAnalyzer::statementAlwaysReturns(const Stmt* stmt) const {
    if (!stmt) return false;
    if (dynamic_cast<const ReturnStmt*>(stmt)) return true;
    if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        if (auto* call = dynamic_cast<const CallExpr*>(expression->expr.get())) {
            if ((call->resultType &&
                 call->resultType->kind == TypeKind::Never) ||
                (call->intrinsicType &&
                 call->intrinsicType->kind == TypeKind::Never))
                return true;
        }
    }
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) return blockAlwaysReturns(block);
    if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        return conditional->elseBranch &&
               blockAlwaysReturns(conditional->thenBlock.get()) &&
               statementAlwaysReturns(conditional->elseBranch.get());
    }
    if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        return !match->arms.empty() &&
               std::all_of(match->arms.begin(), match->arms.end(),
                           [&](const MatchArm& arm) {
                               return blockAlwaysReturns(arm.body.get());
                           });
    }
    if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt))
        return apply->body && blockAlwaysReturns(apply->body.get());
    return false;
}

bool SemanticAnalyzer::blockAlwaysReturns(const BlockStmt* block) const {
    if (!block) return false;
    for (const auto& stmt : block->stmts) {
        if (statementAlwaysReturns(stmt.get())) return true;
    }
    return false;
}

// ─── Expression analysis ───────────────────────────────────────────

TypePtr SemanticAnalyzer::analyzeExpr(Expr* expr) {
    setDiagnosticLocation(expr);
    if (dynamic_cast<IntLiteralExpr*>(expr)) return TyI32;
    if (dynamic_cast<FloatLiteralExpr*>(expr)) return TyF64;
    if (dynamic_cast<StringLiteralExpr*>(expr)) return TyString;
    if (dynamic_cast<BoolLiteralExpr*>(expr)) return TyBool;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        const std::string declarationKey = sourceDeclarationKey(id->name);
        auto family = mFunctionFamilies.find(declarationKey);
        if (family != mFunctionFamilies.end() && family->second.size() > 1) {
            error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", id->line, id->col);
            return TyUnknown;
        }
        auto* sym = lookupSymbol(id->name);
        if (!sym) {
            error("undefined name '" + id->name + "'", id->line, id->col);
            return TyUnknown;
        }
        if (sym->kind == SymbolKind::Function)
            return Type::makeFunction(sym->paramTypes,
                                      sym->returnType ? sym->returnType : TyUnit,
                                      sym->paramContracts,
                                      {luna::ownership::Relation::Owned,
                                       sym->returnUsage});
        return sym->type ? resolved(sym->type) : TyUnknown;
    }
    if (auto* selection = dynamic_cast<SelectExpr*>(expr))
        return analyzeSelect(selection);
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        TypePtr lhsType = analyzeExpr(bin->lhs.get());
        TypePtr rhsType = analyzeExpr(bin->rhs.get());
        switch (bin->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                requireNumeric(lhsType, "left operand of arithmetic expression");
                requireNumeric(rhsType, "right operand of arithmetic expression");
                constrain(lhsType, rhsType, "arithmetic operands");
                return resolved(lhsType);
            case TokenKind::Ampersand:
            case TokenKind::BitOr:
            case TokenKind::BitXor:
                requireInteger(lhsType, "left operand of bitwise expression");
                requireInteger(rhsType, "right operand of bitwise expression");
                if ((resolved(lhsType)->kind == TypeKind::InferenceVar || isIntegerType(resolved(lhsType))) &&
                    (resolved(rhsType)->kind == TypeKind::InferenceVar || isIntegerType(resolved(rhsType))))
                    constrain(lhsType, rhsType, "bitwise operands");
                return resolved(lhsType);
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                requireInteger(lhsType, "left operand of shift expression");
                requireInteger(rhsType, "shift count");
                return resolved(lhsType);
            case TokenKind::EqEq:
            case TokenKind::Neq:
                constrain(lhsType, rhsType, "equality operands");
                return TyBool;
            case TokenKind::Lt:
            case TokenKind::LtEq:
            case TokenKind::Gt:
            case TokenKind::GtEq:
                requireNumeric(lhsType, "left operand of comparison expression");
                requireNumeric(rhsType, "right operand of comparison expression");
                constrain(lhsType, rhsType, "comparison operands");
                return TyBool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                requireBool(lhsType, "left operand of logical expression");
                requireBool(rhsType, "right operand of logical expression");
                return TyBool;
            default: return TyUnknown;
        }
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        TypePtr opType = analyzeExpr(un->operand.get());
        switch (un->op) {
            case TokenKind::Minus:
                requireNumeric(opType, "unary '-' operand");
                return opType;
            case TokenKind::Not:
                requireBool(opType, "'!' operand");
                return TyBool;
            case TokenKind::Tilde:
                requireInteger(opType, "'~' operand");
                return opType;
            case TokenKind::Star: {
                auto resolvedOp = resolved(opType);
                if (resolvedOp->kind == TypeKind::InferenceVar) {
                    auto inner = mConstraints.fresh();
                    constrain(opType, Type::makeReference(inner), "dereference operand");
                    return inner;
                }
                if (resolvedOp->kind == TypeKind::Reference) return resolvedOp->inner;
                error("Cannot dereference non-reference type");
                return TyUnknown;
            }
            default: return TyUnknown;
        }
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr)) {
        auto nominalIt = mDeclaredTypes.find(sourceDeclarationKey(variant->typeName));
        if (nominalIt == mDeclaredTypes.end() ||
            nominalIt->second->kind != TypeKind::Enum) {
            error("'" + variant->typeName + "' is not an enum type");
            return TyUnknown;
        }

        TypeVec typeArgs;
        for (auto& arg : variant->typeArgs)
            typeArgs.push_back(resolveTypeAST(arg.get(), {}));
        if (typeArgs.empty()) {
            for (size_t i = 0; i < nominalIt->second->typeParams.size(); ++i)
                typeArgs.push_back(mConstraints.fresh());
        } else if (typeArgs.size() != nominalIt->second->typeParams.size()) {
            error("Enum '" + variant->typeName + "' expects " +
                  std::to_string(nominalIt->second->typeParams.size()) +
                  " type arguments");
        }

        auto constructed = instantiateNominal(nominalIt->second, typeArgs);
        variant->constructedType = constructed;
        mInferenceRoots.emplace_back(constructed,
                                     "type arguments of '" + variant->typeName +
                                     "::" + variant->variantName + "'");
        const TypeVariant* selected = nullptr;
        for (auto& candidate : constructed->variants) {
            if (candidate.name == variant->variantName) {
                selected = &candidate;
                break;
            }
        }
        if (!selected) {
            error("Enum '" + variant->typeName + "' has no variant '" +
                  variant->variantName + "'");
            return TyUnknown;
        }
        if (selected->fields.size() != variant->args.size()) {
            error("Variant '" + variant->variantName + "' expects " +
                  std::to_string(selected->fields.size()) + " arguments");
            return constructed;
        }
        for (size_t i = 0; i < variant->args.size(); ++i) {
            TypePtr actual = analyzeExpr(variant->args[i].get());
            const TypePtr expected = resolved(selected->fields[i]);
            // Literals are representationally polymorphic at a statically
            // known enum field, matching call and FFI argument behavior.
            if (dynamic_cast<IntLiteralExpr*>(
                    variant->args[i].get()) &&
                isNumericType(expected))
                continue;
            if (dynamic_cast<StringLiteralExpr*>(
                    variant->args[i].get()) &&
                expected->kind == TypeKind::CStr)
                continue;
            constrain(actual, expected, "enum variant argument");
        }
        return constructed;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return analyzeLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr)) return analyzeCall(call);
    if (auto* fa = dynamic_cast<FieldAccessExpr*>(expr)) {
        TypePtr objectType = resolved(analyzeExpr(fa->object.get()));
        if (objectType->kind == TypeKind::Reference && objectType->inner)
            objectType = resolved(objectType->inner);
        if ((objectType->kind == TypeKind::Rc ||
             objectType->kind == TypeKind::Arc) && objectType->inner)
            objectType = resolved(objectType->inner);
        if (objectType->kind != TypeKind::Struct &&
            objectType->kind != TypeKind::Record &&
            objectType->kind != TypeKind::Metadata) {
            error("Field access requires a product type, got " + objectType->toString());
            return TyUnknown;
        }
        for (auto& field : objectType->fields) {
            if (field.name == fa->field) return field.type;
        }
        error("Type '" + objectType->toString() + "' has no field '" + fa->field + "'");
        return TyUnknown;
    }
    if (auto* propagation = dynamic_cast<TryExpr*>(expr)) {
        if (mCurrentFragmentDecl) {
            error("`?` may not propagate across a fragment/slot boundary; "
                  "handle the Result explicitly inside the fragment",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr result = resolved(analyzeExpr(propagation->operand.get()));
        if (result->kind != TypeKind::Result ||
            result->typeArgs.size() != 2) {
            error("`?` requires Result<T, E>, got " + result->toString(),
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr functionResult = resolved(mCurrentReturnType);
        if (!mInFunction || functionResult->kind != TypeKind::Result ||
            functionResult->typeArgs.size() != 2) {
            error("`?` requires the enclosing function to return Result",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr sourceError = resolved(result->typeArgs[1]);
        TypePtr targetError = resolved(functionResult->typeArgs[1]);
        if (!luna::types::sameType(sourceError, targetError)) {
            auto targetConversions =
                mFromConversions.find(typeIdentity(targetError));
            auto conversion = targetConversions == mFromConversions.end()
                ? std::unordered_map<std::string, FromConversion>::const_iterator{}
                : targetConversions->second.find(typeIdentity(sourceError));
            if (targetConversions == mFromConversions.end() ||
                conversion == targetConversions->second.end() ||
                !conversion->second.method ||
                conversion->second.symbol.empty()) {
                error("`?` cannot convert error '" + sourceError->toString() +
                      "' to '" + targetError->toString() +
                      "'; implement `From<" + sourceError->toString() +
                      "> for " + targetError->toString() + "`",
                      propagation->line, propagation->col);
                return TyUnknown;
            }
            propagation->errorConversionSymbol =
                conversion->second.symbol;
        }
        propagation->resultType = result;
        propagation->propagatedResultType = functionResult;
        propagation->valueType = result->typeArgs[0];
        propagation->errorType = sourceError;
        propagation->propagatedErrorType = targetError;
        return propagation->valueType;
    }
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr)) {
        if (mInKernel) {
            error("kernel body may not allocate heap memory with `new`; allocate device memory on the host and pass a borrowed device_buffer parameter",
                  ha->line, ha->col);
            return TyUnknown;
        }
        ha->allocatedType = ha->allocatedTypeAST
            ? resolveTypeAST(ha->allocatedTypeAST.get(), {}) : TyUnknown;
        if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
            TypeVec argumentTypes;
            for (auto& arg : initCall->args)
                argumentTypes.push_back(analyzeExpr(arg.get()));
            if (ha->allocatedType->kind == TypeKind::Struct ||
                ha->allocatedType->kind == TypeKind::Record) {
                if (initCall->args.size() != ha->allocatedType->fields.size()) {
                    error("Constructor for '" + ha->allocatedType->toString() +
                          "' expects " + std::to_string(ha->allocatedType->fields.size()) +
                          " field values");
                } else {
                    for (size_t i = 0; i < initCall->args.size(); ++i)
                        constrain(argumentTypes[i],
                                  ha->allocatedType->fields[i].type,
                                  "field '" + ha->allocatedType->fields[i].name + "' initializer");
                }
            } else if (ha->allocatedType->kind == TypeKind::I32 ||
                       ha->allocatedType->kind == TypeKind::I64 ||
                       ha->allocatedType->kind == TypeKind::F32 ||
                       ha->allocatedType->kind == TypeKind::F64 ||
                       ha->allocatedType->kind == TypeKind::Bool ||
                       ha->allocatedType->kind == TypeKind::String) {
                if (argumentTypes.size() != 1)
                    error("Primitive allocation requires exactly one initializer");
                else
                    constrain(argumentTypes[0], ha->allocatedType,
                              "primitive allocation initializer");
            }
        }
        switch (ha->storage) {
            case HeapStorageKind::Unique:
                ha->resultType = ha->allocatedType;
                break;
            case HeapStorageKind::Rc:
                ha->resultType = Type::makeRc(ha->allocatedType);
                break;
            case HeapStorageKind::Arc:
                ha->resultType = Type::makeArc(ha->allocatedType);
                break;
        }
        return ha->resultType;
    }
    if (auto* mv = dynamic_cast<MoveExpr*>(expr)) {
        return analyzeExpr(mv->operand.get());
    }
    if (auto* bw = dynamic_cast<BorrowExpr*>(expr)) {
        TypePtr inner = analyzeExpr(bw->operand.get());
        return Type::makeReference(inner, bw->isMutable);
    }
    if (auto* dr = dynamic_cast<DerefExpr*>(expr)) {
        TypePtr op = resolved(analyzeExpr(dr->operand.get()));
        if (op->kind == TypeKind::Reference && op->inner) return op->inner;
        error("Cannot dereference non-reference type");
        return TyUnknown;
    }
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr)) {
        TypePtr op = analyzeExpr(ad->operand.get());
        return Type::makeReference(op, ad->isMutable);
    }
    if (auto* le = dynamic_cast<LambdaExpr*>(expr)) {
        // Analyze lambda: enter new scope, register params, analyze body
        TypePtr savedReturn = mCurrentReturnType;
        bool savedSawReturn = mSawReturn;
        bool savedReturnsLinear =
            mCurrentFunctionReturnsLinear;
        auto savedReturnUsage =
            mCurrentFunctionReturnUsage;
        mSymTable.enterScope();
        for (auto& p : le->params) {
            TypePtr pt = p.inferredType ? p.inferredType : declaredType(p.type.get(), {});
            p.inferredType = pt;
            SymbolInfo info;
            info.kind = SymbolKind::Variable;
            info.type = pt;
            const bool explicitUsage = p.hasExplicitUsage || p.isLinear ||
                dynamic_cast<LinearTypeAST*>(p.type.get()) ||
                dynamic_cast<AffineTypeAST*>(p.type.get());
            info.usage = p.isLinear ? luna::ownership::Usage::Linear
                : (explicitUsage ? p.usage : defaultUsageForType(pt));
            const auto contract = parameterContractFor(pt, info.usage, explicitUsage);
            info.relation = contract.relation;
            info.usage = contract.usage;
            info.isLinear = info.usage == luna::ownership::Usage::Linear;
            p.usage = info.usage;
            p.relation = info.relation;
            mSymTable.define(p.name, info);
        }
        TypePtr bodyRet = le->returnType ? declaredType(le->returnType.get(), {}) : mConstraints.fresh();
        const bool returnsLinear =
            dynamic_cast<LinearTypeAST*>(
                le->returnType.get()) != nullptr;
        const bool returnsAffine =
            dynamic_cast<AffineTypeAST*>(
                le->returnType.get()) != nullptr;
        const auto returnUsage = returnsLinear
            ? luna::ownership::Usage::Linear
            : (returnsAffine
                   ? luna::ownership::Usage::Affine
                   : defaultUsageForType(bodyRet));
        mCurrentReturnType = bodyRet;
        mCurrentFunctionReturnsLinear =
            returnsLinear;
        mCurrentFunctionReturnUsage =
            returnUsage;
        mSawReturn = false;
        if (le->body) analyzeBlock(le->body.get(), bodyRet);
        if (!mSawReturn) constrain(bodyRet, TyUnit, "lambda without a return value");
        mSymTable.exitScope();
        mCurrentReturnType = savedReturn;
        mCurrentFunctionReturnsLinear =
            savedReturnsLinear;
        mCurrentFunctionReturnUsage =
            savedReturnUsage;
        mSawReturn = savedSawReturn;

        // Build closure function type: fn(ParamTypes) -> ReturnType
        TypeVec paramTypes;
        std::vector<luna::ownership::Contract> paramContracts;
        for (auto& p : le->params) {
            paramTypes.push_back(p.inferredType);
            paramContracts.push_back({p.relation, p.usage});
        }
        TypePtr retType = bodyRet;
        le->closureType = Type::makeFunction(
            paramTypes, retType, std::move(paramContracts),
            {luna::ownership::Relation::Owned, returnUsage});

        // Capture analysis: scan body for free variables from enclosing scopes
        // (simplified: just note lambda for codegen, captures resolved later)
        return le->closureType;
    }
    if (auto* as = dynamic_cast<AssignExpr*>(expr)) {
        TypePtr rhs = analyzeExpr(as->rhs.get());
        TypePtr lhs = analyzeExpr(as->lhs.get());
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            if (auto* symbol = mSymTable.lookup(id->name); symbol && symbol->isConst)
                error("cannot assign to const binding '" + id->name + "'", id->line, id->col);
        }
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(index->object.get())) {
                if (auto* symbol = mSymTable.lookup(id->name); symbol && symbol->isConst)
                    error("cannot assign through index of const array binding '" + id->name + "'",
                          id->line, id->col);
            }
        }
        bool operandsValid = true;
        if (as->op != TokenKind::Eq) {
            switch (as->op) {
                case TokenKind::AndEq:
                case TokenKind::OrEq:
                case TokenKind::XorEq:
                case TokenKind::ShiftLeftEq:
                case TokenKind::ShiftRightEq:
                    requireInteger(lhs, "left operand of compound bitwise assignment");
                    requireInteger(rhs, "right operand of compound bitwise assignment");
                    operandsValid =
                        (resolved(lhs)->kind == TypeKind::InferenceVar || isIntegerType(resolved(lhs))) &&
                        (resolved(rhs)->kind == TypeKind::InferenceVar || isIntegerType(resolved(rhs)));
                    break;
                default:
                    requireNumeric(lhs, "left operand of compound arithmetic assignment");
                    requireNumeric(rhs, "right operand of compound arithmetic assignment");
                    break;
            }
        }
        if (operandsValid) constrain(rhs, lhs, "assignment");
        return rhs;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
        TypePtr object = resolved(analyzeExpr(ix->object.get()));
        TypePtr index = resolved(analyzeExpr(ix->index.get()));
        if (!isIntegerType(index))
            error("array index must be an integer, got " + index->toString(), ix->line, ix->col);
        if (object->kind != TypeKind::Array && object->kind != TypeKind::Slice) {
            error("cannot index " + object->toString() + "; only array<T, N> and slice<T> support safe indexing",
                  ix->line, ix->col);
            return TyUnknown;
        }
        if (object->kind == TypeKind::Array) if (auto* literal = dynamic_cast<IntLiteralExpr*>(ix->index.get());
            literal && (literal->value < 0 || static_cast<uint64_t>(literal->value) >= object->arrayLength))
            error("array index " + std::to_string(literal->value) + " is outside array length " +
                  std::to_string(object->arrayLength), ix->line, ix->col);
        return object->inner;
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        if (array->elements.empty()) {
            error("cannot infer the element type of an empty array", array->line, array->col);
            return TyUnknown;
        }
        TypePtr element = analyzeExpr(array->elements.front().get());
        for (size_t i = 1; i < array->elements.size(); ++i)
            constrain(analyzeExpr(array->elements[i].get()), element, "array element");
        array->elementType = resolved(element);
        return Type::makeArray(array->elementType, array->elements.size());
    }
    if (auto* ie = dynamic_cast<IfExpr*>(expr)) {
        requireBool(analyzeExpr(ie->cond.get()), "if-expression condition");
        auto branchType = [this](Expr* branch) {
            if (auto* be = dynamic_cast<BlockExpr*>(branch)) {
                analyzeBlock(be->block.get(), nullptr);
                return TyUnit;
            }
            return analyzeExpr(branch);
        };
        TypePtr thenType = branchType(ie->thenExpr.get());
        TypePtr elseType = branchType(ie->elseExpr.get());
        if (resolved(thenType)->kind == TypeKind::Never) return elseType;
        if (resolved(elseType)->kind == TypeKind::Never) return thenType;
        constrain(thenType, elseType, "if-expression branches");
        return thenType;
    }
    return TyUnknown;
}

TypePtr SemanticAnalyzer::analyzeSelect(SelectExpr* selection) {
    const std::string targetKey = sourceDeclarationKey(selection->targetName);
    const std::string selectorKey = sourceDeclarationKey(selection->selectorName);
    auto targetFamily = mFunctionFamilies.find(targetKey);
    if (targetFamily == mFunctionFamilies.end() || targetFamily->second.empty()) {
        error("unknown declaration family '" + selection->targetName + "'",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto selectorFamily = mFunctionFamilies.find(selectorKey);
    if (selectorFamily == mFunctionFamilies.end() || selectorFamily->second.size() != 1) {
        error("selector function '" + selection->selectorName +
              "' must resolve to exactly one declaration",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto* selectorFunction = selectorFamily->second.front();
    const auto selectorSymbol = selectorFunction->generatedSymbolName.empty()
        ? selectorFunction->name : selectorFunction->generatedSymbolName;
    selection->resolvedSelectorDeclarationId = nominalDeclarationIdentity(
        mProgram, "fn", selectorSymbol, selectorFunction);
    selection->resolvedFamilyId = nominalDeclarationIdentity(
        mProgram, "fn", targetFamily->second.front()->name,
        targetFamily->second.front());
    if (selectorFunction->params.empty() ||
        resolved(selectorFunction->params.front().inferredType)->kind !=
            TypeKind::DeclarationView) {
        error("selector function '" + selection->selectorName +
              "' must declare declaration_view as its first parameter",
              selection->line, selection->col);
        return TyUnknown;
    }
    if (selection->selectorArgs.size() + 1 != selectorFunction->params.size()) {
        error("selector function '" + selection->selectorName + "' expects " +
              std::to_string(selectorFunction->params.size() - 1) +
              " explicit arguments", selection->line, selection->col);
        return TyUnknown;
    }
    if (resolved(selectorFunction->inferredReturnType)->kind !=
        TypeKind::DeclarationRef) {
        error("selector function '" + selection->selectorName +
              "' must return declaration_ref",
              selection->line, selection->col);
        return TyUnknown;
    }

    std::unordered_map<std::string, ConstValue> selectorLocals;
    std::vector<ConstValue> staticSelectorArguments;
    for (size_t index = 0; index < selection->selectorArgs.size(); ++index) {
        constrain(analyzeExpr(selection->selectorArgs[index].get()),
                  selectorFunction->params[index + 1].inferredType,
                  "selector argument " + std::to_string(index + 1));
        if (!selection->isDynamic) {
            auto value = evaluateConstExpr(selection->selectorArgs[index].get());
            if (!value) {
                error("static selector argument " + std::to_string(index + 1) +
                      " is not a compile-time value", selection->line, selection->col);
                return TyUnknown;
            }
            selectorLocals[selectorFunction->params[index + 1].name] = *value;
            staticSelectorArguments.push_back(*value);
        }
    }

    if (!selection->isDynamic) {
        TypePtr callableType;
        std::vector<luna::selector::Candidate> candidates;
        for (auto* candidate : targetFamily->second) {
            TypeVec parameters;
            std::vector<luna::ownership::Contract> contracts;
            for (const auto& parameter : candidate->params) {
                parameters.push_back(resolved(parameter.inferredType));
                contracts.push_back({parameter.relation, parameter.usage});
            }
            auto candidateType = Type::makeFunction(
                std::move(parameters), resolved(candidate->inferredReturnType),
                std::move(contracts),
                {luna::ownership::Relation::Owned, candidate->returnUsage});
            if (!callableType) callableType = candidateType;
            else if (!luna::types::sameType(callableType, candidateType)) {
                error("declaration family '" + selection->targetName +
                      "' contains incompatible callable signatures",
                      selection->line, selection->col);
                return TyUnknown;
            }

            luna::selector::Candidate viewCandidate;
            viewCandidate.symbolName = candidate->generatedSymbolName.empty()
                ? candidate->name : candidate->generatedSymbolName;
            viewCandidate.declarationId = nominalDeclarationIdentity(
                mProgram, "fn", viewCandidate.symbolName, candidate);
            viewCandidate.familyId = selection->resolvedFamilyId;
            viewCandidate.callableType = candidateType;
            for (const auto& attachment : candidate->metadata) {
                luna::selector::Metadata instance;
                instance.schemaId = attachment.resolvedSchemaId;
                instance.values = attachment.evaluatedArguments;
                if (attachment.retention == RetentionKind::Runtime)
                    instance.retention = luna::selector::Retention::Runtime;
                else if (attachment.retention == RetentionKind::Dynamic)
                    instance.retention = luna::selector::Retention::Dynamic;
                viewCandidate.metadata.push_back(std::move(instance));
            }
            candidates.push_back(std::move(viewCandidate));
        }

        luna::selector::DeclarationView view(std::move(candidates));
        std::string evaluationFailure;
        auto selectedId = evaluateSelectorFunction(
            selectorFunction, view, staticSelectorArguments, evaluationFailure);
        luna::selector::Engine engine;
        auto result = engine.validate(
            view, selectedId ? std::vector<std::string>{*selectedId}
                             : std::vector<std::string>{});
        if (!selectedId || !result.success()) {
            const std::string reason = !evaluationFailure.empty()
                ? evaluationFailure : result.message;
            error("selector '" + selection->selectorName +
                  "' failed for family '" + selection->targetName + "': " +
                  reason, selection->line, selection->col);
            return TyUnknown;
        }
        selection->resolvedDeclarationId = result.selected->declarationId;
        selection->resolvedSymbolName = result.selected->symbolName;
        selection->selectedType = callableType;
        selectorFunction->isSelector = true;
        return callableType;
    }

    // The existing runtime protocol remains frozen until runtime/dynamic
    // capabilities are specified. Static selection above no longer depends
    // on this exact-match primitive.
    ReturnStmt* selectorReturn = nullptr;
    if (selectorFunction->body) {
        for (auto& statement : selectorFunction->body->stmts) {
            if (auto* returned = dynamic_cast<ReturnStmt*>(statement.get())) {
                selectorReturn = returned;
                break;
            }
        }
    }
    auto* protocolCall = selectorReturn
        ? dynamic_cast<CallExpr*>(selectorReturn->value.get()) : nullptr;
    auto* protocolName = protocolCall
        ? dynamic_cast<IdentifierExpr*>(protocolCall->callee.get()) : nullptr;
    if (!protocolName || protocolName->name != "select_unique" ||
        protocolCall->args.size() != 2) {
        error("selector function '" + selection->selectorName +
              "' must use select_unique(view, metadata_value) in the "
              "provisional dynamic exact-match protocol",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto* metadataCall = dynamic_cast<CallExpr*>(protocolCall->args[1].get());
    auto* metadataName = metadataCall
        ? dynamic_cast<IdentifierExpr*>(metadataCall->callee.get()) : nullptr;
    std::string metadataKey;
    if (metadataName) {
        const std::string savedPackage = mCurrentPackageId;
        const std::string savedModule = mCurrentModulePath;
        setDeclarationContext(selectorFunction);
        metadataKey = sourceDeclarationKey(metadataName->name);
        mCurrentPackageId = savedPackage;
        mCurrentModulePath = savedModule;
    }
    if (!metadataName || !mMetadataSchemas.count(metadataKey)) {
        error("select_unique requires a user-declared metadata value as its filter",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto* schemaDeclaration = mMetadataSchemas[metadataKey];
    const auto schemaSymbol = schemaDeclaration->generatedSymbolName.empty()
        ? schemaDeclaration->name : schemaDeclaration->generatedSymbolName;
    selection->dynamicMetadataSchemaId = nominalDeclarationIdentity(
        mProgram, "meta", schemaSymbol, schemaDeclaration);
    std::vector<ConstValue> wantedValues;
    selection->dynamicFilterArguments.clear();
    for (auto& argument : metadataCall->args) {
        if (!selection->isDynamic) {
            auto value = evaluateConstExpr(argument.get(), selectorLocals);
            if (!value) {
                error("selector metadata expression is not compile-time evaluable",
                      selection->line, selection->col);
                return TyUnknown;
            }
            wantedValues.push_back(*value);
            continue;
        }

        SelectExpr::DynamicFilterArgument binding;
        if (auto* identifier = dynamic_cast<IdentifierExpr*>(argument.get())) {
            for (size_t parameterIndex = 1;
                 parameterIndex < selectorFunction->params.size(); ++parameterIndex) {
                if (selectorFunction->params[parameterIndex].name == identifier->name) {
                    binding.selectorArgumentIndex = parameterIndex - 1;
                    break;
                }
            }
        }
        if (!binding.selectorArgumentIndex) {
            auto constant = evaluateConstExpr(argument.get());
            if (constant) binding.constant = *constant;
        }
        if (!binding.selectorArgumentIndex && !binding.constant) {
            error("dynamic selector metadata expressions must be an explicit selector "
                  "argument or a compile-time literal in the initial protocol",
                  selection->line, selection->col);
            return TyUnknown;
        }
        selection->dynamicFilterArguments.push_back(std::move(binding));
    }

    TypePtr callableType;
    std::vector<luna::selector::Candidate> candidates;
    std::vector<std::string> matches;
    auto retention = [](RetentionKind value) {
        if (value == RetentionKind::Runtime) return luna::selector::Retention::Runtime;
        if (value == RetentionKind::Dynamic) return luna::selector::Retention::Dynamic;
        return luna::selector::Retention::CompileTime;
    };
    selection->dynamicCandidates.clear();
    for (auto* candidate : targetFamily->second) {
        TypeVec parameters;
        std::vector<luna::ownership::Contract> contracts;
        for (const auto& parameter : candidate->params) {
            parameters.push_back(resolved(parameter.inferredType));
            contracts.push_back({parameter.relation, parameter.usage});
        }
        auto candidateType = Type::makeFunction(
            std::move(parameters), resolved(candidate->inferredReturnType),
            std::move(contracts),
            {luna::ownership::Relation::Owned, candidate->returnUsage});
        if (!callableType) callableType = candidateType;
        else if (!luna::types::sameType(callableType, candidateType)) {
            error("declaration family '" + selection->targetName +
                  "' contains incompatible callable signatures",
                  selection->line, selection->col);
            return TyUnknown;
        }
        luna::selector::Candidate viewCandidate;
        viewCandidate.symbolName = candidate->generatedSymbolName.empty()
            ? candidate->name : candidate->generatedSymbolName;
        viewCandidate.declarationId = nominalDeclarationIdentity(
            mProgram, "fn", viewCandidate.symbolName, candidate);
        viewCandidate.familyId = selection->resolvedFamilyId;
        viewCandidate.callableType = candidateType;
        viewCandidate.retention = retention(candidate->retention);
        bool staticMatched = false;
        size_t dynamicSchemaAttachmentCount = 0;
        for (const auto& attachment : candidate->metadata) {
            luna::selector::Metadata instance;
            instance.schemaId = attachment.resolvedSchemaId;
            instance.values = attachment.evaluatedArguments;
            instance.retention = retention(attachment.retention);
            viewCandidate.metadata.push_back(std::move(instance));
            if (attachment.schemaName != metadataName->name) continue;
            if (!selection->isDynamic && attachment.evaluatedArguments == wantedValues)
                staticMatched = true;
            if (selection->isDynamic) {
                ++dynamicSchemaAttachmentCount;
                if (attachment.retention == RetentionKind::CompileTime) {
                    error("dynamic selector cannot inspect compile-time-only metadata '" +
                          attachment.schemaName + "' on '" +
                          viewCandidate.declarationId + "'",
                          selection->line, selection->col);
                    return TyUnknown;
                }
                SelectExpr::DynamicCandidate dynamicCandidate;
                dynamicCandidate.declarationId = viewCandidate.declarationId;
                dynamicCandidate.symbolName = viewCandidate.symbolName;
                dynamicCandidate.metadataValues = attachment.evaluatedArguments;
                selection->dynamicCandidates.push_back(std::move(dynamicCandidate));
            }
        }
        if (staticMatched) matches.push_back(viewCandidate.declarationId);
        if (selection->isDynamic && dynamicSchemaAttachmentCount > 1) {
            error("dynamic selector initially requires at most one '" +
                  metadataName->name + "' attachment per declaration",
                  selection->line, selection->col);
            return TyUnknown;
        }
        candidates.push_back(std::move(viewCandidate));
    }

    luna::selector::DeclarationView view(std::move(candidates));
    luna::selector::Engine engine;
    if (selection->isDynamic) {
        std::string planError;
        auto plan = engine.planDynamic(view,
            selectorFunction->generatedSymbolName.empty()
                ? selectorFunction->name : selectorFunction->generatedSymbolName,
            planError);
        if (!plan) {
            error(planError, selection->line, selection->col);
            return TyUnknown;
        }
        selection->dynamicCandidateIds = plan->candidateIds;
        if (selection->dynamicCandidates.empty()) {
            error("dynamic selector has no runtime-visible '" + metadataName->name +
                  "' metadata candidates", selection->line, selection->col);
            return TyUnknown;
        }

        // An exact-match selector must never have an input that maps to two
        // declarations.  Reject duplicate retained metadata at compile time;
        // the generated runtime check then only distinguishes unique/no-match.
        auto valueKey = [](const ConstValue& value) {
            if (auto* integer = std::get_if<int64_t>(&value))
                return std::string("i:") + std::to_string(*integer);
            if (auto* floating = std::get_if<double>(&value)) {
                std::ostringstream out;
                out << "f:" << std::setprecision(17) << *floating;
                return out.str();
            }
            if (auto* boolean = std::get_if<bool>(&value))
                return std::string(*boolean ? "b:1" : "b:0");
            const auto& string = std::get<std::string>(value);
            return "s:" + std::to_string(string.size()) + ":" + string;
        };
        std::unordered_map<std::string, std::string> retainedKeys;
        for (const auto& candidate : selection->dynamicCandidates) {
            std::string key;
            for (const auto& value : candidate.metadataValues)
                key += valueKey(value) + ";";
            auto [existing, inserted] = retainedKeys.emplace(key, candidate.declarationId);
            if (!inserted && existing->second != candidate.declarationId) {
                error("dynamic selector metadata is ambiguous between '" +
                      existing->second + "' and '" + candidate.declarationId + "'",
                      selection->line, selection->col);
                return TyUnknown;
            }
        }
        selection->resolvedDeclarationId.clear();
        selection->resolvedSymbolName.clear();
        selection->selectedType = callableType;
        selectorFunction->isSelector = true;
        selectorFunction->isDynamicSelector = true;
        return callableType;
    }
    auto result = engine.validate(view, matches);
    if (!result.success()) {
        error("selector '" + selection->selectorName + "' failed for family '" +
              selection->targetName + "': " + result.message,
              selection->line, selection->col);
        return TyUnknown;
    }
    selection->resolvedDeclarationId = result.selected->declarationId;
    selection->resolvedSymbolName = result.selected->symbolName;
    selection->selectedType = callableType;
    selectorFunction->isSelector = true;
    return callableType;
}

TypePtr SemanticAnalyzer::analyzeCall(CallExpr* call) {
    auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get());
    if (auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get()))
        return analyzeMemberCall(call, member);
    if (auto* selection = dynamic_cast<SelectExpr*>(call->callee.get())) {
        auto selected = resolved(analyzeSelect(selection));
        if (selected->kind != TypeKind::Function) return TyUnknown;
        if (selected->paramTypes.size() != call->args.size()) {
            error("Argument count mismatch for selected declaration family '" +
                  selection->targetName + "'", call->line, call->col);
            return TyUnknown;
        }
        for (size_t index = 0; index < call->args.size(); ++index)
            constrain(analyzeExpr(call->args[index].get()), selected->paramTypes[index],
                      "selected call argument " + std::to_string(index + 1));
        call->resolvedSymbolName = selection->resolvedSymbolName;
        call->resultType = selected->returnType;
        return selected->returnType;
    }
    if (id) {
        if (id->name == "range") {
            if (call->args.size() != 2) {
                error("range expects start and end integer values",
                      call->line, call->col);
                return TyUnknown;
            }
            requireInteger(analyzeExpr(call->args[0].get()), "range start");
            requireInteger(analyzeExpr(call->args[1].get()), "range end");
            call->iteratorOp = IteratorOp::Range;
            call->iteratorInputType = TyI32;
            call->iteratorOutputType = TyI32;
            call->resultType =
                Type::makeIterator(TyI32, IteratorMode::Range);
            return call->resultType;
        }
        if (id->name == "Ok" || id->name == "Err") {
            if (call->args.size() != 1) {
                error(id->name + " expects exactly one payload value",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr valueType = mConstraints.fresh();
            TypePtr errorType = mConstraints.fresh();
            if (!call->typeArgASTs.empty()) {
                if (call->typeArgASTs.size() != 2) {
                    error(id->name +
                          " explicit arguments must be `<Value, Error>`",
                          call->line, call->col);
                    return TyUnknown;
                }
                valueType =
                    resolveTypeAST(call->typeArgASTs[0].get(), {});
                errorType =
                    resolveTypeAST(call->typeArgASTs[1].get(), {});
            }
            TypePtr payload = analyzeExpr(call->args.front().get());
            constrain(payload, id->name == "Ok" ? valueType : errorType,
                      id->name + " payload");
            call->intrinsicType =
                Type::makeResult(valueType, errorType);
            mInferenceRoots.emplace_back(
                call->intrinsicType,
                "type arguments of '" + id->name + "'");
            call->returnUsage =
                defaultUsageForType(call->intrinsicType);
            return call->intrinsicType;
        }
        if (id->name == "is_ok" || id->name == "is_err" ||
            id->name == "unwrap" || id->name == "unwrap_err") {
            if (call->args.size() != 1) {
                error(id->name + " expects exactly one Result value",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr result = resolved(analyzeExpr(call->args.front().get()));
            if (result->kind != TypeKind::Result ||
                result->typeArgs.size() != 2) {
                error(id->name + " expects Result<T, E>, got " +
                      result->toString(), call->line, call->col);
                return TyUnknown;
            }
            call->intrinsicType = result;
            if (id->name == "is_ok" || id->name == "is_err")
                return TyBool;
            TypePtr extracted = result->typeArgs[
                id->name == "unwrap" ? 0 : 1];
            call->returnUsage = defaultUsageForType(extracted);
            return extracted;
        }
        if (id->name == "panic") {
            if (call->args.size() != 1) {
                error("panic expects exactly one string message",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr message = resolved(analyzeExpr(call->args.front().get()));
            if (message->kind != TypeKind::String &&
                message->kind != TypeKind::CStr)
                error("panic message must be string or cstr, got " +
                      message->toString(), call->line, call->col);
            call->intrinsicType = TyNever;
            call->resultType = TyNever;
            return TyNever;
        }
        if (id->name == "clone") {
            if (call->args.size() != 1) {
                error("clone expects exactly one rc<T> or arc<T> handle",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr handle = resolved(analyzeExpr(call->args.front().get()));
            if (handle->kind != TypeKind::Rc && handle->kind != TypeKind::Arc) {
                error("clone is only defined for rc<T> and arc<T>, got " +
                      handle->toString(), call->line, call->col);
                return TyUnknown;
            }
            call->returnUsage = luna::ownership::Usage::Affine;
            call->typeArgs = {handle};
            return handle;
        }
        if (id->name == "declaration_of" ||
            id->name == "declaration_id" ||
            id->name == "declaration_signature")
            return analyzeDeclarationReflectionCall(call, id->name);
        if (id->name == "declaration_count") {
            if (call->args.size() != 1 ||
                resolved(analyzeExpr(call->args.front().get()))->kind !=
                    TypeKind::DeclarationView)
                error("declaration_count expects one declaration_view",
                      call->line, call->col);
            return TyI32;
        }
        if (id->name == "declaration_at") {
            if (call->args.size() != 2) {
                error("declaration_at expects a declaration_view and an index",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr view = resolved(analyzeExpr(call->args[0].get()));
            requireInteger(analyzeExpr(call->args[1].get()),
                           "declaration_at index");
            if (view->kind != TypeKind::DeclarationView) {
                error("first argument of declaration_at must be declaration_view",
                      call->line, call->col);
                return TyUnknown;
            }
            return Type::makeDeclarationRef(view->inner);
        }
        if (id->name == "metadata" ||
            id->name == "declaration_has_metadata") {
            if (call->typeArgASTs.size() != 1 || call->args.size() != 1) {
                error(id->name +
                      " expects one metadata type argument and one declaration_ref",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr metadataType =
                resolved(resolveTypeAST(call->typeArgASTs.front().get(), {}));
            TypePtr declaration = resolved(analyzeExpr(call->args.front().get()));
            if (metadataType->kind != TypeKind::Metadata)
                error(id->name + " type argument must be a meta schema",
                      call->line, call->col);
            if (declaration->kind != TypeKind::DeclarationRef)
                error(id->name + " value argument must be declaration_ref",
                      call->line, call->col);
            if (id->name == "declaration_has_metadata" &&
                metadataType->kind == TypeKind::Metadata) {
                std::string declarationId;
                if (auto* reflected =
                        dynamic_cast<CallExpr*>(call->args.front().get()))
                    declarationId = reflected->compileTimeDeclarationId;
                else if (auto* identifier = dynamic_cast<IdentifierExpr*>(
                             call->args.front().get())) {
                    if (auto* symbol = mSymTable.lookup(identifier->name))
                        declarationId = symbol->compileTimeDeclarationId;
                }
                if (!declarationId.empty()) {
                    bool attached = false;
                    for (const auto& [familyName, family] : mFunctionFamilies) {
                        for (auto* candidate : family) {
                            const auto symbol =
                                candidate->generatedSymbolName.empty()
                                ? candidate->name
                                : candidate->generatedSymbolName;
                            if (nominalDeclarationIdentity(
                                    mProgram, "fn", symbol, candidate) !=
                                declarationId)
                                continue;
                            for (const auto& instance : candidate->metadata)
                                if (instance.resolvedSchemaId ==
                                    metadataType->nominalId)
                                    attached = true;
                        }
                    }
                    call->compileTimeValue = attached;
                }
            }
            return id->name == "metadata"
                ? Type::makeMetadataView(metadataType) : TyBool;
        }
        auto* symbol = lookupSymbol(id->name);
        if (symbol && symbol->kind == SymbolKind::Metadata) {
            if (call->args.size() != symbol->paramTypes.size()) {
                error("metadata constructor '" + id->name + "' expects " +
                      std::to_string(symbol->paramTypes.size()) + " arguments",
                      call->line, call->col);
                return TyUnknown;
            }
            for (size_t index = 0; index < call->args.size(); ++index)
                constrain(analyzeExpr(call->args[index].get()), symbol->paramTypes[index],
                          "metadata constructor argument " + std::to_string(index + 1));
            return symbol->returnType;
        }
        if (id->name == "select_unique") {
            if (call->args.size() != 2) {
                error("select_unique expects a DeclarationView and one metadata value",
                      call->line, call->col);
                return TyUnknown;
            }
            auto view = resolved(analyzeExpr(call->args[0].get()));
            auto metadata = resolved(analyzeExpr(call->args[1].get()));
            if (view->kind != TypeKind::DeclarationView)
                error("first argument of select_unique must be declaration_view",
                      call->line, call->col);
            if (metadata->kind != TypeKind::Metadata)
                error("second argument of select_unique must be a metadata value",
                      call->line, call->col);
            return Type::makeDeclarationRef(view->inner);
        }
        const std::string conceptKey = sourceDeclarationKey(id->name, false);
        auto constraintIt = mConcepts.find(conceptKey);
        if (constraintIt != mConcepts.end()) {
            if (!call->args.empty() ||
                call->typeArgASTs.size() != constraintIt->second->typeParams.size()) {
                error("constraint '" + constraintIt->second->name + "' expects " +
                      std::to_string(constraintIt->second->typeParams.size()) +
                      " type arguments and no value arguments",
                      call->line, call->col);
            } else {
                for (auto& type : call->typeArgASTs)
                    resolveTypeAST(type.get(), {});
            }
            return TyBool;
        }
    }
    if (id && (id->name == "type_of" || id->name == "type_kind" ||
               id->name == "type_id" || id->name == "type_shape" ||
               id->name == "type_domain" || id->name == "type_nominal" ||
               id->name == "type_size" ||
               id->name == "type_field_count" || id->name == "type_field_name" ||
               id->name == "type_field_type" || id->name == "type_variant_count" ||
               id->name == "type_variant_name" || id->name == "type_variant_field_count" ||
               id->name == "type_is_struct" || id->name == "type_is_enum" ||
               id->name == "type_is_nominal" || id->name == "type_is_structural" ||
               id->name == "type_is_meta" || id->name == "type_is_reference" ||
               id->name == "type_same" || id->name == "type_same_shape" ||
               id->name == "type_abi_compatible"))
        return analyzeReflectionCall(call, id->name);
    if (id && id->name == "slice") {
        if (call->args.size() != 3) { error("slice expects `slice(borrow array, start, end)`", call->line, call->col); return TyUnknown; }
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr source = resolved(analyzeExpr(call->args[0].get()));
        if (!borrow || source->kind != TypeKind::Reference || !source->inner || source->inner->kind != TypeKind::Array) {
            error("slice requires an explicit `borrow array` as its first argument", call->line, call->col); return TyUnknown;
        }
        requireInteger(analyzeExpr(call->args[1].get()), "slice start");
        requireInteger(analyzeExpr(call->args[2].get()), "slice end");
        return Type::makeSlice(source->inner->inner);
    }
    if (id && (id->name == "gpu_alloc_i32" || id->name == "gpu_load_i32" ||
               id->name == "gpu_store_i32" || id->name == "gpu_free" ||
               id->name == "gpu_copy_from_host_i32" ||
               id->name == "gpu_copy_to_host_i32")) {
        auto requireCount = [this, call, id](size_t expected) {
            if (call->args.size() != expected) {
                error("'" + id->name + "' expects " + std::to_string(expected) +
                      " argument" + (expected == 1 ? "" : "s"), call->line, call->col);
                return false;
            }
            return true;
        };
        if (id->name == "gpu_alloc_i32") {
            if (!requireCount(1)) return TyUnknown;
            if (mInKernel)
                error("'gpu_alloc_i32' is a host operation and cannot run inside a kernel", call->line, call->col);
            requireInteger(analyzeExpr(call->args[0].get()), "gpu_alloc_i32 element count");
            return Type::makeDeviceBuffer(TyI32);
        }
        if (id->name == "gpu_free") {
            if (!requireCount(1)) return TyUnknown;
            if (mInKernel)
                error("'gpu_free' is a host operation and cannot run inside a kernel", call->line, call->col);
            if (!dynamic_cast<MoveExpr*>(call->args[0].get()))
                error("'gpu_free' requires `move buffer` to make ownership transfer explicit",
                      call->line, call->col);
            constrain(analyzeExpr(call->args[0].get()), Type::makeDeviceBuffer(TyI32),
                      "gpu_free buffer");
            return TyUnit;
        }
        if (id->name == "gpu_copy_from_host_i32" || id->name == "gpu_copy_to_host_i32") {
            if (!requireCount(3)) return TyUnknown;
            if (mInKernel)
                error("'" + id->name + "' is a host operation and cannot run inside a kernel",
                      call->line, call->col);
            const bool isUpload = id->name == "gpu_copy_from_host_i32";
            auto* deviceBorrow = dynamic_cast<BorrowExpr*>(call->args[0 + (isUpload ? 0 : 1)].get());
            auto* hostBorrow = dynamic_cast<BorrowExpr*>(call->args[1 - (isUpload ? 0 : 1)].get());
            TypePtr deviceActual = analyzeExpr(call->args[isUpload ? 0 : 1].get());
            TypePtr hostActual = analyzeExpr(call->args[isUpload ? 1 : 0].get());
            const bool needsMutableDevice = isUpload;
            const bool needsMutableHost = !isUpload;
            if (!deviceBorrow || deviceBorrow->isMutable != needsMutableDevice)
                error("'" + id->name + "' requires " +
                      std::string(needsMutableDevice ? "`borrow mut buffer`" : "`borrow buffer`") +
                      " for its device-buffer argument", call->line, call->col);
            if (!hostBorrow || hostBorrow->isMutable != needsMutableHost)
                error("'" + id->name + "' requires " +
                      std::string(needsMutableHost ? "`borrow mut host`" : "`borrow host`") +
                      " for its host raw-pointer argument", call->line, call->col);
            constrain(deviceActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), needsMutableDevice),
                      "device-buffer argument of '" + id->name + "'");
            constrain(hostActual,
                      Type::makeReference(Type::makeRawPointer(TyI32), needsMutableHost),
                      "host raw-pointer argument of '" + id->name + "'");
            requireInteger(analyzeExpr(call->args[2].get()),
                           "element count of '" + id->name + "'");
            if (auto* literal = dynamic_cast<IntLiteralExpr*>(call->args[2].get()); literal && literal->value < 0)
                error("'" + id->name + "' requires a non-negative element count",
                      literal->line, literal->col);
            return TyUnit;
        }
        const bool isStore = id->name == "gpu_store_i32";
        if (!requireCount(isStore ? 3 : 2)) return TyUnknown;
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr bufferActual = analyzeExpr(call->args[0].get());
        TypePtr resolvedBufferActual = resolved(bufferActual);
        const bool isDeviceReference = resolvedBufferActual->kind == TypeKind::Reference &&
            resolvedBufferActual->inner &&
            resolvedBufferActual->inner->kind == TypeKind::DeviceBuffer;
        if ((!borrow && !isDeviceReference) || (borrow && isStore && !borrow->isMutable))
            error(std::string("'") + id->name + "' requires an explicit " +
                  (isStore ? "`borrow mut buffer`" : "`borrow buffer`") +
                  " (or an already-borrowed kernel parameter)",
                  call->line, call->col);
        if (isStore) {
            constrain(bufferActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), true),
                      std::string("first argument of '") + id->name + "'");
        } else if (isDeviceReference) {
            // A mutable kernel parameter is also valid for a read-only load.
            constrain(resolvedBufferActual->inner, Type::makeDeviceBuffer(TyI32),
                      std::string("first argument of '") + id->name + "'");
        }
        requireInteger(analyzeExpr(call->args[1].get()), std::string("index argument of '") + id->name + "'");
        if (isStore) constrain(analyzeExpr(call->args[2].get()), TyI32,
                               "value argument of 'gpu_store_i32'");
        return isStore ? TyUnit : TyI32;
    }
    if (mInKernel && id) {
        error("kernel body may only call device built-ins (`gpu_load_i32` and `gpu_store_i32`) in the initial ABI",
              call->line, call->col);
        return TyUnknown;
    }
    SymbolInfo* sym = id ? lookupSymbol(id->name) : nullptr;
    auto constrainArgument = [this](Expr* expr, const TypePtr& expected,
                                     const std::string& context) {
        TypePtr actual = analyzeExpr(expr);
        // Integer literals are representationally polymorphic at an FFI
        // boundary; the code generator will widen/truncate them to the ABI
        // parameter width.
        if (dynamic_cast<IntLiteralExpr*>(expr) && isNumericType(resolved(expected)))
            return;
        if (dynamic_cast<StringLiteralExpr*>(expr) &&
            resolved(expected)->kind == TypeKind::CStr)
            return;
        constrain(actual, expected, context);
    };

    if (id) {
        const std::string declarationKey = sourceDeclarationKey(id->name);
        auto family = mFunctionFamilies.find(declarationKey);
        if (family != mFunctionFamilies.end() && family->second.size() > 1) {
            error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", call->line, call->col);
            return TyUnknown;
        }
        if (family != mFunctionFamilies.end() && family->second.size() == 1) {
            auto* declaration = family->second.front();
            call->resolvedSymbolName = declaration->generatedSymbolName.empty()
                ? declaration->name : declaration->generatedSymbolName;
        }
    }

    // Direct calls and closure calls both become constraints. This is the
    // important consumer side of inference: an untyped declaration can be
    // solved by the arguments at its call sites.
    if (!sym) {
        TypePtr calleeType = analyzeExpr(call->callee.get());
        if (calleeType->kind != TypeKind::Function) {
            error("Expression is not callable");
            return TyUnknown;
        }
        if (calleeType->paramTypes.size() != call->args.size()) {
            error("Argument count mismatch in indirect call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), calleeType->paramTypes[i], "call argument");
        return calleeType->returnType;
    }

    if (sym->kind == SymbolKind::Variable && sym->type &&
        resolved(sym->type)->kind == TypeKind::Function) {
        auto fn = resolved(sym->type);
        if (fn->paramTypes.size() != call->args.size()) {
            error("Argument count mismatch in closure call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), fn->paramTypes[i], "closure call argument");
        return fn->returnType;
    }
    if (sym->kind != SymbolKind::Function) {
        error("'" + id->name + "' is not callable");
        return TyUnknown;
    }
    if (call->args.size() != sym->paramTypes.size()) {
        error("Argument count mismatch for '" + id->name + "'");
        return TyUnknown;
    }

    // Generic functions retain their existing monomorphization path. Their
    // type parameters are explicit variables, whereas omitted ordinary
    // signatures use the inference variables in paramTypes/returnType below.
    if (!sym->typeParams.empty() && sym->genericDecl) {
        TypeVec actualTypes;
        for (auto& argument : call->args)
            actualTypes.push_back(resolved(analyzeExpr(argument.get())));
        TypeVec concreteTypes;
        if (!call->typeArgASTs.empty()) {
            if (call->typeArgASTs.size() != sym->typeParams.size()) {
                error("generic function '" + id->name + "' expects " +
                      std::to_string(sym->typeParams.size()) +
                      " type argument(s)", call->line, call->col);
                return TyUnknown;
            }
            for (auto& typeArgument : call->typeArgASTs)
                concreteTypes.push_back(resolved(
                    resolveTypeAST(typeArgument.get(), {})));
        } else {
            std::unordered_map<std::string, TypePtr> inferred;
            std::function<void(const TypePtr&, const TypePtr&)> infer =
                [&](const TypePtr& patternValue,
                    const TypePtr& actualValue) {
                const TypePtr pattern = resolved(patternValue);
                const TypePtr actual = resolved(actualValue);
                if (!pattern || !actual) return;
                if (pattern->kind == TypeKind::TypeParam) {
                    if (std::find(sym->typeParams.begin(),
                                  sym->typeParams.end(),
                                  pattern->name) ==
                        sym->typeParams.end())
                        return;
                    auto existing = inferred.find(pattern->name);
                    if (existing == inferred.end())
                        inferred[pattern->name] = actual;
                    else if (!luna::types::sameType(
                                 existing->second, actual))
                        error("conflicting inference for generic type '" +
                              pattern->name + "'", call->line, call->col);
                    return;
                }
                if (pattern->kind != actual->kind) return;
                if (!pattern->typeArgs.empty() &&
                    pattern->typeArgs.size() ==
                        actual->typeArgs.size()) {
                    for (size_t index = 0;
                         index < pattern->typeArgs.size(); ++index)
                        infer(pattern->typeArgs[index],
                              actual->typeArgs[index]);
                }
                if (pattern->inner && actual->inner)
                    infer(pattern->inner, actual->inner);
                if (pattern->kind == TypeKind::Function &&
                    pattern->paramTypes.size() ==
                        actual->paramTypes.size()) {
                    for (size_t index = 0;
                         index < pattern->paramTypes.size(); ++index)
                        infer(pattern->paramTypes[index],
                              actual->paramTypes[index]);
                    infer(pattern->returnType, actual->returnType);
                }
            };
            for (size_t index = 0;
                 index < sym->paramTypes.size() &&
                 index < actualTypes.size(); ++index)
                infer(sym->paramTypes[index], actualTypes[index]);
            for (const auto& parameter : sym->typeParams) {
                auto found = inferred.find(parameter);
                if (found == inferred.end()) {
                    error("could not infer generic type argument '" +
                          parameter + "' for '" + id->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                concreteTypes.push_back(found->second);
            }
        }
        call->typeArgs = concreteTypes;

        std::unordered_map<std::string, TypePtr> constraintBindings;
        for (size_t index = 0;
             index < sym->genericDecl->typeParams.size() &&
             index < concreteTypes.size(); ++index)
            constraintBindings[sym->genericDecl->typeParams[index]] =
                resolved(concreteTypes[index]);
        for (size_t index = 0;
             index < sym->paramTypes.size() &&
             index < call->args.size(); ++index) {
            constrainArgument(
                call->args[index].get(),
                substituteNominalType(
                    resolved(sym->paramTypes[index]),
                    constraintBindings),
                "argument " + std::to_string(index + 1) +
                    " of generic call");
        }
        for (auto& clause : sym->genericDecl->whereClauses) {
            if (clause.kind == WhereClause::Kind::TraitBound) {
                const std::string& tpName = clause.typeParam;
                const std::string& traitId = clause.trait.resolvedTraitId;
                auto concrete = constraintBindings.find(tpName);
                if (concrete != constraintBindings.end() &&
                    !satisfiesTrait(traitId, concrete->second))
                    error("Type '" + typeIdentity(concrete->second) +
                          "' does not satisfy trait '" +
                          displayTraitRef(clause.trait) + "'");
                continue;
            }

            TypeVec conceptArguments;
            for (auto& argument : clause.constraintTypeArgs)
                conceptArguments.push_back(resolved(
                    resolveTypeAST(argument.get(), constraintBindings)));
            std::vector<std::string> activeConstraints;
            auto satisfied = evaluateConstraint(
                clause.constraintName, conceptArguments, activeConstraints);
            if (!satisfied) {
                error("constraint '" + clause.constraintName +
                      "' is not compile-time evaluable");
            } else if (!*satisfied) {
                std::string types;
                for (size_t index = 0; index < conceptArguments.size(); ++index) {
                    if (index) types += ", ";
                    types += conceptArguments[index]->toString();
                }
                error("constraint '" + clause.constraintName +
                      "<" + types + ">' is not satisfied");
            }
        }
        auto* specialized = monomorphize(sym->genericDecl, concreteTypes);
        if (specialized && mProgram) {
            const bool newlyCreated = !mGeneratedInstances.empty() &&
                                      mGeneratedInstances.back().get() == specialized;
            if (newlyCreated) {
                mProgram->declarations.push_back(std::move(mGeneratedInstances.back()));
                const std::string savedPackage = mCurrentPackageId;
                const std::string savedModule = mCurrentModulePath;
                setDeclarationContext(specialized);
                declareFunction(specialized);
                analyzeFunction(specialized);
                mCurrentPackageId = savedPackage;
                mCurrentModulePath = savedModule;
            }
            call->resolvedSymbolName = specialized->generatedSymbolName.empty()
                ? specialized->name : specialized->generatedSymbolName;
            call->returnsLinear = specialized->returnsLinear;
            call->returnUsage = specialized->returnUsage;
            call->resultType = specialized->inferredReturnType
                ? specialized->inferredReturnType : TyUnit;
            return call->resultType;
        }
    }

    // Built-in print is polymorphic but has no user AST to monomorphize.
    if (!sym->typeParams.empty() && !sym->genericDecl) {
        for (auto& arg : call->args) analyzeExpr(arg.get());
        return sym->returnType ? sym->returnType : TyUnit;
    }

    for (size_t i = 0; i < call->args.size(); ++i)
        constrainArgument(call->args[i].get(), sym->paramTypes[i],
                          "argument " + std::to_string(i + 1) + " of '" + id->name + "'");
    if (id) {
        const std::string constexprName = call->resolvedSymbolName.empty()
            ? id->name : call->resolvedSymbolName;
        auto constexprIt = mConstexprFunctions.find(constexprName);
        if (constexprIt != mConstexprFunctions.end()) {
            if (auto value = evaluateConstExpr(call)) call->compileTimeValue = std::move(*value);
        }
    }
    call->returnsLinear = sym->returnsLinear;
    call->returnUsage = sym->returnUsage;
    call->resultType = sym->returnType ? sym->returnType : TyUnit;
    return call->resultType;
}

TypePtr SemanticAnalyzer::analyzeMemberCall(
    CallExpr* call, FieldAccessExpr* member) {
    TypePtr receiver =
        resolved(analyzeExpr(member->object.get()));
    const std::string methodName = member->field;
    const bool collectionEntry =
        (receiver->kind == TypeKind::Array ||
         receiver->kind == TypeKind::Slice) &&
        (methodName == "iter" ||
         methodName == "iter_mut" ||
         methodName == "into_iter");
    const bool recipeOperation =
        receiver->kind == TypeKind::Iterator &&
        (methodName == "map" ||
         methodName == "filter" ||
         methodName == "take" ||
         methodName == "fold" ||
         methodName == "for_each" ||
         methodName == "count" ||
         methodName == "collect");
    if (collectionEntry || recipeOperation)
        return analyzeIteratorCall(call, member);

    if (mInKernel) {
        error("user trait method calls are not yet available in kernel code",
              call->line, call->col);
        return TyUnknown;
    }
    if (!call->typeArgASTs.empty()) {
        error("generic trait methods are not yet supported by member syntax",
              call->line, call->col);
        return TyUnknown;
    }

    TypePtr target = receiver;
    if (target->kind == TypeKind::Reference && target->inner)
        target = resolved(target->inner);
    const std::string targetId = typeIdentity(target);

    struct Candidate {
        std::string traitId;
        FunctionDecl* method = nullptr;
        TypePtr receiverType;
    };
    std::vector<Candidate> candidates;
    for (const auto& [traitId, targets] : mImpls) {
        if (traitId == luna::sysmeta::DropTraitId ||
            traitId == luna::sysmeta::FromTraitId)
            continue;
        auto implementation = targets.find(targetId);
        if (implementation == targets.end()) continue;
        auto method = implementation->second.find(methodName);
        if (method == implementation->second.end() ||
            !method->second ||
            method->second->params.empty())
            continue;
        TypePtr expected = resolved(
            method->second->params.front().inferredType);
        bool acceptsReceiver =
            luna::types::sameType(expected, receiver) ||
            luna::types::sameType(expected, target);
        if (expected->kind == TypeKind::Reference &&
            expected->inner)
            acceptsReceiver = luna::types::sameType(
                resolved(expected->inner), target);
        if (acceptsReceiver)
            candidates.push_back(
                {traitId, method->second, expected});
    }

    if (candidates.empty()) {
        error("no trait method '" + methodName +
              "' is implemented for receiver type '" +
              receiver->toString() + "'", call->line, call->col);
        return TyUnknown;
    }
    if (candidates.size() > 1) {
        std::string traits;
        for (const auto& candidate : candidates) {
            if (!traits.empty()) traits += ", ";
            traits += candidate.traitId;
        }
        error("member call '" + methodName +
              "' is ambiguous for type '" + target->toString() +
              "' across traits: " + traits,
              call->line, call->col);
        return TyUnknown;
    }

    auto selected = candidates.front();
    FunctionDecl* method = selected.method;
    if (call->args.size() + 1 != method->params.size()) {
        error("trait method '" + methodName + "' expects " +
              std::to_string(method->params.size() - 1) +
              " explicit argument(s)", call->line, call->col);
        return TyUnknown;
    }

    std::unique_ptr<FieldAccessExpr> ownedMember(
        static_cast<FieldAccessExpr*>(call->callee.release()));
    std::unique_ptr<Expr> implicitReceiver =
        std::move(ownedMember->object);
    if (selected.receiverType->kind ==
            TypeKind::Reference &&
        receiver->kind != TypeKind::Reference) {
        auto borrow = std::make_unique<BorrowExpr>();
        borrow->isMutable =
            selected.receiverType->isMutable;
        borrow->operand = std::move(implicitReceiver);
        implicitReceiver = std::move(borrow);
    }
    call->args.insert(call->args.begin(),
                      std::move(implicitReceiver));
    auto callee =
        std::make_unique<IdentifierExpr>(methodName);
    callee->sourcePath = call->sourcePath;
    callee->line = call->line;
    callee->col = call->col;
    call->callee = std::move(callee);
    call->resolvedSymbolName =
        method->generatedSymbolName.empty()
            ? method->name
            : method->generatedSymbolName;

    for (size_t index = 0;
         index < call->args.size(); ++index) {
        TypePtr expected =
            resolved(method->params[index].inferredType);
        TypePtr actual =
            resolved(analyzeExpr(call->args[index].get()));
        if (dynamic_cast<IntLiteralExpr*>(
                call->args[index].get()) &&
            isNumericType(expected))
            continue;
        if (dynamic_cast<StringLiteralExpr*>(
                call->args[index].get()) &&
            expected->kind == TypeKind::CStr)
            continue;
        constrain(actual, expected,
                  "argument " + std::to_string(index + 1) +
                  " of trait method '" + methodName + "'");
    }
    call->returnsLinear = method->returnsLinear;
    call->returnUsage = method->returnUsage;
    call->resultType = method->inferredReturnType
        ? resolved(method->inferredReturnType) : TyUnit;
    return call->resultType;
}

TypePtr SemanticAnalyzer::analyzeIteratorCall(
    CallExpr* call, FieldAccessExpr* member) {
    if (mInKernel) {
        error("kernel iterator pipelines are reserved until their adapter "
              "closures can be cloned into the device module",
              call->line, call->col);
        return TyUnknown;
    }
    const std::string& name = member->field;
    TypePtr receiver = resolved(analyzeExpr(member->object.get()));
    if (name != "collect" && !call->typeArgASTs.empty()) {
        error("iterator `" + name +
              "` does not accept explicit type arguments",
              call->line, call->col);
        return TyUnknown;
    }
    call->iteratorRecipeStateName.clear();
    call->iteratorRecipeSourceType.reset();
    const auto markTerminalRecipe =
        [&](IteratorOp op) {
            if (op != IteratorOp::Fold &&
                op != IteratorOp::ForEach &&
                op != IteratorOp::Count &&
                op != IteratorOp::Collect)
                return;
            std::function<void(Expr*)> findSource =
                [&](Expr* expression) {
                    auto* sourceCall =
                        dynamic_cast<CallExpr*>(
                            expression);
                    if (!sourceCall) return;
                    auto* sourceMember =
                        dynamic_cast<FieldAccessExpr*>(
                            sourceCall->callee.get());
                    if (!sourceMember) return;
                    if (sourceCall->iteratorOp ==
                        IteratorOp::IntoIter) {
                        TypePtr sourceType;
                        if (sourceCall->resultType &&
                            !sourceCall->resultType->
                                typeArgs.empty())
                            sourceType = resolved(
                                sourceCall->resultType->
                                    typeArgs.front());
                        if (!sourceType ||
                            sourceType->kind !=
                                TypeKind::Array ||
                            !sourceType->inner ||
                            defaultUsageForType(
                                sourceType->inner) ==
                                luna::ownership::Usage::Copy)
                            return;
                        if (!dynamic_cast<
                                IdentifierExpr*>(
                                sourceMember->
                                    object.get())) {
                            error("move-only iterator terminal "
                                  "currently requires a local "
                                  "array source binding",
                                  call->line, call->col);
                            return;
                        }
                        if (luna::ownership::mustConsume(
                                defaultUsageForType(
                                    sourceType))) {
                            error("linear iterator terminal "
                                  "state cannot be hidden from "
                                  "explicit consumption",
                                  call->line, call->col);
                            return;
                        }
                        call->iteratorRecipeSourceType =
                            sourceType;
                        call->iteratorRecipeStateName =
                            "$terminal.recipe." +
                            std::to_string(
                                mIteratorStateCounter++);
                        return;
                    }
                    findSource(
                        sourceMember->object.get());
                };
            findSource(member->object.get());
        };
    const auto finish = [&](IteratorOp op, const TypePtr& result,
                            const TypePtr& input, const TypePtr& output) {
        call->iteratorOp = op;
        call->resultType = result;
        call->iteratorInputType = input;
        call->iteratorOutputType = output;
        markTerminalRecipe(op);
        return result;
    };
    const auto requireCount = [&](size_t expected) {
        if (call->args.size() == expected) return true;
        error("iterator `" + name + "` expects " +
              std::to_string(expected) + " argument" +
              (expected == 1 ? "" : "s"), call->line, call->col);
        return false;
    };

    if (name == "iter" || name == "iter_mut" ||
        name == "into_iter") {
        if (!requireCount(0)) return TyUnknown;
        if (receiver->kind != TypeKind::Array &&
            receiver->kind != TypeKind::Slice) {
            error("`" + name + "` requires an array or slice receiver, got " +
                  receiver->toString(), call->line, call->col);
            return TyUnknown;
        }
        TypePtr element = receiver->inner;
        IteratorMode mode = IteratorMode::Shared;
        IteratorOp op = IteratorOp::Iter;
        TypePtr item = Type::makeReference(element);
        if (name == "iter_mut") {
            mode = IteratorMode::Mutable;
            op = IteratorOp::IterMut;
            item = Type::makeReference(element, true);
        } else if (name == "into_iter") {
            mode = IteratorMode::Consuming;
            op = IteratorOp::IntoIter;
            item = element;
        }
        return finish(
            op, Type::makeIterator(item, mode, receiver),
            element, item);
    }

    if (receiver->kind != TypeKind::Iterator || !receiver->inner) {
        error("`" + name + "` requires an iterator receiver, got " +
              receiver->toString(), call->line, call->col);
        return TyUnknown;
    }
    const TypePtr item = receiver->inner;

    auto callable = [&](size_t argumentIndex, size_t parameterCount,
                        const std::string& context) -> TypePtr {
        TypePtr type = resolved(analyzeExpr(call->args[argumentIndex].get()));
        if (type->kind != TypeKind::Function ||
            type->paramTypes.size() != parameterCount) {
            error(context + " requires a callable with " +
                  std::to_string(parameterCount) + " parameter" +
                  (parameterCount == 1 ? "" : "s"),
                  call->line, call->col);
            return TyUnknown;
        }
        return type;
    };

    if (name == "map") {
        if (!requireCount(1)) return TyUnknown;
        TypePtr transform = callable(0, 1, "iterator map");
        if (transform->kind != TypeKind::Function) return TyUnknown;
        constrain(item, transform->paramTypes[0], "iterator map input");
        TypePtr output = resolved(transform->returnType);
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (transform->paramContracts.empty() ||
             transform->paramContracts[0].relation !=
                 luna::ownership::Relation::Owned))
            error("map transform must own a move-only input "
                  "because the input does not continue downstream",
                  call->line, call->col);
        return finish(
            IteratorOp::Map,
            Type::makeIterator(output, receiver->iteratorMode, receiver),
            item, output);
    }
    if (name == "filter") {
        if (!requireCount(1)) return TyUnknown;
        TypePtr predicate = callable(0, 1, "iterator filter");
        if (predicate->kind != TypeKind::Function) return TyUnknown;
        constrain(item, predicate->paramTypes[0], "iterator filter input");
        requireBool(predicate->returnType, "iterator filter predicate");
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (predicate->paramContracts.empty() ||
             predicate->paramContracts[0].relation !=
                 luna::ownership::Relation::SharedBorrow))
            error("filter predicate must borrow a move-only item "
                  "because accepted items continue downstream",
                  call->line, call->col);
        return finish(
            IteratorOp::Filter,
            Type::makeIterator(item, receiver->iteratorMode, receiver),
            item, item);
    }
    if (name == "take") {
        if (!requireCount(1)) return TyUnknown;
        requireInteger(analyzeExpr(call->args[0].get()),
                       "iterator take count");
        return finish(
            IteratorOp::Take,
            Type::makeIterator(item, receiver->iteratorMode, receiver),
            item, item);
    }
    if (name == "fold") {
        if (!requireCount(2)) return TyUnknown;
        TypePtr accumulator = resolved(analyzeExpr(call->args[0].get()));
        TypePtr reducer = callable(1, 2, "iterator fold");
        if (reducer->kind != TypeKind::Function) return TyUnknown;
        constrain(accumulator, reducer->paramTypes[0],
                  "iterator fold accumulator");
        constrain(item, reducer->paramTypes[1], "iterator fold item");
        constrain(reducer->returnType, accumulator,
                  "iterator fold result");
        const auto accumulatorUsage =
            defaultUsageForType(accumulator);
        if (luna::ownership::mustConsume(
                accumulatorUsage)) {
            error("linear fold accumulators are reserved "
                  "until terminal state can expose an "
                  "explicit linear obligation",
                  call->line, call->col);
        } else if (accumulatorUsage !=
                   luna::ownership::Usage::Copy) {
            if (reducer->paramContracts.empty() ||
                reducer->paramContracts[0].relation !=
                    luna::ownership::Relation::Owned)
                error("fold reducer must own a move-only "
                      "accumulator",
                      call->line, call->col);
            if (reducer->returnContract.relation !=
                    luna::ownership::Relation::Owned ||
                reducer->returnContract.usage !=
                    accumulatorUsage)
                error("fold reducer must return ownership "
                      "of the replacement accumulator",
                      call->line, call->col);
        }
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (reducer->paramContracts.size() < 2 ||
             reducer->paramContracts[1].relation !=
                 luna::ownership::Relation::Owned))
            error("fold reducer must own a move-only item",
                  call->line, call->col);
        TypePtr result = finish(
            IteratorOp::Fold, accumulator,
            item, accumulator);
        call->returnUsage = accumulatorUsage;
        call->returnsLinear =
            accumulatorUsage ==
            luna::ownership::Usage::Linear;
        return result;
    }
    if (name == "for_each") {
        if (!requireCount(1)) return TyUnknown;
        TypePtr action = callable(0, 1, "iterator for_each");
        if (action->kind != TypeKind::Function) return TyUnknown;
        constrain(item, action->paramTypes[0], "iterator for_each item");
        constrain(action->returnType, TyUnit, "iterator for_each result");
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (action->paramContracts.empty() ||
             action->paramContracts[0].relation !=
                 luna::ownership::Relation::Owned))
            error("for_each action must own a move-only item",
                  call->line, call->col);
        return finish(IteratorOp::ForEach, TyUnit, item, TyUnit);
    }
    if (name == "count") {
        if (!requireCount(0)) return TyUnknown;
        return finish(IteratorOp::Count, TyI32, item, TyI32);
    }
    if (name == "collect") {
        if (!requireCount(0)) return TyUnknown;
        if (call->typeArgASTs.size() != 1) {
            error("iterator `collect` requires exactly one explicit target "
                  "type: `.collect::<Target>()`",
                  call->line, call->col);
            return TyUnknown;
        }
        TypePtr target = resolved(
            resolveTypeAST(call->typeArgASTs.front().get(), {}));
        if (!target || target->kind == TypeKind::Unknown ||
            target->domain != luna::types::TypeDomain::Value) {
            error("iterator `collect` target must be a concrete value type",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto implementation =
            mFromIteratorImplementations.find(typeIdentity(target));
        if (implementation == mFromIteratorImplementations.end()) {
            error("no coherent Core `FromIterator` implementation exists "
                  "for collect target '" + target->toString() + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto& protocol = implementation->second;
        if (!luna::types::sameType(
                resolved(protocol.item), resolved(item))) {
            error("Core `FromIterator` for '" + target->toString() +
                  "' collects '" + protocol.item->toString() +
                  "', but this iterator yields '" + item->toString() + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        if (!protocol.begin || !protocol.push || !protocol.finish) {
            error("Core `FromIterator` implementation for '" +
                  target->toString() +
                  "' does not provide the complete begin/push/finish protocol",
                  call->line, call->col);
            return TyUnknown;
        }
        call->iteratorCollectTargetType = target;
        call->iteratorCollectBuilderType =
            resolved(protocol.builder);
        call->iteratorCollectBeginSymbol =
            protocol.begin->generatedSymbolName;
        call->iteratorCollectPushSymbol =
            protocol.push->generatedSymbolName;
        call->iteratorCollectFinishSymbol =
            protocol.finish->generatedSymbolName;
        TypePtr result = finish(
            IteratorOp::Collect, target, item, target);
        call->returnUsage = protocol.finish->returnUsage;
        call->returnsLinear =
            protocol.finish->returnsLinear;
        return result;
    }

    error("unknown iterator adapter or terminal `" + name + "`",
          call->line, call->col);
    return TyUnknown;
}

TypePtr SemanticAnalyzer::analyzeLaunch(LaunchExpr* launch) {
    if (mInKernel) {
        error("kernel bodies cannot launch another kernel in the initial device ABI",
              launch->line, launch->col);
        return TyUnknown;
    }

    requireInteger(analyzeExpr(launch->threads.get()), "launch thread count");

    FunctionDecl* kernel = nullptr;
    auto family = mFunctionFamilies.find(sourceDeclarationKey(launch->kernelName));
    if (family != mFunctionFamilies.end() && family->second.size() > 1) {
        error("kernel declaration family '" + launch->kernelName +
              "' is ambiguous; dynamic kernel selection requires an explicit future kernel binding operation",
              launch->line, launch->col);
    } else if (family != mFunctionFamilies.end() && !family->second.empty()) {
        kernel = family->second.front();
    } else {
        error("unknown kernel '" + launch->kernelName + "'", launch->line, launch->col);
    }
    if (!kernel) return TyUnknown;
    if (!kernel->isKernel) {
        error("'" + launch->kernelName + "' is a normal function; only `kernel fn` declarations may be launched",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (kernel->params.empty() ||
        !luna::types::sameType(
            resolved(kernel->params.front().inferredType), TyI32)) {
        error("kernel '" + launch->kernelName + "' does not satisfy the required `index: i32` launch ABI",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (launch->args.size() + 1 != kernel->params.size()) {
        error("launch of kernel '" + launch->kernelName + "' expects " +
              std::to_string(kernel->params.size() - 1) + " argument" +
              (kernel->params.size() == 2 ? "" : "s") + " after the implicit index",
              launch->line, launch->col);
        return TyUnknown;
    }

    launch->inFlightResources.clear();
    for (size_t i = 0; i < launch->args.size(); ++i) {
        const TypePtr expected = resolved(kernel->params[i + 1].inferredType);
        const TypePtr actual = analyzeExpr(launch->args[i].get());
        constrain(actual, expected, "launch argument " + std::to_string(i + 1) +
                                   " of kernel '" + launch->kernelName + "'");

        if (expected->kind == TypeKind::Reference && expected->inner &&
            expected->inner->kind == TypeKind::DeviceBuffer) {
            auto* borrow = dynamic_cast<BorrowExpr*>(launch->args[i].get());
            auto* resource = borrow ? dynamic_cast<IdentifierExpr*>(borrow->operand.get()) : nullptr;
            if (!borrow || !resource) {
                error("device-buffer launch argument " + std::to_string(i + 1) +
                      " must be an explicit borrow of a named buffer", launch->line, launch->col);
            } else {
                if (borrow->isMutable != expected->isMutable) {
                    error("device-buffer launch argument " + std::to_string(i + 1) +
                          (expected->isMutable ? " requires `borrow mut`" :
                                                 " requires a shared `borrow`"),
                          launch->line, launch->col);
                }
                launch->inFlightResources.emplace_back(resource->name, expected->isMutable);
            }
        }
    }

    launch->resolvedKernelName = kernel->generatedSymbolName.empty()
        ? kernel->name : kernel->generatedSymbolName;
    return TyEvent;
}

TypePtr SemanticAnalyzer::analyzeReflectionCall(CallExpr* call, const std::string& name) {
    const bool isBinaryRelation = name == "type_same" ||
        name == "type_same_shape" || name == "type_abi_compatible";
    if (isBinaryRelation) {
        if (call->typeArgASTs.size() != 2 || !call->args.empty()) {
            error(name + " expects exactly two type arguments and no value arguments",
                  call->line, call->col);
            return TyUnknown;
        }
        auto lhs = resolved(resolveTypeAST(call->typeArgASTs[0].get(), {}));
        auto rhs = resolved(resolveTypeAST(call->typeArgASTs[1].get(), {}));
        if (name == "type_same")
            call->compileTimeValue = luna::types::sameType(lhs, rhs);
        else if (name == "type_same_shape")
            call->compileTimeValue = luna::types::sameShape(lhs, rhs);
        else
            call->compileTimeValue = luna::types::isAbiCompatible(lhs, rhs);
        return TyBool;
    }

    TypePtr type;
    if (!call->typeArgASTs.empty()) {
        if (call->typeArgASTs.size() != 1) {
            error(name + " expects exactly one type argument");
            return TyUnknown;
        }
        type = resolved(resolveTypeAST(call->typeArgASTs[0].get(), {}));
    } else if (call->args.size() == 1) {
        type = resolved(analyzeExpr(call->args[0].get()));
    } else {
        error(name + " expects either `<Type>()` or one value argument",
              call->line, call->col);
        return TyUnknown;
    }

    auto kindName = [](const TypePtr& t) {
        switch (t->kind) {
            case TypeKind::I8: case TypeKind::I16: case TypeKind::I32: case TypeKind::I64:
            case TypeKind::U8: case TypeKind::U16: case TypeKind::U32: case TypeKind::U64:
            case TypeKind::USize: case TypeKind::ISize: return std::string("integer");
            case TypeKind::F32: case TypeKind::F64: return std::string("float");
            case TypeKind::Bool: return std::string("bool");
            case TypeKind::String: case TypeKind::CStr: return std::string("string");
            case TypeKind::RawPointer: return std::string("raw_pointer");
            case TypeKind::Rc: return std::string("rc");
            case TypeKind::Arc: return std::string("arc");
            case TypeKind::Reference: return std::string("reference");
            case TypeKind::Function: return std::string("function");
            case TypeKind::Struct: return std::string("struct");
            case TypeKind::Record: return std::string("record");
            case TypeKind::Enum: return std::string("enum");
            case TypeKind::Result: return std::string("result");
            case TypeKind::Trait: return std::string("trait");
            case TypeKind::Unit: return std::string("unit");
            case TypeKind::Never: return std::string("never");
            default: return std::string("unknown");
        }
    };
    std::function<int64_t(const TypePtr&)> typeSize = [&](const TypePtr& t) -> int64_t {
        switch (t->kind) {
            case TypeKind::I8: case TypeKind::U8: case TypeKind::Bool: return 1;
            case TypeKind::I16: case TypeKind::U16: return 2;
            case TypeKind::I32: case TypeKind::U32: case TypeKind::F32: return 4;
            case TypeKind::I64: case TypeKind::U64: case TypeKind::USize: case TypeKind::ISize:
            case TypeKind::F64: case TypeKind::String: case TypeKind::CStr:
            case TypeKind::RawPointer: case TypeKind::Reference:
            case TypeKind::Rc: case TypeKind::Arc: return 8;
            case TypeKind::Array:
                return static_cast<int64_t>(t->arrayLength) *
                    typeSize(t->inner);
            case TypeKind::Slice: return 16;
            case TypeKind::Struct: case TypeKind::Record: {
                int64_t total = 0;
                for (const auto& field : t->fields) total += typeSize(field.type);
                return total == 0 ? 8 : total;
            }
            case TypeKind::Enum:
            case TypeKind::Result:
                return static_cast<int64_t>(
                    luna::layout::valueSize(t));
            default: return 0;
        }
    };
    auto constIndex = [&]() -> std::optional<size_t> {
        if (call->args.size() != 1) {
            error(name + " requires one compile-time integer index");
            return std::nullopt;
        }
        analyzeExpr(call->args[0].get());
        auto value = evaluateConstExpr(call->args[0].get());
        if (!value || !std::holds_alternative<int64_t>(*value) || std::get<int64_t>(*value) < 0) {
            error(name + " requires a non-negative compile-time integer index");
            return std::nullopt;
        }
        return static_cast<size_t>(std::get<int64_t>(*value));
    };

    if (name == "type_of") call->compileTimeValue = type->toString();
    else if (name == "type_kind") call->compileTimeValue = kindName(type);
    else if (name == "type_id")
        call->compileTimeValue = luna::types::typeId(type).value;
    else if (name == "type_shape")
        call->compileTimeValue = luna::types::shapeId(type).value;
    else if (name == "type_domain") {
        switch (type->domain) {
            case luna::types::TypeDomain::Value: call->compileTimeValue = std::string("value"); break;
            case luna::types::TypeDomain::Meta: call->compileTimeValue = std::string("meta"); break;
            case luna::types::TypeDomain::Compiler: call->compileTimeValue = std::string("compiler"); break;
            case luna::types::TypeDomain::Inference: call->compileTimeValue = std::string("inference"); break;
            case luna::types::TypeDomain::Error: call->compileTimeValue = std::string("error"); break;
        }
    }
    else if (name == "type_nominal") call->compileTimeValue = type->nominalId;
    else if (name == "type_size") call->compileTimeValue = typeSize(type);
    else if (name == "type_is_struct") call->compileTimeValue = type->kind == TypeKind::Struct;
    else if (name == "type_is_enum") call->compileTimeValue = type->kind == TypeKind::Enum;
    else if (name == "type_is_nominal") call->compileTimeValue = !type->nominalId.empty();
    else if (name == "type_is_structural")
        call->compileTimeValue =
            type->identityMode == luna::types::IdentityMode::Structural;
    else if (name == "type_is_meta")
        call->compileTimeValue = type->domain == luna::types::TypeDomain::Meta;
    else if (name == "type_is_reference") call->compileTimeValue = type->kind == TypeKind::Reference;
    else if (name == "type_field_count") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            error(name + " requires a struct or record type"); return TyUnknown;
        }
        call->compileTimeValue = static_cast<int64_t>(type->fields.size());
    } else if (name == "type_field_name" || name == "type_field_type") {
        if (type->kind != TypeKind::Struct && type->kind != TypeKind::Record) {
            error(name + " requires a struct or record type"); return TyUnknown;
        }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->fields.size()) {
            error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_field_name"
            ? type->fields[*index].name : type->fields[*index].type->toString();
    } else if (name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) { error(name + " requires an enum type"); return TyUnknown; }
        call->compileTimeValue = static_cast<int64_t>(type->variants.size());
    } else if (name == "type_variant_name" || name == "type_variant_field_count") {
        if (type->kind != TypeKind::Enum) { error(name + " requires an enum type"); return TyUnknown; }
        auto index = constIndex();
        if (!index) return TyUnknown;
        if (*index >= type->variants.size()) {
            error(name + " index " + std::to_string(*index) + " is out of range"); return TyUnknown;
        }
        call->compileTimeValue = name == "type_variant_name"
            ? std::variant<int64_t, double, bool, std::string>(type->variants[*index].name)
            : std::variant<int64_t, double, bool, std::string>(static_cast<int64_t>(type->variants[*index].fields.size()));
    }

    if (name == "type_size" || name == "type_field_count" || name == "type_variant_count" ||
        name == "type_variant_field_count") return TyI32;
    if (name == "type_is_struct" || name == "type_is_enum" ||
        name == "type_is_nominal" || name == "type_is_structural" ||
        name == "type_is_meta" || name == "type_is_reference") return TyBool;
    return TyString;
}

TypePtr SemanticAnalyzer::analyzeDeclarationReflectionCall(
    CallExpr* call, const std::string& name) {
    if (name == "declaration_of") {
        if (call->args.size() != 1) {
            error("declaration_of expects exactly one declaration name",
                  call->line, call->col);
            return TyUnknown;
        }
        auto* identifier =
            dynamic_cast<IdentifierExpr*>(call->args.front().get());
        if (!identifier) {
            error("declaration_of requires a statically named declaration",
                  call->line, call->col);
            return TyUnknown;
        }
        auto family = mFunctionFamilies.find(
            sourceDeclarationKey(identifier->name));
        if (family == mFunctionFamilies.end() || family->second.empty()) {
            error("unknown declaration '" + identifier->name + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        TypePtr requested;
        if (!call->typeArgASTs.empty()) {
            if (call->typeArgASTs.size() != 1) {
                error("declaration_of accepts at most one callable type argument",
                      call->line, call->col);
                return TyUnknown;
            }
            requested = resolved(
                resolveTypeAST(call->typeArgASTs.front().get(), {}));
            if (requested->kind != TypeKind::Function)
                error("declaration_of type argument must be a callable type",
                      call->line, call->col);
        }

        FunctionDecl* selected = nullptr;
        TypePtr selectedType;
        for (auto* candidate : family->second) {
            TypeVec parameters;
            std::vector<luna::ownership::Contract> contracts;
            for (const auto& parameter : candidate->params) {
                parameters.push_back(resolved(parameter.inferredType));
                contracts.push_back({parameter.relation, parameter.usage});
            }
            TypePtr callable = Type::makeFunction(
                std::move(parameters), resolved(candidate->inferredReturnType),
                std::move(contracts),
                {luna::ownership::Relation::Owned, candidate->returnUsage});
            if (requested && !luna::types::sameType(requested, callable))
                continue;
            if (selected) {
                error("declaration_of '" + identifier->name +
                      "' is ambiguous; provide a unique callable signature "
                      "or use select for an open declaration family",
                      call->line, call->col);
                return TyUnknown;
            }
            selected = candidate;
            selectedType = callable;
        }
        if (!selected) {
            error("declaration_of found no declaration matching the requested "
                  "signature for '" + identifier->name + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto symbol = selected->generatedSymbolName.empty()
            ? selected->name : selected->generatedSymbolName;
        call->compileTimeDeclarationId = nominalDeclarationIdentity(
            mProgram, "fn", symbol, selected);
        call->resolvedSymbolName = symbol;
        return Type::makeDeclarationRef(selectedType);
    }

    if (call->args.size() != 1) {
        error(name + " expects exactly one declaration_ref",
              call->line, call->col);
        return TyUnknown;
    }
    TypePtr reference = resolved(analyzeExpr(call->args.front().get()));
    if (reference->kind != TypeKind::DeclarationRef) {
        error(name + " expects a declaration_ref",
              call->line, call->col);
        return TyUnknown;
    }
    if (auto* nested =
            dynamic_cast<CallExpr*>(call->args.front().get());
        nested && !nested->compileTimeDeclarationId.empty()) {
        if (name == "declaration_id")
            call->compileTimeValue = nested->compileTimeDeclarationId;
        else if (reference->inner)
            call->compileTimeValue =
                luna::types::typeId(reference->inner).value;
    } else if (auto* identifier =
                   dynamic_cast<IdentifierExpr*>(call->args.front().get())) {
        auto* symbol = mSymTable.lookup(identifier->name);
        if (symbol && !symbol->compileTimeDeclarationId.empty()) {
            if (name == "declaration_id")
                call->compileTimeValue =
                    symbol->compileTimeDeclarationId;
            else if (reference->inner)
                call->compileTimeValue =
                    luna::types::typeId(reference->inner).value;
        }
    }
    return TyString;
}

void SemanticAnalyzer::enterConstScope() { mConstScopes.emplace_back(); }

void SemanticAnalyzer::exitConstScope() {
    if (mConstScopes.size() > 1) mConstScopes.pop_back();
}

void SemanticAnalyzer::defineConst(const std::string& name, const ConstValue& value) {
    if (mConstScopes.empty()) enterConstScope();
    mConstScopes.back()[name] = value;
}

const SemanticAnalyzer::ConstValue* SemanticAnalyzer::lookupConst(const std::string& name) const {
    for (auto it = mConstScopes.rbegin(); it != mConstScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evaluateConstExpr(Expr* expr, const std::unordered_map<std::string, ConstValue>& locals) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = locals.find(value->name);
        if (local != locals.end()) return local->second;
        if (auto* global = lookupConst(value->name)) return *global;
        return std::nullopt;
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        if (call->compileTimeValue) return *call->compileTimeValue;
        auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        if (!callee) return std::nullopt;
        const std::string& constexprName = call->resolvedSymbolName.empty()
            ? callee->name : call->resolvedSymbolName;
        auto function = mConstexprFunctions.find(constexprName);
        if (function == mConstexprFunctions.end()) return std::nullopt;
        std::vector<ConstValue> args;
        for (auto& arg : call->args) {
            auto value = evaluateConstExpr(arg.get(), locals);
            if (!value) return std::nullopt;
            args.push_back(std::move(*value));
        }
        return evaluateConstFunction(function->second, args);
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateConstExpr(unary->operand.get(), locals);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Minus) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return -*i;
            if (auto* f = std::get_if<double>(&*operand)) return -*f;
        }
        if (unary->op == TokenKind::Not) {
            if (auto* b = std::get_if<bool>(&*operand)) return !*b;
        }
        if (unary->op == TokenKind::Tilde) {
            if (auto* i = std::get_if<int64_t>(&*operand)) return ~*i;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateConstExpr(binary->lhs.get(), locals);
        auto rhs = evaluateConstExpr(binary->rhs.get(), locals);
        if (!lhs || !rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs); auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri; case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri; case TokenKind::Slash: if (*ri) return *li / *ri; break;
                case TokenKind::Percent: if (*ri) return *li % *ri; break;
                case TokenKind::Ampersand: return *li & *ri; case TokenKind::BitOr: return *li | *ri;
                case TokenKind::BitXor: return *li ^ *ri; case TokenKind::ShiftLeft: return *li << *ri;
                case TokenKind::ShiftRight: return *li >> *ri; case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri; case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri; case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri; default: break;
            }
        }
        auto lf = std::get_if<double>(&*lhs); auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
                case TokenKind::Plus: return *lf + *rf; case TokenKind::Minus: return *lf - *rf;
                case TokenKind::Star: return *lf * *rf; case TokenKind::Slash: if (*rf != 0) return *lf / *rf; break;
                case TokenKind::Percent: if (*rf != 0) return std::fmod(*lf, *rf); break;
                case TokenKind::EqEq: return *lf == *rf; case TokenKind::Neq: return *lf != *rf;
                case TokenKind::Lt: return *lf < *rf; case TokenKind::LtEq: return *lf <= *rf;
                case TokenKind::Gt: return *lf > *rf; case TokenKind::GtEq: return *lf >= *rf;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs); auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
    }
    return std::nullopt;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evaluateConstFunction(FunctionDecl* function, const std::vector<ConstValue>& args) {
    if (!function || !function->isConstexpr || function->params.size() != args.size())
        return std::nullopt;
    if (++mConstEvaluationDepth > 128) {
        --mConstEvaluationDepth;
        return std::nullopt;
    }
    std::unordered_map<std::string, ConstValue> locals;
    for (size_t i = 0; i < args.size(); ++i) locals[function->params[i].name] = args[i];
    std::optional<ConstValue> result;
    bool completed = evaluateConstBlock(function->body.get(), locals, result);
    --mConstEvaluationDepth;
    return completed ? result : std::nullopt;
}

bool SemanticAnalyzer::evaluateConstBlock(BlockStmt* block,
                                          std::unordered_map<std::string, ConstValue>& locals,
                                          std::optional<ConstValue>& result) {
    if (!block) return false;
    for (auto& statement : block->stmts) {
        if (auto* let = dynamic_cast<LetStmt*>(statement.get())) {
            auto value = evaluateConstExpr(let->initializer.get(), locals);
            if (!value) return false;
            locals[let->name] = *value;
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(statement.get())) {
            if (!ret->value) return false;
            result = evaluateConstExpr(ret->value.get(), locals);
            return result.has_value();
        } else if (auto* conditional = dynamic_cast<IfStmt*>(statement.get())) {
            auto condition = evaluateConstExpr(conditional->cond.get(), locals);
            auto boolValue = condition ? std::get_if<bool>(&*condition) : nullptr;
            if (!boolValue) return false;
            if (*boolValue) return evaluateConstBlock(conditional->thenBlock.get(), locals, result);
            if (auto* elseBlock = dynamic_cast<BlockStmt*>(conditional->elseBranch.get()))
                return evaluateConstBlock(elseBlock, locals, result);
            return false;
        } else {
            return false;
        }
    }
    return false;
}

std::optional<SemanticAnalyzer::ConstValue>
SemanticAnalyzer::evaluateConstraintExpr(
    Expr* expr, const std::unordered_map<std::string, TypePtr>& bindings,
    std::vector<std::string>& active) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr)) {
        if (auto* value = lookupConst(identifier->name)) return *value;
        return std::nullopt;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateConstraintExpr(unary->operand.get(), bindings, active);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Not) {
            if (auto* value = std::get_if<bool>(&*operand)) return !*value;
        }
        if (unary->op == TokenKind::Minus) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return -*value;
            if (auto* value = std::get_if<double>(&*operand)) return -*value;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateConstraintExpr(binary->lhs.get(), bindings, active);
        if (!lhs) return std::nullopt;
        if (binary->op == TokenKind::AndAnd) {
            auto* boolean = std::get_if<bool>(&*lhs);
            if (!boolean) return std::nullopt;
            if (!*boolean) return false;
        }
        if (binary->op == TokenKind::OrOr) {
            auto* boolean = std::get_if<bool>(&*lhs);
            if (!boolean) return std::nullopt;
            if (*boolean) return true;
        }
        auto rhs = evaluateConstraintExpr(binary->rhs.get(), bindings, active);
        if (!rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs);
        auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri;
                case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri;
                case TokenKind::Slash: if (*ri != 0) return *li / *ri; break;
                case TokenKind::Percent: if (*ri != 0) return *li % *ri; break;
                case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri;
                case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri;
                case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs);
        auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
        auto ls = std::get_if<std::string>(&*lhs);
        auto rs = std::get_if<std::string>(&*rhs);
        if (ls && rs) {
            if (binary->op == TokenKind::EqEq) return *ls == *rs;
            if (binary->op == TokenKind::Neq) return *ls != *rs;
        }
        return std::nullopt;
    }
    auto* call = dynamic_cast<CallExpr*>(expr);
    auto* callee = call
        ? dynamic_cast<IdentifierExpr*>(call->callee.get()) : nullptr;
    if (!call || !callee || !call->args.empty()) return std::nullopt;

    const std::string conceptKey =
        sourceDeclarationKey(callee->name, false);
    if (mConcepts.count(conceptKey)) {
        TypeVec arguments;
        for (auto& argument : call->typeArgASTs)
            arguments.push_back(resolved(
                resolveTypeAST(argument.get(), bindings)));
        auto value = evaluateConstraint(conceptKey, arguments, active);
        return value ? std::optional<ConstValue>(*value) : std::nullopt;
    }

    auto resolveArgument = [&](size_t index) -> TypePtr {
        if (index >= call->typeArgASTs.size()) return TyUnknown;
        return resolved(resolveTypeAST(
            call->typeArgASTs[index].get(), bindings));
    };
    const bool binaryRelation = callee->name == "type_same" ||
        callee->name == "type_same_shape" ||
        callee->name == "type_abi_compatible";
    if (binaryRelation) {
        if (call->typeArgASTs.size() != 2) return std::nullopt;
        TypePtr lhs = resolveArgument(0);
        TypePtr rhs = resolveArgument(1);
        if (callee->name == "type_same")
            return luna::types::sameType(lhs, rhs);
        if (callee->name == "type_same_shape")
            return luna::types::sameShape(lhs, rhs);
        return luna::types::isAbiCompatible(lhs, rhs);
    }
    if (call->typeArgASTs.size() != 1) return std::nullopt;
    TypePtr type = resolveArgument(0);
    if (!type || type->kind == TypeKind::Unknown ||
        type->kind == TypeKind::TypeParam ||
        type->kind == TypeKind::InferenceVar)
        return std::nullopt;
    if (callee->name == "type_is_struct")
        return type->kind == TypeKind::Struct;
    if (callee->name == "type_is_enum")
        return type->kind == TypeKind::Enum;
    if (callee->name == "type_is_nominal")
        return !type->nominalId.empty();
    if (callee->name == "type_is_structural")
        return type->identityMode == luna::types::IdentityMode::Structural;
    if (callee->name == "type_is_meta")
        return type->domain == luna::types::TypeDomain::Meta;
    if (callee->name == "type_is_reference")
        return type->kind == TypeKind::Reference;
    if (callee->name == "type_field_count") {
        if (type->kind != TypeKind::Struct &&
            type->kind != TypeKind::Record)
            return std::nullopt;
        return static_cast<int64_t>(type->fields.size());
    }
    if (callee->name == "type_variant_count") {
        if (type->kind != TypeKind::Enum) return std::nullopt;
        return static_cast<int64_t>(type->variants.size());
    }
    if (callee->name == "type_id")
        return luna::types::typeId(type).value;
    if (callee->name == "type_shape")
        return luna::types::shapeId(type).value;
    if (callee->name == "type_size") {
        std::function<int64_t(const TypePtr&)> sizeOf =
            [&](const TypePtr& item) -> int64_t {
            switch (item->kind) {
                case TypeKind::I8: case TypeKind::U8:
                case TypeKind::Bool: return 1;
                case TypeKind::I16: case TypeKind::U16: return 2;
                case TypeKind::I32: case TypeKind::U32:
                case TypeKind::F32: return 4;
                case TypeKind::I64: case TypeKind::U64:
                case TypeKind::USize: case TypeKind::ISize:
                case TypeKind::F64: case TypeKind::String:
                case TypeKind::CStr: case TypeKind::RawPointer:
                case TypeKind::Reference: case TypeKind::Rc:
                case TypeKind::Arc: return 8;
                case TypeKind::Array:
                    return static_cast<int64_t>(item->arrayLength) *
                        sizeOf(item->inner);
                case TypeKind::Struct: case TypeKind::Record: {
                    int64_t total = 0;
                    for (const auto& field : item->fields)
                        total += sizeOf(field.type);
                    return total;
                }
                case TypeKind::Enum:
                case TypeKind::Result:
                    return static_cast<int64_t>(
                        luna::layout::valueSize(item));
                default: return 0;
            }
        };
        return sizeOf(type);
    }
    return std::nullopt;
}

std::optional<bool> SemanticAnalyzer::evaluateConstraint(
    const std::string& name, const TypeVec& arguments,
    std::vector<std::string>& active) {
    auto found = mConcepts.find(name);
    if (found == mConcepts.end()) {
        const auto key = sourceDeclarationKey(name, false);
        found = mConcepts.find(key);
    }
    if (found == mConcepts.end() ||
        found->second->typeParams.size() != arguments.size())
        return std::nullopt;
    if (std::find(active.begin(), active.end(), found->first) != active.end())
        return std::nullopt;
    active.push_back(found->first);
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t index = 0; index < arguments.size(); ++index)
        bindings[found->second->typeParams[index]] = arguments[index];
    auto value = evaluateConstraintExpr(
        found->second->predicate.get(), bindings, active);
    active.pop_back();
    if (!value) return std::nullopt;
    if (auto* boolean = std::get_if<bool>(&*value)) return *boolean;
    return std::nullopt;
}

std::optional<SemanticAnalyzer::SelectorValue>
SemanticAnalyzer::evaluateSelectorExpr(
    Expr* expr, std::unordered_map<std::string, SelectorValue>& locals) {
    if (!expr) return std::nullopt;
    if (auto* value = dynamic_cast<IntLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<FloatLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<BoolLiteralExpr*>(expr)) return value->value;
    if (auto* value = dynamic_cast<StringLiteralExpr*>(expr)) return value->value;
    if (auto* identifier = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = locals.find(identifier->name);
        if (local != locals.end()) return local->second;
        if (auto* value = lookupConst(identifier->name))
            return std::visit([](const auto& item) -> SelectorValue {
                return item;
            }, *value);
        return std::nullopt;
    }
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr)) {
        auto object = evaluateSelectorExpr(field->object.get(), locals);
        auto* metadata = object
            ? std::get_if<SelectorMetadataValue>(&*object) : nullptr;
        if (!metadata) return std::nullopt;
        for (const auto& [key, schema] : mMetadataSchemas) {
            const auto symbol = schema->generatedSymbolName.empty()
                ? schema->name : schema->generatedSymbolName;
            if (nominalDeclarationIdentity(
                    mProgram, "meta", symbol, schema) != metadata->schemaId)
                continue;
            for (size_t index = 0; index < schema->fields.size(); ++index) {
                if (schema->fields[index].name != field->field ||
                    index >= metadata->fields.size())
                    continue;
                return std::visit([](const auto& item) -> SelectorValue {
                    return item;
                }, metadata->fields[index]);
            }
        }
        return std::nullopt;
    }
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        auto operand = evaluateSelectorExpr(unary->operand.get(), locals);
        if (!operand) return std::nullopt;
        if (unary->op == TokenKind::Not) {
            if (auto* value = std::get_if<bool>(&*operand)) return !*value;
        } else if (unary->op == TokenKind::Minus) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return -*value;
            if (auto* value = std::get_if<double>(&*operand)) return -*value;
        } else if (unary->op == TokenKind::Tilde) {
            if (auto* value = std::get_if<int64_t>(&*operand)) return ~*value;
        }
        return std::nullopt;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        auto lhs = evaluateSelectorExpr(binary->lhs.get(), locals);
        if (!lhs) return std::nullopt;
        if (binary->op == TokenKind::AndAnd) {
            auto* value = std::get_if<bool>(&*lhs);
            if (!value) return std::nullopt;
            if (!*value) return false;
        } else if (binary->op == TokenKind::OrOr) {
            auto* value = std::get_if<bool>(&*lhs);
            if (!value) return std::nullopt;
            if (*value) return true;
        }
        auto rhs = evaluateSelectorExpr(binary->rhs.get(), locals);
        if (!rhs) return std::nullopt;
        auto li = std::get_if<int64_t>(&*lhs);
        auto ri = std::get_if<int64_t>(&*rhs);
        if (li && ri) {
            switch (binary->op) {
                case TokenKind::Plus: return *li + *ri;
                case TokenKind::Minus: return *li - *ri;
                case TokenKind::Star: return *li * *ri;
                case TokenKind::Slash: if (*ri != 0) return *li / *ri; break;
                case TokenKind::Percent: if (*ri != 0) return *li % *ri; break;
                case TokenKind::EqEq: return *li == *ri;
                case TokenKind::Neq: return *li != *ri;
                case TokenKind::Lt: return *li < *ri;
                case TokenKind::LtEq: return *li <= *ri;
                case TokenKind::Gt: return *li > *ri;
                case TokenKind::GtEq: return *li >= *ri;
                case TokenKind::Ampersand: return *li & *ri;
                case TokenKind::BitOr: return *li | *ri;
                case TokenKind::BitXor: return *li ^ *ri;
                default: break;
            }
        }
        auto lf = std::get_if<double>(&*lhs);
        auto rf = std::get_if<double>(&*rhs);
        if (lf && rf) {
            switch (binary->op) {
                case TokenKind::Plus: return *lf + *rf;
                case TokenKind::Minus: return *lf - *rf;
                case TokenKind::Star: return *lf * *rf;
                case TokenKind::Slash: if (*rf != 0.0) return *lf / *rf; break;
                case TokenKind::EqEq: return *lf == *rf;
                case TokenKind::Neq: return *lf != *rf;
                case TokenKind::Lt: return *lf < *rf;
                case TokenKind::LtEq: return *lf <= *rf;
                case TokenKind::Gt: return *lf > *rf;
                case TokenKind::GtEq: return *lf >= *rf;
                default: break;
            }
        }
        auto lb = std::get_if<bool>(&*lhs);
        auto rb = std::get_if<bool>(&*rhs);
        if (lb && rb) {
            if (binary->op == TokenKind::AndAnd) return *lb && *rb;
            if (binary->op == TokenKind::OrOr) return *lb || *rb;
            if (binary->op == TokenKind::EqEq) return *lb == *rb;
            if (binary->op == TokenKind::Neq) return *lb != *rb;
        }
        auto ls = std::get_if<std::string>(&*lhs);
        auto rs = std::get_if<std::string>(&*rhs);
        if (ls && rs) {
            if (binary->op == TokenKind::EqEq) return *ls == *rs;
            if (binary->op == TokenKind::Neq) return *ls != *rs;
            if (binary->op == TokenKind::Lt) return *ls < *rs;
            if (binary->op == TokenKind::LtEq) return *ls <= *rs;
            if (binary->op == TokenKind::Gt) return *ls > *rs;
            if (binary->op == TokenKind::GtEq) return *ls >= *rs;
        }
        return std::nullopt;
    }
    if (auto* assignment = dynamic_cast<AssignExpr*>(expr)) {
        auto* identifier = dynamic_cast<IdentifierExpr*>(assignment->lhs.get());
        if (!identifier || !locals.count(identifier->name)) return std::nullopt;
        auto value = evaluateSelectorExpr(assignment->rhs.get(), locals);
        if (!value) return std::nullopt;
        if (assignment->op == TokenKind::Eq) {
            locals[identifier->name] = *value;
            return *value;
        }
        const TokenKind operation =
            assignment->op == TokenKind::PlusEq ? TokenKind::Plus :
            assignment->op == TokenKind::MinusEq ? TokenKind::Minus :
            assignment->op == TokenKind::StarEq ? TokenKind::Star :
            assignment->op == TokenKind::SlashEq ? TokenKind::Slash :
            TokenKind::Percent;
        auto current = locals[identifier->name];
        auto li = std::get_if<int64_t>(&current);
        auto ri = std::get_if<int64_t>(&*value);
        if (!li || !ri) return std::nullopt;
        int64_t updated = *li;
        if (operation == TokenKind::Plus) updated += *ri;
        else if (operation == TokenKind::Minus) updated -= *ri;
        else if (operation == TokenKind::Star) updated *= *ri;
        else if (operation == TokenKind::Slash && *ri != 0) updated /= *ri;
        else if (operation == TokenKind::Percent && *ri != 0) updated %= *ri;
        else return std::nullopt;
        locals[identifier->name] = updated;
        return updated;
    }
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        if (!callee) return std::nullopt;
        if (callee->name == "declaration_of" && call->args.size() == 1) {
            auto* target =
                dynamic_cast<IdentifierExpr*>(call->args.front().get());
            if (!target) return std::nullopt;
            auto family = mFunctionFamilies.find(
                sourceDeclarationKey(target->name, false));
            if (family == mFunctionFamilies.end()) return std::nullopt;
            TypePtr requested;
            if (call->typeArgASTs.size() == 1)
                requested = resolved(resolveTypeAST(
                    call->typeArgASTs.front().get(), {}));
            FunctionDecl* selected = nullptr;
            for (auto* candidate : family->second) {
                TypeVec parameters;
                std::vector<luna::ownership::Contract> contracts;
                for (const auto& parameter : candidate->params) {
                    parameters.push_back(resolved(parameter.inferredType));
                    contracts.push_back(
                        {parameter.relation, parameter.usage});
                }
                TypePtr callable = Type::makeFunction(
                    std::move(parameters),
                    resolved(candidate->inferredReturnType),
                    std::move(contracts),
                    {luna::ownership::Relation::Owned,
                     candidate->returnUsage});
                if (requested &&
                    !luna::types::sameType(requested, callable))
                    continue;
                if (selected) return std::nullopt;
                selected = candidate;
            }
            if (!selected) return std::nullopt;
            const auto symbol = selected->generatedSymbolName.empty()
                ? selected->name : selected->generatedSymbolName;
            return SelectorDeclarationValue{
                nominalDeclarationIdentity(
                    mProgram, "fn", symbol, selected)};
        }
        std::vector<SelectorValue> arguments;
        for (auto& argument : call->args) {
            auto value = evaluateSelectorExpr(argument.get(), locals);
            if (!value) return std::nullopt;
            arguments.push_back(std::move(*value));
        }
        if (callee->name == "declaration_count" && arguments.size() == 1) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            if (view) return static_cast<int64_t>(view->declarationIds.size());
            return std::nullopt;
        }
        if (callee->name == "declaration_at" && arguments.size() == 2) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            auto* index = std::get_if<int64_t>(&arguments[1]);
            if (!view || !index || *index < 0 ||
                static_cast<size_t>(*index) >= view->declarationIds.size())
                return std::nullopt;
            return SelectorDeclarationValue{
                view->declarationIds[static_cast<size_t>(*index)]};
        }
        if ((callee->name == "declaration_id" ||
             callee->name == "declaration_signature") &&
            arguments.size() == 1) {
            auto* declaration =
                std::get_if<SelectorDeclarationValue>(&arguments[0]);
            if (!declaration) return std::nullopt;
            if (callee->name == "declaration_id")
                return declaration->declarationId;
            const auto* candidate = mActiveSelectorView
                ? mActiveSelectorView->find(declaration->declarationId) : nullptr;
            if (!candidate || !candidate->callableType)
                return std::nullopt;
            return SelectorValue(
                luna::types::typeId(candidate->callableType).value);
        }
        if ((callee->name == "metadata" ||
             callee->name == "declaration_has_metadata") &&
            arguments.size() == 1 && call->typeArgASTs.size() == 1) {
            auto* declaration =
                std::get_if<SelectorDeclarationValue>(&arguments[0]);
            if (!declaration || !mActiveSelectorView) return std::nullopt;
            TypePtr schema = resolved(
                resolveTypeAST(call->typeArgASTs.front().get(), {}));
            const auto* candidate =
                mActiveSelectorView->find(declaration->declarationId);
            if (!candidate || schema->kind != TypeKind::Metadata)
                return std::nullopt;
            SelectorMetadataViewValue matches;
            for (const auto& metadata : candidate->metadata) {
                if (metadata.schemaId == schema->nominalId)
                    matches.values.push_back(
                        {metadata.schemaId, metadata.values});
            }
            if (callee->name == "declaration_has_metadata")
                return !matches.values.empty();
            return matches;
        }
        if (callee->name == "select_unique" && arguments.size() == 2) {
            auto* view = std::get_if<SelectorDeclarationViewValue>(&arguments[0]);
            auto* wanted = std::get_if<SelectorMetadataValue>(&arguments[1]);
            if (!view || !wanted || !mActiveSelectorView) return std::nullopt;
            std::optional<std::string> match;
            for (const auto& id : view->declarationIds) {
                const auto* candidate = mActiveSelectorView->find(id);
                if (!candidate) continue;
                for (const auto& metadata : candidate->metadata) {
                    if (metadata.schemaId != wanted->schemaId ||
                        metadata.values != wanted->fields)
                        continue;
                    if (match) return std::nullopt;
                    match = id;
                }
            }
            return SelectorDeclarationValue{match ? *match : std::string()};
        }
        const std::string metadataKey =
            sourceDeclarationKey(callee->name, false);
        auto metadata = mMetadataSchemas.find(metadataKey);
        if (metadata != mMetadataSchemas.end()) {
            std::vector<ConstValue> fields;
            for (const auto& argument : arguments) {
                if (auto* value = std::get_if<int64_t>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<double>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<bool>(&argument))
                    fields.push_back(*value);
                else if (auto* value = std::get_if<std::string>(&argument))
                    fields.push_back(*value);
                else return std::nullopt;
            }
            const auto symbol = metadata->second->generatedSymbolName.empty()
                ? metadata->second->name
                : metadata->second->generatedSymbolName;
            return SelectorMetadataValue{
                nominalDeclarationIdentity(
                    mProgram, "meta", symbol, metadata->second),
                std::move(fields)};
        }
        auto function = mFunctionFamilies.find(
            sourceDeclarationKey(callee->name, false));
        if (function == mFunctionFamilies.end() ||
            function->second.size() != 1 ||
            !function->second.front()->isConstexpr ||
            function->second.front()->params.size() != arguments.size())
            return std::nullopt;
        if (++mConstEvaluationDepth > 128) {
            --mConstEvaluationDepth;
            return std::nullopt;
        }
        std::unordered_map<std::string, SelectorValue> functionLocals;
        for (size_t index = 0; index < arguments.size(); ++index)
            functionLocals[function->second.front()->params[index].name] =
                arguments[index];
        std::optional<SelectorValue> result;
        bool returned = false;
        const bool evaluated = evaluateSelectorBlock(
                function->second.front()->body.get(), functionLocals,
                result, returned);
        --mConstEvaluationDepth;
        if (!evaluated || !returned)
            return std::nullopt;
        return result;
    }
    return std::nullopt;
}

bool SemanticAnalyzer::evaluateSelectorBlock(
    BlockStmt* block, std::unordered_map<std::string, SelectorValue>& locals,
    std::optional<SelectorValue>& result, bool& returned) {
    if (!block) return false;
    for (auto& statement : block->stmts) {
        if (auto* binding = dynamic_cast<LetStmt*>(statement.get())) {
            auto value = evaluateSelectorExpr(binding->initializer.get(), locals);
            if (!value) return false;
            locals[binding->name] = std::move(*value);
        } else if (auto* ret = dynamic_cast<ReturnStmt*>(statement.get())) {
            if (!ret->value) return false;
            result = evaluateSelectorExpr(ret->value.get(), locals);
            returned = result.has_value();
            return returned;
        } else if (auto* conditional =
                       dynamic_cast<IfStmt*>(statement.get())) {
            auto condition =
                evaluateSelectorExpr(conditional->cond.get(), locals);
            auto* boolean = condition
                ? std::get_if<bool>(&*condition) : nullptr;
            if (!boolean) return false;
            if (*boolean) {
                if (!evaluateSelectorBlock(
                        conditional->thenBlock.get(), locals, result, returned))
                    return false;
            } else if (conditional->elseBranch) {
                if (auto* block = dynamic_cast<BlockStmt*>(
                        conditional->elseBranch.get())) {
                    if (!evaluateSelectorBlock(
                            block, locals, result, returned))
                        return false;
                } else {
                    auto wrapper = dynamic_cast<IfStmt*>(
                        conditional->elseBranch.get());
                    if (!wrapper) return false;
                    auto conditionValue =
                        evaluateSelectorExpr(wrapper->cond.get(), locals);
                    auto* nested = conditionValue
                        ? std::get_if<bool>(&*conditionValue) : nullptr;
                    if (!nested) return false;
                    if (*nested && !evaluateSelectorBlock(
                            wrapper->thenBlock.get(), locals, result, returned))
                        return false;
                    if (!*nested && wrapper->elseBranch) {
                        auto* nestedElse = dynamic_cast<BlockStmt*>(
                            wrapper->elseBranch.get());
                        if (!nestedElse || !evaluateSelectorBlock(
                                nestedElse, locals, result, returned))
                            return false;
                    }
                }
            }
            if (returned) return true;
        } else if (auto* loop = dynamic_cast<ForStmt*>(statement.get())) {
            auto iterable =
                evaluateSelectorExpr(loop->iterable.get(), locals);
            if (!iterable) return false;
            std::vector<SelectorValue> elements;
            if (auto* declarations =
                    std::get_if<SelectorDeclarationViewValue>(&*iterable)) {
                for (const auto& id : declarations->declarationIds)
                    elements.push_back(SelectorDeclarationValue{id});
            } else if (auto* metadata =
                           std::get_if<SelectorMetadataViewValue>(&*iterable)) {
                for (const auto& value : metadata->values)
                    elements.push_back(value);
            } else {
                return false;
            }
            auto previous = locals.find(loop->varName);
            std::optional<SelectorValue> saved =
                previous == locals.end()
                ? std::nullopt
                : std::optional<SelectorValue>(previous->second);
            for (auto& element : elements) {
                locals[loop->varName] = std::move(element);
                if (!evaluateSelectorBlock(
                        loop->body.get(), locals, result, returned))
                    return false;
                if (returned) return true;
            }
            if (saved) locals[loop->varName] = *saved;
            else locals.erase(loop->varName);
        } else if (auto* loop = dynamic_cast<WhileStmt*>(statement.get())) {
            for (size_t iteration = 0; iteration < 10000; ++iteration) {
                auto condition =
                    evaluateSelectorExpr(loop->cond.get(), locals);
                auto* boolean = condition
                    ? std::get_if<bool>(&*condition) : nullptr;
                if (!boolean) return false;
                if (!*boolean) break;
                if (!evaluateSelectorBlock(
                        loop->body.get(), locals, result, returned))
                    return false;
                if (returned) return true;
                if (iteration == 9999) return false;
            }
        } else if (auto* expression =
                       dynamic_cast<ExprStmt*>(statement.get())) {
            if (!evaluateSelectorExpr(expression->expr.get(), locals))
                return false;
        } else {
            return false;
        }
    }
    return true;
}

std::optional<std::string> SemanticAnalyzer::evaluateSelectorFunction(
    FunctionDecl* function, const luna::selector::DeclarationView& view,
    const std::vector<ConstValue>& arguments, std::string& failure) {
    failure.clear();
    if (!function || function->params.size() != arguments.size() + 1) {
        failure = "selector invocation does not match its declaration";
        return std::nullopt;
    }
    std::unordered_map<std::string, SelectorValue> locals;
    SelectorDeclarationViewValue input;
    for (const auto& candidate : view.candidates())
        input.declarationIds.push_back(candidate.declarationId);
    locals[function->params.front().name] = std::move(input);
    for (size_t index = 0; index < arguments.size(); ++index) {
        locals[function->params[index + 1].name] =
            std::visit([](const auto& item) -> SelectorValue {
                return item;
            }, arguments[index]);
    }
    const auto* previousView = mActiveSelectorView;
    const std::string previousPackage = mCurrentPackageId;
    const std::string previousModule = mCurrentModulePath;
    setDeclarationContext(function);
    mActiveSelectorView = &view;
    if (++mConstEvaluationDepth > 128) {
        --mConstEvaluationDepth;
        mActiveSelectorView = previousView;
        mCurrentPackageId = previousPackage;
        mCurrentModulePath = previousModule;
        failure = "selector recursion depth exceeded 128";
        return std::nullopt;
    }
    std::optional<SelectorValue> result;
    bool returned = false;
    const bool evaluated = evaluateSelectorBlock(
        function->body.get(), locals, result, returned);
    --mConstEvaluationDepth;
    mActiveSelectorView = previousView;
    mCurrentPackageId = previousPackage;
    mCurrentModulePath = previousModule;
    if (!evaluated) {
        failure = "selector body is not compile-time evaluable";
        return std::nullopt;
    }
    if (!returned || !result) {
        failure = "selector returned no declaration";
        return std::nullopt;
    }
    auto* declaration = std::get_if<SelectorDeclarationValue>(&*result);
    if (!declaration) {
        failure = "selector result is not a declaration_ref";
        return std::nullopt;
    }
    if (declaration->declarationId.empty()) {
        failure = "selector returned no legal declaration";
        return std::nullopt;
    }
    return declaration->declarationId;
}

FunctionDecl* SemanticAnalyzer::findMatchingImpl(const std::string& traitName,
                                                  const std::string& typeName,
                                                  const std::string& methodName) {
    auto traitIt = mImpls.find(traitName);
    if (traitIt == mImpls.end()) return nullptr;
    auto typeIt = traitIt->second.find(typeName);
    if (typeIt == traitIt->second.end()) return nullptr;
    auto methodIt = typeIt->second.find(methodName);
    if (methodIt == typeIt->second.end()) return nullptr;
    return methodIt->second;
}

// ─── AST cloning helpers for monomorphization ────────────────────

static TypePtr substituteNominalType(
    const TypePtr& type,
    const std::unordered_map<std::string, TypePtr>& bindings);

class MonomorphizationCloner {
public:
    explicit MonomorphizationCloner(
        const std::unordered_map<std::string, TypePtr>& typeBindings)
        : bindings(typeBindings) {}

    std::unique_ptr<BlockStmt> cloneBlock(const BlockStmt* src) {
        if (!src) return nullptr;
        auto clone = located(std::make_unique<BlockStmt>(), src);
        for (const auto& statement : src->stmts) {
            auto clonedStatement = cloneStmt(statement.get());
            if (!clonedStatement) return nullptr;
            clone->stmts.push_back(std::move(clonedStatement));
        }
        return clone;
    }

    const std::string& failure() const { return failureMessage; }

private:
    const std::unordered_map<std::string, TypePtr>& bindings;
    std::string failureMessage;

    template <typename T>
    static std::unique_ptr<T> located(
        std::unique_ptr<T> clone, const ASTNode* source) {
        if (clone && source) {
            clone->sourcePath = source->sourcePath;
            clone->line = source->line;
            clone->col = source->col;
        }
        return clone;
    }

    void fail(const char* category) {
        if (failureMessage.empty())
            failureMessage =
                std::string("unsupported ") + category +
                " AST node in generic function body";
    }

    TypePtr substitute(const TypePtr& type) const {
        return type ? substituteNominalType(type, bindings) : nullptr;
    }

    CleanupObligation cloneCleanup(const CleanupObligation& source) const {
        CleanupObligation clone;
        clone.place = source.place;
        clone.action = source.action;
        clone.type = substitute(source.type);
        return clone;
    }

    std::unique_ptr<TypeAST> cloneType(const TypeAST* src) {
        if (!src) return nullptr;
        if (auto* type = dynamic_cast<const NamedTypeAST*>(src)) {
            auto clone = std::make_unique<NamedTypeAST>(type->name);
            clone->arrayLength = type->arrayLength;
            clone->resolvedType = substitute(type->resolvedType);
            for (const auto& argument : type->typeArgs) {
                auto clonedArgument = cloneType(argument.get());
                if (!clonedArgument) return nullptr;
                clone->typeArgs.push_back(std::move(clonedArgument));
            }
            if (auto bound = bindings.find(type->name);
                bound != bindings.end()) {
                clone->name = bound->second->toString();
                clone->resolvedType = bound->second;
                clone->typeArgs.clear();
                clone->arrayLength.reset();
            }
            return located(std::move(clone), src);
        }
        if (auto* type = dynamic_cast<const RefTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<RefTypeAST>(
                    std::move(inner), type->isMutable),
                src);
        }
        if (auto* type = dynamic_cast<const LinearTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<LinearTypeAST>(std::move(inner)), src);
        }
        if (auto* type = dynamic_cast<const AffineTypeAST*>(src)) {
            auto inner = cloneType(type->inner.get());
            if (!inner) return nullptr;
            return located(
                std::make_unique<AffineTypeAST>(std::move(inner)), src);
        }
        if (auto* type = dynamic_cast<const FunctionTypeAST*>(src)) {
            auto clone = std::make_unique<FunctionTypeAST>();
            for (const auto& parameter : type->paramTypes) {
                auto clonedParameter = cloneType(parameter.get());
                if (!clonedParameter) return nullptr;
                clone->paramTypes.push_back(std::move(clonedParameter));
            }
            clone->returnType = cloneType(type->returnType.get());
            if (type->returnType && !clone->returnType) return nullptr;
            return located(std::move(clone), src);
        }
        fail("type");
        return nullptr;
    }

    Param cloneParam(const Param& source) {
        Param clone;
        clone.name = source.name;
        clone.isLinear = source.isLinear;
        clone.usage = source.usage;
        clone.hasExplicitUsage = source.hasExplicitUsage;
        clone.relation = source.relation;
        clone.type = cloneType(source.type.get());
        clone.inferredType = substitute(source.inferredType);
        return clone;
    }

    std::unique_ptr<Expr> cloneExpr(const Expr* src) {
        if (!src) return nullptr;
        if (auto* expr = dynamic_cast<const IntLiteralExpr*>(src))
            return located(
                std::make_unique<IntLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const FloatLiteralExpr*>(src))
            return located(
                std::make_unique<FloatLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const StringLiteralExpr*>(src))
            return located(
                std::make_unique<StringLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const BoolLiteralExpr*>(src))
            return located(
                std::make_unique<BoolLiteralExpr>(expr->value), src);
        if (auto* expr = dynamic_cast<const IdentifierExpr*>(src))
            return located(
                std::make_unique<IdentifierExpr>(expr->name), src);
        if (auto* expr = dynamic_cast<const BinaryExpr*>(src)) {
            auto clone = std::make_unique<BinaryExpr>();
            clone->lhs = cloneExpr(expr->lhs.get());
            clone->op = expr->op;
            clone->rhs = cloneExpr(expr->rhs.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const UnaryExpr*>(src)) {
            auto clone = std::make_unique<UnaryExpr>();
            clone->op = expr->op;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const CallExpr*>(src)) {
            auto clone = std::make_unique<CallExpr>();
            clone->callee = cloneExpr(expr->callee.get());
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            for (const auto& type : expr->typeArgs)
                clone->typeArgs.push_back(substitute(type));
            for (const auto& type : expr->typeArgASTs)
                clone->typeArgASTs.push_back(cloneType(type.get()));
            clone->resolvedSymbolName = expr->resolvedSymbolName;
            clone->returnsLinear = expr->returnsLinear;
            clone->returnUsage = expr->returnUsage;
            clone->intrinsicType = substitute(expr->intrinsicType);
            clone->resultType = substitute(expr->resultType);
            clone->iteratorInputType = substitute(expr->iteratorInputType);
            clone->iteratorOutputType = substitute(expr->iteratorOutputType);
            clone->iteratorOp = expr->iteratorOp;
            clone->iteratorRecipeStateName =
                expr->iteratorRecipeStateName;
            clone->iteratorRecipeSourceType =
                substitute(expr->iteratorRecipeSourceType);
            clone->iteratorCollectTargetType =
                substitute(expr->iteratorCollectTargetType);
            clone->iteratorCollectBuilderType =
                substitute(expr->iteratorCollectBuilderType);
            clone->iteratorCollectBeginSymbol =
                expr->iteratorCollectBeginSymbol;
            clone->iteratorCollectPushSymbol =
                expr->iteratorCollectPushSymbol;
            clone->iteratorCollectFinishSymbol =
                expr->iteratorCollectFinishSymbol;
            clone->compileTimeValue = expr->compileTimeValue;
            clone->compileTimeDeclarationId =
                expr->compileTimeDeclarationId;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const LaunchExpr*>(src)) {
            auto clone = std::make_unique<LaunchExpr>();
            clone->kernelName = expr->kernelName;
            clone->resolvedKernelName = expr->resolvedKernelName;
            clone->threads = cloneExpr(expr->threads.get());
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->inFlightResources = expr->inFlightResources;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const VariantConstructExpr*>(src)) {
            auto clone = std::make_unique<VariantConstructExpr>();
            clone->typeName = expr->typeName;
            clone->variantName = expr->variantName;
            for (const auto& type : expr->typeArgs)
                clone->typeArgs.push_back(cloneType(type.get()));
            for (const auto& argument : expr->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->constructedType = substitute(expr->constructedType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const FieldAccessExpr*>(src)) {
            auto clone = std::make_unique<FieldAccessExpr>();
            clone->object = cloneExpr(expr->object.get());
            clone->field = expr->field;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const IndexExpr*>(src)) {
            auto clone = std::make_unique<IndexExpr>();
            clone->object = cloneExpr(expr->object.get());
            clone->index = cloneExpr(expr->index.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const ArrayLiteralExpr*>(src)) {
            auto clone = std::make_unique<ArrayLiteralExpr>();
            for (const auto& element : expr->elements)
                clone->elements.push_back(cloneExpr(element.get()));
            clone->elementType = substitute(expr->elementType);
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const HeapAllocExpr*>(src)) {
            auto clone = std::make_unique<HeapAllocExpr>();
            clone->initializer = cloneExpr(expr->initializer.get());
            clone->allocatedTypeAST =
                cloneType(expr->allocatedTypeAST.get());
            clone->allocatedType = substitute(expr->allocatedType);
            clone->resultType = substitute(expr->resultType);
            clone->storage = expr->storage;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const TryExpr*>(src)) {
            auto clone = std::make_unique<TryExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            clone->resultType = substitute(expr->resultType);
            clone->propagatedResultType =
                substitute(expr->propagatedResultType);
            clone->valueType = substitute(expr->valueType);
            clone->errorType = substitute(expr->errorType);
            clone->propagatedErrorType =
                substitute(expr->propagatedErrorType);
            clone->errorConversionSymbol =
                expr->errorConversionSymbol;
            for (const auto& cleanup : expr->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const MoveExpr*>(src)) {
            auto clone = std::make_unique<MoveExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const BorrowExpr*>(src)) {
            auto clone = std::make_unique<BorrowExpr>();
            clone->isMutable = expr->isMutable;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const DerefExpr*>(src)) {
            auto clone = std::make_unique<DerefExpr>();
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const AddrOfExpr*>(src)) {
            auto clone = std::make_unique<AddrOfExpr>();
            clone->isMutable = expr->isMutable;
            clone->operand = cloneExpr(expr->operand.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const BlockExpr*>(src)) {
            auto block = cloneBlock(expr->block.get());
            if (!block) return nullptr;
            return located(
                std::make_unique<BlockExpr>(std::move(block)), src);
        }
        if (auto* expr = dynamic_cast<const IfExpr*>(src)) {
            auto clone = std::make_unique<IfExpr>();
            clone->cond = cloneExpr(expr->cond.get());
            clone->thenExpr = cloneExpr(expr->thenExpr.get());
            clone->elseExpr = cloneExpr(expr->elseExpr.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const LambdaExpr*>(src)) {
            auto clone = std::make_unique<LambdaExpr>();
            for (const auto& parameter : expr->params)
                clone->params.push_back(cloneParam(parameter));
            clone->returnType = cloneType(expr->returnType.get());
            clone->body = cloneBlock(expr->body.get());
            clone->closureType = substitute(expr->closureType);
            clone->captures = expr->captures;
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const AssignExpr*>(src)) {
            auto clone = std::make_unique<AssignExpr>();
            clone->op = expr->op;
            clone->lhs = cloneExpr(expr->lhs.get());
            clone->rhs = cloneExpr(expr->rhs.get());
            return located(std::move(clone), src);
        }
        if (auto* expr = dynamic_cast<const SelectExpr*>(src)) {
            auto clone = std::make_unique<SelectExpr>();
            clone->targetName = expr->targetName;
            clone->selectorName = expr->selectorName;
            for (const auto& argument : expr->selectorArgs)
                clone->selectorArgs.push_back(cloneExpr(argument.get()));
            clone->isDynamic = expr->isDynamic;
            clone->resolvedDeclarationId =
                expr->resolvedDeclarationId;
            clone->resolvedSymbolName = expr->resolvedSymbolName;
            clone->resolvedFamilyId = expr->resolvedFamilyId;
            clone->resolvedSelectorDeclarationId =
                expr->resolvedSelectorDeclarationId;
            clone->dynamicCandidateIds = expr->dynamicCandidateIds;
            clone->dynamicMetadataSchemaId =
                expr->dynamicMetadataSchemaId;
            clone->dynamicFilterArguments =
                expr->dynamicFilterArguments;
            clone->dynamicCandidates = expr->dynamicCandidates;
            clone->selectedType = substitute(expr->selectedType);
            return located(std::move(clone), src);
        }
        fail("expression");
        return nullptr;
    }

    std::unique_ptr<Stmt> cloneStmt(const Stmt* src) {
        if (!src) return nullptr;
        if (auto* statement = dynamic_cast<const BlockStmt*>(src))
            return cloneBlock(statement);
        if (auto* statement = dynamic_cast<const LetStmt*>(src)) {
            auto clone = std::make_unique<LetStmt>();
            clone->name = statement->name;
            clone->isConst = statement->isConst;
            clone->isLinear = statement->isLinear;
            clone->usage = statement->usage;
            clone->hasExplicitUsage = statement->hasExplicitUsage;
            clone->typeAnnotation =
                cloneType(statement->typeAnnotation.get());
            clone->initializer =
                cloneExpr(statement->initializer.get());
            clone->inferredType = substitute(statement->inferredType);
            clone->materializesIteratorRecipe =
                statement->materializesIteratorRecipe;
            clone->materializedIteratorOwnsSource =
                statement->materializedIteratorOwnsSource;
            clone->materializedIteratorSourceType =
                substitute(statement->materializedIteratorSourceType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ReturnStmt*>(src)) {
            auto clone = std::make_unique<ReturnStmt>();
            clone->value = cloneExpr(statement->value.get());
            clone->autoFrees = statement->autoFrees;
            for (const auto& cleanup : statement->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ExprStmt*>(src)) {
            auto clone = std::make_unique<ExprStmt>();
            clone->expr = cloneExpr(statement->expr.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const IfStmt*>(src)) {
            auto clone = std::make_unique<IfStmt>();
            clone->cond = cloneExpr(statement->cond.get());
            clone->thenBlock = cloneBlock(statement->thenBlock.get());
            clone->elseBranch = cloneStmt(statement->elseBranch.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const MatchStmt*>(src)) {
            auto clone = std::make_unique<MatchStmt>();
            clone->scrutinee = cloneExpr(statement->scrutinee.get());
            clone->matchedType = substitute(statement->matchedType);
            for (const auto& sourceArm : statement->arms) {
                MatchArm arm;
                arm.sourcePath = sourceArm.sourcePath;
                arm.line = sourceArm.line;
                arm.col = sourceArm.col;
                arm.typeQualifier = sourceArm.typeQualifier;
                arm.variantName = sourceArm.variantName;
                arm.bindings = sourceArm.bindings;
                arm.variantIndex = sourceArm.variantIndex;
                for (const auto& type : sourceArm.bindingTypes)
                    arm.bindingTypes.push_back(substitute(type));
                arm.body = cloneBlock(sourceArm.body.get());
                clone->arms.push_back(std::move(arm));
            }
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const WhileStmt*>(src)) {
            auto clone = std::make_unique<WhileStmt>();
            clone->cond = cloneExpr(statement->cond.get());
            clone->body = cloneBlock(statement->body.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ForStmt*>(src)) {
            auto clone = std::make_unique<ForStmt>();
            clone->varName = statement->varName;
            clone->iterable = cloneExpr(statement->iterable.get());
            clone->body = cloneBlock(statement->body.get());
            clone->elementType = substitute(statement->elementType);
            clone->protocolNextSymbol = statement->protocolNextSymbol;
            clone->protocolIteratorType =
                substitute(statement->protocolIteratorType);
            clone->protocolOptionType =
                substitute(statement->protocolOptionType);
            clone->protocolNoneVariant =
                statement->protocolNoneVariant;
            clone->protocolSomeVariant =
                statement->protocolSomeVariant;
            clone->protocolIntoSymbol = statement->protocolIntoSymbol;
            clone->protocolInputType =
                substitute(statement->protocolInputType);
            clone->protocolStateName = statement->protocolStateName;
            clone->protocolStateNeedsCleanup =
                statement->protocolStateNeedsCleanup;
            clone->protocolStateCleanup =
                statement->protocolStateCleanup;
            clone->recipeStateName = statement->recipeStateName;
            clone->recipeSourceType =
                substitute(statement->recipeSourceType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const FreeStmt*>(src)) {
            auto clone = std::make_unique<FreeStmt>();
            clone->operand = cloneExpr(statement->operand.get());
            clone->action = statement->action;
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const SlotDeclStmt*>(src)) {
            auto clone = std::make_unique<SlotDeclStmt>();
            clone->name = statement->name;
            clone->acceptedKind = statement->acceptedKind;
            clone->acceptedCardinality =
                statement->acceptedCardinality;
            clone->isDynamic = statement->isDynamic;
            for (const auto& parameter : statement->params)
                clone->params.push_back(cloneParam(parameter));
            clone->defaultFragment = statement->defaultFragment;
            clone->resolvedDefaultFragmentName =
                statement->resolvedDefaultFragmentName;
            clone->structuralType =
                substitute(statement->structuralType);
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const SlotInvokeStmt*>(src)) {
            auto clone = std::make_unique<SlotInvokeStmt>();
            clone->name = statement->name;
            clone->acceptedKind = statement->acceptedKind;
            clone->acceptedCardinality =
                statement->acceptedCardinality;
            clone->isDynamic = statement->isDynamic;
            clone->usesDynamicDispatch =
                statement->usesDynamicDispatch;
            clone->resolvedDynamicFragmentNames =
                statement->resolvedDynamicFragmentNames;
            for (const auto& argument : statement->args)
                clone->args.push_back(cloneExpr(argument.get()));
            clone->continuation =
                cloneBlock(statement->continuation.get());
            clone->isImplicitCapture =
                statement->isImplicitCapture;
            for (const auto& parameter : statement->interfaceParams)
                clone->interfaceParams.push_back(cloneParam(parameter));
            clone->resolvedParamNames =
                statement->resolvedParamNames;
            clone->defaultFragment = statement->defaultFragment;
            clone->resolvedDefaultFragmentName =
                statement->resolvedDefaultFragmentName;
            clone->structuralType =
                substitute(statement->structuralType);
            return located(std::move(clone), src);
        }
        if (dynamic_cast<const ResumeStmt*>(src))
            return located(std::make_unique<ResumeStmt>(), src);
        if (auto* statement = dynamic_cast<const AbortStmt*>(src)) {
            auto clone = std::make_unique<AbortStmt>();
            clone->autoFrees = statement->autoFrees;
            for (const auto& cleanup : statement->cleanups)
                clone->cleanups.push_back(cloneCleanup(cleanup));
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const AwaitStmt*>(src)) {
            auto clone = std::make_unique<AwaitStmt>();
            clone->event = cloneExpr(statement->event.get());
            return located(std::move(clone), src);
        }
        if (auto* statement = dynamic_cast<const ApplyStmt*>(src)) {
            auto clone = std::make_unique<ApplyStmt>();
            clone->slotName = statement->slotName;
            clone->fragmentName = statement->fragmentName;
            clone->isDynamic = statement->isDynamic;
            clone->alternativeFragmentNames =
                statement->alternativeFragmentNames;
            clone->resolvedAlternativeFragmentNames =
                statement->resolvedAlternativeFragmentNames;
            clone->resolvedFragmentName =
                statement->resolvedFragmentName;
            clone->body = cloneBlock(statement->body.get());
            return located(std::move(clone), src);
        }
        fail("statement");
        return nullptr;
    }
};

FunctionDecl* SemanticAnalyzer::monomorphize(FunctionDecl* generic, const TypeVec& concreteTypes) {
    luna::instantiation::Request request;
    const auto genericSymbol = generic->generatedSymbolName.empty()
        ? generic->name : generic->generatedSymbolName;
    request.genericDeclarationId = nominalDeclarationIdentity(
        mProgram, "fn", genericSymbol, generic);
    for (const auto& type : concreteTypes)
        request.typeArguments.push_back(typeIdentity(resolved(type)));
    request.requestedBy = mDiagnosticFile + ":" +
        std::to_string(mDiagnosticLine) + ":" + std::to_string(mDiagnosticCol);
    const std::string requestKey =
        luna::instantiation::Instantiator::keyFor(request);
    auto cached = mInstantiatedFunctions.find(requestKey);
    if (cached != mInstantiatedFunctions.end()) return cached->second;

    const auto& entry = mInstantiator.begin(request);
    if (entry.state == luna::instantiation::State::Failed) {
        error("generic instantiation previously failed: " + entry.failure);
        return nullptr;
    }
    auto specialized = std::make_unique<FunctionDecl>();
    specialized->name = entry.instanceId;
    specialized->generatedSymbolName = entry.instanceId;
    specialized->sourcePath = generic->sourcePath;
    specialized->line = generic->line;
    specialized->col = generic->col;
    specialized->packageId = generic->packageId;
    specialized->modulePath = generic->modulePath;
    specialized->isExported = generic->isExported;
    specialized->isKernel = generic->isKernel;
    specialized->isConstexpr = generic->isConstexpr;
    specialized->returnsLinear = generic->returnsLinear;
    specialized->returnUsage = generic->returnUsage;
    specialized->isTemplateInstance = true;
    specialized->concreteTypeArgs = concreteTypes;

    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < generic->typeParams.size() && i < concreteTypes.size(); ++i) {
        bindings[generic->typeParams[i]] = concreteTypes[i];
    }

    for (auto& p : generic->params) {
        Param cp;
        cp.name = p.name;
        cp.isLinear = p.isLinear;
        cp.usage = p.usage;
        cp.relation = p.relation;
        cp.hasExplicitUsage = p.hasExplicitUsage;
        TypePtr parameterType = p.inferredType
            ? substituteNominalType(
                  mConstraints.resolve(p.inferredType), bindings)
            : (p.type ? resolveType(p.type.get(), bindings)
                      : TyUnknown);
        if (parameterType && parameterType->kind == TypeKind::TypeParam) {
            auto it = bindings.find(parameterType->name);
            if (it != bindings.end()) parameterType = it->second;
        }
        cp.inferredType = parameterType;
        cp.type = typeToAST(parameterType);
        specialized->params.push_back(std::move(cp));
    }

    if (generic->returnType) {
        const TypePtr returnType = generic->inferredReturnType
            ? substituteNominalType(
                  mConstraints.resolve(generic->inferredReturnType),
                  bindings)
            : resolveType(generic->returnType.get(), bindings);
        specialized->returnType = typeToAST(returnType);
        if (specialized->returnsLinear)
            specialized->returnType = std::make_unique<LinearTypeAST>(
                std::move(specialized->returnType));
        else if (generic->returnUsage == luna::ownership::Usage::Affine)
            specialized->returnType = std::make_unique<AffineTypeAST>(
                std::move(specialized->returnType));
        }

    if (generic->body) {
        MonomorphizationCloner cloner(bindings);
        specialized->body = cloner.cloneBlock(generic->body.get());
        if (!specialized->body || !cloner.failure().empty()) {
            const std::string failure = cloner.failure().empty()
                ? "generic function body could not be cloned"
                : cloner.failure();
            mInstantiator.fail(requestKey, failure);
            error("cannot instantiate generic function '" + generic->name +
                  "': " + failure);
            return nullptr;
        }
    } else {
        specialized->body = std::make_unique<BlockStmt>();
    }

    mGeneratedInstances.push_back(std::move(specialized));
    auto* result = mGeneratedInstances.back().get();
    mInstantiatedFunctions[requestKey] = result;
    if (!mInstantiator.complete(requestKey)) {
        mInstantiator.fail(requestKey, "instance state transition failed");
        error("generic instantiator could not finalize '" + entry.instanceId + "'");
        return nullptr;
    }
    return result;
}

TypePtr SemanticAnalyzer::resolveTypeAST(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return TyUnit;
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        // Monomorphization may materialize an already-resolved nominal type
        // into an AST annotation. Preserve that exact identity instead of
        // resolving its display name through the declaration family again.
        if (named->resolvedType) return named->resolvedType;
        auto bound = bindings.find(named->name);
        if (bound != bindings.end()) return bound->second;
        if (auto* symbol = mSymTable.lookup(named->name);
            symbol && symbol->kind == SymbolKind::TypeParam && symbol->type)
            return symbol->type;
        if (named->name == "i32") return TyI32;
        if (named->name == "i64") return TyI64;
        if (named->name == "i8") return TyI8;
        if (named->name == "i16") return TyI16;
        if (named->name == "u8") return TyU8;
        if (named->name == "u16") return TyU16;
        if (named->name == "u32") return TyU32;
        if (named->name == "u64") return TyU64;
        if (named->name == "usize") return TyUSize;
        if (named->name == "isize") return TyISize;
        if (named->name == "f32") return TyF32;
        if (named->name == "f64") return TyF64;
        if (named->name == "bool") return TyBool;
        if (named->name == "string") return TyString;
        if (named->name == "cstr") return TyCStr;
        if (named->name == "unit") return TyUnit;
        if (named->name == "never") return TyNever;
        if (named->name == "Self") return Type::makeTypeParam("Self");

        if (named->name == "raw") {
            if (named->typeArgs.size() != 1) {
                error("raw<T> requires exactly one type argument");
                return TyUnknown;
            }
            return Type::makeRawPointer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "Result") {
            if (named->typeArgs.size() != 2) {
                error("Result<T, E> requires value and error type arguments",
                      named->line, named->col);
                return TyUnknown;
            }
            return Type::makeResult(
                resolveTypeAST(named->typeArgs[0].get(), bindings),
                resolveTypeAST(named->typeArgs[1].get(), bindings));
        }
        if (named->name == "rc" || named->name == "arc") {
            if (named->typeArgs.size() != 1) {
                error(named->name + "<T> requires exactly one type argument");
                return TyUnknown;
            }
            TypePtr element =
                resolveTypeAST(named->typeArgs.front().get(), bindings);
            return named->name == "rc"
                ? Type::makeRc(element) : Type::makeArc(element);
        }
        if (named->name == "device_buffer") {
            if (named->typeArgs.size() != 1) {
                error("device_buffer<T> requires exactly one element type");
                return TyUnknown;
            }
            return Type::makeDeviceBuffer(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "array") {
            if (named->typeArgs.size() != 1 || !named->arrayLength) {
                error("array<T, N> requires one element type and a non-negative compile-time length",
                      named->line, named->col);
                return TyUnknown;
            }
            return Type::makeArray(resolveTypeAST(named->typeArgs[0].get(), bindings),
                                   *named->arrayLength);
        }
        if (named->name == "slice") {
            if (named->typeArgs.size() != 1) { error("slice<T> requires exactly one element type", named->line, named->col); return TyUnknown; }
            return Type::makeSlice(resolveTypeAST(named->typeArgs[0].get(), bindings));
        }
        if (named->name == "event") return TyEvent;
        if (named->name == "metadata_view") {
            if (named->typeArgs.size() != 1) {
                error("metadata_view<M> requires exactly one metadata type",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr metadata = resolveTypeAST(
                named->typeArgs.front().get(), bindings);
            if (resolved(metadata)->kind != TypeKind::Metadata)
                error("metadata_view type argument must be a meta schema",
                      named->line, named->col);
            return Type::makeMetadataView(metadata);
        }
        if (named->name == "declaration_view") {
            if (named->typeArgs.size() > 1) {
                error("declaration_view accepts at most one callable type argument",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr callable = named->typeArgs.empty()
                ? nullptr : resolveTypeAST(named->typeArgs.front().get(), bindings);
            return Type::makeDeclarationView(callable);
        }
        if (named->name == "declaration_ref") {
            if (named->typeArgs.size() > 1) {
                error("declaration_ref accepts at most one callable type argument",
                      named->line, named->col);
                return TyUnknown;
            }
            TypePtr callable = named->typeArgs.empty()
                ? nullptr : resolveTypeAST(named->typeArgs.front().get(), bindings);
            return Type::makeDeclarationRef(callable);
        }
        if (auto metadata = lookupDeclaredType(named->name);
            metadata && metadata->kind == TypeKind::Metadata) {
            return metadata;
        }

        TypePtr nominalType;
        const std::string typeKey = sourceDeclarationKey(named->name);
        auto nominal = mDeclaredTypes.find(typeKey);
        if (nominal != mDeclaredTypes.end()) nominalType = nominal->second;
        if (nominalType) {
            TypeVec args;
            for (auto& arg : named->typeArgs)
                args.push_back(resolveTypeAST(arg.get(), bindings));
            const_cast<NamedTypeAST*>(named)->resolvedType = args.empty()
                ? nominalType : instantiateNominal(nominalType, args);
            return const_cast<NamedTypeAST*>(named)->resolvedType;
        }
        return resolveType(ast, bindings);
    }
    if (auto* ref = dynamic_cast<const RefTypeAST*>(ast))
        return Type::makeReference(resolveTypeAST(ref->inner.get(), bindings),
                                   ref->isMutable);
    if (auto* linear = dynamic_cast<const LinearTypeAST*>(ast))
        return resolveTypeAST(linear->inner.get(), bindings);
    if (auto* affine = dynamic_cast<const AffineTypeAST*>(ast))
        return resolveTypeAST(affine->inner.get(), bindings);
    if (auto* fn = dynamic_cast<const FunctionTypeAST*>(ast)) {
        TypeVec params;
        std::vector<luna::ownership::Contract> contracts;
        for (auto& param : fn->paramTypes) {
            auto type = resolveTypeAST(param.get(), bindings);
            auto usage = dynamic_cast<LinearTypeAST*>(param.get())
                ? luna::ownership::Usage::Linear
                : (dynamic_cast<AffineTypeAST*>(param.get())
                    ? luna::ownership::Usage::Affine
                    : defaultUsageForType(type));
            contracts.push_back(parameterContractFor(
                type, usage, usage != luna::ownership::Usage::Copy));
            params.push_back(std::move(type));
        }
        auto returnType = resolveTypeAST(fn->returnType.get(), bindings);
        auto returnUsage = dynamic_cast<LinearTypeAST*>(fn->returnType.get())
            ? luna::ownership::Usage::Linear
            : (dynamic_cast<AffineTypeAST*>(fn->returnType.get())
                ? luna::ownership::Usage::Affine
                : defaultUsageForType(returnType));
        return Type::makeFunction(
            std::move(params), std::move(returnType), std::move(contracts),
            {luna::ownership::Relation::Owned, returnUsage});
    }
    return TyUnknown;
}

static TypePtr substituteNominalType(
    const TypePtr& type,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!type) return TyUnknown;
    if (type->kind == TypeKind::TypeParam) {
        auto it = bindings.find(type->name);
        return it == bindings.end() ? type : it->second;
    }

    // Preserve every semantic property of the canonical type while replacing
    // type parameters recursively. This also covers arrays, slices, callable
    // ownership contracts, metadata views, slots, and fragments; maintaining
    // a partial kind switch here previously left generic signatures with
    // unresolved nested parameters.
    auto result = std::make_shared<Type>(*type);
    if (type->inner)
        result->inner = substituteNominalType(type->inner, bindings);
    result->typeArgs.clear();
    for (const auto& argument : type->typeArgs)
        result->typeArgs.push_back(
            substituteNominalType(argument, bindings));
    result->paramTypes.clear();
    for (const auto& parameter : type->paramTypes)
        result->paramTypes.push_back(
            substituteNominalType(parameter, bindings));
    if (type->returnType)
        result->returnType =
            substituteNominalType(type->returnType, bindings);
    result->fields.clear();
    for (const auto& field : type->fields)
        result->fields.push_back(
            {field.name, substituteNominalType(field.type, bindings)});
    result->variants.clear();
    for (const auto& variant : type->variants) {
        TypeVariant copied;
        copied.name = variant.name;
        for (const auto& field : variant.fields)
            copied.fields.push_back(
                substituteNominalType(field, bindings));
        result->variants.push_back(std::move(copied));
    }
    return result;
}

TypePtr SemanticAnalyzer::instantiateNominal(const TypePtr& type,
                                             const std::vector<TypePtr>& args) {
    if (!type || type->typeParams.empty()) return type;
    std::unordered_map<std::string, TypePtr> bindings;
    for (size_t i = 0; i < type->typeParams.size() && i < args.size(); ++i)
        bindings[type->typeParams[i]] = args[i];
    auto instance = substituteNominalType(type, bindings);
    instance->typeArgs = args;
    instance->typeParams.clear();
    return instance;
}

TypePtr SemanticAnalyzer::declaredType(const TypeAST* ast,
    const std::unordered_map<std::string, TypePtr>& bindings) {
    if (!ast) return mConstraints.fresh();
    if (auto* named = dynamic_cast<const NamedTypeAST*>(ast)) {
        if (named->name == "auto") return mConstraints.fresh();
    }
    return resolveTypeAST(ast, bindings);
}

TypePtr SemanticAnalyzer::resolved(const TypePtr& type) {
    return mConstraints.resolve(type);
}

bool SemanticAnalyzer::constrain(const TypePtr& actual, const TypePtr& expected,
                                 const std::string& context) {
    if (!actual || !expected || actual->kind == TypeKind::Unknown ||
        expected->kind == TypeKind::Unknown) return true;
    // `never` is the bottom type: a diverging expression can inhabit every
    // expected value type, while ordinary values cannot inhabit `never`.
    if (resolved(actual)->kind == TypeKind::Never) return true;
    std::string reason;
    if (!mConstraints.unify(actual, expected, &reason)) {
        error("Type constraint failed in " + context + ": " + reason);
        return false;
    }
    return true;
}

void SemanticAnalyzer::requireBool(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireBool(t);
        return;
    }
    if (t->kind != TypeKind::Bool)
        error(context + " must be bool, got " + t->toString());
}

void SemanticAnalyzer::requireNumeric(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireNumeric(t);
        return;
    }
    if (!isNumericType(t))
        error(context + " must be numeric, got " + t->toString());
}

void SemanticAnalyzer::requireInteger(const TypePtr& type, const std::string& context) {
    auto t = resolved(type);
    // Integer literals and currently-unbound inferred variables default to i32
    // later in inference. Keep them accepted here while rejecting floats and
    // non-numeric values immediately.
    if (t->kind == TypeKind::InferenceVar) {
        mConstraints.requireNumeric(t);
        return;
    }
    if (!isIntegerType(t))
        error(context + " must be an integer, got " + t->toString());
}

void SemanticAnalyzer::checkUnresolved(const TypePtr& type, const std::string& context) {
    if (type && mConstraints.hasUnresolved(type))
        error("Could not infer " + context);
}

std::unique_ptr<TypeAST> SemanticAnalyzer::typeToAST(const TypePtr& type) {
    auto t = resolved(type);
    if (!t || t->kind == TypeKind::Unknown || t->kind == TypeKind::InferenceVar)
        return std::make_unique<NamedTypeAST>("i32");
    if (t->kind == TypeKind::Reference)
        return std::make_unique<RefTypeAST>(typeToAST(t->inner), t->isMutable);
    if (t->kind == TypeKind::RawPointer) {
        auto raw = std::make_unique<NamedTypeAST>("raw");
        raw->typeArgs.push_back(typeToAST(t->inner));
        return raw;
    }
    if (t->kind == TypeKind::Rc || t->kind == TypeKind::Arc) {
        auto shared = std::make_unique<NamedTypeAST>(
            t->kind == TypeKind::Rc ? "rc" : "arc");
        shared->typeArgs.push_back(typeToAST(t->inner));
        return shared;
    }
    if (t->kind == TypeKind::Result && t->typeArgs.size() == 2) {
        auto result = std::make_unique<NamedTypeAST>("Result");
        result->typeArgs.push_back(typeToAST(t->typeArgs[0]));
        result->typeArgs.push_back(typeToAST(t->typeArgs[1]));
        return result;
    }
    if (t->kind == TypeKind::DeviceBuffer) {
        auto buffer = std::make_unique<NamedTypeAST>("device_buffer");
        buffer->typeArgs.push_back(typeToAST(t->inner));
        return buffer;
    }
    if (t->kind == TypeKind::Array) {
        auto array = std::make_unique<NamedTypeAST>("array");
        array->typeArgs.push_back(typeToAST(t->inner));
        array->arrayLength = t->arrayLength;
        return array;
    }
    if (t->kind == TypeKind::Slice) {
        auto slice = std::make_unique<NamedTypeAST>("slice");
        slice->typeArgs.push_back(typeToAST(t->inner));
        return slice;
    }
    if (t->kind == TypeKind::Event)
        return std::make_unique<NamedTypeAST>("event");
    if (t->kind == TypeKind::Function) {
        auto fn = std::make_unique<FunctionTypeAST>();
        for (auto& p : t->paramTypes) fn->paramTypes.push_back(typeToAST(p));
        fn->returnType = typeToAST(t->returnType);
        return fn;
    }
    if (t->kind == TypeKind::Struct || t->kind == TypeKind::Enum) {
        auto named = std::make_unique<NamedTypeAST>(t->name);
        named->resolvedType = t;
        for (auto& arg : t->typeArgs)
            named->typeArgs.push_back(typeToAST(arg));
        return named;
    }
    if (t->kind == TypeKind::TypeParam)
        return std::make_unique<NamedTypeAST>(t->name);
    return std::make_unique<NamedTypeAST>(t->toString());
}

void SemanticAnalyzer::materializeInferredTypes(Program* program) {
    std::function<void(Expr*)> visitExpr;
    std::function<void(BlockStmt*)> visitBlock;
    std::function<void(Stmt*)> visitStmt;
    const auto needsConcreteAnnotation = [](const std::unique_ptr<TypeAST>& type) {
        if (!type) return true;
        const auto* named = dynamic_cast<const NamedTypeAST*>(type.get());
        return named && named->name == "auto";
    };

    visitExpr = [&](Expr* expr) {
        if (!expr) return;
        if (auto* l = dynamic_cast<LambdaExpr*>(expr)) {
            for (auto& p : l->params) {
                checkUnresolved(p.inferredType, "lambda parameter '" + p.name + "'");
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
            }
            checkUnresolved(l->closureType, "lambda type");
            l->closureType = resolved(l->closureType);
            if (needsConcreteAnnotation(l->returnType) && l->closureType &&
                l->closureType->kind == TypeKind::Function)
                l->returnType = typeToAST(l->closureType->returnType);
            visitBlock(l->body.get());
            return;
        }
        if (auto* b = dynamic_cast<BinaryExpr*>(expr)) { visitExpr(b->lhs.get()); visitExpr(b->rhs.get()); return; }
        if (auto* u = dynamic_cast<UnaryExpr*>(expr)) { visitExpr(u->operand.get()); return; }
        if (auto* c = dynamic_cast<CallExpr*>(expr)) {
            if (c->intrinsicType)
                c->intrinsicType = resolved(c->intrinsicType);
            if (c->resultType)
                c->resultType = resolved(c->resultType);
            if (c->iteratorInputType)
                c->iteratorInputType = resolved(c->iteratorInputType);
            if (c->iteratorOutputType)
                c->iteratorOutputType = resolved(c->iteratorOutputType);
            for (auto& type : c->typeArgs) type = resolved(type);
            visitExpr(c->callee.get());
            for (auto& a : c->args) visitExpr(a.get());
            return;
        }
        if (auto* s = dynamic_cast<SelectExpr*>(expr)) {
            if (s->selectedType)
                s->selectedType = resolved(s->selectedType);
            for (auto& a : s->selectorArgs) visitExpr(a.get());
            return;
        }
        if (auto* l = dynamic_cast<LaunchExpr*>(expr)) {
            visitExpr(l->threads.get()); for (auto& a : l->args) visitExpr(a.get()); return;
        }
        if (auto* v = dynamic_cast<VariantConstructExpr*>(expr)) {
            v->constructedType = resolved(v->constructedType);
            for (auto& a : v->args) visitExpr(a.get()); return;
        }
        if (auto* a = dynamic_cast<AssignExpr*>(expr)) { visitExpr(a->lhs.get()); visitExpr(a->rhs.get()); return; }
        if (auto* f = dynamic_cast<FieldAccessExpr*>(expr)) { visitExpr(f->object.get()); return; }
        if (auto* i = dynamic_cast<IndexExpr*>(expr)) { visitExpr(i->object.get()); visitExpr(i->index.get()); return; }
        if (auto* h = dynamic_cast<HeapAllocExpr*>(expr)) { visitExpr(h->initializer.get()); return; }
        if (auto* m = dynamic_cast<MoveExpr*>(expr)) { visitExpr(m->operand.get()); return; }
        if (auto* b = dynamic_cast<BorrowExpr*>(expr)) { visitExpr(b->operand.get()); return; }
        if (auto* d = dynamic_cast<DerefExpr*>(expr)) { visitExpr(d->operand.get()); return; }
        if (auto* a = dynamic_cast<AddrOfExpr*>(expr)) { visitExpr(a->operand.get()); return; }
        if (auto* t = dynamic_cast<TryExpr*>(expr)) {
            visitExpr(t->operand.get());
            t->resultType = resolved(t->resultType);
            t->propagatedResultType = resolved(t->propagatedResultType);
            t->valueType = resolved(t->valueType);
            t->errorType = resolved(t->errorType);
            t->propagatedErrorType = resolved(t->propagatedErrorType);
            return;
        }
        if (auto* i = dynamic_cast<IfExpr*>(expr)) { visitExpr(i->cond.get()); visitExpr(i->thenExpr.get()); visitExpr(i->elseExpr.get()); return; }
        if (auto* b = dynamic_cast<BlockExpr*>(expr)) { visitBlock(b->block.get()); return; }
    };
    visitStmt = [&](Stmt* stmt) {
        if (!stmt) return;
        if (auto* b = dynamic_cast<BlockStmt*>(stmt)) { visitBlock(b); return; }
        if (auto* l = dynamic_cast<LetStmt*>(stmt)) {
            l->inferredType = resolved(l->inferredType);
            visitExpr(l->initializer.get());
            return;
        }
        if (auto* r = dynamic_cast<ReturnStmt*>(stmt)) { visitExpr(r->value.get()); return; }
        if (auto* a = dynamic_cast<AwaitStmt*>(stmt)) { visitExpr(a->event.get()); return; }
        if (auto* e = dynamic_cast<ExprStmt*>(stmt)) { visitExpr(e->expr.get()); return; }
        if (auto* i = dynamic_cast<IfStmt*>(stmt)) { visitExpr(i->cond.get()); visitBlock(i->thenBlock.get()); visitStmt(i->elseBranch.get()); return; }
        if (auto* m = dynamic_cast<MatchStmt*>(stmt)) {
            visitExpr(m->scrutinee.get());
            m->matchedType = resolved(m->matchedType);
            for (auto& arm : m->arms) {
                for (auto& type : arm.bindingTypes)
                    type = resolved(type);
                visitBlock(arm.body.get());
            }
            return;
        }
        if (auto* w = dynamic_cast<WhileStmt*>(stmt)) { visitExpr(w->cond.get()); visitBlock(w->body.get()); return; }
        if (auto* f = dynamic_cast<ForStmt*>(stmt)) {
            visitExpr(f->iterable.get());
            f->elementType = resolved(f->elementType);
            if (f->protocolIteratorType)
                f->protocolIteratorType =
                    resolved(f->protocolIteratorType);
            if (f->protocolOptionType)
                f->protocolOptionType =
                    resolved(f->protocolOptionType);
            if (f->protocolInputType)
                f->protocolInputType =
                    resolved(f->protocolInputType);
            if (f->recipeSourceType)
                f->recipeSourceType =
                    resolved(f->recipeSourceType);
            visitBlock(f->body.get());
            return;
        }
        if (auto* f = dynamic_cast<FreeStmt*>(stmt)) { visitExpr(f->operand.get()); return; }
    };
    visitBlock = [&](BlockStmt* block) {
        if (!block) return;
        for (auto& stmt : block->stmts) visitStmt(stmt.get());
    };

    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) {
            for (auto& p : f->params) {
                p.inferredType = resolved(p.inferredType);
                if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
            }
            f->inferredReturnType = resolved(f->inferredReturnType);
            if (needsConcreteAnnotation(f->returnType))
                f->returnType = typeToAST(f->inferredReturnType);
            visitBlock(f->body.get());
        } else if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& f : i->methods) {
                for (auto& p : f->params) {
                    p.inferredType = resolved(p.inferredType);
                    if (needsConcreteAnnotation(p.type)) p.type = typeToAST(p.inferredType);
                }
                f->inferredReturnType = resolved(f->inferredReturnType);
                if (needsConcreteAnnotation(f->returnType))
                    f->returnType = typeToAST(f->inferredReturnType);
                visitBlock(f->body.get());
            }
        }
    }
}

void SemanticAnalyzer::error(const std::string& msg, int line, int col) {
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

void SemanticAnalyzer::setDiagnosticLocation(const ASTNode* node) {
    if (!node) return;
    if (!node->sourcePath.empty()) mDiagnosticFile = node->sourcePath;
    if (node->line > 0) mDiagnosticLine = node->line;
    if (node->col > 0) mDiagnosticCol = node->col;
}

void SemanticAnalyzer::setDeclarationContext(const Decl* declaration) {
    mCurrentPackageId = effectivePackageId(mProgram, declaration);
    mCurrentModulePath = declaration ? declaration->modulePath : std::string{};
}

std::string SemanticAnalyzer::sourceDeclarationKey(
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

SymbolInfo* SemanticAnalyzer::lookupSymbol(const std::string& name) {
    // Lexical bindings and compiler built-ins deliberately shadow package
    // declarations for an unqualified name.
    if (name.find("::") == std::string::npos) {
        if (auto* direct = mSymTable.lookup(name)) return direct;
    }
    return mSymTable.lookup(sourceDeclarationKey(name));
}

TypePtr SemanticAnalyzer::lookupDeclaredType(const std::string& name) {
    if (auto direct = mSymTable.lookupType(name)) return direct;
    return mSymTable.lookupType(sourceDeclarationKey(name));
}
