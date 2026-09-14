#include "Lowering.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"
#include "../lexer/Token.h"
#include "../parser/AST.h"
#include "../sema/SemanticAnalysisSupport.h"
#include "../sema/SymbolTable.h"

#include <algorithm>
#include <tuple>

namespace moon {

namespace {

std::string declarationName(const ::Decl* declaration) {
    if (auto* function = dynamic_cast<const ::FunctionDecl*>(declaration)) return function->name;
    if (auto* fragment = dynamic_cast<const ::FragmentDecl*>(declaration)) return fragment->name;
    if (auto* slot = dynamic_cast<const ::SlotDecl*>(declaration)) return slot->name;
    if (auto* structure = dynamic_cast<const ::StructDecl*>(declaration)) return structure->name;
    if (auto* enumeration = dynamic_cast<const ::EnumDecl*>(declaration)) return enumeration->name;
    if (auto* trait = dynamic_cast<const ::TraitDecl*>(declaration)) return trait->name;
    return "implementation";
}

std::string declarationSymbol(const ::Decl* declaration) {
    const auto name = declarationName(declaration);
    return declaration && !declaration->generatedSymbolName.empty()
        ? declaration->generatedSymbolName : name;
}

std::string declarationNamespace(const ::Decl* declaration,
                                 const std::string& fallbackPackage) {
    std::string result = declaration && !declaration->packageId.empty()
        ? declaration->packageId : fallbackPackage;
    if (declaration && !declaration->modulePath.empty())
        result += "::" + declaration->modulePath;
    return result;
}

std::string declarationIdentity(const ::Decl* declaration,
                                const std::string& fallbackPackage,
                                const std::string& kind,
                                const std::string& symbol) {
    return declarationNamespace(declaration, fallbackPackage) +
           "::" + kind + "::" + symbol;
}

} // namespace

std::unique_ptr<moon::BlockStmt> LunaLowerer::lowerBlock(const ::BlockStmt* block) {
    if (!block) return nullptr;
    const auto savedCompileTimeBindings =
        mCompileTimeDeclarationBindings;
    auto result = std::make_unique<moon::BlockStmt>();
    result->location = locationOf(block);
    for (const auto& statement : block->stmts) {
        auto lowered = lowerStmt(statement.get());
        if (lowered) result->stmts.push_back(std::move(lowered));
    }
    mCompileTimeDeclarationBindings = savedCompileTimeBindings;
    return result;
}

std::unique_ptr<moon::FunctionDecl> LunaLowerer::lowerFunction(
    const ::FunctionDecl* function) {
    if (!function) return nullptr;
    auto result = std::make_unique<moon::FunctionDecl>();
    result->location = locationOf(function);
    result->name = function->name;
    result->generatedSymbolName = function->generatedSymbolName.empty()
        ? function->name : function->generatedSymbolName;
    result->familyId = declarationIdentity(function, mModule->name, "fn", function->name);
    result->declarationId =
        functionDeclarationIdentity(mProgram, function);
    result->isExported = function->isExported;
    result->isKernel = function->isKernel;
    result->isExtern = function->isExtern;
    result->isConstexpr = function->isConstexpr;
    result->isSelector = function->isSelector;
    result->abi = function->abi;
    result->linkName = function->linkName;
    result->typeParams = function->typeParams;
    TypeVec parameterTypes;
    for (const auto& parameter : function->params) {
        parameterTypes.push_back(parameterType(parameter));
        result->params.push_back(lowerParam(parameter));
    }
    const TypePtr returnType = function->inferredReturnType
        ? function->inferredReturnType
        : (function->returnType
            ? lowerType(function->returnType.get()) : TyUnit);
    result->returnType = typeRef(returnType);
    result->returnsLinear = function->returnsLinear;
    result->returnUsage = function->returnUsage;
    result->body = lowerBlock(function->body.get());
    result->isTemplateInstance = function->isTemplateInstance;
    result->concreteTypeArgs = typeRefs(function->concreteTypeArgs);
    lowerCommonDeclaration(function, *result);

    std::vector<luna::ownership::Contract> parameterContracts;
    for (const auto& parameter : result->params) {
        parameterContracts.push_back({parameter.relation, parameter.usage});
    }
    const auto callableType = Type::makeFunction(
        std::move(parameterTypes), returnType, std::move(parameterContracts),
        {luna::ownership::Relation::Owned, result->returnUsage});
    result->sysmeta = callableType->sysmeta;
    result->sysmeta.capability.ffi =
        result->isExtern || !result->abi.empty();
    result->sysmeta.capability.gpu = result->isKernel;
    result->sysmeta.capability.hostOnly = !result->isKernel;
    result->sysmeta.capability.runtimeRetained =
        function->retention != RetentionKind::CompileTime;
    result->sysmeta.abi.stableBoundary =
        result->isExtern || !result->abi.empty();
    addDeclarationRecord(*result, DeclarationKind::Function, callableType);
    if (!result->isKernel && !result->isSelector &&
               (result->typeParams.empty() || result->isTemplateInstance)) {
        mModule->costs.push_back({CostKind::MachineCode, result->declarationId,
                                  result->isTemplateInstance
                                      ? "concrete generic instance" : "concrete function",
                                  result->location});
    }
    if (result->isTemplateInstance) {
        mModule->costs.push_back({CostKind::GenericInstantiation, result->declarationId,
                                  "requested by a statically resolved call",
                                  result->location});
    }
    return result;
}

std::unique_ptr<moon::Decl> LunaLowerer::lowerDecl(const ::Decl* declaration) {
    if (auto* function = dynamic_cast<const ::FunctionDecl*>(declaration)) {
        // A purely static selector has already reduced every use to a concrete
        // declaration. Erase its compiler-only function and view types before
        // MoonIR so the zero-cost rule is structural, not merely a backend
        // optimization.
        if (function->isSelector)
            return nullptr;
        return lowerFunction(function);
    }
    if (auto* slot = dynamic_cast<const ::SlotDecl*>(declaration)) {
        auto result = std::make_unique<moon::SlotDecl>();
        result->location = locationOf(slot);
        result->name = slot->name;
        result->generatedSymbolName = declarationSymbol(slot);
        result->familyId = declarationIdentity(
            slot, mModule->name, "slot", slot->name);
        result->declarationId = declarationIdentity(
            slot, mModule->name, "slot", result->generatedSymbolName);
        result->isExported = slot->isExported;
        result->acceptedKind = slot->acceptedKind == ::FragmentKind::Interceptor
            ? moon::FragmentKind::Interceptor : moon::FragmentKind::Context;
        result->acceptedCardinality = moon::FragmentCardinality::Once;
        for (const auto& parameter : slot->params)
            result->params.push_back(lowerParam(parameter));
        result->structuralType = typeRef(slot->structuralType);
        if (slot->structuralType) result->sysmeta = slot->structuralType->sysmeta;
        lowerCommonDeclaration(slot, *result);
        if (!slot->resolvedDefaultFragmentName.empty())
            deferDeclarationRef(
                result->defaultFragment,
                slot->resolvedDefaultFragmentName, slot,
                "default fragment for slot '" + slot->name + "'");
        addDeclarationRecord(*result, DeclarationKind::Slot,
                             slot->structuralType);
        return result;
    }
    if (auto* fragment = dynamic_cast<const ::FragmentDecl*>(declaration)) {
        auto result = std::make_unique<moon::FragmentDecl>();
        result->location = locationOf(fragment);
        result->name = fragment->name;
        result->generatedSymbolName = declarationSymbol(fragment);
        result->familyId = declarationIdentity(fragment, mModule->name, "fragment", fragment->name);
        result->declarationId = declarationIdentity(
            fragment, mModule->name, "fragment", result->generatedSymbolName);
        result->isExported = fragment->isExported;
        result->kind = fragment->kind == ::FragmentKind::Interceptor
            ? moon::FragmentKind::Interceptor : moon::FragmentKind::Context;
        result->cardinality = fragment->cardinality == ::FragmentCardinality::Once
            ? moon::FragmentCardinality::Once : moon::FragmentCardinality::Many;
        deferDeclarationRef(
            result->targetSlot, fragment->resolvedTargetSlotName, fragment,
            "nominal target of fragment '" + fragment->name + "'");
        for (const auto& parameter : fragment->params)
            result->params.push_back(lowerParam(parameter));
        result->body = lowerBlock(fragment->body.get());
        const TypePtr structuralType = fragment->structuralType;
        result->structuralType = typeRef(structuralType);
        if (structuralType)
            result->sysmeta = structuralType->sysmeta;
        lowerCommonDeclaration(fragment, *result);
        addDeclarationRecord(*result, DeclarationKind::Fragment, structuralType);
        return result;
    }
    if (auto* structure = dynamic_cast<const ::StructDecl*>(declaration)) {
        auto result = std::make_unique<moon::StructDecl>();
        result->location = locationOf(structure);
        result->name = structure->name;
        result->generatedSymbolName = declarationSymbol(structure);
        result->familyId = declarationIdentity(structure, mModule->name, "struct", structure->name);
        result->declarationId = declarationIdentity(
            structure, mModule->name, "struct", result->generatedSymbolName);
        result->isExported = structure->isExported;
        result->typeParams = structure->typeParams;
        for (const auto& field : structure->fields)
            result->fields.push_back(lowerParam(field));
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(result->generatedSymbolName) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(structure->name);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(structure, *result);
        addDeclarationRecord(*result, DeclarationKind::Struct, declarationType);
        return result;
    }
    if (auto* enumeration = dynamic_cast<const ::EnumDecl*>(declaration)) {
        auto result = std::make_unique<moon::EnumDecl>();
        result->location = locationOf(enumeration);
        result->name = enumeration->name;
        result->generatedSymbolName = declarationSymbol(enumeration);
        result->familyId = declarationIdentity(enumeration, mModule->name, "enum", enumeration->name);
        result->declarationId = declarationIdentity(
            enumeration, mModule->name, "enum", result->generatedSymbolName);
        result->isExported = enumeration->isExported;
        result->typeParams = enumeration->typeParams;
        for (const auto& variant : enumeration->variants) {
            moon::EnumDecl::Variant lowered;
            lowered.name = variant.name;
            for (const auto& field : variant.fields)
                lowered.fields.push_back(typeRef(lowerType(field.get())));
            result->variants.push_back(std::move(lowered));
        }
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(result->generatedSymbolName) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(enumeration->name);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(enumeration, *result);
        addDeclarationRecord(*result, DeclarationKind::Enum, declarationType);
        return result;
    }
    if (auto* trait = dynamic_cast<const ::TraitDecl*>(declaration)) {
        auto result = std::make_unique<moon::TraitDecl>();
        result->location = locationOf(trait);
        result->name = trait->name;
        result->generatedSymbolName = declarationSymbol(trait);
        result->familyId = declarationIdentity(trait, mModule->name, "trait", trait->name);
        result->declarationId = declarationIdentity(
            trait, mModule->name, "trait", result->generatedSymbolName);
        result->isExported = trait->isExported;
        result->typeParams = trait->typeParams;
        for (const auto& parameter : trait->traitParams)
            result->traitParams.push_back(lowerParam(parameter));
        for (const auto& method : trait->methods) {
            moon::TraitDecl::MethodSig lowered;
            lowered.name = method.name;
            for (const auto& parameter : method.params)
                lowered.params.push_back(lowerParam(parameter));
            lowered.returnType = typeRef(lowerType(method.returnType.get()));
            result->methods.push_back(std::move(lowered));
        }
        TypePtr declarationType = mSymbols
            ? mSymbols->lookupType(trait->resolvedTraitId) : nullptr;
        if (!declarationType && mSymbols)
            declarationType = mSymbols->lookupType(result->generatedSymbolName);
        result->type = typeRef(declarationType);
        lowerCommonDeclaration(trait, *result);
        addDeclarationRecord(*result, DeclarationKind::Trait, declarationType);
        return result;
    }
    if (auto* implementation = dynamic_cast<const ::ImplDecl*>(declaration)) {
        auto result = std::make_unique<moon::ImplDecl>();
        result->location = locationOf(implementation);
        result->name = "impl";
        const std::string resolvedTraitId =
            implementation->trait.resolvedTraitId;
        const std::string resolvedTargetTypeId =
            implementation->resolvedTargetTypeId;
        result->generatedSymbolName = implementation->generatedSymbolName.empty()
            ? resolvedTraitId + "__for__" + resolvedTargetTypeId
            : implementation->generatedSymbolName;
        result->familyId = declarationIdentity(
            implementation, mModule->name, "impl", resolvedTraitId);
        result->declarationId = declarationIdentity(
            implementation, mModule->name, "impl", result->generatedSymbolName);
        result->typeParams = implementation->typeParams;
        deferDeclarationRef(
            result->traitRef, resolvedTraitId,
            implementation, "implementation trait", true);
        const TypePtr targetType = lowerType(implementation->targetType.get());
        result->targetType = typeRef(targetType);
        for (const auto& method : implementation->methods)
            result->methods.push_back(lowerFunction(method.get()));
        lowerCommonDeclaration(implementation, *result);
        addDeclarationRecord(*result, DeclarationKind::Implementation, targetType);
        return result;
    }
    if (auto* metadata = dynamic_cast<const ::MetaDecl*>(declaration)) {
        MetadataSchema schema;
        const auto schemaSymbol = metadata->generatedSymbolName.empty()
            ? metadata->name : metadata->generatedSymbolName;
        schema.id = declarationIdentity(metadata, mModule->name, "meta", schemaSymbol);
        schema.name = metadata->name;
        schema.location = locationOf(metadata);
        for (const auto& field : metadata->fields)
            schema.fields.push_back({field.name, typeRef(field.inferredType)});
        mModule->metadataSchemas.push_back(std::move(schema));

        DeclarationRecord record;
        record.id = declarationIdentity(metadata, mModule->name, "meta",
            metadata->generatedSymbolName.empty()
                ? metadata->name : metadata->generatedSymbolName);
        record.familyId = declarationIdentity(
            metadata, mModule->name, "meta", metadata->name);
        record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
        record.sourceName = metadata->name;
        record.linkageName = metadata->generatedSymbolName;
        record.kind = DeclarationKind::MetadataSchema;
        record.retention = lowerRetention(metadata->retention);
        record.sysmeta.capability.runtimeRetained =
            metadata->retention != RetentionKind::CompileTime;
        record.location = locationOf(metadata);
        const TypePtr metadataType = mSymbols
            ? mSymbols->lookupType(schemaSymbol) : nullptr;
        record.type = typeRef(metadataType);
        record.canonicalContract = moon::canonicalContract(record);
        record.contractId = luna::identity::contractIdFromCanonical(
            record.canonicalContract);
        record.sysmeta.identity.symbol = record.symbolId;
        record.sysmeta.identity.contract = record.contractId;
        mModule->declarationTable.push_back(std::move(record));
        return nullptr;
    }
    // Named constraints are fully discharged by semantic analysis. Keeping
    // them out of MoonIR preserves the default zero-runtime-cost boundary.
    if (dynamic_cast<const ::ConstraintDecl*>(declaration))
        return nullptr;
    error(declaration, "unsupported declaration reached MoonIR lowering");
    return nullptr;
}

Retention LunaLowerer::lowerRetention(RetentionKind retention) const {
    if (retention == RetentionKind::Runtime) return Retention::Runtime;
    return Retention::CompileTime;
}

void LunaLowerer::lowerCommonDeclaration(const ::Decl* source,
                                         moon::Decl& target) {
    target.symbolId = luna::identity::symbolIdFromCanonical(
        target.declarationId);
    target.packageId = source->packageId.empty() ? mModule->name : source->packageId;
    target.modulePath = source->modulePath;
    target.retention = lowerRetention(source->retention);
    target.sysmeta.capability.runtimeRetained =
        source->retention != RetentionKind::CompileTime;
    for (const auto& attachment : source->metadata) {
        MetadataInstance instance;
        instance.schemaId = attachment.resolvedSchemaId;
        instance.values = attachment.evaluatedArguments;
        instance.retention = lowerRetention(attachment.retention);
        instance.location = locationOf(source);
        target.metadata.push_back(std::move(instance));
        if (attachment.retention != RetentionKind::CompileTime) {
            mModule->features.runtime = true;
            mModule->costs.push_back({CostKind::RuntimeMetadata,
                                      target.declarationId + "@" + attachment.schemaName,
                                      "explicit metadata retention",
                                      target.location});
        }
    }
    if (source->retention != RetentionKind::CompileTime) {
        mModule->features.runtime = true;
        mModule->costs.push_back({CostKind::RuntimeDescriptor,
                                  target.declarationId,
                                  "runtime declaration",
                                  target.location});
    }
}

TypePtr LunaLowerer::inferredExprType(const ::Expr* expression) const {
    if (!expression) return nullptr;
    if (auto* literal = dynamic_cast<const ::IntLiteralExpr*>(expression))
        return literal->inferredType ? literal->inferredType : TyI32;
    if (dynamic_cast<const ::FloatLiteralExpr*>(expression)) return TyF64;
    if (dynamic_cast<const ::StringLiteralExpr*>(expression)) return TyString;
    if (dynamic_cast<const ::BoolLiteralExpr*>(expression)) return TyBool;
    if (auto* variant = dynamic_cast<const ::VariantConstructExpr*>(expression))
        return variant->constructedType;
    if (auto* array = dynamic_cast<const ::ArrayLiteralExpr*>(expression))
        return Type::makeArray(array->elementType, array->elements.size());
    if (auto* record = dynamic_cast<const ::RecordLiteralExpr*>(expression))
        return record->recordType;
    if (auto* allocation = dynamic_cast<const ::HeapAllocExpr*>(expression))
        return allocation->resultType
            ? allocation->resultType
            : Type::makeRawPointer(allocation->allocatedType);
    if (dynamic_cast<const ::LaunchExpr*>(expression)) return TyEvent;
    if (dynamic_cast<const ::BlockExpr*>(expression) ||
        dynamic_cast<const ::IfExpr*>(expression))
        return TyUnit;
    return nullptr;
}

void LunaLowerer::addDeclarationRecord(const moon::Decl& declaration,
                                       DeclarationKind kind, TypePtr type) {
    DeclarationRecord record;
    record.id = declaration.declarationId;
    record.familyId = declaration.familyId;
    record.symbolId = luna::identity::symbolIdFromCanonical(record.id);
    record.sourceName = declaration.name;
    record.linkageName = declaration.generatedSymbolName;
    record.kind = kind;
    record.retention = declaration.retention;
    record.metadata = declaration.metadata;
    record.type = typeRef(type);
    record.sysmeta = declaration.sysmeta;
    if (type) {
        record.sysmeta.resource = type->sysmeta.resource;
        if (type->kind == TypeKind::Slot ||
            type->kind == TypeKind::Fragment)
            record.sysmeta.control = type->sysmeta.control;
    }
    record.canonicalContract = moon::canonicalContract(record);
    record.contractId = luna::identity::contractIdFromCanonical(
        record.canonicalContract);
    record.sysmeta.identity.symbol = record.symbolId;
    record.sysmeta.identity.contract = record.contractId;
    record.location = declaration.location;
    mModule->declarationTable.push_back(std::move(record));
}

void LunaLowerer::deferDeclarationRef(
    DeclarationRef& target,
    std::string lookup,
    const ASTNode* source,
    std::string context,
    bool lookupById,
    luna::identity::SymbolId expectedSymbol,
    luna::identity::ContractId expectedContract) {
    if (lookup.empty()) return;
    mPendingDeclarationRefs.push_back({
        &target, std::move(lookup), source, std::move(context), lookupById,
        std::move(expectedSymbol), std::move(expectedContract)});
}

void LunaLowerer::resolveDeclarationReferences() {
    if (!mModule) return;
    mModule->rebuildIndexes();

    // Drop glue belongs to the frozen type/declaration contract, not to an
    // opaque backend linkage string. Resolve it first because declaration
    // ContractIds include the expected drop implementation.
    for (auto& type : mModule->typeTable) {
        const std::string linkage = type.sysmeta.abi.dropGlueSymbol;
        type.sysmeta.abi.dropGlueSymbol.clear();
        if (linkage.empty()) continue;
        const auto* declaration =
            mModule->findDeclarationByLinkage(linkage);
        if (!declaration) {
            error(nullptr, "type '" + type.displayName +
                           "' references missing Drop glue declaration '" +
                           linkage + "'");
            continue;
        }
        type.dropGlue = {declaration->symbolId, declaration->contractId};
    }

    for (auto& declaration : mModule->declarationTable) {
        if (const auto* type = mModule->findType(declaration.type))
            declaration.dropGlue = type->dropGlue;
        declaration.sysmeta.abi.dropGlueSymbol.clear();
        declaration.canonicalContract = canonicalContract(declaration);
        declaration.contractId = luna::identity::contractIdFromCanonical(
            declaration.canonicalContract);
        declaration.sysmeta.identity.contract = declaration.contractId;
    }
    mModule->rebuildIndexes();

    for (const auto& pending : mPendingDeclarationRefs) {
        if (!pending.target) continue;
        const auto* declaration = pending.lookupById
            ? mModule->findDeclarationById(pending.lookup)
            : mModule->findDeclarationByLinkage(pending.lookup);
        if (!declaration) {
            error(pending.source, pending.context +
                  " references missing MoonIR declaration '" +
                  pending.lookup + "'");
            continue;
        }
        if ((!pending.expectedSymbol.empty() &&
             pending.expectedSymbol != declaration->symbolId) ||
            (!pending.expectedContract.empty() &&
             pending.expectedContract != declaration->contractId)) {
            error(pending.source, pending.context +
                  " disagrees with the sealed SymbolId/ContractId");
            continue;
        }
        *pending.target = {
            declaration->symbolId, declaration->contractId};
    }

    const auto finalizeExecutable = [&](auto&& self, moon::Decl& declaration)
        -> void {
        const auto* record = mModule->findDeclaration(declaration.symbolId);
        if (record) declaration.contractId = record->contractId;
        declaration.sysmeta.abi.dropGlueSymbol.clear();
        if (auto* implementation =
                dynamic_cast<moon::ImplDecl*>(&declaration))
            for (auto& method : implementation->methods)
                if (method) self(self, *method);
    };
    for (auto& declaration : mModule->declarations)
        if (declaration) finalizeExecutable(
            finalizeExecutable, *declaration);
}

void LunaLowerer::buildModuleInterfaces() {
    if (!mModule || !mProgram) return;
    mModule->imports.clear();
    mModule->exports.clear();

    for (const auto& use : mProgram->packageUses) {
        ImportRecord record;
        record.kind = ImportKind::Package;
        record.ownerPackageId = use.ownerPackageId.empty()
            ? mModule->name : use.ownerPackageId;
        record.packageId = use.packageId;
        record.alias = use.alias;
        record.location = locationOf(&use);
        mModule->imports.push_back(std::move(record));
    }

    const auto visitExecutable = [&](auto&& self, moon::Decl& declaration,
                                     const auto& operation) -> void {
        operation(declaration);
        if (auto* implementation =
                dynamic_cast<moon::ImplDecl*>(&declaration))
            for (auto& method : implementation->methods)
                if (method) self(self, *method, operation);
    };

    for (const auto& host : mProgram->hostImports) {
        moon::FunctionDecl* match = nullptr;
        size_t matchCount = 0;
        for (auto& declaration : mModule->declarations) {
            if (!declaration) continue;
            visitExecutable(visitExecutable, *declaration,
                [&](moon::Decl& candidate) {
                    auto* function = dynamic_cast<moon::FunctionDecl*>(&candidate);
                    if (!function || function->packageId != host.ownerPackageId)
                        return;
                    const std::string localName = function->modulePath.empty()
                        ? function->name
                        : function->modulePath + "::" + function->name;
                    if (localName == host.localName) {
                        match = function;
                        ++matchCount;
                    }
                });
        }
        if (matchCount != 1 || !match || !match->isExtern || match->abi != "C") {
            error(&host, "host import '" + host.localName +
                "' does not identify one extern \"C\" function");
            continue;
        }
        const auto* declaration = mModule->findDeclaration(match->symbolId);
        if (!declaration) {
            error(&host, "host import '" + host.localName +
                "' has no canonical declaration record");
            continue;
        }
        ImportRecord record;
        record.kind = ImportKind::Host;
        record.ownerPackageId = host.ownerPackageId;
        record.localName = host.localName;
        record.capabilityId = host.capabilityId;
        record.linkSymbol = match->linkName.empty()
            ? match->generatedSymbolName : match->linkName;
        record.abi = match->abi;
        record.declaration = {declaration->symbolId, declaration->contractId};
        record.type = declaration->type;
        record.location = locationOf(&host);
        mModule->imports.push_back(std::move(record));
    }

    for (auto& executable : mModule->declarations) {
        if (!executable || !executable->isExported ||
            executable->packageId != mModule->name)
            continue;
        const auto* declaration = mModule->findDeclaration(executable->symbolId);
        if (!declaration) {
            error(nullptr, "exported declaration '" + executable->name +
                "' has no canonical declaration record");
            continue;
        }
        ExportRecord record;
        record.name = executable->generatedSymbolName.empty()
            ? executable->name : executable->generatedSymbolName;
        record.declaration = {declaration->symbolId, declaration->contractId};
        record.type = declaration->type;
        record.kind = declaration->kind;
        if (const auto* function =
                dynamic_cast<const moon::FunctionDecl*>(executable.get()))
            record.abi = function->abi;
        record.location = executable->location;
        mModule->exports.push_back(std::move(record));
    }

    std::sort(mModule->imports.begin(), mModule->imports.end(),
              [](const ImportRecord& left, const ImportRecord& right) {
        return std::tie(left.kind, left.ownerPackageId, left.localName,
                        left.packageId, left.alias) <
               std::tie(right.kind, right.ownerPackageId, right.localName,
                        right.packageId, right.alias);
    });
    std::sort(mModule->exports.begin(), mModule->exports.end(),
              [](const ExportRecord& left, const ExportRecord& right) {
        return std::tie(left.name, left.declaration.symbol.value) <
               std::tie(right.name, right.declaration.symbol.value);
    });
}

} // namespace moon
