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

SemanticContext::SemanticContext() {
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
    enterSlotScope();
    mFragments.clear();
    mMetadataSchemas.clear();
    mConcepts.clear();
    mFunctionFamilies.clear();
    mSymbolCatalog.reset();
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
    mTraitOwners[luna::sysmeta::DropTraitId] =
        luna::core_contracts::PackageId;
    mTraitMethods[luna::sysmeta::FromTraitId] = {
        {luna::sysmeta::FromMethodName, nullptr}
    };
    mTraitOwners[luna::sysmeta::FromTraitId] =
        luna::core_contracts::PackageId;
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
        else if (auto* s = dynamic_cast<SlotDecl*>(declaration)) name = s->name;
        else if (auto* s = dynamic_cast<StructDecl*>(declaration)) name = s->name;
        else if (auto* e = dynamic_cast<EnumDecl*>(declaration)) name = e->name;
        else if (auto* t = dynamic_cast<TraitDecl*>(declaration)) name = t->name;
        else if (auto* m = dynamic_cast<MetaDecl*>(declaration)) name = m->name;
        else if (auto* c = dynamic_cast<ConstraintDecl*>(declaration)) name = c->name;
        if (!name.empty())
            ++linkageNameCounts[
                metadataDeclarationName(name, declaration)];
    }
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto* declaration = program->declarations[i].get();
        setDeclarationContext(declaration);
        setDiagnosticLocation(declaration);
        std::string name;
        if (auto* f = dynamic_cast<FunctionDecl*>(declaration)) name = f->name;
        else if (auto* f = dynamic_cast<FragmentDecl*>(declaration)) name = f->name;
        else if (auto* s = dynamic_cast<SlotDecl*>(declaration)) name = s->name;
        else if (auto* s = dynamic_cast<StructDecl*>(declaration)) name = s->name;
        else if (auto* e = dynamic_cast<EnumDecl*>(declaration)) name = e->name;
        else if (auto* t = dynamic_cast<TraitDecl*>(declaration)) name = t->name;
        else if (auto* m = dynamic_cast<MetaDecl*>(declaration)) name = m->name;
        else if (auto* c = dynamic_cast<ConstraintDecl*>(declaration)) name = c->name;
        if (!name.empty()) {
            const std::string familyKey = qualifiedDeclarationKey(
                mCurrentPackageId, mCurrentModulePath, name);
            const std::string sourceLinkage = metadataDeclarationName(name, declaration);
            const std::string sourceIdentity =
                declarationSourceIdentity(name, declaration);
            const bool isRootEntry = name == "main" &&
                mCurrentPackageId == (program->packageName.empty()
                    ? std::string("main") : program->packageName);
            if (isRootEntry && ++rootEntryCount > 1)
                error("Package has more than one 'main' entry declaration");
            declaration->generatedSymbolName = isRootEntry
                ? "main"
                : (linkageNameCounts[sourceLinkage] > 1 || sourceLinkage == "main")
                ? isolatedLinkageName(
                      familyKey + "::" + sourceIdentity,
                      sourceLinkage)
                : sourceLinkage;
            const std::string identity = familyKey + "::" + sourceIdentity;
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
            if (isPredefinedTypeName(s->name)) continue;
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
            if (isPredefinedTypeName(e->name)) continue;
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
    // Module-level slots must be registered before fragments because every
    // fragment names one nominal SlotId as its target.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* slot = dynamic_cast<SlotDecl*>(
                program->declarations[i].get())) {
            setDeclarationContext(slot);
            setDiagnosticLocation(slot);
            declareSlot(slot);
        }
    }
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
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        if (auto* slot = dynamic_cast<SlotDecl*>(
                program->declarations[i].get())) {
            setDeclarationContext(slot);
            setDiagnosticLocation(slot);
            finalizeSlot(slot);
        }
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
    // Pass 2b: validate implementation contracts before ordinary bodies.
    // Drop changes the inherent ResourceContract of its nominal family, so
    // function semantics must not depend on whether an impl appeared before
    // or after the function in source order.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        if (auto* implementation = dynamic_cast<ImplDecl*>(decl.get())) {
            setDeclarationContext(implementation);
            setDiagnosticLocation(implementation);
            analyzeImpl(implementation);
        }
    }
    // All source declaration signatures, metadata, nominal resource facts,
    // and impl-derived Drop contracts are now stable for body analysis. Build
    // one immutable compile-time function projection for every selector in
    // this semantic run.
    rebuildSymbolCatalog();
    // Pass 2c: analyze all remaining ordinary bodies.
    for (size_t i = 0; i < sourceDeclarationCount; ++i) {
        auto& decl = program->declarations[i];
        if (dynamic_cast<ImplDecl*>(decl.get())) continue;
        setDeclarationContext(decl.get());
        setDiagnosticLocation(decl.get());
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) analyzeFunction(f);
        else if (auto* s = dynamic_cast<StructDecl*>(decl.get())) analyzeStruct(s);
        else if (auto* e = dynamic_cast<EnumDecl*>(decl.get())) analyzeEnum(e);
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

