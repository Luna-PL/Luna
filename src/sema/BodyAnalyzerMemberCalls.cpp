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

TypePtr BodyAnalyzer::analyzeMemberCall(
    CallExpr* call, FieldAccessExpr* member) {
    TypePtr receiver = analyzeExpr(member->object.get());
    // Method lookup is an immediate type-selection boundary, just like
    // generic monomorphization. Resolve the default for an otherwise
    // unconstrained numeric receiver before consulting trait impls.
    mContext.mConstraints.defaultNumeric(receiver);
    receiver = mContext.resolved(receiver);
    const std::string methodName = member->field;
    if (receiver->kind == TypeKind::SymbolSet) {
        bool hasSet = false;
        std::vector<std::string> declarationIds;
        if (auto* source = dynamic_cast<CallExpr*>(member->object.get());
            source && source->isCompileTimeSymbolSet) {
            hasSet = true;
            declarationIds = source->compileTimeSymbolSetDeclarationIds;
        } else if (auto* identifier =
                       dynamic_cast<IdentifierExpr*>(member->object.get())) {
            if (auto* symbol = mContext.lookupSymbol(identifier->name);
                symbol && symbol->isCompileTimeSymbolSet) {
                hasSet = true;
                declarationIds =
                    symbol->compileTimeSymbolSetDeclarationIds;
            }
        }
        if (!hasSet || !mContext.mSymbolCatalog ||
            !mContext.mSymbolCatalog->valid()) {
            mContext.error("symbol_set value is not backed by the active "
                  "compile-time Symbol Catalog", call->line, call->col);
            return TyUnknown;
        }
        luna::selector::SymbolQuery query;
        query.phase = luna::selector::QueryPhase::CompileTime;
        switch (receiver->inner
                    ? mContext.resolved(receiver->inner)->kind
                    : TypeKind::Unknown) {
            case TypeKind::Function:
                query.kind =
                    luna::selector::CatalogSymbolKind::Function;
                break;
            case TypeKind::Fragment:
                query.kind =
                    luna::selector::CatalogSymbolKind::Fragment;
                break;
            case TypeKind::Slot:
                query.kind =
                    luna::selector::CatalogSymbolKind::Slot;
                break;
            case TypeKind::Struct:
                query.kind = luna::selector::CatalogSymbolKind::Struct;
                break;
            case TypeKind::Enum:
                query.kind = luna::selector::CatalogSymbolKind::Enum;
                break;
            case TypeKind::Trait:
                query.kind = luna::selector::CatalogSymbolKind::Trait;
                break;
            case TypeKind::Metadata:
                query.kind =
                    luna::selector::CatalogSymbolKind::MetadataSchema;
                break;
            default:
                mContext.error("symbol_set has an unsupported declaration type '" +
                      (receiver->inner
                          ? receiver->inner->toString()
                          : std::string{"?"}) + "'",
                      call->line, call->col);
                return TyUnknown;
        }
        if (receiver->inner)
            query.typeId = luna::types::typeId(receiver->inner);
        auto universe = mContext.mSymbolCatalog->query(query);
        std::vector<luna::identity::SymbolId> symbolIds;
        symbolIds.reserve(declarationIds.size());
        for (const auto& id : declarationIds)
            symbolIds.push_back(
                luna::identity::symbolIdFromCanonical(id));
        auto symbols = universe.select(symbolIds);
        if (!symbols.valid()) {
            mContext.error(symbols.error(), call->line, call->col);
            return TyUnknown;
        }

        if (methodName == "matching") {
            if (!call->typeArgASTs.empty() || call->args.size() != 1) {
                mContext.error("symbol_set.matching expects exactly one "
                      "metadata value", call->line, call->col);
                return TyUnknown;
            }
            TypePtr metadataType = mContext.resolved(
                analyzeExpr(call->args.front().get()));
            auto* metadataCall =
                dynamic_cast<CallExpr*>(call->args.front().get());
            auto* metadataName = metadataCall
                ? dynamic_cast<IdentifierExpr*>(
                      metadataCall->callee.get())
                : nullptr;
            if (metadataType->kind != TypeKind::Metadata ||
                !metadataCall || !metadataName) {
                mContext.error("symbol_set.matching requires a directly "
                      "constructed metadata value", call->line, call->col);
                return TyUnknown;
            }
            const std::string metadataKey =
                mContext.sourceDeclarationKey(metadataName->name, false);
            auto schema = mContext.mMetadataSchemas.find(metadataKey);
            if (schema == mContext.mMetadataSchemas.end()) {
                mContext.error("unknown metadata schema '" +
                      metadataName->name + "'", call->line, call->col);
                return TyUnknown;
            }
            std::vector<ConstValue> values;
            values.reserve(metadataCall->args.size());
            for (auto& argument : metadataCall->args) {
                auto value = mContext.evaluateConstExpr(argument.get());
                if (!value) {
                    mContext.error("symbol_set.matching metadata arguments "
                          "must be compile-time values",
                          call->line, call->col);
                    return TyUnknown;
                }
                values.push_back(std::move(*value));
            }
            const auto schemaSymbol =
                schema->second->generatedSymbolName.empty()
                ? schema->second->name
                : schema->second->generatedSymbolName;
            const auto schemaId = nominalDeclarationIdentity(
                mContext.mProgram, "meta", schemaSymbol,
                schema->second);
            auto matches = symbols.filterMetadata(schemaId, values);
            call->isCompileTimeSymbolSet = true;
            call->compileTimeSymbolSetDeclarationIds.clear();
            for (const auto* symbol : matches.orderedSymbols())
                call->compileTimeSymbolSetDeclarationIds.push_back(
                    symbol->declarationId);
            call->resultType = Type::makeSymbolSet(receiver->inner);
            return call->resultType;
        }

        if (methodName == "one") {
            if (!call->typeArgASTs.empty() || !call->args.empty()) {
                mContext.error("symbol_set.one expects no arguments",
                      call->line, call->col);
                return TyUnknown;
            }
            auto terminal = symbols.one();
            if (!terminal.oneSucceeded()) {
                mContext.error(terminal.message,
                      call->line, call->col);
                return TyUnknown;
            }
            call->compileTimeDeclarationId =
                terminal.selected->declarationId;
            call->resolvedSymbolName =
                terminal.selected->symbolName;
            call->resolvedSymbolId =
                terminal.selected->symbolId;
            call->resolvedContractId =
                terminal.selected->contractId;
            call->isCompileTimeQueryDeclarationRef = true;
            call->resultType =
                Type::makeDeclarationRef(receiver->inner);
            return call->resultType;
        }

        if (methodName == "optional") {
            if (!call->typeArgASTs.empty() || !call->args.empty()) {
                mContext.error("symbol_set.optional expects no arguments",
                      call->line, call->col);
                return TyUnknown;
            }
            auto terminal = symbols.optional();
            if (!terminal.optionalSucceeded()) {
                mContext.error(terminal.message,
                      call->line, call->col);
                return TyUnknown;
            }
            call->isCompileTimeOptionalDeclarationRef = true;
            call->compileTimeOptionalHasValue =
                terminal.oneSucceeded();
            if (terminal.oneSucceeded()) {
                call->compileTimeDeclarationId =
                    terminal.selected->declarationId;
                call->resolvedSymbolName =
                    terminal.selected->symbolName;
                call->resolvedSymbolId =
                    terminal.selected->symbolId;
                call->resolvedContractId =
                    terminal.selected->contractId;
            }
            call->resultType = Type::makeCompileTimeOption(
                Type::makeDeclarationRef(receiver->inner));
            return call->resultType;
        }
        if (methodName == "all") {
            if (!call->args.empty() || call->typeArgASTs.size() > 1) {
                mContext.error("symbol_set.all expects no values and at most "
                      "one metadata ordering type", call->line, call->col);
                return TyUnknown;
            }
            luna::selector::SymbolSet ordered = symbols.all();
            if (!call->typeArgASTs.empty()) {
                TypePtr metadataType = mContext.resolved(
                    mContext.resolveTypeAST(
                        call->typeArgASTs.front().get(), {}));
                if (metadataType->kind != TypeKind::Metadata) {
                    mContext.error("symbol_set.all ordering type must be a meta schema",
                          call->line, call->col);
                    return TyUnknown;
                }
                call->typeArgs = {metadataType};
                ordered = symbols.allByMetadata(metadataType->nominalId);
            }
            if (!ordered.valid()) {
                mContext.error(ordered.error(), call->line, call->col);
                return TyUnknown;
            }
            call->isCompileTimeQueryDeclarationView = true;
            call->compileTimeDeclarationViewDeclarationIds.clear();
            for (const auto* symbol : ordered.orderedSymbols())
                call->compileTimeDeclarationViewDeclarationIds.push_back(
                    symbol->declarationId);
            call->resultType =
                Type::makeDeclarationView(receiver->inner);
            return call->resultType;
        }
        mContext.error("symbol_set has no query operation '" +
              methodName + "'", call->line, call->col);
        return TyUnknown;
    }
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

    if (mContext.mInKernel) {
        mContext.error("user trait method calls are not yet available in kernel code",
              call->line, call->col);
        return TyUnknown;
    }
    if (!call->typeArgASTs.empty()) {
        mContext.error("generic trait methods are not yet supported by member syntax",
              call->line, call->col);
        return TyUnknown;
    }

    TypePtr target = receiver;
    if (target->kind == TypeKind::Reference && target->inner)
        target = mContext.resolved(target->inner);
    const std::string targetId = mContext.typeIdentity(target);

    struct Candidate {
        std::string traitId;
        FunctionDecl* method = nullptr;
        TypePtr receiverType;
        TypeVec implArguments;
    };
    std::vector<Candidate> candidates;
    for (const auto& [traitId, targets] : mContext.mImpls) {
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
        TypePtr expected = mContext.resolved(
            method->second->params.front().inferredType);
        bool acceptsReceiver =
            luna::types::sameType(expected, receiver) ||
            luna::types::sameType(expected, target);
        if (expected->kind == TypeKind::Reference &&
            expected->inner)
            acceptsReceiver = luna::types::sameType(
                mContext.resolved(expected->inner), target);
        if (acceptsReceiver)
            candidates.push_back(
                {traitId, method->second, expected, {}});
    }

    const auto matchImplPattern = [&](const TypePtr& patternRoot,
                                      const TypePtr& actualRoot,
                                      const std::vector<std::string>& parameters,
                                      std::unordered_map<std::string, TypePtr>& bindings) {
        std::function<bool(const TypePtr&, const TypePtr&)> match =
            [&](const TypePtr& patternValue,
                const TypePtr& actualValue) -> bool {
                const TypePtr pattern = mContext.resolved(patternValue);
                const TypePtr actual = mContext.resolved(actualValue);
                if (!pattern || !actual) return false;
                if (pattern->kind == TypeKind::TypeParam &&
                    std::find(parameters.begin(), parameters.end(),
                              pattern->name) != parameters.end()) {
                    auto existing = bindings.find(pattern->name);
                    if (existing == bindings.end()) {
                        bindings[pattern->name] = actual;
                        return true;
                    }
                    return luna::types::sameType(existing->second, actual);
                }
                if (pattern->kind != actual->kind ||
                    pattern->isMutable != actual->isMutable ||
                    pattern->arrayLength != actual->arrayLength)
                    return false;
                if (!pattern->nominalId.empty() &&
                    pattern->nominalId != actual->nominalId)
                    return false;
                if (pattern->typeArgs.size() != actual->typeArgs.size())
                    return false;
                for (size_t index = 0; index < pattern->typeArgs.size(); ++index)
                    if (!match(pattern->typeArgs[index], actual->typeArgs[index]))
                        return false;
                if (static_cast<bool>(pattern->inner) !=
                    static_cast<bool>(actual->inner))
                    return false;
                return !pattern->inner || match(pattern->inner, actual->inner);
            };
        return match(patternRoot, actualRoot);
    };

    // Exact impl lookup above remains the fast path. Generic impls are
    // ordinary templates: match their target nominal pattern, then route the
    // selected method through the existing function monomorphizer.
    if (mContext.mProgram) {
        for (const auto& declaration : mContext.mProgram->declarations) {
            auto* implementation = dynamic_cast<ImplDecl*>(declaration.get());
            if (!implementation || implementation->typeParams.empty() ||
                implementation->trait.resolvedTraitId.empty())
                continue;
            const std::string traitId =
                implementation->trait.resolvedTraitId;
            if (traitId == luna::sysmeta::DropTraitId ||
                traitId == luna::sysmeta::FromTraitId)
                continue;
            std::unordered_map<std::string, TypePtr> patternBindings;
            for (const auto& parameter : implementation->typeParams)
                patternBindings[parameter] = Type::makeTypeParam(parameter);
            TypePtr pattern = mContext.resolved(mContext.resolveTypeAST(
                implementation->targetType.get(), patternBindings));
            std::unordered_map<std::string, TypePtr> concreteBindings;
            if (!matchImplPattern(pattern, target,
                                  implementation->typeParams,
                                  concreteBindings))
                continue;
            auto method = std::find_if(
                implementation->methods.begin(), implementation->methods.end(),
                [&](const std::unique_ptr<FunctionDecl>& candidate) {
                    return candidate && candidate->name == methodName;
                });
            if (method == implementation->methods.end() ||
                !*method || (*method)->params.empty())
                continue;
            TypeVec arguments;
            bool complete = true;
            for (const auto& parameter : implementation->typeParams) {
                auto found = concreteBindings.find(parameter);
                if (found == concreteBindings.end()) {
                    complete = false;
                    break;
                }
                arguments.push_back(found->second);
            }
            if (!complete) continue;
            TypePtr expected = substituteNominalType(
                mContext.resolved((*method)->params.front().inferredType),
                concreteBindings);
            bool acceptsReceiver =
                luna::types::sameType(expected, receiver) ||
                luna::types::sameType(expected, target);
            if (expected->kind == TypeKind::Reference && expected->inner)
                acceptsReceiver = luna::types::sameType(
                    mContext.resolved(expected->inner), target);
            if (acceptsReceiver)
                candidates.push_back(
                    {traitId, method->get(), expected,
                     std::move(arguments)});
        }
    }

    if (candidates.empty()) {
        mContext.error("no trait method '" + methodName +
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
        mContext.error("member call '" + methodName +
              "' is ambiguous for type '" + target->toString() +
              "' across traits: " + traits,
              call->line, call->col);
        return TyUnknown;
    }

    auto selected = candidates.front();
    FunctionDecl* sourceMethod = selected.method;
    FunctionDecl* method = sourceMethod;
    if (!selected.implArguments.empty()) {
        method = mContext.monomorphize(
            sourceMethod, selected.implArguments);
        if (method && mContext.mProgram) {
            const bool newlyCreated =
                !mContext.mGeneratedInstances.empty() &&
                mContext.mGeneratedInstances.back().get() == method;
            if (newlyCreated) {
                mContext.mProgram->declarations.push_back(
                    std::move(mContext.mGeneratedInstances.back()));
                const std::string savedPackage = mContext.mCurrentPackageId;
                const std::string savedModule = mContext.mCurrentModulePath;
                mContext.setDeclarationContext(method);
                mContext.declareFunction(method);
                analyzeFunction(method);
                mContext.mCurrentPackageId = savedPackage;
                mContext.mCurrentModulePath = savedModule;
            }
            selected.receiverType = mContext.resolved(
                method->params.front().inferredType);
        }
        if (!method) return TyUnknown;
    }
    if (call->args.size() + 1 != method->params.size()) {
        mContext.error("trait method '" + methodName + "' expects " +
              std::to_string(method->params.size() - 1) +
              " explicit argument(s)", call->line, call->col);
        return TyUnknown;
    }

    mContext.recordDeclarationReference(
        member, member->field.size(), sourceMethod);

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
    call->resolvedSymbolName =
        method->generatedSymbolName.empty()
            ? method->name
            : method->generatedSymbolName;
    callee->resolvedSymbolName = call->resolvedSymbolName;
    call->callee = std::move(callee);

    for (size_t index = 0;
         index < call->args.size(); ++index) {
        TypePtr expected =
            mContext.resolved(method->params[index].inferredType);
        TypePtr actual =
            mContext.resolved(analyzeExpr(call->args[index].get()));
        bool isQueryPayload = false;
        if (auto* identifier = dynamic_cast<IdentifierExpr*>(
                call->args[index].get())) {
            if (const auto* symbol =
                    mContext.lookupSymbol(identifier->name))
                isQueryPayload =
                    symbol->isCompileTimeOptionalPayload ||
                    symbol->isCompileTimeQueryDeclarationRef ||
                    symbol->isCompileTimeQueryDeclarationView;
        } else if (auto* queryCall = dynamic_cast<CallExpr*>(
                       call->args[index].get())) {
            isQueryPayload =
                queryCall->isCompileTimeQueryDeclarationRef ||
                queryCall->isCompileTimeQueryDeclarationView;
        }
        if (body_analyzer_detail::isCompilerOnlyValue(actual) || isQueryPayload) {
            mContext.error("compiler-only value cannot cross a trait method "
                  "call boundary; consume it before the call",
                  call->args[index]->line,
                  call->args[index]->col);
            continue;
        }
        if (dynamic_cast<IntLiteralExpr*>(
                call->args[index].get()) &&
            isNumericType(expected))
            continue;
        if (dynamic_cast<StringLiteralExpr*>(
                call->args[index].get()) &&
            expected->kind == TypeKind::CStr)
            continue;
        mContext.constrain(actual, expected,
                  "argument " + std::to_string(index + 1) +
                  " of trait method '" + methodName + "'");
    }
    call->returnsLinear = method->returnsLinear;
    call->returnUsage = method->returnUsage;
    call->resultType = method->inferredReturnType
        ? mContext.resolved(method->inferredReturnType) : TyUnit;
    return call->resultType;
}


TypePtr BodyAnalyzer::analyzeIteratorCall(
    CallExpr* call, FieldAccessExpr* member) {
    if (mContext.mInKernel) {
        mContext.error("kernel iterator pipelines are reserved until their adapter "
              "closures can be cloned into the device module",
              call->line, call->col);
        return TyUnknown;
    }
    const std::string& name = member->field;
    TypePtr receiver = mContext.resolved(analyzeExpr(member->object.get()));
    if (name != "collect" && !call->typeArgASTs.empty()) {
        mContext.error("iterator `" + name +
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
                            sourceType = mContext.resolved(
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
                            mContext.error("move-only iterator terminal "
                                  "currently requires a local "
                                  "array source binding",
                                  call->line, call->col);
                            return;
                        }
                        if (luna::ownership::mustConsume(
                                defaultUsageForType(
                                    sourceType))) {
                            mContext.error("linear iterator terminal "
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
                                mContext.mIteratorStateCounter++);
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
        mContext.error("iterator `" + name + "` expects " +
              std::to_string(expected) + " argument" +
              (expected == 1 ? "" : "s"), call->line, call->col);
        return false;
    };

    if (name == "iter" || name == "iter_mut" ||
        name == "into_iter") {
        if (!requireCount(0)) return TyUnknown;
        if (receiver->kind != TypeKind::Array &&
            receiver->kind != TypeKind::Slice) {
            mContext.error("`" + name + "` requires an array or slice receiver, got " +
                  receiver->toString(), call->line, call->col);
            return TyUnknown;
        }
        if (receiver->kind == TypeKind::Slice && name != "iter") {
            mContext.error("`" + name +
                  "` requires an owning array receiver; slice<T> is a "
                  "read-only shared view",
                  call->line, call->col);
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
        mContext.error("`" + name + "` requires an iterator receiver, got " +
              receiver->toString(), call->line, call->col);
        return TyUnknown;
    }
    const TypePtr item = receiver->inner;

    auto callable = [&](size_t argumentIndex, size_t parameterCount,
                        const std::string& context) -> TypePtr {
        TypePtr type = mContext.resolved(analyzeExpr(call->args[argumentIndex].get()));
        if (type->kind != TypeKind::Function &&
            type->kind != TypeKind::Closure) {
            mContext.error(context + " requires a callable function or "
                  "closure, got " + type->toString(),
                  call->line, call->col);
            return TyUnknown;
        }
        if (type->paramTypes.size() != parameterCount) {
            mContext.error(context + " requires a callable with " +
                  std::to_string(parameterCount) + " parameter" +
                  (parameterCount == 1 ? "" : "s") + ", got " +
                  std::to_string(type->paramTypes.size()),
                  call->line, call->col);
            return TyUnknown;
        }
        return type;
    };

    if (name == "map") {
        if (!requireCount(1)) return TyUnknown;
        TypePtr transform = callable(0, 1, "iterator map");
        if (transform->kind != TypeKind::Function &&
            transform->kind != TypeKind::Closure) return TyUnknown;
        mContext.constrain(item, transform->paramTypes[0], "iterator map input");
        TypePtr output = mContext.resolved(transform->returnType);
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (transform->paramContracts.empty() ||
             transform->paramContracts[0].relation !=
                 luna::ownership::Relation::Owned))
            mContext.error("map transform must own a move-only input "
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
        if (predicate->kind != TypeKind::Function &&
            predicate->kind != TypeKind::Closure) return TyUnknown;
        mContext.constrain(item, predicate->paramTypes[0], "iterator filter input");
        mContext.requireBool(predicate->returnType, "iterator filter predicate");
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (predicate->paramContracts.empty() ||
             predicate->paramContracts[0].relation !=
                 luna::ownership::Relation::SharedBorrow))
            mContext.error("filter predicate must borrow a move-only item "
                  "because accepted items continue downstream",
                  call->line, call->col);
        return finish(
            IteratorOp::Filter,
            Type::makeIterator(item, receiver->iteratorMode, receiver),
            item, item);
    }
    if (name == "take") {
        if (!requireCount(1)) return TyUnknown;
        mContext.requireInteger(analyzeExpr(call->args[0].get()),
                       "iterator take count");
        return finish(
            IteratorOp::Take,
            Type::makeIterator(item, receiver->iteratorMode, receiver),
            item, item);
    }
    if (name == "fold") {
        if (!requireCount(2)) return TyUnknown;
        TypePtr accumulator = mContext.resolved(analyzeExpr(call->args[0].get()));
        TypePtr reducer = callable(1, 2, "iterator fold");
        if (reducer->kind != TypeKind::Function &&
            reducer->kind != TypeKind::Closure) return TyUnknown;
        mContext.constrain(accumulator, reducer->paramTypes[0],
                  "iterator fold accumulator");
        mContext.constrain(item, reducer->paramTypes[1], "iterator fold item");
        mContext.constrain(reducer->returnType, accumulator,
                  "iterator fold result");
        const auto accumulatorUsage =
            defaultUsageForType(accumulator);
        if (luna::ownership::mustConsume(
                accumulatorUsage)) {
            mContext.error("linear fold accumulators are reserved "
                  "until terminal state can expose an "
                  "explicit linear obligation",
                  call->line, call->col);
        } else if (accumulatorUsage !=
                   luna::ownership::Usage::Copy) {
            if (reducer->paramContracts.empty() ||
                reducer->paramContracts[0].relation !=
                    luna::ownership::Relation::Owned)
                mContext.error("fold reducer must own a move-only "
                      "accumulator",
                      call->line, call->col);
            if (reducer->returnContract.relation !=
                    luna::ownership::Relation::Owned ||
                reducer->returnContract.usage !=
                    accumulatorUsage)
                mContext.error("fold reducer must return ownership "
                      "of the replacement accumulator",
                      call->line, call->col);
        }
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (reducer->paramContracts.size() < 2 ||
             reducer->paramContracts[1].relation !=
                 luna::ownership::Relation::Owned))
            mContext.error("fold reducer must own a move-only item",
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
        if (action->kind != TypeKind::Function &&
            action->kind != TypeKind::Closure) return TyUnknown;
        mContext.constrain(item, action->paramTypes[0], "iterator for_each item");
        mContext.constrain(action->returnType, TyUnit, "iterator for_each result");
        if (defaultUsageForType(item) !=
                luna::ownership::Usage::Copy &&
            (action->paramContracts.empty() ||
             action->paramContracts[0].relation !=
                 luna::ownership::Relation::Owned))
            mContext.error("for_each action must own a move-only item",
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
            mContext.error("iterator `collect` requires exactly one explicit target "
                  "type: `.collect::<Target>()`",
                  call->line, call->col);
            return TyUnknown;
        }
        TypePtr target = mContext.resolved(
            mContext.resolveTypeAST(call->typeArgASTs.front().get(), {}));
        if (!target || target->kind == TypeKind::Unknown ||
            target->domain != luna::types::TypeDomain::Value) {
            mContext.error("iterator `collect` target must be a concrete value type",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto implementation =
            mContext.mFromIteratorImplementations.find(mContext.typeIdentity(target));
        if (implementation == mContext.mFromIteratorImplementations.end()) {
            mContext.error("no coherent Core `FromIterator` implementation exists "
                  "for collect target '" + target->toString() + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        const auto& protocol = implementation->second;
        mContext.mConstraints.defaultNumeric(item);
        const TypePtr resolvedItem = mContext.resolved(item);
        if (!luna::types::sameType(
                mContext.resolved(protocol.item), resolvedItem)) {
            mContext.error("Core `FromIterator` for '" + target->toString() +
                  "' collects '" + protocol.item->toString() +
                  "', but this iterator yields '" + resolvedItem->toString() + "'",
                  call->line, call->col);
            return TyUnknown;
        }
        if (!protocol.begin || !protocol.push || !protocol.finish) {
            mContext.error("Core `FromIterator` implementation for '" +
                  target->toString() +
                  "' does not provide the complete begin/push/finish protocol",
                  call->line, call->col);
            return TyUnknown;
        }
        call->iteratorCollectTargetType = target;
        call->iteratorCollectBuilderType =
            mContext.resolved(protocol.builder);
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

    mContext.error("unknown iterator adapter or terminal `" + name + "`",
          call->line, call->col);
    return TyUnknown;
}


TypePtr BodyAnalyzer::analyzeLaunch(LaunchExpr* launch) {
    if (mContext.mInKernel) {
        mContext.error("kernel bodies cannot launch another kernel in the initial device ABI",
              launch->line, launch->col);
        return TyUnknown;
    }

    mContext.requireInteger(analyzeExpr(launch->threads.get()), "launch thread count");

    FunctionDecl* kernel = nullptr;
    auto family = mContext.mFunctionFamilies.find(mContext.sourceDeclarationKey(launch->kernelName));
    if (family != mContext.mFunctionFamilies.end() && family->second.size() > 1) {
        mContext.error("kernel declaration family '" + launch->kernelName +
              "' is ambiguous; dynamic kernel selection requires an explicit future kernel binding operation",
              launch->line, launch->col);
    } else if (family != mContext.mFunctionFamilies.end() && !family->second.empty()) {
        kernel = family->second.front();
    } else {
        mContext.error("unknown kernel '" + launch->kernelName + "'", launch->line, launch->col);
    }
    if (!kernel) return TyUnknown;
    if (!kernel->isKernel) {
        mContext.error("'" + launch->kernelName + "' is a normal function; only `kernel fn` declarations may be launched",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (kernel->params.empty() ||
        !luna::types::sameType(
            mContext.resolved(kernel->params.front().inferredType), TyI32)) {
        mContext.error("kernel '" + launch->kernelName + "' does not satisfy the required `index: i32` launch ABI",
              launch->line, launch->col);
        return TyUnknown;
    }
    if (launch->args.size() + 1 != kernel->params.size()) {
        mContext.error("launch of kernel '" + launch->kernelName + "' expects " +
              std::to_string(kernel->params.size() - 1) + " argument" +
              (kernel->params.size() == 2 ? "" : "s") + " after the implicit index",
              launch->line, launch->col);
        return TyUnknown;
    }

    launch->inFlightResources.clear();
    for (size_t i = 0; i < launch->args.size(); ++i) {
        const TypePtr expected = mContext.resolved(kernel->params[i + 1].inferredType);
        const TypePtr actual = analyzeExpr(launch->args[i].get());
        mContext.constrain(actual, expected, "launch argument " + std::to_string(i + 1) +
                                   " of kernel '" + launch->kernelName + "'");

        if (expected->kind == TypeKind::Reference && expected->inner &&
            expected->inner->kind == TypeKind::DeviceBuffer) {
            auto* borrow = dynamic_cast<BorrowExpr*>(launch->args[i].get());
            auto* resource = borrow ? dynamic_cast<IdentifierExpr*>(borrow->operand.get()) : nullptr;
            if (!borrow || !resource) {
                mContext.error("device-buffer launch argument " + std::to_string(i + 1) +
                      " must be an explicit borrow of a named buffer", launch->line, launch->col);
            } else {
                if (borrow->isMutable != expected->isMutable) {
                    mContext.error("device-buffer launch argument " + std::to_string(i + 1) +
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
