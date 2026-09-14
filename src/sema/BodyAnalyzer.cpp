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

luna::ownership::Usage BodyAnalyzer::inherentUsageForInitializer(
    Expr* initializer, const TypePtr& type) {
    auto usage = defaultUsageForType(mContext.resolved(type));
    if (auto* call = dynamic_cast<CallExpr*>(initializer)) {
        auto callUsage = call->returnsLinear
            ? luna::ownership::Usage::Linear
            : call->returnUsage;
        usage = luna::ownership::strongerUsage(usage, callUsage);
    }
    if (auto* moved = dynamic_cast<MoveExpr*>(initializer)) {
        if (auto* identifier =
                dynamic_cast<IdentifierExpr*>(moved->operand.get())) {
            if (auto* source = mContext.lookupSymbol(identifier->name))
                usage = luna::ownership::strongerUsage(
                    usage, source->usage);
        }
    }
    return usage;
}

luna::ownership::Usage BodyAnalyzer::finalizeBindingUsage(
    const std::string& name, const TypePtr& type, Expr* initializer,
    luna::ownership::Usage requested, bool isExplicit,
    int line, int column) {
    const auto inherent =
        inherentUsageForInitializer(initializer, type);
    if (isExplicit &&
        !luna::ownership::satisfiesUsageRequirement(
            requested, inherent)) {
        mContext.error(
            "binding '" + name + "' declares " +
                std::string(luna::ownership::usageName(requested)) +
                " usage, but its type or initializer requires at least " +
                std::string(luna::ownership::usageName(inherent)),
            line, column);
    }
    return luna::ownership::strongerUsage(requested, inherent);
}

