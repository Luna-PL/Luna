#include "BodyAnalyzer.h"

#include "SemanticAnalysisSupport.h"
#include "../core/TypeLayout.h"
#include "../core/TypeRelations.h"
#include "../diagnostics/Diagnostic.h"
#include "../selector/Selector.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>
#include <unordered_set>

#include "BodyAnalyzerInternal.h"

TypePtr BodyAnalyzer::analyzeIntrinsicCall(CallExpr* call, IdentifierExpr* id) {
        if (id->name == "popcount_u32") {
            if (call->args.size() != 1 || !call->typeArgASTs.empty()) {
                mContext.error("popcount_u32 expects exactly one u32 argument",
                      call->line, call->col);
                return TyUnknown;
            }
            mContext.constrain(analyzeExpr(call->args.front().get()), TyU32,
                      "popcount_u32 argument");
            call->resultType = TyU32;
            return call->resultType;
        }
        if (id->name == "symbols") {
            if (call->args.size() != 1) {
                mContext.error("symbols expects exactly one declaration family",
                      call->line, call->col);
                return TyUnknown;
            }
            auto* familyName =
                dynamic_cast<IdentifierExpr*>(call->args.front().get());
            if (!familyName) {
                mContext.error("symbols requires a statically named declaration family",
                      call->line, call->col);
                return TyUnknown;
            }
            const std::string declarationKey =
                mContext.sourceDeclarationKey(familyName->name);
            auto family = mContext.mFunctionFamilies.find(declarationKey);
            if (family == mContext.mFunctionFamilies.end() ||
                family->second.empty()) {
                auto declaration =
                    mContext.mQualifiedDeclarations.find(declarationKey);
                if (declaration ==
                    mContext.mQualifiedDeclarations.end()) {
                    mContext.error("unknown declaration family '" +
                          familyName->name + "'", call->line, call->col);
                    return TyUnknown;
                }
                if (!call->typeArgASTs.empty()) {
                    mContext.error("non-function symbols query infers its "
                          "declaration type and accepts no type argument",
                          call->line, call->col);
                    return TyUnknown;
                }

                Decl* source = declaration->second;
                TypePtr declarationType;
                luna::selector::CatalogSymbolKind kind;
                const char* kindSpelling = nullptr;
                std::string symbolName;
                bool isUnspecializedGeneric = false;
                if (auto* structure = dynamic_cast<StructDecl*>(source)) {
                    kind = luna::selector::CatalogSymbolKind::Struct;
                    kindSpelling = "struct";
                    symbolName = structure->generatedSymbolName.empty()
                        ? structure->name : structure->generatedSymbolName;
                    declarationType = mContext.mSymTable.lookupType(symbolName);
                    if (!declarationType)
                        declarationType =
                            mContext.lookupDeclaredType(structure->name);
                    isUnspecializedGeneric = !structure->typeParams.empty();
                } else if (auto* enumeration =
                               dynamic_cast<EnumDecl*>(source)) {
                    kind = luna::selector::CatalogSymbolKind::Enum;
                    kindSpelling = "enum";
                    symbolName = enumeration->generatedSymbolName.empty()
                        ? enumeration->name : enumeration->generatedSymbolName;
                    declarationType = mContext.mSymTable.lookupType(symbolName);
                    if (!declarationType)
                        declarationType =
                            mContext.lookupDeclaredType(enumeration->name);
                    isUnspecializedGeneric = !enumeration->typeParams.empty();
                } else if (auto* trait =
                               dynamic_cast<TraitDecl*>(source)) {
                    kind = luna::selector::CatalogSymbolKind::Trait;
                    kindSpelling = "trait";
                    symbolName = trait->generatedSymbolName.empty()
                        ? trait->name : trait->generatedSymbolName;
                    declarationType = mContext.mSymTable.lookupType(
                        trait->resolvedTraitId);
                    if (!declarationType)
                        declarationType =
                            mContext.mSymTable.lookupType(symbolName);
                    isUnspecializedGeneric = !trait->typeParams.empty();
                } else if (auto* slot =
                               dynamic_cast<SlotDecl*>(source)) {
                    kind = luna::selector::CatalogSymbolKind::Slot;
                    kindSpelling = "slot";
                    symbolName = slot->generatedSymbolName.empty()
                        ? slot->name : slot->generatedSymbolName;
                    declarationType = slot->structuralType;
                } else if (auto* fragment =
                               dynamic_cast<FragmentDecl*>(source)) {
                    kind = luna::selector::CatalogSymbolKind::Fragment;
                    kindSpelling = "fragment";
                    symbolName = fragment->generatedSymbolName.empty()
                        ? fragment->name : fragment->generatedSymbolName;
                    declarationType = fragment->structuralType;
                } else if (auto* metadata =
                               dynamic_cast<MetaDecl*>(source)) {
                    kind =
                        luna::selector::CatalogSymbolKind::MetadataSchema;
                    kindSpelling = "meta";
                    symbolName = metadata->generatedSymbolName.empty()
                        ? metadata->name : metadata->generatedSymbolName;
                    declarationType =
                        mContext.mSymTable.lookupType(symbolName);
                } else {
                    mContext.error("symbols does not expose declaration kind "
                          "for '" + familyName->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                if (isUnspecializedGeneric) {
                    mContext.error("symbols cannot query unspecialized generic "
                          "declaration '" + familyName->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                declarationType = mContext.resolved(declarationType);
                if (!declarationType || declarationType == TyUnknown) {
                    mContext.error("symbols could not resolve declaration type "
                          "for '" + familyName->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                if (!mContext.mSymbolCatalog ||
                    !mContext.mSymbolCatalog->valid()) {
                    mContext.error("symbols requires a valid compile-time Symbol Catalog",
                          call->line, call->col);
                    return TyUnknown;
                }
                luna::selector::SymbolQuery query;
                query.phase = luna::selector::QueryPhase::CompileTime;
                query.kind = kind;
                query.familyId =
                    luna::identity::symbolIdFromCanonical(
                        nominalDeclarationIdentity(
                            mContext.mProgram, kindSpelling,
                            familyName->name, source));
                query.typeId = luna::types::typeId(declarationType);
                auto symbols = mContext.mSymbolCatalog->query(query);
                if (!symbols.valid() || symbols.size() != 1) {
                    mContext.error("declaration '" + familyName->name +
                          "' does not have one complete stable projection in "
                          "the compile-time Symbol Catalog",
                          call->line, call->col);
                    return TyUnknown;
                }
                call->isCompileTimeSymbolSet = true;
                call->compileTimeSymbolSetDeclarationIds.clear();
                for (const auto* symbol : symbols.orderedSymbols())
                    call->compileTimeSymbolSetDeclarationIds.push_back(
                        symbol->declarationId);
                call->resultType =
                    Type::makeSymbolSet(declarationType);
                return call->resultType;
            }
            TypePtr requested;
            if (!call->typeArgASTs.empty()) {
                if (call->typeArgASTs.size() != 1) {
                    mContext.error("symbols accepts at most one declaration type argument",
                          call->line, call->col);
                    return TyUnknown;
                }
                requested = mContext.resolved(mContext.resolveTypeAST(
                    call->typeArgASTs.front().get(), {}));
                if (requested->kind != TypeKind::Function) {
                    mContext.error("symbols type argument must be a callable type",
                          call->line, call->col);
                    return TyUnknown;
                }
            }
            TypePtr callableType;
            bool skippedUnspecializedGeneric = false;
            for (auto* candidate : family->second) {
                if (!candidate->typeParams.empty()) {
                    skippedUnspecializedGeneric = true;
                    continue;
                }
                TypeVec parameters;
                std::vector<luna::ownership::Contract> contracts;
                for (const auto& parameter : candidate->params) {
                    parameters.push_back(
                        mContext.resolved(parameter.inferredType));
                    contracts.push_back(
                        {parameter.relation, parameter.usage});
                }
                auto candidateType = Type::makeFunction(
                    std::move(parameters),
                    mContext.resolved(candidate->inferredReturnType),
                    std::move(contracts),
                    {luna::ownership::Relation::Owned,
                     candidate->returnUsage});
                if (requested &&
                    !luna::types::sameType(requested, candidateType))
                    continue;
                if (!callableType) callableType = candidateType;
                else if (!luna::types::sameType(
                             callableType, candidateType)) {
                    mContext.error("declaration family '" +
                          familyName->name +
                          "' has multiple callable types; provide "
                          "symbols::<Signature>(family)",
                          call->line, call->col);
                    return TyUnknown;
                }
            }
            if (skippedUnspecializedGeneric && !requested) {
                mContext.error("declaration family '" + familyName->name +
                      "' contains an unspecialized generic declaration; "
                      "provide a concrete non-generic Signature or query an "
                      "instantiated declaration after generic catalog support",
                      call->line, call->col);
                return TyUnknown;
            }
            if (!callableType) {
                if (skippedUnspecializedGeneric) {
                    mContext.error("symbols cannot return an unspecialized "
                          "generic declaration from family '" +
                          familyName->name + "'",
                          call->line, call->col);
                    return TyUnknown;
                }
                mContext.error("symbols found no declaration matching the requested "
                      "signature for '" + familyName->name + "'",
                      call->line, call->col);
                return TyUnknown;
            }
            if (!mContext.mSymbolCatalog ||
                !mContext.mSymbolCatalog->valid()) {
                mContext.error("symbols requires a valid compile-time Symbol Catalog",
                      call->line, call->col);
                return TyUnknown;
            }
            const auto familyDeclarationId = nominalDeclarationIdentity(
                mContext.mProgram, "fn", family->second.front()->name,
                family->second.front());
            luna::selector::SymbolQuery query;
            query.phase = luna::selector::QueryPhase::CompileTime;
            query.kind = luna::selector::CatalogSymbolKind::Function;
            query.familyId = luna::identity::symbolIdFromCanonical(
                familyDeclarationId);
            query.typeId = luna::types::typeId(callableType);
            auto symbols = mContext.mSymbolCatalog->query(query);
            size_t expected = 0;
            for (auto* candidate : family->second) {
                if (!candidate->typeParams.empty()) continue;
                TypeVec parameters;
                std::vector<luna::ownership::Contract> contracts;
                for (const auto& parameter : candidate->params) {
                    parameters.push_back(
                        mContext.resolved(parameter.inferredType));
                    contracts.push_back(
                        {parameter.relation, parameter.usage});
                }
                auto candidateType = Type::makeFunction(
                    std::move(parameters),
                    mContext.resolved(candidate->inferredReturnType),
                    std::move(contracts),
                    {luna::ownership::Relation::Owned,
                     candidate->returnUsage});
                if (!requested || luna::types::sameType(
                        requested, candidateType))
                    ++expected;
            }
            if (!symbols.valid() || symbols.size() != expected) {
                mContext.error("declaration family '" + familyName->name +
                      "' does not have a complete stable signature projection "
                      "in the compile-time Symbol Catalog",
                      call->line, call->col);
                return TyUnknown;
            }
            call->isCompileTimeSymbolSet = true;
            call->compileTimeSymbolSetDeclarationIds.clear();
            for (const auto* symbol : symbols.orderedSymbols())
                call->compileTimeSymbolSetDeclarationIds.push_back(
                    symbol->declarationId);
            call->resultType = Type::makeSymbolSet(callableType);
            return call->resultType;
        }
        if (id->name == "pointer_cast") {
            if (call->typeArgASTs.size() != 1 || call->args.size() != 1) {
                mContext.error(
                    "pointer_cast expects one target type argument and one raw pointer",
                    call->line, call->col);
                return TyUnknown;
            }
            TypePtr source = mContext.resolved(
                analyzeExpr(call->args.front().get()));
            if (source->kind != TypeKind::RawPointer) {
                mContext.error("pointer_cast source must be raw<T>, got " +
                      source->toString(), call->line, call->col);
                return TyUnknown;
            }
            TypePtr target = mContext.resolved(
                mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            call->typeArgs = {target};
            call->intrinsicType = target;
            call->resultType = Type::makeRawPointer(target);
            return call->resultType;
        }
        if (id->name == "drop_callback") {
            if (call->typeArgASTs.size() != 1 || !call->args.empty()) {
                mContext.error(
                    "drop_callback expects one type argument and no values",
                    call->line, call->col);
                return TyUnknown;
            }
            TypePtr target = mContext.resolved(
                mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            call->typeArgs = {target};
            call->intrinsicType = target;
            call->resultType = Type::makeRawPointer(TyU8);
            return call->resultType;
        }
        if (id->name == "range") {
            if (call->args.size() != 2) {
                mContext.error("range expects start and end integer values",
                      call->line, call->col);
                return TyUnknown;
            }
            mContext.requireInteger(analyzeExpr(call->args[0].get()), "range start");
            mContext.requireInteger(analyzeExpr(call->args[1].get()), "range end");
            call->iteratorOp = IteratorOp::Range;
            call->iteratorInputType = TyI32;
            call->iteratorOutputType = TyI32;
            call->resultType =
                Type::makeIterator(TyI32, IteratorMode::Range);
            return call->resultType;
        }
        if (id->name == "Ok" || id->name == "Err") {
            if (call->args.size() != 1) {
                mContext.error(id->name + " expects exactly one payload value",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr valueType = mContext.mConstraints.fresh();
            TypePtr errorType = mContext.mConstraints.fresh();
            if (!call->typeArgASTs.empty()) {
                if (call->typeArgASTs.size() != 2) {
                    mContext.error(id->name +
                          " explicit arguments must be `<Value, Error>`",
                          call->line, call->col);
                    return TyUnknown;
                }
                valueType =
                    mContext.resolveTypeAST(call->typeArgASTs[0].get(), {});
                errorType =
                    mContext.resolveTypeAST(call->typeArgASTs[1].get(), {});
            }
            TypePtr payload = analyzeExpr(call->args.front().get());
            mContext.constrain(payload, id->name == "Ok" ? valueType : errorType,
                      id->name + " payload");
            call->intrinsicType =
                Type::makeResult(valueType, errorType);
            mContext.mInferenceRoots.emplace_back(
                call->intrinsicType,
                "type arguments of '" + id->name + "'");
            call->returnUsage =
                defaultUsageForType(call->intrinsicType);
            return call->intrinsicType;
        }
        if (id->name == "is_ok" || id->name == "is_err" ||
            id->name == "unwrap" || id->name == "unwrap_err") {
            if (call->args.size() != 1) {
                mContext.error(id->name + " expects exactly one Result value",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr result = mContext.resolved(analyzeExpr(call->args.front().get()));
            if (result->kind != TypeKind::Result ||
                result->typeArgs.size() != 2) {
                mContext.error(id->name + " expects Result<T, E>, got " +
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
                mContext.error("panic expects exactly one string message",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr message = mContext.resolved(analyzeExpr(call->args.front().get()));
            if (message->kind != TypeKind::String &&
                message->kind != TypeKind::CStr)
                mContext.error("panic message must be string or cstr, got " +
                      message->toString(), call->line, call->col);
            call->intrinsicType = TyNever;
            call->resultType = TyNever;
            return TyNever;
        }
        if (id->name == "declaration_of" ||
            id->name == "declaration_id" ||
            id->name == "declaration_signature")
            return mContext.analyzeDeclarationReflectionCall(call, id->name);
        if (id->name == "declaration_count") {
            if (call->args.size() != 1) {
                mContext.error("declaration_count expects one declaration_view",
                      call->line, call->col);
                return TyI32;
            }
            TypePtr view = mContext.resolved(
                analyzeExpr(call->args.front().get()));
            if (view->kind != TypeKind::DeclarationView) {
                mContext.error("declaration_count expects one declaration_view",
                      call->line, call->col);
                return TyI32;
            }
            if (auto* source = dynamic_cast<CallExpr*>(
                    call->args.front().get());
                source && source->isCompileTimeQueryDeclarationView) {
                call->compileTimeValue = static_cast<int64_t>(
                    source->compileTimeDeclarationViewDeclarationIds.size());
            } else if (auto* identifier = dynamic_cast<IdentifierExpr*>(
                           call->args.front().get())) {
                if (const auto* symbol =
                        mContext.lookupSymbol(identifier->name);
                    symbol && symbol->isCompileTimeQueryDeclarationView)
                    call->compileTimeValue = static_cast<int64_t>(
                        symbol->compileTimeDeclarationViewDeclarationIds.size());
            }
            return TyI32;
        }
        if (id->name == "declaration_at") {
            if (call->args.size() != 2) {
                mContext.error("declaration_at expects a declaration_view and an index",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr view = mContext.resolved(analyzeExpr(call->args[0].get()));
            mContext.requireInteger(analyzeExpr(call->args[1].get()),
                           "declaration_at index");
            if (view->kind != TypeKind::DeclarationView) {
                mContext.error("first argument of declaration_at must be declaration_view",
                      call->line, call->col);
                return TyUnknown;
            }
            const std::vector<std::string>* declarationIds = nullptr;
            if (auto* source = dynamic_cast<CallExpr*>(call->args[0].get());
                source && source->isCompileTimeQueryDeclarationView)
                declarationIds =
                    &source->compileTimeDeclarationViewDeclarationIds;
            else if (auto* identifier = dynamic_cast<IdentifierExpr*>(
                         call->args[0].get())) {
                if (const auto* symbol =
                        mContext.lookupSymbol(identifier->name);
                    symbol && symbol->isCompileTimeQueryDeclarationView)
                    declarationIds =
                        &symbol->compileTimeDeclarationViewDeclarationIds;
            }
            if (declarationIds) {
                auto indexValue =
                    mContext.evaluateConstExpr(call->args[1].get());
                const auto* index = indexValue
                    ? std::get_if<int64_t>(&*indexValue) : nullptr;
                if (!index) {
                    mContext.error("declaration_at index must be a compile-time integer",
                          call->line, call->col);
                } else if (*index < 0 ||
                           static_cast<size_t>(*index) >= declarationIds->size()) {
                    mContext.error("declaration_at index " +
                          std::to_string(*index) + " is out of range",
                          call->line, call->col);
                } else {
                    const auto* selected = mContext.mSymbolCatalog
                        ? mContext.mSymbolCatalog->findDeclaration(
                              (*declarationIds)[static_cast<size_t>(*index)])
                        : nullptr;
                    if (!selected) {
                        mContext.error("declaration_at result is outside the active Symbol Catalog",
                              call->line, call->col);
                    } else {
                        call->compileTimeDeclarationId =
                            selected->declarationId;
                        call->resolvedSymbolName = selected->symbolName;
                        call->resolvedSymbolId = selected->symbolId;
                        call->resolvedContractId = selected->contractId;
                        call->isCompileTimeQueryDeclarationRef = true;
                    }
                }
            }
            call->resultType = Type::makeDeclarationRef(view->inner);
            return call->resultType;
        }
        if (id->name == "metadata" ||
            id->name == "declaration_has_metadata") {
            if (call->typeArgASTs.size() != 1 || call->args.size() != 1) {
                mContext.error(id->name +
                      " expects one metadata type argument and one declaration_ref",
                      call->line, call->col);
                return TyUnknown;
            }
            TypePtr metadataType =
                mContext.resolved(mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
            TypePtr declaration = mContext.resolved(analyzeExpr(call->args.front().get()));
            if (metadataType->kind != TypeKind::Metadata)
                mContext.error(id->name + " type argument must be a meta schema",
                      call->line, call->col);
            if (declaration->kind != TypeKind::DeclarationRef)
                mContext.error(id->name + " value argument must be declaration_ref",
                      call->line, call->col);
            if (id->name == "declaration_has_metadata" &&
                metadataType->kind == TypeKind::Metadata) {
                std::string declarationId;
                if (auto* reflected =
                        dynamic_cast<CallExpr*>(call->args.front().get()))
                    declarationId = reflected->compileTimeDeclarationId;
                else if (auto* identifier = dynamic_cast<IdentifierExpr*>(
                             call->args.front().get())) {
                    if (auto* symbol = mContext.mSymTable.lookup(identifier->name))
                        declarationId = symbol->compileTimeDeclarationId;
                }
                if (!declarationId.empty()) {
                    bool attached = false;
                    for (const auto& [familyName, family] : mContext.mFunctionFamilies) {
                        for (auto* candidate : family) {
                            if (functionDeclarationIdentity(
                                    mContext.mProgram, candidate) !=
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
            call->resultType = id->name == "metadata"
                ? Type::makeMetadataView(metadataType) : TyBool;
            return call->resultType;
        }
        auto* symbol = mContext.lookupSymbol(id->name);
        if (symbol && symbol->kind == SymbolKind::Metadata) {
            if (call->args.size() != symbol->paramTypes.size()) {
                mContext.error("metadata constructor '" + id->name + "' expects " +
                      std::to_string(symbol->paramTypes.size()) + " arguments",
                      call->line, call->col);
                return TyUnknown;
            }
            for (size_t index = 0; index < call->args.size(); ++index)
                mContext.constrain(analyzeExpr(call->args[index].get()), symbol->paramTypes[index],
                          "metadata constructor argument " + std::to_string(index + 1));
            return symbol->returnType;
        }
        if (id->name == "select_unique") {
            if (call->args.size() != 2) {
                mContext.error("select_unique expects a DeclarationView and one metadata value",
                      call->line, call->col);
                return TyUnknown;
            }
            auto view = mContext.resolved(analyzeExpr(call->args[0].get()));
            auto metadata = mContext.resolved(analyzeExpr(call->args[1].get()));
            if (view->kind != TypeKind::DeclarationView)
                mContext.error("first argument of select_unique must be declaration_view",
                      call->line, call->col);
            if (metadata->kind != TypeKind::Metadata)
                mContext.error("second argument of select_unique must be a metadata value",
                      call->line, call->col);
            return Type::makeDeclarationRef(view->inner);
        }
        const std::string conceptKey = mContext.sourceDeclarationKey(id->name, false);
        auto constraintIt = mContext.mConcepts.find(conceptKey);
        if (constraintIt != mContext.mConcepts.end()) {
            if (!call->args.empty() ||
                call->typeArgASTs.size() != constraintIt->second->typeParams.size()) {
                mContext.error("constraint '" + constraintIt->second->name + "' expects " +
                      std::to_string(constraintIt->second->typeParams.size()) +
                      " type arguments and no value arguments",
                      call->line, call->col);
            } else {
                for (auto& type : call->typeArgASTs)
                    mContext.resolveTypeAST(type.get(), {});
            }
            return TyBool;
        }
    return nullptr;
}
