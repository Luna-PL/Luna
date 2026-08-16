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

namespace {

bool hasLayoutDependentTypeParameter(
    const TypePtr& type,
    std::unordered_set<const Type*>& active) {
    if (!type || !active.insert(type.get()).second) return false;
    bool dependent = type->kind == TypeKind::TypeParam;
    if (!dependent && type->kind == TypeKind::Array)
        dependent = hasLayoutDependentTypeParameter(type->inner, active);
    if (!dependent && type->kind == TypeKind::Record)
        for (const auto& field : type->fields)
            dependent = dependent ||
                hasLayoutDependentTypeParameter(field.type, active);
    if (!dependent && type->kind == TypeKind::Enum)
        for (const auto& variant : type->variants)
            for (const auto& field : variant.fields)
                dependent = dependent ||
                    hasLayoutDependentTypeParameter(field, active);
    if (!dependent && type->kind == TypeKind::Result)
        for (const auto& argument : type->typeArgs)
            dependent = dependent ||
                hasLayoutDependentTypeParameter(argument, active);
    // Pointer-represented nominal products, references, raw pointers, shared
    // handles, slices, and device handles are representation barriers.
    active.erase(type.get());
    return dependent;
}

bool genericDropLayoutDependsOnParameter(const TypePtr& target) {
    if (!target) return false;
    std::unordered_set<const Type*> active;
    if (target->kind == TypeKind::Struct) {
        for (const auto& field : target->fields)
            if (hasLayoutDependentTypeParameter(field.type, active))
                return true;
        return false;
    }
    if (target->kind == TypeKind::Enum) {
        for (const auto& variant : target->variants)
            for (const auto& field : variant.fields)
                if (hasLayoutDependentTypeParameter(field, active))
                    return true;
    }
    return false;
}

} // namespace

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
            genericDropLayoutDependsOnParameter(target)) {
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


TypePtr BodyAnalyzer::analyzeMatchStmt(MatchStmt* match, TypePtr expectedReturn) {
        TypePtr matched = mContext.resolved(analyzeExpr(match->scrutinee.get()));
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
            mContext.error("match requires an enum or Result value",
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
                mContext.error("unknown variant '" + arm.variantName +
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
                mContext.error("match pattern qualifier '" + arm.typeQualifier +
                      "' does not name matched type '" +
                      matched->toString() + "'", arm.line, arm.col);
            }
            if (!seenVariants.insert(arm.variantName).second)
                mContext.error("duplicate match arm for variant '" +
                      arm.variantName + "'", arm.line, arm.col);
            if (arm.bindings.size() != selected->fields.size()) {
                mContext.error("variant '" + arm.variantName + "' expects " +
                      std::to_string(selected->fields.size()) +
                      " payload binding(s), got " +
                      std::to_string(arm.bindings.size()),
                      arm.line, arm.col);
            }
            arm.variantIndex = selected->physicalIndex;
            arm.bindingTypes = selected->fields;
            arm.bindingUsages.clear();
            for (size_t index = 0;
                 index < arm.bindingTypes.size(); ++index) {
                const auto blockDefault =
                    index < arm.bindingUsageDefaults.size()
                        ? arm.bindingUsageDefaults[index]
                        : luna::ownership::Usage::Copy;
                arm.bindingUsages.push_back(
                    luna::ownership::strongerUsage(
                        blockDefault,
                        defaultUsageForType(
                            arm.bindingTypes[index])));
            }
            if (matched->kind == TypeKind::Enum &&
                !matched->declarationLinkageName.empty()) {
                if (!arm.typeQualifier.empty())
                    mContext.recordResolvedReference(
                        arm.sourcePath, arm.qualifierLine, arm.qualifierCol,
                        arm.typeQualifier.size(),
                        matched->declarationLinkageName);
                mContext.recordResolvedReference(
                    arm.sourcePath, arm.line, arm.col,
                    arm.variantName.size(),
                    matched->declarationLinkageName + "::variant::" +
                        arm.variantName);
            }

            mContext.mSymTable.enterScope();
            std::unordered_set<std::string> seenBindings;
            const size_t count =
                std::min(arm.bindings.size(), arm.bindingTypes.size());
            for (size_t index = 0; index < count; ++index) {
                if (!seenBindings.insert(arm.bindings[index]).second) {
                    mContext.error("duplicate payload binding '" +
                          arm.bindings[index] + "'", arm.line, arm.col);
                    continue;
                }
                SymbolInfo binding;
                binding.kind = SymbolKind::Variable;
                binding.type = arm.bindingTypes[index];
                binding.usage =
                    index < arm.bindingUsages.size()
                        ? arm.bindingUsages[index]
                        : defaultUsageForType(
                              arm.bindingTypes[index]);
                binding.isLinear =
                    binding.usage == luna::ownership::Usage::Linear;
                mContext.mSymTable.define(arm.bindings[index], binding);
            }
            analyzeBlock(arm.body.get(), expectedReturn);
            mContext.mSymTable.exitScope();
        }

        if (seenVariants.size() != variants.size()) {
            std::string missing;
            for (const auto& variant : variants) {
                if (seenVariants.count(variant.name)) continue;
                if (!missing.empty()) missing += ", ";
                missing += variant.name;
            }
            if (matched->kind == TypeKind::Result) {
                mContext.error("Result match must contain exactly one `Ok` arm "
                      "and one `Err` arm", match->line, match->col);
            } else {
                mContext.error("match on '" + matched->toString() +
                      "' is not exhaustive; missing variant(s): " +
                      missing, match->line, match->col);
            }
        }
        return TyUnit;
}

TypePtr BodyAnalyzer::analyzeRecordLiteralExpr(RecordLiteralExpr* record) {
        std::vector<std::pair<RecordLiteralExpr::Field*, TypePtr>> fields;
        fields.reserve(record->fields.size());
        std::set<std::string> names;
        for (auto& field : record->fields) {
            if (!names.insert(field.name).second) {
                mContext.error("duplicate record field '" + field.name + "'",
                               field.line, field.col);
            }
            fields.push_back({&field, analyzeExpr(field.value.get())});
        }
        if (record->targetType) {
            record->recordType = mContext.resolved(
                mContext.resolveTypeAST(record->targetType.get(), {}));
            if (!record->recordType ||
                record->recordType->kind != TypeKind::Struct) {
                mContext.error(
                    "named record construction requires a struct type",
                    record->line, record->col);
                return TyUnknown;
            }
            std::set<std::string> initialized;
            for (auto& entry : fields) {
                auto* field = entry.first;
                const auto& type = entry.second;
                auto declared = std::find_if(
                    record->recordType->fields.begin(),
                    record->recordType->fields.end(),
                    [&](const TypeField& candidate) {
                        return candidate.name == field->name;
                    });
                if (declared == record->recordType->fields.end()) {
                    mContext.error(
                        "struct '" + record->recordType->toString() +
                            "' has no field '" + field->name + "'",
                        field->line, field->col);
                    continue;
                }
                initialized.insert(field->name);
                mContext.constrain(
                    type, declared->type,
                    "field '" + field->name + "' initializer");
            }
            for (const auto& declared : record->recordType->fields) {
                if (initialized.find(declared.name) == initialized.end())
                    mContext.error(
                        "named construction of '" +
                            record->recordType->toString() +
                            "' is missing field '" + declared.name + "'",
                        record->line, record->col);
            }
            return record->recordType;
        }
        std::vector<TypeField> structuralFields;
        structuralFields.reserve(fields.size());
        for (auto& entry : fields)
            structuralFields.push_back(
                {entry.first->name, std::move(entry.second)});
        record->recordType = Type::makeRecord(std::move(structuralFields));
        return record->recordType;
}

TypePtr BodyAnalyzer::analyzeVariantConstructExpr(VariantConstructExpr* variant) {
        auto nominalIt = mContext.mDeclaredTypes.find(mContext.sourceDeclarationKey(variant->typeName));
        if (nominalIt == mContext.mDeclaredTypes.end() ||
            nominalIt->second->kind != TypeKind::Enum) {
            mContext.error("'" + variant->typeName + "' is not an enum type");
            return TyUnknown;
        }

        TypeVec typeArgs;
        for (auto& arg : variant->typeArgs)
            typeArgs.push_back(mContext.resolveTypeAST(arg.get(), {}));
        if (typeArgs.empty()) {
            for (size_t i = 0; i < nominalIt->second->typeParams.size(); ++i)
                typeArgs.push_back(mContext.mConstraints.fresh());
        } else if (typeArgs.size() != nominalIt->second->typeParams.size()) {
            mContext.error("Enum '" + variant->typeName + "' expects " +
                  std::to_string(nominalIt->second->typeParams.size()) +
                  " type arguments");
        }

        auto constructed = mContext.instantiateNominal(nominalIt->second, typeArgs);
        variant->constructedType = constructed;
        mContext.mInferenceRoots.emplace_back(constructed,
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
            mContext.error("Enum '" + variant->typeName + "' has no variant '" +
                  variant->variantName + "'");
            return TyUnknown;
        }
        const std::string enumLinkage =
            nominalIt->second->declarationLinkageName;
        if (!enumLinkage.empty()) {
            mContext.recordResolvedReference(
                variant->typeSourcePath, variant->typeLine, variant->typeCol,
                variant->typeName.size(), enumLinkage);
            mContext.recordResolvedReference(
                variant->sourcePath, variant->line, variant->col,
                variant->variantName.size(),
                enumLinkage + "::variant::" + variant->variantName);
        }
        if (selected->fields.size() != variant->args.size()) {
            mContext.error("Variant '" + variant->variantName + "' expects " +
                  std::to_string(selected->fields.size()) + " arguments");
            return constructed;
        }
        for (size_t i = 0; i < variant->args.size(); ++i) {
            TypePtr actual = analyzeExpr(variant->args[i].get());
            const TypePtr expected = mContext.resolved(selected->fields[i]);
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
            mContext.constrain(actual, expected, "enum variant argument");
        }
        return constructed;
}

TypePtr BodyAnalyzer::analyzeLetStmt(LetStmt* ls, TypePtr expectedReturn) {
    (void)expectedReturn;
        TypePtr rhsType = analyzeExpr(ls->initializer.get());

        // Check if rhs is a HeapAllocExpr — mark as heap allocated
        bool isHeap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()) != nullptr;

        TypePtr declaredType;
        if (ls->typeAnnotation) {
            std::unordered_map<std::string, TypePtr> bindings;
            declaredType = this->mContext.declaredType(ls->typeAnnotation.get(), bindings);
            if (!(dynamic_cast<StringLiteralExpr*>(ls->initializer.get()) &&
                  mContext.resolved(declaredType)->kind == TypeKind::CStr))
                mContext.constrain(rhsType, declaredType, "let binding '" + ls->name + "'");
        } else {
            declaredType = rhsType; // auto inference
        }

        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = declaredType;
        info.isConst = ls->isConst;
        info.isHeapAllocated = isHeap || typeRequiresCleanup(declaredType);
        if (auto* reflected =
                dynamic_cast<CallExpr*>(ls->initializer.get());
            reflected && !reflected->compileTimeDeclarationId.empty())
            info.compileTimeDeclarationId =
                reflected->compileTimeDeclarationId;
        auto finalType = mContext.resolved(declaredType);
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
                    mContext.error("materialized iterator recipe "
                          "requires a local array or slice "
                          "source",
                          ls->line, ls->col);
                    supported = false;
                }
                TypePtr sourceType;
                if (base->resultType &&
                    !base->resultType->typeArgs.empty())
                    sourceType = mContext.resolved(
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
                        mContext.error("materialized iterator recipe cannot hide a "
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
                mContext.error("an iterator binding cannot be "
                      "re-materialized from another recipe; "
                      "consume the existing binding directly",
                      ls->line, ls->col);
            }
            ls->materializesIteratorRecipe = supported;
        }
        const bool annotationLinear =
            dynamic_cast<LinearTypeAST*>(
                ls->typeAnnotation.get()) != nullptr;
        const bool annotationAffine =
            dynamic_cast<AffineTypeAST*>(
                ls->typeAnnotation.get()) != nullptr;
        const bool hasAnnotationUsage =
            annotationLinear || annotationAffine;
        const auto annotationUsage = annotationLinear
            ? luna::ownership::Usage::Linear
            : luna::ownership::Usage::Affine;
        if (ls->hasExplicitUsage && hasAnnotationUsage &&
            ls->usage != annotationUsage) {
            mContext.error(
                "binding '" + ls->name +
                    "' has conflicting explicit usage contracts",
                ls->line, ls->col);
        }
        const bool hasExplicitUsage =
            ls->hasExplicitUsage || hasAnnotationUsage;
        auto requestedUsage = ls->hasExplicitUsage
            ? ls->usage
            : (hasAnnotationUsage
                   ? annotationUsage
                   : (ls->hasInheritedUsage
                          ? ls->inheritedUsage
                          : defaultUsageForType(finalType)));
        ls->usage = finalizeBindingUsage(
            ls->name, finalType, ls->initializer.get(),
            requestedUsage, hasExplicitUsage,
            ls->line, ls->col);
        ls->usageResolved = true;
        ls->isLinear =
            ls->usage == luna::ownership::Usage::Linear;
        info.usage = ls->usage;
        info.isLinear = info.usage == luna::ownership::Usage::Linear;
        mContext.mSymTable.define(ls->name, info);
        // Device resources and completion events carry ownership semantics.
        // Preserve their inferred type on the AST so the ownership and codegen
        // passes observe the same ABI even without an explicit annotation.
        if (!ls->typeAnnotation &&
            (finalType->kind == TypeKind::DeviceBuffer || finalType->kind == TypeKind::Event ||
             finalType->kind == TypeKind::Array || finalType->kind == TypeKind::Slice))
            ls->typeAnnotation = mContext.typeToAST(finalType);
        if (ls->isConst) {
            auto value = mContext.evaluateConstExpr(ls->initializer.get());
            if (!value) {
                mContext.error("const binding '" + ls->name + "' is not a compile-time expression");
            } else {
                mContext.defineConst(ls->name, *value);
            }
        }
        return TyUnit;
}