void BodyAnalyzer::analyzeFunction(FunctionDecl* decl) {
    TypePtr savedReturn = mContext.mCurrentReturnType;
    bool savedInFunction = mContext.mInFunction;
    bool savedInKernel = mContext.mInKernel;
    bool savedReturnsLinear = mContext.mCurrentFunctionReturnsLinear;
    auto savedReturnUsage = mContext.mCurrentFunctionReturnUsage;
    bool savedSawReturn = mContext.mSawReturn;
    mContext.mInFunction = true;
    mContext.mInKernel = decl->isKernel;
    mContext.mCurrentFunctionReturnsLinear = decl->returnsLinear;
    if (decl->returnUsage == luna::ownership::Usage::Copy && decl->inferredReturnType)
        decl->returnUsage = defaultUsageForType(mContext.resolved(decl->inferredReturnType));
    mContext.mCurrentFunctionReturnUsage = decl->returnUsage;
    mContext.mSawReturn = false;
    mContext.mSymTable.enterScope();
    mContext.enterConstScope();
    mContext.enterSlotScope();

    // Build type param bindings FIRST
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        TypePtr tpType = Type::makeTypeParam(tp);
        SymbolInfo info;
        info.kind = SymbolKind::TypeParam;
        info.type = tpType;
        mContext.mSymTable.define(tp, info);
        typeBindings[tp] = tpType;
    }

    mContext.mCurrentReturnType = decl->inferredReturnType
        ? decl->inferredReturnType
        : mContext.declaredType(decl->returnType.get(), typeBindings);
    if (body_analyzer_detail::isCompilerOnlyValue(
            mContext.resolved(mContext.mCurrentReturnType)))
        mContext.error("function '" + decl->name +
              "' cannot return a compiler-only value; consume it inside "
              "the declaring function",
              decl->line, decl->col);

    if (decl->isKernel) {
        if (decl->isExtern || decl->isConstexpr || !decl->abi.empty())
            mContext.error("kernel '" + decl->name + "' cannot be extern, constexpr, or ABI-exported",
                  decl->line, decl->col);
        if (!decl->typeParams.empty())
            mContext.error("kernel '" + decl->name + "' cannot be generic in the initial device ABI",
                  decl->line, decl->col);
        if (decl->params.empty()) {
            mContext.error("kernel '" + decl->name + "' requires an explicit first parameter `index: i32`",
                  decl->line, decl->col);
        } else if (!decl->params.front().type ||
                   !luna::types::sameType(
                       mContext.resolved(decl->params.front().inferredType), TyI32)) {
            mContext.error("kernel '" + decl->name + "' must declare its first parameter as `index: i32`",
                  decl->line, decl->col);
        }
    }

    for (auto& p : decl->params) {
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = p.inferredType
            ? p.inferredType
            : mContext.declaredType(p.type.get(), typeBindings);
        if (body_analyzer_detail::isCompilerOnlyValue(mContext.resolved(info.type)))
            mContext.error("function parameter '" + p.name +
                  "' cannot carry a compiler-only value across a call boundary",
                  p.nameLine, p.nameCol);
        const bool explicitUsage = p.hasExplicitUsage || p.isLinear ||
            dynamic_cast<LinearTypeAST*>(p.type.get()) != nullptr ||
            dynamic_cast<AffineTypeAST*>(p.type.get()) != nullptr;
        info.usage = p.isLinear || dynamic_cast<LinearTypeAST*>(p.type.get())
            ? luna::ownership::Usage::Linear
            : (explicitUsage ? p.usage : defaultUsageForType(mContext.resolved(info.type)));
        const auto contract = parameterContractFor(
            mContext.resolved(info.type), info.usage, explicitUsage);
        info.relation = contract.relation;
        info.usage = contract.usage;
        info.isLinear = info.usage == luna::ownership::Usage::Linear;
        p.usage = info.usage;
        p.relation = info.relation;
        p.inferredType = info.type;
        info.isHeapAllocated = false;
        mContext.mSymTable.define(p.name, info);
    }

    for (auto& clause : decl->whereClauses) {
        if (clause.kind != WhereClause::Kind::ConstraintExpression)
            continue;
        TypePtr predicate = mContext.resolved(
            analyzeExpr(clause.constraintExpression.get()));
        if (predicate->kind != TypeKind::Bool &&
            predicate->kind != TypeKind::InferenceVar)
            mContext.error("inline where predicate must have type bool",
                  clause.constraintExpression
                      ? clause.constraintExpression->line : decl->line,
                  clause.constraintExpression
                      ? clause.constraintExpression->col : decl->col);
    }

    if (decl->isKernel) {
        for (size_t i = 1; i < decl->params.size(); ++i) {
            const auto& parameter = decl->params[i];
            const TypePtr type = mContext.resolved(parameter.inferredType);
            if (!parameter.type)
                mContext.error("kernel parameter '" + parameter.name + "' requires an explicit ABI type",
                      decl->line, decl->col);
            if (type->kind == TypeKind::DeviceBuffer)
                mContext.error("kernel parameter '" + parameter.name +
                      "' must use `&device_buffer<T>` or `&mut device_buffer<T>`; device buffers are never passed by value",
                      decl->line, decl->col);
            if (type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind != TypeKind::DeviceBuffer)
                mContext.error("kernel parameter '" + parameter.name +
                      "' may only use references to device_buffer<T> in the initial device ABI",
                      decl->line, decl->col);
        }
    }

    if (decl->body) {
        analyzeBlock(decl->body.get(), mContext.mCurrentReturnType);
    }

    // A function without an explicit return value is a unit-producing
    // declaration. Explicit return statements have already constrained this
    // variable through analyzeStmt.
    if (!decl->isExtern && !mContext.mSawReturn &&
        !(decl->body && blockAlwaysReturns(decl->body.get())))
        mContext.constrain(mContext.mCurrentReturnType, TyUnit, "empty function body");

    // A single return statement is not enough for a non-unit function: every
    // reachable path must return a value.  Keeping this in semantic analysis
    // gives a source-level diagnostic instead of letting host IR verification
    // discover an unterminated LLVM basic block later in the pipeline.
    if (!decl->isExtern && decl->body &&
        !luna::types::sameType(mContext.resolved(mContext.mCurrentReturnType), TyUnit) &&
        !blockAlwaysReturns(decl->body.get())) {
        mContext.error("function '" + decl->name + "' may finish without returning '" +
              mContext.resolved(mContext.mCurrentReturnType)->toString() + "'",
              decl->line, decl->col);
    }

    if (decl->isKernel &&
        !luna::types::sameType(mContext.resolved(mContext.mCurrentReturnType), TyUnit))
        mContext.error("kernel '" + decl->name + "' must return unit", decl->line, decl->col);

    mContext.mSymTable.exitScope();
    mContext.exitConstScope();
    mContext.exitSlotScope();
    mContext.mCurrentReturnType = savedReturn;
    mContext.mInFunction = savedInFunction;
    mContext.mInKernel = savedInKernel;
    mContext.mCurrentFunctionReturnsLinear = savedReturnsLinear;
    mContext.mCurrentFunctionReturnUsage = savedReturnUsage;
    mContext.mSawReturn = savedSawReturn;
}
void BodyAnalyzer::analyzeStruct(StructDecl* decl) {
    // Just validate fields don't have errors
    std::unordered_map<std::string, TypePtr> typeBindings;
    for (auto& tp : decl->typeParams) {
        typeBindings[tp] = Type::makeTypeParam(tp);
    }
    for (auto& field : decl->fields) {
        mContext.resolveTypeAST(field.type.get(), typeBindings);
    }
}