void SemanticContext::rebuildSymbolCatalog() {
    std::vector<luna::selector::CatalogSymbol> symbols;
    if (!mProgram) {
        mSymbolCatalog = std::make_shared<luna::selector::SymbolCatalog>();
        return;
    }
    struct ProjectionState {
        const Decl* declaration = nullptr;
        std::string dropGlueLinkage;
    };
    std::vector<ProjectionState> projectionStates;
    std::unordered_map<std::string, size_t> functionsByLinkage;

    const auto retention = [](RetentionKind value) {
        if (value == RetentionKind::Runtime)
            return luna::selector::Retention::Runtime;
        return luna::selector::Retention::CompileTime;
    };
    const auto declarationTypeForFunction = [&](FunctionDecl* function) {
        TypeVec parameters;
        std::vector<luna::ownership::Contract> contracts;
        for (const auto& parameter : function->params) {
            parameters.push_back(resolved(parameter.inferredType));
            contracts.push_back({parameter.relation, parameter.usage});
        }
        return Type::makeFunction(
            std::move(parameters), resolved(function->inferredReturnType),
            std::move(contracts),
            {luna::ownership::Relation::Owned, function->returnUsage});
    };
    const auto appendProjection = [&](Decl* declaration,
                                      luna::selector::CatalogSymbolKind kind,
                                      const std::string& kindSpelling,
                                      const std::string& sourceName,
                                      const std::string& symbolName,
                                      const std::string& familyName,
                                      TypePtr declarationType) {
        if (!declarationType) {
            error("Symbol Catalog could not resolve type for declaration '" +
                  sourceName + "'", declaration->line, declaration->col);
            return;
        }

        luna::selector::CatalogSymbol symbol;
        symbol.symbolName = symbolName;
        symbol.declarationId =
            kind == luna::selector::CatalogSymbolKind::Function
                ? functionDeclarationIdentity(
                      mProgram,
                      dynamic_cast<FunctionDecl*>(declaration))
                : nominalDeclarationIdentity(
                      mProgram, kindSpelling.c_str(), symbolName,
                      declaration);
        symbol.familyDeclarationId = nominalDeclarationIdentity(
            mProgram, kindSpelling.c_str(), familyName, declaration);
        symbol.symbolId = luna::identity::symbolIdFromCanonical(
            symbol.declarationId);
        symbol.familyId = luna::identity::symbolIdFromCanonical(
            symbol.familyDeclarationId);
        symbol.kind = kind;
        symbol.canonicalContract = declarationCanonicalContract(
            declaration, kind, declarationType);
        symbol.contractId = luna::identity::contractIdFromCanonical(
            symbol.canonicalContract);
        symbol.typeId = luna::types::typeId(declarationType);
        symbol.type = std::move(declarationType);
        symbol.retention = retention(declaration->retention);
        for (const auto& attachment : declaration->metadata) {
            luna::selector::Metadata metadata;
            metadata.schemaId = attachment.resolvedSchemaId;
            metadata.values = attachment.evaluatedArguments;
            metadata.retention = retention(attachment.retention);
            symbol.metadata.push_back(std::move(metadata));
        }
        if (kind == luna::selector::CatalogSymbolKind::Function) {
            const auto insertion = functionsByLinkage.emplace(
                symbolName, symbols.size());
            if (!insertion.second)
                error("Symbol Catalog contains duplicate function linkage '" +
                      symbolName + "'", declaration->line, declaration->col);
        }
        projectionStates.push_back({
            declaration, symbol.type->sysmeta.abi.dropGlueSymbol});
        symbols.push_back(std::move(symbol));
    };
    const auto appendFunction = [&](FunctionDecl* function) {
        auto type = declarationTypeForFunction(function);
        // Inferred returns are finalized by body analysis. They are not stable
        // query identities at this pre-body snapshot checkpoint and therefore
        // must not leak an inference-variable TypeId into the catalog.
        if (containsInferenceIdentity(type)) return;
        const std::string symbolName = function->generatedSymbolName.empty()
            ? function->name : function->generatedSymbolName;
        appendProjection(
            function, luna::selector::CatalogSymbolKind::Function,
            "fn", function->name, symbolName, function->name,
            std::move(type));
    };

    for (const auto& declaration : mProgram->declarations) {
        if (auto* function =
                dynamic_cast<FunctionDecl*>(declaration.get())) {
            appendFunction(function);
        } else if (auto* slot =
                       dynamic_cast<SlotDecl*>(declaration.get())) {
            const std::string symbolName = slot->generatedSymbolName.empty()
                ? slot->name : slot->generatedSymbolName;
            appendProjection(
                slot, luna::selector::CatalogSymbolKind::Slot,
                "slot", slot->name, symbolName, slot->name,
                slot->structuralType);
        } else if (auto* fragment =
                       dynamic_cast<FragmentDecl*>(declaration.get())) {
            const std::string symbolName = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            appendProjection(
                fragment, luna::selector::CatalogSymbolKind::Fragment,
                "fragment", fragment->name, symbolName, fragment->name,
                fragment->structuralType);
        } else if (auto* structure =
                       dynamic_cast<StructDecl*>(declaration.get())) {
            const std::string symbolName = structure->generatedSymbolName.empty()
                ? structure->name : structure->generatedSymbolName;
            auto type = mSymTable.lookupType(symbolName);
            if (!type) type = mSymTable.lookupType(structure->name);
            appendProjection(
                structure, luna::selector::CatalogSymbolKind::Struct,
                "struct", structure->name, symbolName, structure->name,
                std::move(type));
        } else if (auto* enumeration =
                       dynamic_cast<EnumDecl*>(declaration.get())) {
            const std::string symbolName = enumeration->generatedSymbolName.empty()
                ? enumeration->name : enumeration->generatedSymbolName;
            auto type = mSymTable.lookupType(symbolName);
            if (!type) type = mSymTable.lookupType(enumeration->name);
            appendProjection(
                enumeration, luna::selector::CatalogSymbolKind::Enum,
                "enum", enumeration->name, symbolName, enumeration->name,
                std::move(type));
        } else if (auto* trait =
                       dynamic_cast<TraitDecl*>(declaration.get())) {
            const std::string symbolName = trait->generatedSymbolName.empty()
                ? trait->name : trait->generatedSymbolName;
            auto type = mSymTable.lookupType(trait->resolvedTraitId);
            if (!type) type = mSymTable.lookupType(symbolName);
            appendProjection(
                trait, luna::selector::CatalogSymbolKind::Trait,
                "trait", trait->name, symbolName, trait->name,
                std::move(type));
        } else if (auto* implementation =
                       dynamic_cast<ImplDecl*>(declaration.get())) {
            std::unordered_map<std::string, TypePtr> bindings;
            for (const auto& parameter : implementation->typeParams)
                bindings[parameter] = Type::makeTypeParam(parameter);
            auto targetType = resolveTypeAST(
                implementation->targetType.get(), bindings);
            const std::string traitId =
                implementation->trait.resolvedTraitId;
            const std::string symbolName =
                implementation->generatedSymbolName.empty()
                ? traitId + "__for__" + implementation->resolvedTargetTypeId
                : implementation->generatedSymbolName;
            appendProjection(
                implementation,
                luna::selector::CatalogSymbolKind::Implementation,
                "impl", "impl", symbolName, traitId,
                std::move(targetType));
            for (const auto& method : implementation->methods)
                appendFunction(method.get());
        } else if (auto* metadata =
                       dynamic_cast<MetaDecl*>(declaration.get())) {
            const std::string symbolName = metadata->generatedSymbolName.empty()
                ? metadata->name : metadata->generatedSymbolName;
            appendProjection(
                metadata,
                luna::selector::CatalogSymbolKind::MetadataSchema,
                "meta", metadata->name, symbolName, metadata->name,
                mSymTable.lookupType(symbolName));
        }
        // Named constraints are compiler predicates and intentionally erase
        // before both the Symbol Catalog and MoonIR declaration table.
    }

    // Seal resource contracts only after every function row exists: nominal
    // type and impl contracts bind Drop by strong declaration identity, not by
    // the frontend linkage string carried during semantic analysis.
    for (size_t index = 0; index < symbols.size(); ++index) {
        const auto& dropGlueLinkage =
            projectionStates[index].dropGlueLinkage;
        if (dropGlueLinkage.empty()) continue;
        const auto drop = functionsByLinkage.find(dropGlueLinkage);
        if (drop == functionsByLinkage.end()) {
            error("Symbol Catalog cannot resolve Drop glue '" +
                  dropGlueLinkage + "'");
            continue;
        }
        const auto& dropSymbol = symbols[drop->second];
        auto& symbol = symbols[index];
        symbol.canonicalContract = declarationCanonicalContract(
            projectionStates[index].declaration, symbol.kind, symbol.type,
            dropSymbol.symbolId, dropSymbol.contractId);
        symbol.contractId = luna::identity::contractIdFromCanonical(
            symbol.canonicalContract);
    }
    auto catalog = std::make_shared<luna::selector::SymbolCatalog>(
        std::move(symbols));
    if (!catalog->valid())
        error("invalid Symbol Catalog: " + catalog->error());
    mSymbolCatalog = std::move(catalog);
}

// ─── Declaration pass ──────────────────────────────────────────────
