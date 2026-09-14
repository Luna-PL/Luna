#include "Verifier.h"
#include "VerifierInternal.h"
#include "../core/TypeLayout.h"

#include "../diagnostics/Diagnostic.h"
#include "../core/TypeRelations.h"

#include <algorithm>
#include <cctype>
#include <functional>
#include <unordered_map>
#include <unordered_set>

namespace moon {

using namespace verifier_detail;

void Verifier::verifyDeclaration(const Decl& declaration, const Module& module) {
    if (!validSeparatedName(declaration.packageId, "."))
        error(declaration.location, "declaration has invalid Package ID '" +
                                    declaration.packageId + "'");
    if (!validSeparatedName(declaration.modulePath, "::", true))
        error(declaration.location, "declaration has invalid module path '" +
                                    declaration.modulePath + "'");
    if (!declaration.sysmeta.abi.dropGlueSymbol.empty())
        error(declaration.location,
              "executable declaration retains a linkage-string Drop reference");
    if (auto* function = dynamic_cast<const FunctionDecl*>(&declaration)) {
        verifyFunction(*function, module);
        return;
    }
    if (auto* fragment = dynamic_cast<const FragmentDecl*>(&declaration)) {
        if (!fragment->body)
            error(fragment->location, "fragment '" + fragment->name + "' has no body");
        verifyType(fragment->structuralType, fragment->location,
                   "fragment '" + fragment->name + "' structural type", module);
        const auto* contract = module.findType(fragment->structuralType);
        if (!contract || contract->kind != TypeKind::Fragment) {
            error(fragment->location,
                  "fragment declaration has no frozen fragment contract");
        } else {
            const auto expectedKind = fragment->kind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor
                : ContinuationKind::Context;
            if (contract->continuationKind != expectedKind ||
                contract->isMultiShot !=
                    (fragment->cardinality == FragmentCardinality::Many))
                error(fragment->location,
                      "fragment declaration disagrees with its frozen control contract");
        }
        verifyDeclarationRef(
            fragment->targetSlot, fragment->location,
            "nominal target of fragment '" + fragment->name + "'",
            module, DeclarationKind::Slot);
        const auto* target = module.findDeclaration(fragment->targetSlot);
        if (contract && target && contract->nominalDeclarationId !=
            target->id)
            error(fragment->location,
                  "fragment contract is not nominally bound to its target slot");
        for (const auto& parameter : fragment->params)
            verifyType(parameter.type, fragment->location,
                       "fragment parameter '" + parameter.name + "'", module);
        verifyBlock(fragment->body.get(), module, fragment->name);
        return;
    }
    if (auto* slot = dynamic_cast<const SlotDecl*>(&declaration)) {
        verifyType(slot->structuralType, slot->location,
                   "slot '" + slot->name + "' structural type", module);
        const auto* contract = module.findType(slot->structuralType);
        if (!contract || contract->kind != TypeKind::Slot) {
            error(slot->location,
                  "slot declaration has no frozen nominal slot contract");
        } else {
            const auto expectedKind =
                slot->acceptedKind == FragmentKind::Interceptor
                    ? ContinuationKind::Interceptor
                    : ContinuationKind::Context;
            if (contract->continuationKind != expectedKind ||
                contract->isMultiShot)
                error(slot->location,
                      "slot declaration disagrees with its frozen single-shot control contract");
            if (contract->nominalDeclarationId != slot->declarationId)
                error(slot->location,
                      "slot type is not nominally bound to its declaration identity");
        }
        for (const auto& parameter : slot->params)
            verifyType(parameter.type, slot->location,
                       "slot parameter '" + parameter.name + "'", module);
        if (!slot->defaultFragment.empty())
            verifyDeclarationRef(
                slot->defaultFragment, slot->location,
                "default fragment for slot '" + slot->name + "'",
                module, DeclarationKind::Fragment);
        return;
    }
    if (auto* structure = dynamic_cast<const StructDecl*>(&declaration)) {
        verifyType(structure->type, structure->location,
                   "struct '" + structure->name + "'", module,
                   !structure->typeParams.empty());
        for (const auto& field : structure->fields)
            verifyType(field.type, structure->location,
                       "field '" + structure->name + "." + field.name + "'",
                       module,
                       !structure->typeParams.empty());
        return;
    }
    if (auto* enumeration = dynamic_cast<const EnumDecl*>(&declaration)) {
        verifyType(enumeration->type, enumeration->location,
                   "enum '" + enumeration->name + "'", module,
                   !enumeration->typeParams.empty());
        return;
    }
    if (auto* trait = dynamic_cast<const TraitDecl*>(&declaration)) {
        verifyType(trait->type, trait->location,
                   "trait '" + trait->name + "'", module, true);
        for (const auto& method : trait->methods) {
            for (const auto& parameter : method.params)
                verifyType(parameter.type, trait->location,
                           "trait method parameter '" + method.name + "." +
                               parameter.name + "'", module, true);
            verifyType(method.returnType, trait->location,
                       "trait method return '" + method.name + "'", module, true);
        }
        return;
    }
    if (auto* implementation = dynamic_cast<const ImplDecl*>(&declaration)) {
        verifyDeclarationRef(
            implementation->traitRef, implementation->location,
            "implementation trait", module, DeclarationKind::Trait);
        verifyType(implementation->targetType, implementation->location,
                   "implementation target", module,
                   !implementation->typeParams.empty());
        for (const auto& method : implementation->methods) {
            if (!method) error(implementation->location, "implementation contains a null method");
            else verifyFunction(
                *method, module,
                !implementation->typeParams.empty());
        }
    }
}

void Verifier::verifyFunction(
    const FunctionDecl& function, const Module& module,
    bool inheritsTypeParameters) {
    const bool generic = isGeneric(function) || inheritsTypeParameters;
    const bool previousAllowance = mAllowTypeParameters;
    mAllowTypeParameters = generic;
    if (function.name.empty()) error(function.location, "function has no source name");
    if (function.generatedSymbolName.empty() && function.linkName.empty())
        error(function.location, "function '" + function.name + "' has no linkage identity");
    for (const auto& parameter : function.params) {
        verifyType(parameter.type, function.location,
                   "parameter '" + function.name + "." + parameter.name + "'",
                   module, generic);
        if (parameter.isLinear !=
            (parameter.usage == luna::ownership::Usage::Linear))
            error(function.location, "parameter '" + function.name + "." +
                  parameter.name + "' has inconsistent linear compatibility flag");
        if (parameter.relation != luna::ownership::Relation::Owned &&
            parameter.usage != luna::ownership::Usage::Copy)
            error(function.location, "borrowed parameter '" + function.name + "." +
                  parameter.name + "' must use copy cardinality");
    }
    verifyType(function.returnType, function.location,
               "return type of '" + function.name + "'", module, generic);
    if (function.isExtern) {
        if (function.body || function.controlFlow)
            error(function.location, "extern function '" + function.name +
                  "' carries an executable body");
    } else if (static_cast<bool>(function.body) ==
               static_cast<bool>(function.controlFlow)) {
        error(function.location, "function '" + function.name +
              "' must own exactly one structured or canonical body");
    }
    if (function.returnsLinear !=
        (function.returnUsage == luna::ownership::Usage::Linear))
        error(function.location, "function '" + function.name +
              "' has inconsistent linear return compatibility flag");
    if (function.isKernel && function.isCodegenReachable && !module.features.kernel)
        error(function.location, "kernel '" + function.name +
                                 "' is present without the kernel feature");
    const auto* returnType = module.findType(function.returnType);
    if (function.isKernel && returnType && returnType->kind != TypeKind::Unit)
        error(function.location, "kernel '" + function.name + "' must return unit");
    if (function.body) {
        verifyBlock(function.body.get(), module, function.name);
    } else if (function.controlFlow) {
        const auto* root = function.controlFlow->findRegion(
            function.controlFlow->rootRegion);
        if (!root || root->kind != RegionKind::Function)
            error(function.location,
                  "function canonical body has no function root region");

        std::vector<const LocalRecord*> parameters;
        for (const auto& local : function.controlFlow->locals)
            if (local.kind == LocalKind::Parameter)
                parameters.push_back(&local);
        if (parameters.size() != function.params.size()) {
            error(function.location,
                  "function canonical body parameter table has the wrong arity");
        } else {
            for (size_t index = 0; index < parameters.size(); ++index) {
                const auto& expected = function.params[index];
                const auto& actual = *parameters[index];
                if (actual.scope != function.controlFlow->rootScope ||
                    actual.name != expected.name ||
                    actual.type != expected.type ||
                    actual.usage != expected.usage ||
                    actual.relation != expected.relation)
                    error(function.location,
                          "function canonical parameter disagrees with its signature");
            }
        }

        Verifier nestedVerifier;
        if (!nestedVerifier.verify(*function.controlFlow, module))
            mErrors.insert(mErrors.end(), nestedVerifier.errors().begin(),
                           nestedVerifier.errors().end());
    }
    mAllowTypeParameters = previousAllowance;
}

void Verifier::verifyBlock(const BlockStmt* block, const Module& module,
                           const std::string& owner) {
    if (!block) {
        error({}, "null block in '" + owner + "'");
        return;
    }
    for (const auto& statement : block->stmts)
        verifyStmt(statement.get(), module, owner);
}

void Verifier::verifyStmt(const Stmt* stmt, const Module& module,
                          const std::string& owner) {
    if (!stmt) {
        error({}, "null statement in '" + owner + "'");
        return;
    }
    if (auto* block = dynamic_cast<const BlockStmt*>(stmt)) {
        verifyBlock(block, module, owner);
    } else if (auto* let = dynamic_cast<const LetStmt*>(stmt)) {
        if (let->name.empty()) error(let->location, "binding has no name in '" + owner + "'");
        verifyType(let->type, let->location, "binding '" + let->name + "'", module);
        verifyExpr(let->initializer.get(), module, owner);
        const auto* bindingType = module.findType(let->type);
        auto requiredUsage = bindingType
            ? bindingType->sysmeta.resource.usage
            : luna::ownership::Usage::Copy;
        if (auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get())) {
            const auto callUsage = call->returnsLinear
                ? luna::ownership::Usage::Linear
                : call->returnUsage;
            requiredUsage = luna::ownership::strongerUsage(
                requiredUsage, callUsage);
        }
        if (!luna::ownership::satisfiesUsageRequirement(
                let->usage, requiredUsage))
            error(let->location, "binding '" + let->name +
                  "' weakens its required usage contract in '" + owner + "'");
        if (let->isLinear != (let->usage == luna::ownership::Usage::Linear))
            error(let->location, "binding '" + let->name +
                  "' has inconsistent linear compatibility flag");
        if (let->materializesIteratorRecipe) {
            if (!bindingType || bindingType->kind != TypeKind::Iterator)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no iterator type");
            auto* call = dynamic_cast<const CallExpr*>(
                let->initializer.get());
            if (!call ||
                call->iteratorOp == IteratorOp::None ||
                call->iteratorOp == IteratorOp::Fold ||
                call->iteratorOp == IteratorOp::ForEach ||
                call->iteratorOp == IteratorOp::Count ||
                call->iteratorOp == IteratorOp::Collect)
                error(let->location,
                      "materialized iterator binding '" +
                      let->name +
                      "' has no adapter recipe");
            if (let->materializedIteratorOwnsSource) {
                verifyType(
                    let->materializedIteratorSourceType,
                    let->location,
                    "materialized iterator source", module);
                const auto* source = module.findType(
                    let->materializedIteratorSourceType);
                const auto* sourceElement = source
                    ? module.findType(source->innerTypeId) : nullptr;
                if (!source ||
                    source->kind != TypeKind::Array ||
                    !sourceElement ||
                    sourceElement->sysmeta.resource.usage ==
                        luna::ownership::Usage::Copy ||
                    luna::ownership::mustConsume(
                        source->sysmeta.resource.usage))
                    error(let->location,
                          "owning materialized iterator '" +
                          let->name +
                          "' has no affine move-only array source");
            } else if (!let->materializedIteratorSourceType.empty()) {
                error(let->location,
                      "non-owning materialized iterator '" +
                      let->name +
                      "' carries an owning source witness");
            }
        } else if (let->materializedIteratorOwnsSource ||
                   !let->materializedIteratorSourceType.empty()) {
            error(let->location,
                  "ordinary binding '" + let->name +
                  "' carries materialized iterator source state");
        }
    } else if (auto* ret = dynamic_cast<const ReturnStmt*>(stmt)) {
        if (ret->value) verifyExpr(ret->value.get(), module, owner);
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : ret->cleanups) {
            if (cleanup.place.empty())
                error(ret->location, "return cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(ret->location, "duplicate return cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(ret->location, "return cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, ret->location,
                    "return cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* expression = dynamic_cast<const ExprStmt*>(stmt)) {
        verifyExpr(expression->expr.get(), module, owner);
    } else if (auto* conditional = dynamic_cast<const IfStmt*>(stmt)) {
        verifyExpr(conditional->cond.get(), module, owner);
        verifyBlock(conditional->thenBlock.get(), module, owner);
        if (conditional->elseBranch)
            verifyStmt(conditional->elseBranch.get(), module, owner);
    } else if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
        verifyExpr(match->scrutinee.get(), module, owner);
        verifyType(match->matchedType, match->location, "match type", module);
        if (match->arms.empty())
            error(match->location, "match in '" + owner + "' has no arms");
        const auto* matchedType = module.findType(match->matchedType);
        const size_t expectedVariantCount =
            matchedType && matchedType->kind == TypeKind::Result
                ? 2
                : (matchedType ? matchedType->variants.size() : 0);
        if (match->arms.size() != expectedVariantCount)
            error(match->location, "match in '" + owner +
                  "' is not exhaustive in frozen MoonIR");
        std::unordered_set<uint32_t> variants;
        for (const auto& arm : match->arms) {
            if (!variants.insert(arm.variantIndex).second)
                error(arm.location, "duplicate match variant index in '" +
                      owner + "'");
            if (arm.variantIndex >= expectedVariantCount)
                error(arm.location, "match arm has an out-of-range variant "
                      "index in '" + owner + "'");
            if (arm.bindings.size() != arm.bindingTypes.size())
                error(arm.location, "match arm binding/type arity mismatch in '" +
                      owner + "'");
            if (arm.bindings.size() != arm.bindingUsages.size())
                error(arm.location, "match arm binding/usage arity mismatch in '" +
                      owner + "'");
            TypeRefVec expectedFields;
            if (matchedType && matchedType->kind == TypeKind::Enum &&
                arm.variantIndex <
                    matchedType->variants.size()) {
                expectedFields =
                    matchedType->variants[
                        arm.variantIndex].fields;
            } else if (matchedType &&
                       matchedType->kind ==
                           TypeKind::Result &&
                       matchedType->typeArgumentIds.size() == 2 &&
                       arm.variantIndex < 2) {
                expectedFields.push_back(
                    matchedType->typeArgumentIds[
                        arm.variantIndex == 1 ? 0 : 1]);
            }
            if (arm.bindingTypes.size() != expectedFields.size())
                error(arm.location, "match arm payload arity disagrees with "
                      "its frozen variant in '" + owner + "'");
            const size_t comparable = std::min(
                arm.bindingTypes.size(), expectedFields.size());
            for (size_t index = 0; index < comparable; ++index) {
                if (arm.bindingTypes[index] != expectedFields[index])
                    error(arm.location, "match binding type disagrees with "
                          "its frozen variant payload in '" + owner + "'");
                if (index < arm.bindingUsages.size() &&
                    !luna::ownership::satisfiesUsageRequirement(
                        arm.bindingUsages[index],
                        frozenUsage(module, arm.bindingTypes[index])))
                    error(arm.location, "match binding weakens its required "
                          "usage contract in '" + owner + "'");
            }
            for (const auto& type : arm.bindingTypes)
                verifyType(type, arm.location, "match binding type", module);
            verifyBlock(arm.body.get(), module, owner);
        }
    } else if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
        verifyExpr(loop->cond.get(), module, owner);
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
        verifyExpr(loop->iterable.get(), module, owner);
        verifyType(loop->elementType, loop->location,
                   "for-loop element type", module);
        if (!luna::ownership::satisfiesUsageRequirement(
                loop->bindingUsage,
                frozenUsage(module, loop->elementType)))
            error(loop->location, "for-loop binding weakens its required "
                  "usage contract in '" + owner + "'");
        if (!loop->protocolNext.empty()) {
            verifyDeclarationRef(
                loop->protocolNext, loop->location,
                "Iterator::next protocol witness", module,
                DeclarationKind::Function);
            verifyType(loop->protocolIteratorType, loop->location,
                       "iterator protocol state type", module);
            verifyType(loop->protocolOptionType, loop->location,
                       "iterator protocol option type", module);
            const auto* optionType = module.findType(loop->protocolOptionType);
            if (!optionType || optionType->kind != TypeKind::Enum) {
                error(loop->location,
                      "iterator protocol for-loop requires an enum Option type");
            } else {
                const auto variantCount =
                    optionType->variants.size();
                if (loop->protocolNoneVariant >= variantCount ||
                    loop->protocolSomeVariant >= variantCount ||
                    loop->protocolNoneVariant ==
                        loop->protocolSomeVariant)
                    error(loop->location,
                          "iterator protocol for-loop has invalid Option variants");
            }
            if (!loop->protocolInto.empty()) {
                verifyDeclarationRef(
                    loop->protocolInto, loop->location,
                    "IntoIterator protocol witness", module,
                    DeclarationKind::Function);
                verifyType(loop->protocolInputType,
                           loop->location,
                           "IntoIterator protocol input type", module);
                if (loop->protocolStateName.empty())
                    error(loop->location,
                          "IntoIterator protocol for-loop has no hidden "
                          "state identity");
            } else if (!loop->protocolStateName.empty()) {
                error(loop->location,
                      "direct Iterator for-loop unexpectedly owns a "
                      "hidden state identity");
            }
        }
        if (!loop->recipeStateName.empty()) {
            verifyType(loop->recipeSourceType,
                       loop->location,
                       "consuming recipe source type", module);
            const auto* recipeSource = module.findType(loop->recipeSourceType);
            const auto* recipeElement = recipeSource
                ? module.findType(recipeSource->innerTypeId) : nullptr;
            if (!recipeSource ||
                recipeSource->kind !=
                    TypeKind::Array ||
                !recipeElement ||
                recipeElement->sysmeta.resource.usage ==
                    luna::ownership::Usage::Copy)
                error(loop->location,
                      "consuming recipe state must own a move-only array");
        }
        verifyBlock(loop->body.get(), module, owner);
    } else if (auto* release = dynamic_cast<const FreeStmt*>(stmt)) {
        verifyExpr(release->operand.get(), module, owner);
        if (release->operand && !release->operand->type.empty())
            verifyCleanupAction(
                release->action,
                release->operand->type,
                release->location, "free operation", module);
    } else if (auto* slot = dynamic_cast<const SlotDeclStmt*>(stmt)) {
        verifyType(slot->structuralType, slot->location,
                   "slot '" + slot->name + "' structural contract", module);
        if (!slot->defaultFragment.empty() ||
            !slot->defaultFragmentRef.empty())
            verifyDeclarationRef(
                slot->defaultFragmentRef, slot->location,
                "default fragment for slot '" + slot->name + "'", module,
                DeclarationKind::Fragment);
    } else if (auto* slot = dynamic_cast<const SlotInvokeStmt*>(stmt)) {
        for (const auto& argument : slot->args)
            verifyExpr(argument.get(), module, owner);
        verifyType(slot->structuralType, slot->location,
                   "slot invocation '" + slot->name + "' contract", module);
        if (!slot->defaultFragment.empty() ||
            !slot->defaultFragmentRef.empty())
            verifyDeclarationRef(
                slot->defaultFragmentRef, slot->location,
                "default fragment for slot invocation '" +
                slot->name + "'", module, DeclarationKind::Fragment);
        verifyBlock(slot->continuation.get(), module, owner);
    } else if (auto* apply = dynamic_cast<const ApplyStmt*>(stmt)) {
        verifyDeclarationRef(
            apply->fragmentRef, apply->location,
            "fragment bound by apply for slot '" + apply->slotName + "'",
            module, DeclarationKind::Fragment);
        if (apply->body) verifyBlock(apply->body.get(), module, owner);
    } else if (auto* abort = dynamic_cast<const AbortStmt*>(stmt)) {
        std::unordered_set<std::string> cleanupPlaces;
        for (const auto& cleanup : abort->cleanups) {
            if (cleanup.place.empty())
                error(abort->location, "abort cleanup in '" + owner + "' has no place");
            else if (!cleanupPlaces.insert(cleanup.place).second)
                error(abort->location, "duplicate abort cleanup for place '" +
                      cleanup.place + "' in '" + owner + "'");
            if (cleanup.typeId.empty() || !module.findType(cleanup.typeId))
                error(abort->location, "abort cleanup for place '" + cleanup.place +
                      "' references no frozen type in '" + owner + "'");
            else
                verifyCleanupAction(
                    cleanup.action, cleanup.typeId, abort->location,
                    "abort cleanup for '" + cleanup.place + "'", module);
        }
    } else if (auto* await = dynamic_cast<const AwaitStmt*>(stmt)) {
        verifyExpr(await->event.get(), module, owner);
    }
}


} // namespace moon