void BodyAnalyzer::analyzeEnum(EnumDecl* decl) {
    const std::string identity = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto it = mContext.mDeclaredTypes.find(identity);
    if (it == mContext.mDeclaredTypes.end()) return;
    std::unordered_map<std::string, bool> names;
    for (auto& variant : it->second->variants) {
        if (names[variant.name])
            mContext.error("Duplicate enum variant '" + variant.name + "' in '" + decl->name + "'");
        names[variant.name] = true;
    }
    for (const auto& variant : it->second->variants) {
        for (const auto& field : variant.fields) {
            std::unordered_set<const Type*> active;
            if (reachesInlineType(
                    field, it->second.get(), active)) {
                mContext.error("enum '" + decl->name +
                      "' has an infinite inline layout through variant '" +
                      variant.name +
                      "'; place the recursive value behind a nominal "
                      "struct, raw pointer, rc, arc, or reference",
                      decl->line, decl->col);
            }
        }
    }
}


void BodyAnalyzer::analyzeTrait(TraitDecl* decl) {
    // Store method signatures for constraint checking
    auto& sigs = mContext.mTraitMethods[mContext.traitIdentity(decl)];
    std::unordered_map<std::string, TypePtr> bindings;
    bindings["Self"] = Type::makeTypeParam("Self");
    for (const auto& typeParameter : decl->typeParams)
        bindings[typeParameter] =
            Type::makeTypeParam(typeParameter);
    for (auto& method : decl->methods) {
        for (auto& parameter : method.params) {
            parameter.inferredType =
                mContext.resolveTypeAST(parameter.type.get(), bindings);
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
            mContext.resolveTypeAST(method.returnType.get(), bindings);
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
        mContext.mGeneratedInstances.push_back(std::move(fd));
    }
}


void BodyAnalyzer::analyzeImpl(ImplDecl* decl) {
    const std::string traitId = decl->trait.resolvedTraitId;
    if (traitId.empty()) return;
    const auto coreFromIterator =
        mContext.mTraits.find(luna::sysmeta::FromIteratorTraitId);
    const bool isCoreFromIteratorTrait =
        coreFromIterator != mContext.mTraits.end() &&
        traitId == mContext.traitIdentity(coreFromIterator->second);
    if (traitId == luna::sysmeta::FromTraitId) {
        const TypePtr source = decl->trait.resolvedTypeArgs.size() == 1
            ? mContext.resolved(decl->trait.resolvedTypeArgs.front()) : TyUnknown;
        const TypePtr target = mContext.resolved(
            mContext.resolveTypeAST(decl->targetType.get(), {}));
        FunctionDecl* conversion = nullptr;
        for (auto& method : decl->methods) {
            if (method->name != luna::sysmeta::FromMethodName) {
                mContext.error("impl of `From` defines unknown method '" +
                      method->name + "'", method->line, method->col);
            } else if (conversion) {
                mContext.error("impl of `From` defines `from` more than once",
                      method->line, method->col);
            } else {
                conversion = method.get();
            }
            analyzeFunction(method.get());
        }
        bool valid = conversion != nullptr;
        if (!conversion) {
            mContext.error("impl of `From<" + source->toString() +
                  "> for " + target->toString() +
                  "` is missing method `from`", decl->line, decl->col);
            return;
        }
        if (!conversion->typeParams.empty()) {
            mContext.error("From::from may not be generic",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (conversion->params.size() != 1 ||
            !luna::types::sameType(
                mContext.resolved(conversion->params.front().inferredType), source)) {
            mContext.error("From::from requires exactly one parameter of type '" +
                  source->toString() + "'", conversion->line, conversion->col);
            valid = false;
        } else if (luna::ownership::isMoveOnly(
                       defaultUsageForType(source)) &&
                   (conversion->params.front().relation !=
                        luna::ownership::Relation::Owned ||
                    conversion->params.front().usage !=
                        defaultUsageForType(source))) {
            mContext.error("From::from must take ownership of move-only source '" +
                  source->toString() +
                  "' with an explicit affine or linear parameter",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (!luna::types::sameType(
                mContext.resolved(conversion->inferredReturnType), target)) {
            mContext.error("From::from must return '" + target->toString() + "'",
                  conversion->line, conversion->col);
            valid = false;
        }
        if (!valid) {
            auto targetIt = mContext.mFromConversions.find(mContext.typeIdentity(target));
            if (targetIt != mContext.mFromConversions.end())
                targetIt->second.erase(mContext.typeIdentity(source));
        }
        return;
    }
    // Verify that each impl method matches a trait method
    auto traitIt = mContext.mTraitMethods.find(traitId);
    if (traitIt == mContext.mTraitMethods.end()) {
        mContext.error("Impl for unknown trait '" + traitId + "'");
        return;
    }

    auto implementations = mContext.mImpls.find(traitId);
    auto implementation = implementations == mContext.mImpls.end()
        ? std::unordered_map<std::string, FunctionDecl*>{}
        : implementations->second[decl->resolvedTargetTypeId];
    std::unordered_map<std::string, bool> declaredMethods;
    for (const auto& [methodName, _] : traitIt->second) declaredMethods[methodName] = true;
    for (const auto& [methodName, _] : implementation) {
        if (!declaredMethods.count(methodName)) {
            mContext.error("impl of trait '" + displayTraitRef(decl->trait) +
                  "' defines unknown method '" + methodName + "'",
                  decl->line, decl->col);
        }
    }
    for (const auto& [methodName, _] : traitIt->second) {
        if (!implementation.count(methodName)) {
            mContext.error("impl of trait '" + displayTraitRef(decl->trait) +
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
    for (const auto& [_, candidate] : mContext.mTraits) {
        if (candidate &&
            mContext.traitIdentity(candidate) == traitId) {
            traitDeclaration = candidate;
            break;
        }
    }
    if (traitDeclaration) {
        std::unordered_map<std::string, TypePtr>
            signatureBindings;
        signatureBindings["Self"] = mContext.resolved(
            mContext.resolveTypeAST(decl->targetType.get(),
                           typeBindings));
        for (size_t index = 0;
             index < traitDeclaration->typeParams.size() &&
             index < decl->trait.resolvedTypeArgs.size();
             ++index)
            signatureBindings[
                traitDeclaration->typeParams[index]] =
                mContext.resolved(decl->trait.resolvedTypeArgs[index]);

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
                mContext.error("method '" + signature.name +
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
                        mContext.resolved(method->params[index].inferredType),
                        mContext.resolved(expected)))
                    mContext.error("parameter " +
                          std::to_string(index + 1) +
                          " of method '" + signature.name +
                          "' must be '" + expected->toString() + "'",
                          method->line, method->col);
                if (method->params[index].relation !=
                        signature.params[index].relation ||
                    method->params[index].usage !=
                        signature.params[index].usage)
                    mContext.error("ownership contract of parameter " +
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
                    mContext.resolved(method->inferredReturnType),
                    mContext.resolved(expectedReturn)))
                mContext.error("method '" + signature.name +
                      "' must return '" +
                      expectedReturn->toString() + "'",
                      method->line, method->col);
        }
    }

    if (isCoreFromIteratorTrait) {
        const TypePtr target = mContext.resolved(
            mContext.resolveTypeAST(decl->targetType.get(), typeBindings));
        auto protocolIt =
            mContext.mFromIteratorImplementations.find(mContext.typeIdentity(target));
        bool valid =
            protocolIt != mContext.mFromIteratorImplementations.end();
        FromIteratorImplementation* protocol =
            valid ? &protocolIt->second : nullptr;
        const TypePtr item =
            protocol ? mContext.resolved(protocol->item) : TyUnknown;
        const TypePtr builder =
            protocol ? mContext.resolved(protocol->builder) : TyUnknown;

        const auto rejectGeneric = [&](FunctionDecl* method) {
            if (!method || method->typeParams.empty()) return;
            mContext.error("FromIterator::" + method->name +
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
                    mContext.resolved(protocol->begin->inferredReturnType),
                    builder) ||
                protocol->begin->returnUsage !=
                    luna::ownership::Usage::Affine) {
                mContext.error("FromIterator::begin must take no parameters and "
                      "return `affine " + builder->toString() + "`",
                      protocol->begin->line, protocol->begin->col);
                valid = false;
            }
            if (protocol->push->params.size() != 2) {
                mContext.error("FromIterator::push must take `&mut " +
                      builder->toString() + "` and `affine " +
                      item->toString() + "`",
                      protocol->push->line, protocol->push->col);
                valid = false;
            } else {
                const TypePtr builderParameter = mContext.resolved(
                    protocol->push->params[0].inferredType);
                if (!builderParameter ||
                    builderParameter->kind != TypeKind::Reference ||
                    !builderParameter->isMutable ||
                    !luna::types::sameType(
                        mContext.resolved(builderParameter->inner), builder) ||
                    !luna::types::sameType(
                        mContext.resolved(protocol->push->params[1].inferredType),
                        item) ||
                    protocol->push->params[1].relation !=
                        luna::ownership::Relation::Owned ||
                    protocol->push->params[1].usage !=
                        luna::ownership::Usage::Affine) {
                    mContext.error("FromIterator::push must take `&mut " +
                          builder->toString() + "` and `affine " +
                          item->toString() + "` (got `" +
                          (builderParameter
                              ? builderParameter->toString()
                              : std::string("?")) + "`, `" +
                          mContext.resolved(protocol->push->params[1].inferredType)->
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
                    mContext.resolved(protocol->push->inferredReturnType),
                    TyUnit)) {
                mContext.error("FromIterator::push must return unit",
                      protocol->push->line, protocol->push->col);
                valid = false;
            }
            if (protocol->finish->params.size() != 1 ||
                !luna::types::sameType(
                    mContext.resolved(protocol->finish->params.front().inferredType),
                    builder) ||
                protocol->finish->params.front().relation !=
                    luna::ownership::Relation::Owned ||
                protocol->finish->params.front().usage !=
                    luna::ownership::Usage::Affine ||
                !luna::types::sameType(
                    mContext.resolved(protocol->finish->inferredReturnType),
                    target) ||
                protocol->finish->returnUsage !=
                    luna::ownership::Usage::Affine) {
                mContext.error("FromIterator::finish must take `affine " +
                      builder->toString() + "` and return `affine " +
                      target->toString() + "` (got parameter `" +
                      (protocol->finish->params.empty()
                          ? std::string("?")
                          : mContext.resolved(protocol->finish->params.front().
                                inferredType)->toString()) +
                      "` and return `" +
                      mContext.resolved(protocol->finish->inferredReturnType)->
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
                mContext.error("FromIterator currently requires affine-or-copy item, "
                      "builder, and target types; linear collection state "
                      "needs an explicit hidden obligation",
                      decl->line, decl->col);
                valid = false;
            }
        }
        if (!valid)
            mContext.mFromIteratorImplementations.erase(
                mContext.typeIdentity(target));
    }

    if (traitId == luna::sysmeta::DropTraitId) {
        FunctionDecl* drop = nullptr;
        auto dropIt = implementation.find(luna::sysmeta::DropMethodName);
        if (dropIt != implementation.end()) drop = dropIt->second;
        const TypePtr target = mContext.resolveTypeAST(decl->targetType.get(), typeBindings);
        bool valid = drop != nullptr;
        if (!decl->typeParams.empty() &&
            body_analyzer_detail::genericDropLayoutDependsOnParameter(target)) {
            mContext.error(
                "generic Drop target has type-parameter-dependent storage "
                "layout; use representation-stable nominal or pointer "
                "indirection until Drop bodies are monomorphized",
                decl->line, decl->col);
            valid = false;
        }
        if (drop && !drop->typeParams.empty()) {
            mContext.error("Drop::drop may not be generic", drop->line, drop->col);
            valid = false;
        }
        if (drop && drop->params.size() != 1) {
            mContext.error("Drop::drop requires exactly one parameter of type '&mut " +
                  target->toString() + "'", drop->line, drop->col);
            valid = false;
        } else if (drop) {
            const TypePtr parameter = mContext.resolved(drop->params.front().inferredType);
            if (!parameter || parameter->kind != TypeKind::Reference ||
                !parameter->isMutable ||
                !luna::types::sameType(mContext.resolved(parameter->inner), mContext.resolved(target))) {
                mContext.error("Drop::drop parameter must be '&mut " + target->toString() + "'",
                      drop->line, drop->col);
                valid = false;
            }
        }
        if (drop && !luna::types::sameType(
                mContext.resolved(drop->inferredReturnType), TyUnit)) {
            mContext.error("Drop::drop must return unit", drop->line, drop->col);
            valid = false;
        }
        if (valid) {
            const std::string targetId = mContext.typeIdentity(target);
            for (auto& [_, declared] : mContext.mDeclaredTypes) {
                if (declared &&
                    (mContext.typeIdentity(declared) == targetId ||
                     (!declared->nominalId.empty() &&
                      declared->nominalId == target->nominalId))) {
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
