#include "DeclarationCollector.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include <set>
#include <utility>

void DeclarationCollector::declareFunction(FunctionDecl* decl) {
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    mContext.mFunctionFamilies[sourceKey].push_back(decl);
    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) {
        bindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& clause : decl->whereClauses) {
        if (clause.kind == WhereClause::Kind::ConstraintExpression)
            continue;
        if (clause.kind == WhereClause::Kind::TraitBound) {
            mContext.resolveTraitRef(clause.trait, decl);
            continue;
        }
        const std::string key = mContext.sourceDeclarationKey(clause.constraintName);
        auto constraintIt = mContext.mConcepts.find(key);
        if (constraintIt == mContext.mConcepts.end()) {
            mContext.error("unknown constraint '" + clause.constraintName + "'",
                  decl->line, decl->col);
            continue;
        }
        clause.constraintName = key;
        if (clause.constraintTypeArgs.size() != constraintIt->second->typeParams.size())
            mContext.error("constraint '" + constraintIt->second->name + "' expects " +
                  std::to_string(constraintIt->second->typeParams.size()) +
                  " type arguments", decl->line, decl->col);
    }
    SymbolInfo info;
    info.kind = SymbolKind::Function;
    info.isExported = decl->isExported;
    info.isExtern = decl->isExtern;
    info.returnsLinear = decl->returnsLinear;
    info.returnType = mContext.declaredType(decl->returnType.get(), bindings);
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
        TypePtr pt = mContext.declaredType(p.type.get(), bindings);
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
    if (mContext.mFunctionFamilies[sourceKey].size() == 1) {
        mContext.mSymTable.defineAtRoot(sourceKey, info);
    }
    mContext.mSymTable.defineLinkage(decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName, info);
    if (decl->isConstexpr)
        mContext.mConstexprFunctions[decl->generatedSymbolName.empty()
            ? decl->name : decl->generatedSymbolName] = decl;
    if (decl->isExtern || (decl->isExported && !decl->abi.empty()))
        validateFFIFunction(decl);
}