TypePtr BodyAnalyzer::analyzeForStmt(ForStmt* fs, TypePtr expectedReturn) {
        TypePtr iterable = mContext.resolved(analyzeExpr(fs->iterable.get()));
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
                    mContext.error("move-only consuming array iteration "
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
                            sourceType = mContext.resolved(
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
                mContext.mTraits.find(luna::sysmeta::IteratorTraitId);
            if (coreIterator != mContext.mTraits.end())
                iteratorTrait = coreIterator->second;
            const std::string iteratorTraitId =
                mContext.traitIdentity(iteratorTrait);
            FunctionDecl* next = nullptr;
            FunctionDecl* into = nullptr;
            TypePtr iteratorStateType = iterable;
            TypePtr declaredIntoItem;
            if (!iteratorTraitId.empty()) {
                auto traitImpls = mContext.mImpls.find(iteratorTraitId);
                if (traitImpls != mContext.mImpls.end()) {
                    auto implementation = traitImpls->second.find(
                        mContext.typeIdentity(iterable));
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
                auto coreIntoIterator = mContext.mTraits.find(
                    luna::sysmeta::IntoIteratorTraitId);
                if (coreIntoIterator != mContext.mTraits.end())
                    intoIteratorTrait =
                        coreIntoIterator->second;
                const std::string intoTraitId =
                    mContext.traitIdentity(intoIteratorTrait);
                if (!intoTraitId.empty()) {
                    auto traitImpls =
                        mContext.mImpls.find(intoTraitId);
                    if (traitImpls != mContext.mImpls.end()) {
                        auto implementation =
                            traitImpls->second.find(
                                mContext.typeIdentity(iterable));
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
                            mContext.resolved(into->params.front().
                                inferredType),
                            iterable) ||
                        into->params.front().relation !=
                            luna::ownership::Relation::Owned) {
                        mContext.error("Core IntoIterator::into_iter must "
                              "take ownership of exactly one '" +
                              iterable->toString() +
                              "' value", into->line, into->col);
                    }
                    iteratorStateType = mContext.resolved(
                        into->inferredReturnType);

                    // Recover the declared Item/Iter association from the
                    // exact coherent impl.  Method return type alone carries
                    // Iter but not the associated Item witness.
                    for (const auto& declaration :
                         mContext.mProgram->declarations) {
                        auto* implementation =
                            dynamic_cast<ImplDecl*>(
                                declaration.get());
                        if (!implementation ||
                            implementation->trait.
                                resolvedTraitId !=
                                intoTraitId ||
                            implementation->
                                resolvedTargetTypeId !=
                                mContext.typeIdentity(iterable))
                            continue;
                        if (implementation->trait.
                                resolvedTypeArgs.size() ==
                            2) {
                            declaredIntoItem = mContext.resolved(
                                implementation->trait.
                                    resolvedTypeArgs[0]);
                            TypePtr declaredIterator =
                                mContext.resolved(
                                    implementation->trait.
                                        resolvedTypeArgs[1]);
                            if (!luna::types::sameType(
                                    declaredIterator,
                                    iteratorStateType))
                                mContext.error("Core IntoIterator::into_iter "
                                      "return type disagrees with "
                                      "its Iter argument",
                                      into->line, into->col);
                        }
                        break;
                    }

                    if (!iteratorTraitId.empty()) {
                        auto traitImpls =
                            mContext.mImpls.find(iteratorTraitId);
                        if (traitImpls != mContext.mImpls.end()) {
                            auto implementation =
                                traitImpls->second.find(
                                    mContext.typeIdentity(
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
                        mContext.error("Core IntoIterator for type '" +
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
                    mContext.error("for-loop type '" +
                          iterable->toString() +
                          "' implements neither "
                          "core::iter::Iterator nor "
                          "core::iter::IntoIterator",
                          fs->line, fs->col);
            } else if (!dynamic_cast<IdentifierExpr*>(
                           fs->iterable.get())) {
                mContext.error("Core Iterator/IntoIterator for-loop source must "
                      "currently be a local binding",
                      fs->line, fs->col);
                validProtocol = false;
            }

            if (next) {
                if (next->params.size() != 1) {
                    mContext.error("Core Iterator::next must have exactly one "
                          "receiver parameter", next->line, next->col);
                    validProtocol = false;
                } else {
                    TypePtr receiver = mContext.resolved(
                        next->params.front().inferredType);
                    if (!receiver ||
                        receiver->kind != TypeKind::Reference ||
                        !receiver->isMutable ||
                        !luna::types::sameType(
                            mContext.resolved(receiver->inner),
                            iteratorStateType)) {
                        mContext.error("Core Iterator::next receiver must be '&mut " +
                              iteratorStateType->toString() + "'",
                              next->line, next->col);
                        validProtocol = false;
                    }
                }

                TypePtr option = mContext.resolved(
                    next->inferredReturnType);
                TypePtr coreOption;
                auto optionDeclaration =
                    mContext.mDeclaredTypes.find(
                        luna::sysmeta::OptionTypeId);
                if (optionDeclaration !=
                    mContext.mDeclaredTypes.end())
                    coreOption =
                        mContext.resolved(optionDeclaration->second);
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
                                mContext.resolved(variant.fields.front());
                            foundSome = true;
                        }
                    }
                }
                if (foundSome && declaredIntoItem &&
                    !luna::types::sameType(
                        element, declaredIntoItem)) {
                    mContext.error("Core IntoIterator Item type '" +
                          declaredIntoItem->toString() +
                          "' disagrees with Iterator item type '" +
                          element->toString() + "'",
                          fs->line, fs->col);
                    validProtocol = false;
                }
                if (!isCoreOption || !foundNone ||
                    !foundSome) {
                    mContext.error("Core Iterator::next must return "
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
        fs->bindingUsage =
            luna::ownership::strongerUsage(
                fs->hasInheritedUsage
                    ? fs->inheritedUsage
                    : luna::ownership::Usage::Copy,
                defaultUsageForType(element));
        mContext.mSymTable.enterScope();
        SymbolInfo vi;
        vi.kind = SymbolKind::Variable;
        vi.type = element;
        vi.usage = fs->bindingUsage;
        vi.isLinear = vi.usage == luna::ownership::Usage::Linear;
        mContext.mSymTable.define(fs->varName, vi);
        analyzeBlock(fs->body.get(), expectedReturn);
        mContext.mSymTable.exitScope();
        return TyUnit;
}

TypePtr BodyAnalyzer::analyzeTryExpr(TryExpr* propagation) {
        if (mContext.mCurrentFragmentDecl) {
            mContext.error("`?` may not propagate across a fragment/slot boundary; "
                  "handle the Result explicitly inside the fragment",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr result = mContext.resolved(analyzeExpr(propagation->operand.get()));
        if (result->kind != TypeKind::Result ||
            result->typeArgs.size() != 2) {
            mContext.error("`?` requires Result<T, E>, got " + result->toString(),
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr functionResult = mContext.resolved(mContext.mCurrentReturnType);
        if (!mContext.mInFunction || functionResult->kind != TypeKind::Result ||
            functionResult->typeArgs.size() != 2) {
            mContext.error("`?` requires the enclosing function to return Result",
                  propagation->line, propagation->col);
            return TyUnknown;
        }
        TypePtr sourceError = mContext.resolved(result->typeArgs[1]);
        TypePtr targetError = mContext.resolved(functionResult->typeArgs[1]);
        if (!luna::types::sameType(sourceError, targetError)) {
            auto targetConversions =
                mContext.mFromConversions.find(mContext.typeIdentity(targetError));
            auto conversion = targetConversions == mContext.mFromConversions.end()
                ? std::unordered_map<std::string, FromConversion>::const_iterator{}
                : targetConversions->second.find(mContext.typeIdentity(sourceError));
            if (targetConversions == mContext.mFromConversions.end() ||
                conversion == targetConversions->second.end() ||
                !conversion->second.method ||
                conversion->second.symbol.empty()) {
                mContext.error("`?` cannot convert error '" + sourceError->toString() +
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

TypePtr BodyAnalyzer::analyzeLambdaExpr(LambdaExpr* le) {
        // Analyze lambda: enter new scope, register params, analyze body
        TypePtr savedReturn = mContext.mCurrentReturnType;
        bool savedSawReturn = mContext.mSawReturn;
        bool savedReturnsLinear =
            mContext.mCurrentFunctionReturnsLinear;
        auto savedReturnUsage =
            mContext.mCurrentFunctionReturnUsage;
        const size_t lambdaScopeDepth =
            mContext.mSymTable.depth();
        mCaptureFrames.push_back({lambdaScopeDepth, {}});
        mContext.mSymTable.enterScope();
        for (auto& p : le->params) {
            TypePtr pt = p.inferredType ? p.inferredType : mContext.declaredType(p.type.get(), {});
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
            mContext.mSymTable.define(p.name, info);
        }
        TypePtr bodyRet = le->returnType ? mContext.declaredType(le->returnType.get(), {}) : mContext.mConstraints.fresh();
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
        mContext.mCurrentReturnType = bodyRet;
        mContext.mCurrentFunctionReturnsLinear =
            returnsLinear;
        mContext.mCurrentFunctionReturnUsage =
            returnUsage;
        mContext.mSawReturn = false;
        if (le->body) analyzeBlock(le->body.get(), bodyRet);
        if (!mContext.mSawReturn) mContext.constrain(bodyRet, TyUnit, "lambda without a return value");
        mContext.mSymTable.exitScope();
        mContext.mCurrentReturnType = savedReturn;
        mContext.mCurrentFunctionReturnsLinear =
            savedReturnsLinear;
        mContext.mCurrentFunctionReturnUsage =
            savedReturnUsage;
        mContext.mSawReturn = savedSawReturn;

        CaptureFrame frame = std::move(mCaptureFrames.back());
        mCaptureFrames.pop_back();
        if (!mCaptureFrames.empty()) {
            auto& enclosing = mCaptureFrames.back();
            for (const auto& name : frame.captures) {
                const auto* symbol = mContext.lookupSymbol(name);
                if (!symbol || symbol->kind != SymbolKind::Variable)
                    continue;
                if (mContext.mSymTable.lookupDepth(name) >=
                    enclosing.lambdaScopeDepth)
                    continue;
                if (std::find(enclosing.captures.begin(),
                              enclosing.captures.end(), name) ==
                    enclosing.captures.end())
                    enclosing.captures.push_back(name);
            }
        }

        // Build closure function type: fn(ParamTypes) -> ReturnType
        TypeVec paramTypes;
        std::vector<luna::ownership::Contract> paramContracts;
        for (auto& p : le->params) {
            paramTypes.push_back(p.inferredType);
            paramContracts.push_back({p.relation, p.usage});
        }
        TypePtr retType = bodyRet;

        // Capture resolution: recorded free variables become closure
        // environment fields. Copy captures copy by value; Affine/Linear
        // captures move the outer binding into the environment (C016 CL010).
        // Borrowed (Reference) captures remain a later slice. Function
        // references are not captures.
        std::vector<std::string> captures;
        std::vector<TypeField> captureFields;
        bool captureError = false;
        for (const auto& name : frame.captures) {
            const auto* symbol = mContext.lookupSymbol(name);
            if (!symbol || symbol->kind != SymbolKind::Variable)
                continue;
            if (symbol->type &&
                symbol->type->kind == TypeKind::Reference) {
                mContext.error(
                    "lambda capture of borrowed binding '" + name +
                    "' is not yet supported (C016 CL005)", le->line, le->col);
                captureError = true;
                continue;
            }
            captures.push_back(name);
            captureFields.push_back({name, symbol->type});
        }
        // CL009 requires one canonical capture order shared by Sema, the
        // builder, and the verifier. `Type::makeClosure` keeps fields in
        // name order, so the capture list must use the same order here.
        std::vector<size_t> captureOrder(captureFields.size());
        for (size_t index = 0; index < captureOrder.size(); ++index)
            captureOrder[index] = index;
        std::stable_sort(captureOrder.begin(), captureOrder.end(),
                         [&](size_t lhs, size_t rhs) {
                             return captureFields[lhs].name <
                                    captureFields[rhs].name;
                         });
        std::vector<std::string> orderedCaptures;
        std::vector<TypeField> orderedFields;
        orderedCaptures.reserve(captureOrder.size());
        orderedFields.reserve(captureOrder.size());
        for (const size_t index : captureOrder) {
            orderedCaptures.push_back(captures[index]);
            orderedFields.push_back(std::move(captureFields[index]));
        }
        captures = std::move(orderedCaptures);
        captureFields = std::move(orderedFields);
        if (!captureError) {
            le->captures = captures;
            le->envParamName = captures.empty()
                ? "" : "$closure.env";
            if (captures.empty()) {
                le->closureType = Type::makeFunction(
                    paramTypes, retType, std::move(paramContracts),
                    {luna::ownership::Relation::Owned, returnUsage});
            } else {
                le->closureType = Type::makeClosure(
                    paramTypes, retType, std::move(paramContracts),
                    {luna::ownership::Relation::Owned, returnUsage},
                    std::move(captureFields));
            }
        } else {
            le->closureType = Type::makeFunction(
                paramTypes, retType, std::move(paramContracts),
                {luna::ownership::Relation::Owned, returnUsage});
        }
        return le->closureType;
}

TypePtr BodyAnalyzer::analyzeStmt(Stmt* stmt, TypePtr expectedReturn) {
    mContext.setDiagnosticLocation(stmt);
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) return analyzeBlock(bs, expectedReturn);

    // A device kernel is deliberately a small DeviceMemory-only sublanguage.
    // The source-level continuation constructs are host control flow: lowering
    // them into SIMT code would require a per-lane continuation runtime and
    // could not preserve their resume semantics. Likewise, awaiting, freeing,
    // and host allocation are host-side synchronization/resource effects.
    // Keep this boundary in semantic analysis, before code generation can
    // accidentally inline a fragment or emit a host runtime call into HSACO.
    if (mContext.mInKernel) {
        const char* construct = nullptr;
        if (dynamic_cast<SlotDeclStmt*>(stmt)) construct = "slot declaration";
        else if (dynamic_cast<SlotInvokeStmt*>(stmt)) construct = "slot invocation";
        else if (dynamic_cast<ApplyStmt*>(stmt)) construct = "apply binding";
        else if (dynamic_cast<ResumeStmt*>(stmt)) construct = "resume()";
        else if (dynamic_cast<AbortStmt*>(stmt)) construct = "abort()";
        else if (dynamic_cast<AwaitStmt*>(stmt)) construct = "await";
        else if (dynamic_cast<FreeStmt*>(stmt)) construct = "free";
        if (construct) {
            mContext.error("kernel body may not use " + std::string(construct) +
                  "; device kernels support only DeviceMemory operations and structured scalar control flow",
                  stmt->line, stmt->col);
            return TyUnit;
        }
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        mContext.analyzeSlotDecl(slot);
        return TyUnit;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        mContext.analyzeSlotInvoke(slot, expectedReturn);
        return TyUnit;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        mContext.analyzeApply(apply, expectedReturn);
        return TyUnit;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mContext.mCurrentFragmentDecl)
            mContext.error("`resume()` may only appear inside a fragment", stmt->line, stmt->col);
        else if (mContext.mCurrentFragmentDecl &&
                 mContext.mCurrentFragmentDecl->kind == FragmentKind::Interceptor)
            mContext.error("`resume()` is not allowed in an interceptor; normal completion forwards automatically",
                  stmt->line, stmt->col);
        return TyUnit;
    }
    if (dynamic_cast<AbortStmt*>(stmt)) {
        if (!mContext.mCurrentFragmentDecl)
            mContext.error("`abort()` may only appear inside an interceptor or context", stmt->line, stmt->col);
        return TyUnit;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        TypePtr eventType = mContext.resolved(analyzeExpr(await->event.get()));
        if (eventType->kind != TypeKind::Event)
            mContext.error("`await` requires a launch event, got " + eventType->toString(),
                  await->line, await->col);
        return TyUnit;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt))
        return analyzeLetStmt(ls, expectedReturn);
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        mContext.mSawReturn = true;
        if (rs->value) {
            TypePtr valueType = analyzeExpr(rs->value.get());
            if (!(dynamic_cast<StringLiteralExpr*>(rs->value.get()) &&
                  mContext.resolved(mContext.mCurrentReturnType)->kind == TypeKind::CStr))
                mContext.constrain(valueType, mContext.mCurrentReturnType, "return statement");
            luna::ownership::Usage returningUsage = luna::ownership::Usage::Copy;
            if (auto* call = dynamic_cast<CallExpr*>(rs->value.get())) {
                returningUsage = call->returnsLinear
                    ? luna::ownership::Usage::Linear : call->returnUsage;
            } else if (auto* id = dynamic_cast<IdentifierExpr*>(rs->value.get())) {
                if (auto* symbol = mContext.mSymTable.lookup(id->name))
                    returningUsage = symbol->isLinear
                        ? luna::ownership::Usage::Linear : symbol->usage;
            }
            if (returningUsage == luna::ownership::Usage::Linear &&
                mContext.mCurrentFunctionReturnUsage != luna::ownership::Usage::Linear) {
                mContext.error("returning a linear value requires a linear function return contract",
                      rs->line, rs->col);
            } else if (returningUsage != luna::ownership::Usage::Linear &&
                       mContext.mCurrentFunctionReturnUsage == luna::ownership::Usage::Linear) {
                mContext.error("function declared with `-> linear raw<T>` must return an owning value",
                      rs->line, rs->col);
            } else if (returningUsage == luna::ownership::Usage::Affine &&
                       mContext.mCurrentFunctionReturnUsage == luna::ownership::Usage::Copy) {
                mContext.error("returning an affine value requires an affine function return contract",
                      rs->line, rs->col);
            }
        } else {
            mContext.constrain(TyUnit, mContext.mCurrentReturnType, "unit return statement");
        }
        return TyUnit;
    }
    if (auto* is = dynamic_cast<IfStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(is->cond.get());
        mContext.requireBool(condType, "if condition");
        analyzeBlock(is->thenBlock.get(), expectedReturn);
        if (is->elseBranch) analyzeStmt(is->elseBranch.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt))
        return analyzeMatchStmt(match, expectedReturn);
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        TypePtr condType = analyzeExpr(ws->cond.get());
        mContext.requireBool(condType, "while condition");
        analyzeBlock(ws->body.get(), expectedReturn);
        return TyUnit;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt))
        return analyzeForStmt(fs, expectedReturn);
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) {
        return analyzeExpr(es->expr.get());
    }
    if (auto* fs = dynamic_cast<FreeStmt*>(stmt)) {
        analyzeExpr(fs->operand.get());
        return TyUnit;
    }
    return TyUnit;
}


TypePtr BodyAnalyzer::analyzeBlock(BlockStmt* block, TypePtr expectedReturn) {
    mContext.mSymTable.enterScope();
    mContext.enterConstScope();
    mContext.enterSlotScope();
    for (auto& stmt : block->stmts) {
        analyzeStmt(stmt.get(), expectedReturn);
    }
    mContext.mSymTable.exitScope();
    mContext.exitConstScope();
    mContext.exitSlotScope();
    return TyUnit;
}


bool BodyAnalyzer::statementAlwaysReturns(const Stmt* stmt) const {
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


bool BodyAnalyzer::blockAlwaysReturns(const BlockStmt* block) const {
    if (!block) return false;
    for (const auto& stmt : block->stmts) {
        if (statementAlwaysReturns(stmt.get())) return true;
    }
    return false;
}

// ─── Expression analysis ───────────────────────────────────────────


TypePtr BodyAnalyzer::analyzeExpr(Expr* expr) {
    mContext.setDiagnosticLocation(expr);
    if (dynamic_cast<IntLiteralExpr*>(expr)) return TyI32;
    if (dynamic_cast<FloatLiteralExpr*>(expr)) return TyF64;
    if (dynamic_cast<StringLiteralExpr*>(expr)) return TyString;
    if (dynamic_cast<BoolLiteralExpr*>(expr)) return TyBool;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        const std::string declarationKey = mContext.sourceDeclarationKey(id->name);
        auto family = mContext.mFunctionFamilies.find(declarationKey);
        if (family != mContext.mFunctionFamilies.end() && family->second.size() > 1) {
            mContext.error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", id->line, id->col);
            return TyUnknown;
        }
        auto* sym = mContext.lookupSymbol(id->name);
        if (!sym) {
            mContext.error("undefined name '" + id->name + "'", id->line, id->col);
            return TyUnknown;
        }
        if (!mCaptureFrames.empty() &&
            sym->kind == SymbolKind::Variable) {
            auto& frame = mCaptureFrames.back();
            if (mContext.mSymTable.lookupDepth(id->name) <
                frame.lambdaScopeDepth) {
                if (std::find(frame.captures.begin(),
                              frame.captures.end(),
                              id->name) == frame.captures.end())
                    frame.captures.push_back(id->name);
            }
        }
        if (sym->kind == SymbolKind::Function) {
            if (family != mContext.mFunctionFamilies.end() && family->second.size() == 1) {
                const auto* declaration = family->second.front();
                id->resolvedSymbolName =
                    declaration->generatedSymbolName.empty()
                        ? declaration->name
                        : declaration->generatedSymbolName;
                mContext.recordDeclarationReference(id, id->name.size(),
                                                   declaration);
            }
            return Type::makeFunction(sym->paramTypes,
                                      sym->returnType ? sym->returnType : TyUnit,
                                      sym->paramContracts,
                                      {luna::ownership::Relation::Owned,
                                       sym->returnUsage});
        }
        return sym->type ? mContext.resolved(sym->type) : TyUnknown;
    }
    if (auto* selection = dynamic_cast<SelectExpr*>(expr))
        return analyzeSelect(selection);
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr))
        return analyzeRecordLiteralExpr(record);
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        TypePtr lhsType = analyzeExpr(bin->lhs.get());
        TypePtr rhsType = analyzeExpr(bin->rhs.get());
        switch (bin->op) {
            case TokenKind::Plus:
            case TokenKind::Minus:
            case TokenKind::Star:
            case TokenKind::Slash:
            case TokenKind::Percent:
                mContext.requireNumeric(lhsType, "left operand of arithmetic expression");
                mContext.requireNumeric(rhsType, "right operand of arithmetic expression");
                mContext.constrain(lhsType, rhsType, "arithmetic operands");
                return mContext.resolved(lhsType);
            case TokenKind::Ampersand:
            case TokenKind::BitOr:
            case TokenKind::BitXor:
                mContext.requireInteger(lhsType, "left operand of bitwise expression");
                mContext.requireInteger(rhsType, "right operand of bitwise expression");
                if ((mContext.resolved(lhsType)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(lhsType))) &&
                    (mContext.resolved(rhsType)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(rhsType))))
                    mContext.constrain(lhsType, rhsType, "bitwise operands");
                return mContext.resolved(lhsType);
            case TokenKind::ShiftLeft:
            case TokenKind::ShiftRight:
                mContext.requireInteger(lhsType, "left operand of shift expression");
                mContext.requireInteger(rhsType, "shift count");
                return mContext.resolved(lhsType);
            case TokenKind::EqEq:
            case TokenKind::Neq:
                mContext.constrain(lhsType, rhsType, "equality operands");
                return TyBool;
            case TokenKind::Lt:
            case TokenKind::LtEq:
            case TokenKind::Gt:
            case TokenKind::GtEq:
                mContext.requireNumeric(lhsType, "left operand of comparison expression");
                mContext.requireNumeric(rhsType, "right operand of comparison expression");
                mContext.constrain(lhsType, rhsType, "comparison operands");
                return TyBool;
            case TokenKind::AndAnd:
            case TokenKind::OrOr:
                mContext.requireBool(lhsType, "left operand of logical expression");
                mContext.requireBool(rhsType, "right operand of logical expression");
                return TyBool;
            default: return TyUnknown;
        }
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        TypePtr opType = analyzeExpr(un->operand.get());
        switch (un->op) {
            case TokenKind::Minus:
                mContext.requireNumeric(opType, "unary '-' operand");
                return opType;
            case TokenKind::Not:
                mContext.requireBool(opType, "'!' operand");
                return TyBool;
            case TokenKind::Tilde:
                mContext.requireInteger(opType, "'~' operand");
                return opType;
            case TokenKind::Star: {
                auto resolvedOp = mContext.resolved(opType);
                if (resolvedOp->kind == TypeKind::InferenceVar) {
                    auto inner = mContext.mConstraints.fresh();
                    mContext.constrain(opType, Type::makeReference(inner), "dereference operand");
                    return inner;
                }
                if (resolvedOp->kind == TypeKind::Reference) return resolvedOp->inner;
                mContext.error("Cannot dereference non-reference type");
                return TyUnknown;
            }
            default: return TyUnknown;
        }
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr))
        return analyzeVariantConstructExpr(variant);
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return analyzeLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr)) return analyzeCall(call);
    if (auto* fa = dynamic_cast<FieldAccessExpr*>(expr)) {
        TypePtr objectType = mContext.resolved(analyzeExpr(fa->object.get()));
        if (objectType->kind == TypeKind::Reference && objectType->inner)
            objectType = mContext.resolved(objectType->inner);
        if (objectType->kind != TypeKind::Struct &&
            objectType->kind != TypeKind::Record &&
            objectType->kind != TypeKind::Metadata) {
            mContext.error("Field access requires a product type, got " + objectType->toString());
            return TyUnknown;
        }
        for (auto& field : objectType->fields) {
            if (field.name == fa->field) {
                fa->resultType = mContext.resolved(field.type);
                if (!objectType->declarationLinkageName.empty())
                    mContext.recordResolvedReference(
                        fa->sourcePath, fa->line, fa->col, fa->field.size(),
                        objectType->declarationLinkageName + "::field::" +
                            fa->field);
                return fa->resultType;
            }
        }
        mContext.error("Type '" + objectType->toString() + "' has no field '" + fa->field + "'");
        return TyUnknown;
    }
    if (auto* propagation = dynamic_cast<TryExpr*>(expr))
        return analyzeTryExpr(propagation);
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr)) {
        if (mContext.mInKernel) {
            mContext.error("kernel body may not allocate heap memory with `new`; allocate device memory on the host and pass a borrowed device_buffer parameter",
                  ha->line, ha->col);
            return TyUnknown;
        }
        ha->allocatedType = ha->allocatedTypeAST
            ? mContext.resolveTypeAST(ha->allocatedTypeAST.get(), {}) : TyUnknown;
        if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
            TypeVec argumentTypes;
            for (auto& arg : initCall->args)
                argumentTypes.push_back(analyzeExpr(arg.get()));
            if (ha->allocatedType->kind == TypeKind::Struct ||
                ha->allocatedType->kind == TypeKind::Record) {
                if (initCall->args.size() != ha->allocatedType->fields.size()) {
                    mContext.error("Constructor for '" + ha->allocatedType->toString() +
                          "' expects " + std::to_string(ha->allocatedType->fields.size()) +
                          " field values");
                } else {
                    for (size_t i = 0; i < initCall->args.size(); ++i)
                        mContext.constrain(argumentTypes[i],
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
                    mContext.error("Primitive allocation requires exactly one initializer");
                else
                    mContext.constrain(argumentTypes[0], ha->allocatedType,
                              "primitive allocation initializer");
            }
        }
        ha->resultType = ha->allocatedType;
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
        TypePtr op = mContext.resolved(analyzeExpr(dr->operand.get()));
        if (op->kind == TypeKind::InferenceVar) {
            auto inner = mContext.mConstraints.fresh();
            mContext.constrain(
                op, Type::makeReference(inner), "dereference operand");
            dr->resultType = inner;
            return inner;
        }
        if ((op->kind == TypeKind::Reference ||
             op->kind == TypeKind::RawPointer) && op->inner) {
            dr->resultType = mContext.resolved(op->inner);
            return dr->resultType;
        }
        mContext.error("Cannot dereference non-reference or non-raw-pointer type");
        return TyUnknown;
    }
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr)) {
        TypePtr op = analyzeExpr(ad->operand.get());
        return Type::makeReference(op, ad->isMutable);
    }
    if (auto* le = dynamic_cast<LambdaExpr*>(expr))
        return analyzeLambdaExpr(le);
    if (auto* as = dynamic_cast<AssignExpr*>(expr)) {
        TypePtr rhs = analyzeExpr(as->rhs.get());
        TypePtr lhs = analyzeExpr(as->lhs.get());
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            if (auto* symbol = mContext.mSymTable.lookup(id->name); symbol && symbol->isConst)
                mContext.error("cannot assign to const binding '" + id->name + "'", id->line, id->col);
        }
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(index->object.get())) {
                if (auto* symbol = mContext.mSymTable.lookup(id->name); symbol && symbol->isConst)
                    mContext.error("cannot assign through index of const array binding '" + id->name + "'",
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
                    mContext.requireInteger(lhs, "left operand of compound bitwise assignment");
                    mContext.requireInteger(rhs, "right operand of compound bitwise assignment");
                    operandsValid =
                        (mContext.resolved(lhs)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(lhs))) &&
                        (mContext.resolved(rhs)->kind == TypeKind::InferenceVar || isIntegerType(mContext.resolved(rhs)));
                    break;
                default:
                    mContext.requireNumeric(lhs, "left operand of compound arithmetic assignment");
                    mContext.requireNumeric(rhs, "right operand of compound arithmetic assignment");
                    break;
            }
        }
        if (operandsValid) mContext.constrain(rhs, lhs, "assignment");
        return rhs;
    }
    if (auto* ix = dynamic_cast<IndexExpr*>(expr)) {
        TypePtr object = mContext.resolved(analyzeExpr(ix->object.get()));
        TypePtr index = mContext.resolved(analyzeExpr(ix->index.get()));
        if (!isIntegerType(index))
            mContext.error("array index must be an integer, got " + index->toString(), ix->line, ix->col);
        if (object->kind != TypeKind::Array && object->kind != TypeKind::Slice) {
            mContext.error("cannot index " + object->toString() + "; only array<T, N> and slice<T> support safe indexing",
                  ix->line, ix->col);
            return TyUnknown;
        }
        if (object->kind == TypeKind::Array) if (auto* literal = dynamic_cast<IntLiteralExpr*>(ix->index.get());
            literal && (literal->value < 0 || static_cast<uint64_t>(literal->value) >= object->arrayLength))
            mContext.error("array index " + std::to_string(literal->value) + " is outside array length " +
                  std::to_string(object->arrayLength), ix->line, ix->col);
        return object->inner;
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        if (array->elements.empty()) {
            mContext.error("cannot infer the element type of an empty array", array->line, array->col);
            return TyUnknown;
        }
        TypePtr element = analyzeExpr(array->elements.front().get());
        for (size_t i = 1; i < array->elements.size(); ++i)
            mContext.constrain(analyzeExpr(array->elements[i].get()), element, "array element");
        array->elementType = mContext.resolved(element);
        return Type::makeArray(array->elementType, array->elements.size());
    }
    if (auto* ie = dynamic_cast<IfExpr*>(expr)) {
        mContext.requireBool(analyzeExpr(ie->cond.get()), "if-expression condition");
        auto branchType = [this](Expr* branch) {
            if (auto* be = dynamic_cast<BlockExpr*>(branch)) {
                analyzeBlock(be->block.get(), nullptr);
                return TyUnit;
            }
            return analyzeExpr(branch);
        };
        TypePtr thenType = branchType(ie->thenExpr.get());
        TypePtr elseType = branchType(ie->elseExpr.get());
        if (mContext.resolved(thenType)->kind == TypeKind::Never) return elseType;
        if (mContext.resolved(elseType)->kind == TypeKind::Never) return thenType;
        mContext.constrain(thenType, elseType, "if-expression branches");
        return thenType;
    }
    return TyUnknown;
}


TypePtr BodyAnalyzer::analyzeSelect(SelectExpr* selection) {
    const std::string targetKey = mContext.sourceDeclarationKey(selection->targetName);
    const std::string selectorKey = mContext.sourceDeclarationKey(selection->selectorName);
    auto targetFamily = mContext.mFunctionFamilies.find(targetKey);
    if (targetFamily == mContext.mFunctionFamilies.end() || targetFamily->second.empty()) {
        mContext.error("unknown declaration family '" + selection->targetName + "'",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto selectorFamily = mContext.mFunctionFamilies.find(selectorKey);
    if (selectorFamily == mContext.mFunctionFamilies.end() || selectorFamily->second.size() != 1) {
        mContext.error("selector function '" + selection->selectorName +
              "' must resolve to exactly one declaration",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto* selectorFunction = selectorFamily->second.front();
    const auto selectorSymbol = selectorFunction->generatedSymbolName.empty()
        ? selectorFunction->name : selectorFunction->generatedSymbolName;
    selection->resolvedSelectorDeclarationId = nominalDeclarationIdentity(
        mContext.mProgram, "fn", selectorSymbol, selectorFunction);
    selection->resolvedFamilyId = nominalDeclarationIdentity(
        mContext.mProgram, "fn", targetFamily->second.front()->name,
        targetFamily->second.front());
    if (selectorFunction->params.empty() ||
        mContext.resolved(selectorFunction->params.front().inferredType)->kind !=
            TypeKind::DeclarationView) {
        mContext.error("selector function '" + selection->selectorName +
              "' must declare declaration_view as its first parameter",
              selection->line, selection->col);
        return TyUnknown;
    }
    if (selection->selectorArgs.size() + 1 != selectorFunction->params.size()) {
        mContext.error("selector function '" + selection->selectorName + "' expects " +
              std::to_string(selectorFunction->params.size() - 1) +
              " explicit arguments", selection->line, selection->col);
        return TyUnknown;
    }
    if (mContext.resolved(selectorFunction->inferredReturnType)->kind !=
        TypeKind::DeclarationRef) {
        mContext.error("selector function '" + selection->selectorName +
              "' must return declaration_ref",
              selection->line, selection->col);
        return TyUnknown;
    }

    std::unordered_map<std::string, ConstValue> selectorLocals;
    std::vector<ConstValue> staticSelectorArguments;
    for (size_t index = 0; index < selection->selectorArgs.size(); ++index) {
        mContext.constrain(analyzeExpr(selection->selectorArgs[index].get()),
                  selectorFunction->params[index + 1].inferredType,
                  "selector argument " + std::to_string(index + 1));
        if (!selection->isDynamic) {
            auto value = mContext.evaluateConstExpr(selection->selectorArgs[index].get());
            if (!value) {
                mContext.error("static selector argument " + std::to_string(index + 1) +
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
                parameters.push_back(mContext.resolved(parameter.inferredType));
                contracts.push_back({parameter.relation, parameter.usage});
            }
            auto candidateType = Type::makeFunction(
                std::move(parameters), mContext.resolved(candidate->inferredReturnType),
                std::move(contracts),
                {luna::ownership::Relation::Owned, candidate->returnUsage});
            if (!callableType) callableType = candidateType;
            else if (!luna::types::sameType(callableType, candidateType)) {
                mContext.error("declaration family '" + selection->targetName +
                      "' contains incompatible callable signatures",
                      selection->line, selection->col);
                return TyUnknown;
            }

            luna::selector::Candidate viewCandidate;
            viewCandidate.symbolName = candidate->generatedSymbolName.empty()
                ? candidate->name : candidate->generatedSymbolName;
            viewCandidate.declarationId = nominalDeclarationIdentity(
                mContext.mProgram, "fn", viewCandidate.symbolName, candidate);
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
        auto selectedId = mContext.evaluateSelectorFunction(
            selectorFunction, view, staticSelectorArguments, evaluationFailure);
        luna::selector::Engine engine;
        auto result = engine.validate(
            view, selectedId ? std::vector<std::string>{*selectedId}
                             : std::vector<std::string>{});
        if (!selectedId || !result.success()) {
            const std::string reason = !evaluationFailure.empty()
                ? evaluationFailure : result.message;
            mContext.error("selector '" + selection->selectorName +
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
        mContext.error("selector function '" + selection->selectorName +
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
        const std::string savedPackage = mContext.mCurrentPackageId;
        const std::string savedModule = mContext.mCurrentModulePath;
        mContext.setDeclarationContext(selectorFunction);
        metadataKey = mContext.sourceDeclarationKey(metadataName->name);
        mContext.mCurrentPackageId = savedPackage;
        mContext.mCurrentModulePath = savedModule;
    }
    if (!metadataName || !mContext.mMetadataSchemas.count(metadataKey)) {
        mContext.error("select_unique requires a user-declared metadata value as its filter",
              selection->line, selection->col);
        return TyUnknown;
    }
    auto* schemaDeclaration = mContext.mMetadataSchemas[metadataKey];
    const auto schemaSymbol = schemaDeclaration->generatedSymbolName.empty()
        ? schemaDeclaration->name : schemaDeclaration->generatedSymbolName;
    selection->dynamicMetadataSchemaId = nominalDeclarationIdentity(
        mContext.mProgram, "meta", schemaSymbol, schemaDeclaration);
    std::vector<ConstValue> wantedValues;
    selection->dynamicFilterArguments.clear();
    for (auto& argument : metadataCall->args) {
        if (!selection->isDynamic) {
            auto value = mContext.evaluateConstExpr(argument.get(), selectorLocals);
            if (!value) {
                mContext.error("selector metadata expression is not compile-time evaluable",
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
            auto constant = mContext.evaluateConstExpr(argument.get());
            if (constant) binding.constant = *constant;
        }
        if (!binding.selectorArgumentIndex && !binding.constant) {
            mContext.error("dynamic selector metadata expressions must be an explicit selector "
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
            parameters.push_back(mContext.resolved(parameter.inferredType));
            contracts.push_back({parameter.relation, parameter.usage});
        }
        auto candidateType = Type::makeFunction(
            std::move(parameters), mContext.resolved(candidate->inferredReturnType),
            std::move(contracts),
            {luna::ownership::Relation::Owned, candidate->returnUsage});
        if (!callableType) callableType = candidateType;
        else if (!luna::types::sameType(callableType, candidateType)) {
            mContext.error("declaration family '" + selection->targetName +
                  "' contains incompatible callable signatures",
                  selection->line, selection->col);
            return TyUnknown;
        }
        luna::selector::Candidate viewCandidate;
        viewCandidate.symbolName = candidate->generatedSymbolName.empty()
            ? candidate->name : candidate->generatedSymbolName;
        viewCandidate.declarationId = nominalDeclarationIdentity(
            mContext.mProgram, "fn", viewCandidate.symbolName, candidate);
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
                    mContext.error("dynamic selector cannot inspect compile-time-only metadata '" +
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
            mContext.error("dynamic selector initially requires at most one '" +
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
            mContext.error(planError, selection->line, selection->col);
            return TyUnknown;
        }
        selection->dynamicCandidateIds = plan->candidateIds;
        if (selection->dynamicCandidates.empty()) {
            mContext.error("dynamic selector has no runtime-visible '" + metadataName->name +
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
                mContext.error("dynamic selector metadata is ambiguous between '" +
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
        mContext.error("selector '" + selection->selectorName + "' failed for family '" +
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


TypePtr BodyAnalyzer::analyzeIntrinsicCall(CallExpr* call, IdentifierExpr* id) {
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
            if (call->args.size() != 1 ||
                mContext.resolved(analyzeExpr(call->args.front().get()))->kind !=
                    TypeKind::DeclarationView)
                mContext.error("declaration_count expects one declaration_view",
                      call->line, call->col);
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
            return Type::makeDeclarationRef(view->inner);
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
                            const auto symbol =
                                candidate->generatedSymbolName.empty()
                                ? candidate->name
                                : candidate->generatedSymbolName;
                            if (nominalDeclarationIdentity(
                                    mContext.mProgram, "fn", symbol, candidate) !=
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

TypePtr BodyAnalyzer::analyzeCall(CallExpr* call) {
    auto* id = dynamic_cast<IdentifierExpr*>(call->callee.get());
    if (auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get()))
        return analyzeMemberCall(call, member);
    if (auto* selection = dynamic_cast<SelectExpr*>(call->callee.get())) {
        auto selected = mContext.resolved(analyzeSelect(selection));
        if (selected->kind != TypeKind::Function) return TyUnknown;
        if (selected->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch for selected declaration family '" +
                  selection->targetName + "'", call->line, call->col);
            return TyUnknown;
        }
        for (size_t index = 0; index < call->args.size(); ++index)
            mContext.constrain(analyzeExpr(call->args[index].get()), selected->paramTypes[index],
                      "selected call argument " + std::to_string(index + 1));
        call->resolvedSymbolName = selection->resolvedSymbolName;
        call->resultType = selected->returnType;
        return selected->returnType;
    }
    if (id) {
        if (auto result = analyzeIntrinsicCall(call, id))
            return result;
    }
    if (id && (id->name == "type_of" || id->name == "type_kind" ||
               id->name == "type_id" || id->name == "type_shape" ||
               id->name == "type_domain" || id->name == "type_nominal" ||
               id->name == "type_size" || id->name == "type_alignment" ||
               id->name == "type_field_count" || id->name == "type_field_name" ||
               id->name == "type_field_type" || id->name == "type_variant_count" ||
               id->name == "type_variant_name" || id->name == "type_variant_field_count" ||
               id->name == "type_is_struct" || id->name == "type_is_enum" ||
               id->name == "type_is_nominal" || id->name == "type_is_structural" ||
               id->name == "type_is_meta" || id->name == "type_is_reference" ||
               id->name == "type_same" || id->name == "type_same_shape" ||
               id->name == "type_abi_compatible"))
        return mContext.analyzeReflectionCall(call, id->name);
    if (id && id->name == "slice") {
        if (call->args.size() != 3) { mContext.error("slice expects `slice(borrow array, start, end)`", call->line, call->col); return TyUnknown; }
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr source = mContext.resolved(analyzeExpr(call->args[0].get()));
        if (!borrow || source->kind != TypeKind::Reference || !source->inner || source->inner->kind != TypeKind::Array) {
            mContext.error("slice requires an explicit `borrow array` as its first argument", call->line, call->col); return TyUnknown;
        }
        mContext.requireInteger(analyzeExpr(call->args[1].get()), "slice start");
        mContext.requireInteger(analyzeExpr(call->args[2].get()), "slice end");
        return Type::makeSlice(source->inner->inner);
    }
    if (id && (id->name == "gpu_alloc_i32" || id->name == "gpu_load_i32" ||
               id->name == "gpu_store_i32" || id->name == "gpu_free" ||
               id->name == "gpu_copy_from_host_i32" ||
               id->name == "gpu_copy_to_host_i32")) {
        auto requireCount = [this, call, id](size_t expected) {
            if (call->args.size() != expected) {
                mContext.error("'" + id->name + "' expects " + std::to_string(expected) +
                      " argument" + (expected == 1 ? "" : "s"), call->line, call->col);
                return false;
            }
            return true;
        };
        if (id->name == "gpu_alloc_i32") {
            if (!requireCount(1)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'gpu_alloc_i32' is a host operation and cannot run inside a kernel", call->line, call->col);
            mContext.requireInteger(analyzeExpr(call->args[0].get()), "gpu_alloc_i32 element count");
            return Type::makeDeviceBuffer(TyI32);
        }
        if (id->name == "gpu_free") {
            if (!requireCount(1)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'gpu_free' is a host operation and cannot run inside a kernel", call->line, call->col);
            if (!dynamic_cast<MoveExpr*>(call->args[0].get()))
                mContext.error("'gpu_free' requires `move buffer` to make ownership transfer explicit",
                      call->line, call->col);
            mContext.constrain(analyzeExpr(call->args[0].get()), Type::makeDeviceBuffer(TyI32),
                      "gpu_free buffer");
            return TyUnit;
        }
        if (id->name == "gpu_copy_from_host_i32" || id->name == "gpu_copy_to_host_i32") {
            if (!requireCount(3)) return TyUnknown;
            if (mContext.mInKernel)
                mContext.error("'" + id->name + "' is a host operation and cannot run inside a kernel",
                      call->line, call->col);
            const bool isUpload = id->name == "gpu_copy_from_host_i32";
            auto* deviceBorrow = dynamic_cast<BorrowExpr*>(call->args[0 + (isUpload ? 0 : 1)].get());
            auto* hostBorrow = dynamic_cast<BorrowExpr*>(call->args[1 - (isUpload ? 0 : 1)].get());
            TypePtr deviceActual = analyzeExpr(call->args[isUpload ? 0 : 1].get());
            TypePtr hostActual = analyzeExpr(call->args[isUpload ? 1 : 0].get());
            const bool needsMutableDevice = isUpload;
            const bool needsMutableHost = !isUpload;
            if (!deviceBorrow || deviceBorrow->isMutable != needsMutableDevice)
                mContext.error("'" + id->name + "' requires " +
                      std::string(needsMutableDevice ? "`borrow mut buffer`" : "`borrow buffer`") +
                      " for its device-buffer argument", call->line, call->col);
            if (!hostBorrow || hostBorrow->isMutable != needsMutableHost)
                mContext.error("'" + id->name + "' requires " +
                      std::string(needsMutableHost ? "`borrow mut host`" : "`borrow host`") +
                      " for its host raw-pointer argument", call->line, call->col);
            mContext.constrain(deviceActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), needsMutableDevice),
                      "device-buffer argument of '" + id->name + "'");
            mContext.constrain(hostActual,
                      Type::makeReference(Type::makeRawPointer(TyI32), needsMutableHost),
                      "host raw-pointer argument of '" + id->name + "'");
            mContext.requireInteger(analyzeExpr(call->args[2].get()),
                           "element count of '" + id->name + "'");
            if (auto* literal = dynamic_cast<IntLiteralExpr*>(call->args[2].get()); literal && literal->value < 0)
                mContext.error("'" + id->name + "' requires a non-negative element count",
                      literal->line, literal->col);
            return TyUnit;
        }
        const bool isStore = id->name == "gpu_store_i32";
        if (!requireCount(isStore ? 3 : 2)) return TyUnknown;
        auto* borrow = dynamic_cast<BorrowExpr*>(call->args[0].get());
        TypePtr bufferActual = analyzeExpr(call->args[0].get());
        TypePtr resolvedBufferActual = mContext.resolved(bufferActual);
        const bool isDeviceReference = resolvedBufferActual->kind == TypeKind::Reference &&
            resolvedBufferActual->inner &&
            resolvedBufferActual->inner->kind == TypeKind::DeviceBuffer;
        if ((!borrow && !isDeviceReference) || (borrow && isStore && !borrow->isMutable))
            mContext.error(std::string("'") + id->name + "' requires an explicit " +
                  (isStore ? "`borrow mut buffer`" : "`borrow buffer`") +
                  " (or an already-borrowed kernel parameter)",
                  call->line, call->col);
        if (isStore) {
            mContext.constrain(bufferActual,
                      Type::makeReference(Type::makeDeviceBuffer(TyI32), true),
                      std::string("first argument of '") + id->name + "'");
        } else if (isDeviceReference) {
            // A mutable kernel parameter is also valid for a read-only load.
            mContext.constrain(resolvedBufferActual->inner, Type::makeDeviceBuffer(TyI32),
                      std::string("first argument of '") + id->name + "'");
        }
        mContext.requireInteger(analyzeExpr(call->args[1].get()), std::string("index argument of '") + id->name + "'");
        if (isStore) mContext.constrain(analyzeExpr(call->args[2].get()), TyI32,
                               "value argument of 'gpu_store_i32'");
        return isStore ? TyUnit : TyI32;
    }
    if (mContext.mInKernel && id) {
        mContext.error("kernel body may only call device built-ins (`gpu_load_i32` and `gpu_store_i32`) in the initial ABI",
              call->line, call->col);
        return TyUnknown;
    }
    SymbolInfo* sym = id ? mContext.lookupSymbol(id->name) : nullptr;
    auto constrainArgument = [this](Expr* expr, const TypePtr& expected,
                                     const std::string& context) {
        TypePtr actual = analyzeExpr(expr);
        // Integer literals are representationally polymorphic at an FFI
        // boundary; the code generator will widen/truncate them to the ABI
        // parameter width.
        if (dynamic_cast<IntLiteralExpr*>(expr) && isNumericType(mContext.resolved(expected)))
            return;
        if (dynamic_cast<StringLiteralExpr*>(expr) &&
            mContext.resolved(expected)->kind == TypeKind::CStr)
            return;
        mContext.constrain(actual, expected, context);
    };

    if (id) {
        const std::string declarationKey = mContext.sourceDeclarationKey(id->name);
        auto family = mContext.mFunctionFamilies.find(declarationKey);
        if (family != mContext.mFunctionFamilies.end() && family->second.size() > 1) {
            mContext.error("declaration family '" + id->name +
                  "' is ambiguous; use `select " + id->name +
                  " with selector(...)`", call->line, call->col);
            return TyUnknown;
        }
        if (sym && sym->kind == SymbolKind::Function &&
            family != mContext.mFunctionFamilies.end() && family->second.size() == 1) {
            auto* declaration = family->second.front();
            call->resolvedSymbolName = declaration->generatedSymbolName.empty()
                ? declaration->name : declaration->generatedSymbolName;
            id->resolvedSymbolName = call->resolvedSymbolName;
            mContext.recordDeclarationReference(id, id->name.size(), declaration);
        }
    }

    // Direct calls and closure calls both become constraints. This is the
    // important consumer side of inference: an untyped declaration can be
    // solved by the arguments at its call sites.
    if (!sym) {
        TypePtr calleeType = analyzeExpr(call->callee.get());
        if (calleeType->kind != TypeKind::Function &&
            calleeType->kind != TypeKind::Closure) {
            mContext.error("Expression is not callable");
            return TyUnknown;
        }
        if (calleeType->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch in indirect call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), calleeType->paramTypes[i], "call argument");
        call->resultType = calleeType->returnType;
        return call->resultType;
    }

    if (sym->kind == SymbolKind::Variable && sym->type &&
        (mContext.resolved(sym->type)->kind == TypeKind::Function ||
         mContext.resolved(sym->type)->kind == TypeKind::Closure)) {
        auto fn = mContext.resolved(sym->type);
        if (fn->paramTypes.size() != call->args.size()) {
            mContext.error("Argument count mismatch in closure call");
            return TyUnknown;
        }
        for (size_t i = 0; i < call->args.size(); ++i)
            constrainArgument(call->args[i].get(), fn->paramTypes[i], "closure call argument");
        call->resultType = fn->returnType;
        return call->resultType;
    }
    if (sym->kind != SymbolKind::Function) {
        mContext.error("'" + id->name + "' is not callable");
        return TyUnknown;
    }
    if (call->args.size() != sym->paramTypes.size()) {
        mContext.error("Argument count mismatch for '" + id->name + "'");
        return TyUnknown;
    }

    // Generic functions retain their existing monomorphization path. Their
    // type parameters are explicit variables, whereas omitted ordinary
    // signatures use the inference variables in paramTypes/returnType below.
    if (!sym->typeParams.empty() && sym->genericDecl) {
        TypeVec actualTypes;
        for (auto& argument : call->args)
            actualTypes.push_back(mContext.resolved(analyzeExpr(argument.get())));
        TypeVec concreteTypes;
        if (!call->typeArgASTs.empty()) {
            if (call->typeArgASTs.size() != sym->typeParams.size()) {
                mContext.error("generic function '" + id->name + "' expects " +
                      std::to_string(sym->typeParams.size()) +
                      " type argument(s)", call->line, call->col);
                return TyUnknown;
            }
            for (auto& typeArgument : call->typeArgASTs)
                concreteTypes.push_back(mContext.resolved(
                    mContext.resolveTypeAST(typeArgument.get(), {})));
        } else {
            std::unordered_map<std::string, TypePtr> inferred;
            std::function<void(const TypePtr&, const TypePtr&)> infer =
                [&](const TypePtr& patternValue,
                    const TypePtr& actualValue) {
                const TypePtr pattern = mContext.resolved(patternValue);
                const TypePtr actual = mContext.resolved(actualValue);
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
                        mContext.error("conflicting inference for generic type '" +
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
                    mContext.error("could not infer generic type argument '" +
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
                mContext.resolved(concreteTypes[index]);
        for (size_t index = 0;
             index < sym->paramTypes.size() &&
             index < call->args.size(); ++index) {
            constrainArgument(
                call->args[index].get(),
                substituteNominalType(
                    mContext.resolved(sym->paramTypes[index]),
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
                    !mContext.satisfiesTrait(traitId, concrete->second))
                    mContext.error("Type '" + mContext.typeIdentity(concrete->second) +
                          "' does not satisfy trait '" +
                          displayTraitRef(clause.trait) + "'");
                continue;
            }

            if (clause.kind == WhereClause::Kind::ConstraintExpression) {
                std::vector<std::string> activeConstraints;
                auto value = mContext.evaluateConstraintExpr(
                    clause.constraintExpression.get(),
                    constraintBindings, activeConstraints);
                if (!value) {
                    mContext.error(
                        "inline where predicate is not compile-time evaluable",
                        call->line, call->col);
                } else if (auto* satisfied = std::get_if<bool>(&*value)) {
                    if (!*satisfied)
                        mContext.error(
                            "inline where predicate is not satisfied",
                            call->line, call->col);
                } else {
                    mContext.error(
                        "inline where predicate must evaluate to bool",
                        call->line, call->col);
                }
                continue;
            }

            TypeVec conceptArguments;
            for (auto& argument : clause.constraintTypeArgs)
                conceptArguments.push_back(mContext.resolved(
                    mContext.resolveTypeAST(argument.get(), constraintBindings)));
            std::vector<std::string> activeConstraints;
            auto satisfied = mContext.evaluateConstraint(
                clause.constraintName, conceptArguments, activeConstraints);
            if (!satisfied) {
                mContext.error("constraint '" + clause.constraintName +
                      "' is not compile-time evaluable");
            } else if (!*satisfied) {
                std::string types;
                for (size_t index = 0; index < conceptArguments.size(); ++index) {
                    if (index) types += ", ";
                    types += conceptArguments[index]->toString();
                }
                mContext.error("constraint '" + clause.constraintName +
                      "<" + types + ">' is not satisfied");
            }
        }
        auto* specialized = mContext.monomorphize(sym->genericDecl, concreteTypes);
        if (specialized && mContext.mProgram) {
            const bool newlyCreated = !mContext.mGeneratedInstances.empty() &&
                                      mContext.mGeneratedInstances.back().get() == specialized;
            if (newlyCreated) {
                mContext.mProgram->declarations.push_back(std::move(mContext.mGeneratedInstances.back()));
                const std::string savedPackage = mContext.mCurrentPackageId;
                const std::string savedModule = mContext.mCurrentModulePath;
                mContext.setDeclarationContext(specialized);
                mContext.declareFunction(specialized);
                analyzeFunction(specialized);
                mContext.mCurrentPackageId = savedPackage;
                mContext.mCurrentModulePath = savedModule;
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
        for (auto& arg : call->args) {
            const TypePtr argumentType = mContext.resolved(analyzeExpr(arg.get()));
            if (id && id->name == "print" &&
                argumentType->kind != TypeKind::I32 &&
                argumentType->kind != TypeKind::String &&
                argumentType->kind != TypeKind::CStr) {
                mContext.error("temporary print supports only i32, string, or cstr; "
                      "got " + argumentType->toString(),
                      call->line, call->col);
            }
        }
        return sym->returnType ? sym->returnType : TyUnit;
    }

    for (size_t i = 0; i < call->args.size(); ++i)
        constrainArgument(call->args[i].get(), sym->paramTypes[i],
                          "argument " + std::to_string(i + 1) + " of '" + id->name + "'");
    if (id) {
        const std::string constexprName = call->resolvedSymbolName.empty()
            ? id->name : call->resolvedSymbolName;
        auto constexprIt = mContext.mConstexprFunctions.find(constexprName);
        if (constexprIt != mContext.mConstexprFunctions.end()) {
            if (auto value = mContext.evaluateConstExpr(call)) call->compileTimeValue = std::move(*value);
        }
    }
    call->returnsLinear = sym->returnsLinear;
    call->returnUsage = sym->returnUsage;
    call->resultType = sym->returnType ? sym->returnType : TyUnit;
    return call->resultType;
}


TypePtr BodyAnalyzer::analyzeMemberCall(
    CallExpr* call, FieldAccessExpr* member) {
    TypePtr receiver =
        mContext.resolved(analyzeExpr(member->object.get()));
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
        if (!luna::types::sameType(
                mContext.resolved(protocol.item), mContext.resolved(item))) {
            mContext.error("Core `FromIterator` for '" + target->toString() +
                  "' collects '" + protocol.item->toString() +
                  "', but this iterator yields '" + item->toString() + "'",
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