void DeclarationCollector::declareMeta(MetaDecl* decl) {
    if (!decl) return;
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    if (mContext.mMetadataSchemas.count(sourceKey)) {
        mContext.error("duplicate metadata schema '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    std::set<std::string> fieldNames;
    std::vector<TypeField> fields;
    for (auto& field : decl->fields) {
        if (!fieldNames.insert(field.name).second)
            mContext.error("duplicate metadata field '" + field.name + "' in '" +
                  decl->name + "'", decl->line, decl->col);
        field.inferredType = mContext.declaredType(field.type.get(), {});
        fields.push_back({field.name, field.inferredType});
    }
    const auto schemaSymbol = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto metadataType = Type::makeMetadata(
        nominalDeclarationIdentity(mContext.mProgram, "meta", schemaSymbol, decl),
        std::move(fields));
    metadataType->name = decl->name;
    metadataType->declarationLinkageName = schemaSymbol;
    mContext.mMetadataSchemas[sourceKey] = decl;
    mContext.mSymTable.defineType(sourceKey, metadataType);
    mContext.mSymTable.defineType(schemaSymbol, metadataType);

    SymbolInfo constructor;
    constructor.kind = SymbolKind::Metadata;
    constructor.type = metadataType;
    constructor.returnType = metadataType;
    for (const auto& field : decl->fields)
        constructor.paramTypes.push_back(field.inferredType);
    if (!mContext.mSymTable.defineAtRoot(sourceKey, std::move(constructor)))
        mContext.error("metadata schema name '" + decl->name +
              "' conflicts with an existing declaration", decl->line, decl->col);
}

void DeclarationCollector::declareConstraint(ConstraintDecl* decl) {
    if (!decl) return;
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    if (!mContext.mConcepts.emplace(sourceKey, decl).second) {
        mContext.error("duplicate constraint '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    std::set<std::string> parameters;
    for (const auto& parameter : decl->typeParams) {
        if (!parameters.insert(parameter).second)
            mContext.error("duplicate type parameter '" + parameter +
                  "' in constraint '" + decl->name + "'", decl->line, decl->col);
    }
}

void DeclarationCollector::analyzeConstraint(ConstraintDecl* decl) {
    if (!decl || !decl->predicate) return;
    mContext.mSymTable.enterScope();
    for (const auto& parameter : decl->typeParams) {
        SymbolInfo info;
        info.kind = SymbolKind::TypeParam;
        info.type = Type::makeTypeParam(parameter);
        mContext.mSymTable.define(parameter, info);
    }
    TypePtr predicate = mContext.resolved(mContext.analyzeExpr(decl->predicate.get()));
    if (predicate->kind != TypeKind::Bool &&
        predicate->kind != TypeKind::InferenceVar)
        mContext.error("constraint '" + decl->name +
              "' predicate must have type bool", decl->line, decl->col);
    mContext.mSymTable.exitScope();
}

void DeclarationCollector::analyzeMeta(MetaDecl* decl) {
    if (!decl) return;
    for (auto& field : decl->fields)
        mContext.checkUnresolved(field.inferredType,
                        "metadata field '" + decl->name + "." + field.name + "'");
}

void DeclarationCollector::validateMetadata(Decl* decl) {
    if (!decl) return;
    for (auto& attachment : decl->metadata) {
        auto schema = mContext.mMetadataSchemas.find(mContext.sourceDeclarationKey(attachment.schemaName));
        if (schema == mContext.mMetadataSchemas.end()) {
            mContext.error("unknown metadata schema '" + attachment.schemaName + "'",
                  decl->line, decl->col);
            continue;
        }
        const auto schemaSymbol = schema->second->generatedSymbolName.empty()
            ? schema->second->name : schema->second->generatedSymbolName;
        attachment.resolvedSchemaId = nominalDeclarationIdentity(
            mContext.mProgram, "meta", schemaSymbol, schema->second);
        if (attachment.arguments.size() != schema->second->fields.size()) {
            mContext.error("metadata '" + attachment.schemaName + "' expects " +
                  std::to_string(schema->second->fields.size()) + " arguments, got " +
                  std::to_string(attachment.arguments.size()), decl->line, decl->col);
            continue;
        }
        attachment.evaluatedArguments.clear();
        for (size_t index = 0; index < attachment.arguments.size(); ++index) {
            auto value = mContext.evaluateConstExpr(attachment.arguments[index].get());
            if (!value) {
                mContext.error("metadata argument " + std::to_string(index + 1) + " of '" +
                      attachment.schemaName + "' is not a compile-time value",
                      decl->line, decl->col);
                continue;
            }
            TypePtr actual = TyUnknown;
            if (std::holds_alternative<int64_t>(*value)) actual = TyI32;
            else if (std::holds_alternative<double>(*value)) actual = TyF64;
            else if (std::holds_alternative<bool>(*value)) actual = TyBool;
            else if (std::holds_alternative<std::string>(*value)) actual = TyString;
            mContext.constrain(actual, schema->second->fields[index].inferredType,
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

void DeclarationCollector::declareFragment(FragmentDecl* decl) {
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    if (!mContext.mFragments.emplace(sourceKey, decl).second) {
        mContext.error("duplicate fragment declaration '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    SymbolInfo info;
    info.kind = SymbolKind::Fragment;
    TypeVec parameterTypes;
    std::vector<luna::ownership::Contract> parameterContracts;
    for (auto& parameter : decl->params) {
        parameter.inferredType = mContext.declaredType(parameter.type.get(), {});
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
    mContext.mSymTable.defineAtRoot(sourceKey, info);
}

bool DeclarationCollector::isFFIType(const TypePtr& type, const std::string& context) {
    auto t = mContext.resolved(type);
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
    mContext.error("Unsupported FFI type " + t->toString() + " in " + context);
    return false;
}

void DeclarationCollector::validateFFIFunction(FunctionDecl* decl) {
    if (!decl->abi.empty() && decl->abi != "C")
        mContext.error("Unsupported ABI '" + decl->abi + "' for function '" + decl->name + "'");
    if (decl->isExtern && decl->isExported)
        mContext.error("An extern function cannot also be exported: '" + decl->name + "'");
    if (decl->isExtern && decl->isConstexpr)
        mContext.error("An extern function cannot be constexpr: '" + decl->name + "'");
    if (!decl->typeParams.empty())
        mContext.error("C ABI function cannot be generic: '" + decl->name + "'");
    if (decl->returnsLinear &&
        (!decl->inferredReturnType || mContext.resolved(decl->inferredReturnType)->kind != TypeKind::RawPointer)) {
        mContext.error("owning FFI return of '" + decl->name + "' must use `linear raw<T>`");
    }
    for (auto& param : decl->params) {
        if (!param.type) {
            mContext.error("C ABI parameter '" + param.name + "' of '" + decl->name +
                  "' requires an explicit type");
            continue;
        }
        isFFIType(param.inferredType, "parameter '" + param.name + "' of '" + decl->name + "'");
    }
    isFFIType(decl->inferredReturnType,
              "return type of FFI function '" + decl->name + "'");
}

void DeclarationCollector::declareStruct(StructDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto declared = mContext.mDeclaredTypes[identity];
    if (!declared) {
        declared = Type::makeStruct(
            decl->name, {},
            nominalDeclarationIdentity(
                mContext.mProgram, "struct", identity, decl));
        mContext.mDeclaredTypes[identity] = declared;
    }
    declared->declarationLinkageName = identity;
    declared->typeParams = decl->typeParams;
    declared->fields.clear();
    mContext.mDeclaredTypes[identity] = declared;

    SymbolInfo info;
    info.kind = SymbolKind::Struct;
    info.isExported = decl->isExported;
    info.type = declared;
    info.typeParams = decl->typeParams;
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    mContext.mSymTable.defineAtRoot(sourceKey, info);
    mContext.mSymTable.defineType(sourceKey, declared);
    mContext.mSymTable.defineType(identity, declared);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& field : decl->fields) {
        TypeField typedField;
        typedField.name = field.name;
        typedField.type = mContext.resolveTypeAST(field.type.get(), bindings);
        field.inferredType = typedField.type;
        declared->fields.push_back(std::move(typedField));
    }
}

void DeclarationCollector::declareEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto declared = mContext.mDeclaredTypes[identity];
    if (!declared) {
        declared = Type::makeEnum(
            decl->name, {},
            nominalDeclarationIdentity(
                mContext.mProgram, "enum", identity, decl));
        mContext.mDeclaredTypes[identity] = declared;
    }
    declared->declarationLinkageName = identity;
    declared->typeParams = decl->typeParams;
    declared->variants.clear();
    mContext.mDeclaredTypes[identity] = declared;
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    mContext.mSymTable.defineType(sourceKey, declared);
    mContext.mSymTable.defineType(identity, declared);

    std::unordered_map<std::string, TypePtr> bindings;
    for (auto& tp : decl->typeParams) bindings[tp] = Type::makeTypeParam(tp);
    for (auto& variant : decl->variants) {
        TypeVariant typedVariant;
        typedVariant.name = variant.name;
        variant.inferredFields.clear();
        for (auto& field : variant.fields) {
            auto resolvedField = mContext.resolveTypeAST(field.get(), bindings);
            variant.inferredFields.push_back(resolvedField);
            typedVariant.fields.push_back(std::move(resolvedField));
        }
        declared->variants.push_back(std::move(typedVariant));
    }
}

void DeclarationCollector::declareTrait(TraitDecl* decl) {
    if (decl->name == "Drop" || decl->name == "From") {
        mContext.error("trait name '" + decl->name +
              "' is reserved for a compiler-known resource/error contract",
              decl->line, decl->col);
        return;
    }
    const std::string identity = mContext.traitIdentity(decl);
    decl->resolvedTraitId = identity;
    const std::string sourceKey = qualifiedDeclarationKey(
        mContext.mCurrentPackageId, mContext.mCurrentModulePath, decl->name);
    if (!mContext.mTraits.emplace(sourceKey, decl).second)
        mContext.error("duplicate trait declaration '" + decl->name + "'", decl->line, decl->col);
    mContext.mTraitTypeParams[identity] = decl->typeParams;
    mContext.mTraitOwners[identity] = effectivePackageId(mContext.mProgram, decl);
    // Traits are compile-time interfaces with stable declaration identity.
    mContext.mSymTable.defineType(identity, Type::makeTrait(identity));
    mContext.mSymTable.defineType(sourceKey, Type::makeTrait(identity));
}

void DeclarationCollector::declareImpl(ImplDecl* decl) {
    std::unordered_map<std::string, TypePtr> implBindings;
    for (const auto& parameter : decl->typeParams)
        implBindings[parameter] = Type::makeTypeParam(parameter);
    const std::string traitId = mContext.resolveTraitRef(decl->trait, decl);
    if (traitId.empty()) return;
    const auto coreFromIterator =
        mContext.mTraits.find(luna::sysmeta::FromIteratorTraitId);
    const bool isCoreFromIteratorTrait =
        coreFromIterator != mContext.mTraits.end() &&
        traitId == mContext.traitIdentity(coreFromIterator->second);
    const TypePtr targetType = mContext.resolveTypeAST(
        decl->targetType.get(), implBindings);
    const std::string targetId = mContext.typeIdentity(targetType);
    decl->resolvedTargetTypeId = targetId;
    const std::string implPackage =
        effectivePackageId(mContext.mProgram, decl);
    const std::string targetOwner =
        nominalTypeOwner(mContext.resolved(targetType));
    const auto traitOwnerEntry = mContext.mTraitOwners.find(traitId);
    const std::string traitOwner =
        traitOwnerEntry == mContext.mTraitOwners.end()
            ? std::string{} : traitOwnerEntry->second;

    auto registerMethod = [&](FunctionDecl* method,
                              const std::string& symbol,
                              bool exposeUnqualified = true) {
        method->generatedSymbolName = symbol;
        SymbolInfo info;
        info.kind = SymbolKind::Function;
        info.typeParams = method->typeParams;
        info.genericDecl = method->typeParams.empty() ? nullptr : method;
        info.returnType = mContext.declaredType(
            method->returnType.get(), implBindings);
        method->inferredReturnType = info.returnType;
        info.returnsLinear = method->returnsLinear;
        method->returnUsage = method->returnsLinear
            ? luna::ownership::Usage::Linear
            : defaultUsageForType(info.returnType);
        info.returnUsage = method->returnUsage;
        for (auto& parameter : method->params) {
            parameter.inferredType = mContext.declaredType(
                parameter.type.get(), implBindings);
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
            mContext.mSymTable.defineAtRoot(method->name, info);
        mContext.mSymTable.defineLinkage(symbol, info);
    };

    if (traitId == luna::sysmeta::FromTraitId) {
        if (!decl->typeParams.empty()) {
            mContext.error("generic `impl From<Source>` is not yet supported; use concrete error types",
                  decl->line, decl->col);
            return;
        }
        if (decl->trait.typeArgs.size() != 1) {
            mContext.error("From expects exactly one source type argument",
                  decl->line, decl->col);
            return;
        }
        TypePtr sourceType = mContext.resolveTypeAST(
            decl->trait.typeArgs.front().get(), {});
        decl->trait.resolvedTypeArgs = {sourceType};
        const std::string sourceOwner =
            nominalTypeOwner(mContext.resolved(sourceType));
        if (targetOwner != implPackage &&
            sourceOwner != implPackage) {
            mContext.error("orphan impl of compiler trait `From`: package '" +
                  implPackage +
                  "' owns neither target type '" +
                  targetType->toString() + "' nor source type '" +
                  sourceType->toString() + "'",
                  decl->line, decl->col);
            return;
        }
        const std::string sourceId = mContext.typeIdentity(sourceType);
        auto& conversions = mContext.mFromConversions[targetId];
        if (conversions.count(sourceId)) {
            mContext.error("duplicate `From<" + sourceType->toString() +
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
        conversions.emplace(sourceId, DeclarationContextAccess::FromConversion{
            sourceType, targetType, conversion,
            conversion ? conversion->generatedSymbolName : std::string{}
        });
        return;
    }

    const auto traitParameters = mContext.mTraitTypeParams.find(traitId);
    const size_t expectedTraitArgumentCount =
        traitParameters == mContext.mTraitTypeParams.end()
            ? 0 : traitParameters->second.size();
    if (decl->trait.typeArgs.size() !=
        expectedTraitArgumentCount) {
        mContext.error("trait '" + displayTraitRef(decl->trait) +
              "' expects " +
              std::to_string(expectedTraitArgumentCount) +
              " type argument(s)", decl->line, decl->col);
        return;
    }
    decl->trait.resolvedTypeArgs.clear();
    for (auto& argument : decl->trait.typeArgs)
        decl->trait.resolvedTypeArgs.push_back(
            mContext.resolveTypeAST(argument.get(), implBindings));

    if (isCoreFromIteratorTrait &&
        !decl->typeParams.empty()) {
        mContext.error("generic `FromIterator` impls are reserved until impl "
              "specialization participates in coherence; use a concrete "
              "collection, item, and builder type",
              decl->line, decl->col);
        return;
    }

    if (traitId == luna::sysmeta::DropTraitId) {
        if (targetOwner != implPackage) {
            mContext.error("orphan impl of `Drop`: package '" + implPackage +
                  "' does not own nominal target type '" +
                  targetType->toString() + "'",
                  decl->line, decl->col);
            return;
        }
    } else if (traitOwner != implPackage &&
               targetOwner != implPackage) {
        mContext.error("orphan impl of trait '" + displayTraitRef(decl->trait) +
              "': package '" + implPackage +
              "' owns neither the trait nor nominal target type '" +
              targetType->toString() + "'",
              decl->line, decl->col);
        return;
    }

    auto& implementations = mContext.mImpls[traitId];
    if (implementations.count(targetId)) {
        mContext.error("duplicate impl of trait '" + traitId + "' for type '" + targetId + "'",
              decl->line, decl->col);
        return;
    }
    implementations.emplace(targetId, std::unordered_map<std::string, FunctionDecl*>{});
    auto& methods = implementations.at(targetId);
    for (auto& method : decl->methods) {
        if (methods.count(method->name)) {
            mContext.error("duplicate method '" + method->name + "' in impl of trait '" + traitId + "'",
                  method->line, method->col);
            continue;
        }
        // Ordinary generic impl methods use the same monomorphization path as
        // generic free functions. Drop remains one representation-stable
        // erased finalizer until generic Drop bodies gain their own witness
        // specialization path.
        if (traitId != luna::sysmeta::DropTraitId)
            method->typeParams = decl->typeParams;
        methods[method->name] = method.get();
        registerMethod(
            method.get(),
            traitId + "__for__" + targetId + "__" + method->name);
    }
    if (isCoreFromIteratorTrait &&
        decl->trait.resolvedTypeArgs.size() == 2) {
        DeclarationContextAccess::FromIteratorImplementation protocol;
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
        mContext.mFromIteratorImplementations[targetId] =
            std::move(protocol);
    }
}

// ─── Analysis pass ─────────────────────────────────────────────────
