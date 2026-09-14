#include "OwnershipChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <functional>
#include <algorithm>
#include <limits>
#include <unordered_set>

OwnershipChecker::OwnershipChecker() {
    enterScope();
}

OwnershipChecker::FlowResult OwnershipChecker::checkAbortStmt(AbortStmt* abort) {
        if (!mCurrentFragmentAbortExits) {
            error("`abort()` may only appear in an applied interceptor or context",
                  abort->line, abort->col);
            return false;
        }
        bool ok = true;
        for (size_t index = mCurrentFragmentScopeBase; index < mScopes.size(); ++index) {
            for (const auto& [name, info] : mScopes[index]) {
                if (luna::ownership::mustConsume(info.usage) &&
                    info.state == OwnState::Valid) {
                    error("Linear variable '" + name +
                          "' must be consumed before aborting the fragment",
                          abort->line, abort->col);
                    ok = false;
                }
            }
        }
        if (ok) {
            abort->autoFrees = collectFreesAtFragmentExit();
            abort->cleanups.clear();
            for (const auto& place : abort->autoFrees) {
                auto* variable = lookupCleanupVariable(place);
                abort->cleanups.push_back({
                    place, cleanupActionForType(variable ? variable->type : nullptr),
                    variable ? variable->type : nullptr});
            }
        }
        CheckerState exit = captureState();
        exit.scopes.resize(mCurrentFragmentScopeBase);
        exit.loans.resize(mCurrentFragmentScopeBase);
        exit.applyScopes.resize(mCurrentFragmentApplyBase);
        exit.slotScopes.resize(mCurrentFragmentSlotBase);
        mCurrentFragmentAbortExits->push_back(std::move(exit));
        return {ok, false};
}

OwnershipChecker::FlowResult OwnershipChecker::checkLetStmt(LetStmt* let) {
        const size_t loanCount = mLoansInScope.back().size();
        VarInfo* movedSource = nullptr;
        std::optional<Place> movedPlace;
        if (auto* move = dynamic_cast<MoveExpr*>(let->initializer.get())) {
            movedPlace = extractPlace(move->operand.get());
            if (movedPlace) movedSource = lookup(movedPlace->root);
        }
        if (!checkExpr(let->initializer.get())) return false;

        TypePtr type = let->inferredType ? let->inferredType
            : (let->typeAnnotation ? resolveType(let->typeAnnotation.get(), {}) : TyUnknown);
        bool isReference = type && type->kind == TypeKind::Reference;
        bool isMutableReference = isReference && type->isMutable;
        if (let->materializedIteratorOwnsSource) {
            CallExpr* base = nullptr;
            std::function<void(Expr*)> findBase =
                [&](Expr* expression) {
                    auto* call =
                        dynamic_cast<CallExpr*>(expression);
                    if (!call) return;
                    if (call->iteratorOp ==
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
            findBase(let->initializer.get());
            auto* member = base
                ? dynamic_cast<FieldAccessExpr*>(
                      base->callee.get())
                : nullptr;
            auto source = member
                ? extractPlace(member->object.get())
                : std::nullopt;
            auto* sourceVariable = source
                ? lookup(source->root) : nullptr;
            if (sourceVariable &&
                luna::ownership::mustConsume(
                    sourceVariable->usage)) {
                error("materialized iterator recipe cannot hide linear "
                      "source '" + source->root + "'",
                      let->line, let->col);
                return false;
            }
            if (!source ||
                !consume(*source,
                         "owning materialized iterator binding")) {
                if (!source)
                    error("owning materialized iterator requires a local "
                          "source binding",
                          let->line, let->col);
                return false;
            }
        }
        if (let->materializesIteratorRecipe) {
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
            findBase(let->initializer.get());
            if (base &&
                (base->iteratorOp ==
                     IteratorOp::Iter ||
                 base->iteratorOp ==
                     IteratorOp::IterMut)) {
                auto* member =
                    dynamic_cast<FieldAccessExpr*>(
                        base->callee.get());
                auto source = member
                    ? extractPlace(
                          member->object.get())
                    : std::nullopt;
                if (!source ||
                    !acquireLoan(
                        *source,
                        base->iteratorOp ==
                            IteratorOp::IterMut))
                    return false;
            }
        }
        bool isHeap =
            dynamic_cast<HeapAllocExpr*>(
                let->initializer.get()) != nullptr ||
            let->materializedIteratorOwnsSource;
        luna::ownership::Usage usage = let->usageResolved
            ? let->usage
            : (let->hasExplicitUsage
                   ? let->usage : defaultUsageForType(type));
        const auto annotatedUsage = usageFromTypeAST(let->typeAnnotation.get());
        if (!let->usageResolved &&
            annotatedUsage != luna::ownership::Usage::Copy)
            usage = annotatedUsage;
        if (auto* call =
                dynamic_cast<CallExpr*>(let->initializer.get())) {
            if (!let->usageResolved) {
                if (call->returnUsage !=
                    luna::ownership::Usage::Copy)
                    usage = call->returnUsage;
                else if (call->returnsLinear)
                    usage = luna::ownership::Usage::Linear;
            }
            if (luna::ownership::isMoveOnly(
                    call->returnUsage) &&
                typeRequiresCleanup(type))
                isHeap = true;
        }
        if (auto* call = dynamic_cast<CallExpr*>(let->initializer.get())) {
            if (auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
                callee && callee->name == "slice") {
                // slice(borrow array, ...) retains the loan for the lexical
                // lifetime of the view, exactly like a named reference.
                isReference = true;
            }
        }

        if (movedSource) {
            isHeap = isHeap || movedSource->isHeapAllocated;
            usage = let->usageResolved
                ? luna::ownership::strongerUsage(
                      usage, movedSource->usage)
                : movedSource->usage;
            isReference = isReference || movedSource->isReference;
            isMutableReference = isMutableReference || movedSource->isMutableReference;
            if (!type || type == TyUnknown) type = movedSource->type;
        }
        if (auto* borrow = dynamic_cast<BorrowExpr*>(let->initializer.get())) {
            isReference = true;
            isMutableReference = borrow->isMutable;
        }
        if (auto* address = dynamic_cast<AddrOfExpr*>(let->initializer.get())) {
            isReference = true;
            isMutableReference = address->isMutable;
        }

        if (auto* id = dynamic_cast<IdentifierExpr*>(let->initializer.get())) {
            auto* source = lookup(id->name);
            if (source && luna::ownership::isMoveOnly(source->usage)) {
                error(std::string(luna::ownership::usageName(source->usage)) +
                      " variable '" + id->name +
                      "' must be moved explicitly when initializing '" + let->name + "'");
                return false;
            }
        }

        const bool borrowedCallResult = dynamic_cast<CallExpr*>(let->initializer.get()) &&
            usage == luna::ownership::Usage::Copy && typeRequiresCleanup(type);
        const auto relation = isReference
            ? (isMutableReference ? luna::ownership::Relation::MutableBorrow
                                  : luna::ownership::Relation::SharedBorrow)
            : (borrowedCallResult ? luna::ownership::Relation::SharedBorrow
                                  : luna::ownership::Relation::Owned);
        let->usage = usage;
        let->isLinear = usage == luna::ownership::Usage::Linear;
        define(let->name, type, isHeap, usage, relation,
               isReference, isMutableReference);
        if (auto* recipe = lookup(let->name)) {
            recipe->materializedIteratorOwnsSource =
                let->materializedIteratorOwnsSource;
            recipe->materializedIteratorSourceType =
                let->materializedIteratorSourceType;
        }
        if (auto* launch = dynamic_cast<LaunchExpr*>(let->initializer.get())) {
            auto* event = lookup(let->name);
            if (event) {
                event->isGpuEvent = true;
                event->eventResources.clear();
                for (const auto& resource : launch->inFlightResources)
                    event->eventResources.push_back({{resource.first, {}}, resource.second});
            }
        } else if (movedSource && movedSource->isGpuEvent) {
            // Events are linear handles, not mere integers. Moving one must
            // move the in-flight buffer loans as well, so await on the new
            // binding releases exactly the resources launched by the old one.
            auto* event = lookup(let->name);
            if (event) {
                event->isGpuEvent = true;
                event->eventResources = std::move(movedSource->eventResources);
            }
        }
        if (!isReference &&
            !let->materializesIteratorRecipe) {
            while (mLoansInScope.back().size() > loanCount) {
                releaseLoan(mLoansInScope.back().back());
                mLoansInScope.back().pop_back();
            }
        }
        return true;
}

OwnershipChecker::FlowResult OwnershipChecker::checkReturnStmt(ReturnStmt* ret) {
        const size_t loanCount = mLoansInScope.back().size();
        bool ok = true;
        if (ret->value) {
            ok = checkExpr(ret->value.get());
            if (isReferenceExpr(ret->value.get())) {
                error("Reference cannot escape the function through return");
                ok = false;
            }
            if (auto place = extractPlace(ret->value.get())) {
                auto* var = lookup(place->root);
                if (var && var->isGpuEvent) {
                    error("launch event '" + place->root + "' cannot escape; await it before returning",
                          ret->value->line, ret->value->col);
                    ok = false;
                } else if (var && (luna::ownership::isMoveOnly(var->usage) ||
                                   var->isHeapAllocated ||
                                   (var->relation != luna::ownership::Relation::Owned &&
                                    defaultUsageForType(typeOfPlace(*place)) !=
                                        luna::ownership::Usage::Copy))) {
                    // Returning an owned heap value transfers it to the
                    // caller.  Without this transition an automatic cleanup
                    // at the return point would free the returned pointer.
                    ok = consume(*place, "return");
                }
            }
        }
        while (mLoansInScope.back().size() > loanCount) {
            releaseLoan(mLoansInScope.back().back());
            mLoansInScope.back().pop_back();
        }
        if (mCurrentFragmentAbortExits && !mCheckingSlotContinuation) {
            // Returning from a fragment ends only that fragment. Its local
            // resources must be closed, while enclosing function resources
            // remain available to the code after the slot.
            for (size_t index = mCurrentFragmentScopeBase; index < mScopes.size(); ++index) {
                for (const auto& [name, info] : mScopes[index]) {
                    if (luna::ownership::mustConsume(info.usage) &&
                        info.state == OwnState::Valid) {
                        error("Linear variable '" + name +
                              "' must be consumed before returning from the fragment",
                              ret->line, ret->col);
                        ok = false;
                    }
                }
            }
            if (ok) {
                ret->autoFrees = collectFreesAtFragmentExit();
                ret->cleanups.clear();
                for (const auto& place : ret->autoFrees) {
                    auto* variable = lookupCleanupVariable(place);
                ret->cleanups.push_back({
                        place, cleanupActionForType(variable ? variable->type : nullptr),
                        variable ? variable->type : nullptr});
                }
                CheckerState exit = captureState();
                exit.scopes.resize(mCurrentFragmentScopeBase);
                exit.loans.resize(mCurrentFragmentScopeBase);
                exit.applyScopes.resize(mCurrentFragmentApplyBase);
                exit.slotScopes.resize(mCurrentFragmentSlotBase);
                mCurrentFragmentAbortExits->push_back(std::move(exit));
            }
            return {ok, false};
        }
        const size_t errorsBeforeReturnValidation = mErrors.size();
        if (ok) {
            validateLinearReturnPath();
            ret->autoFrees = collectFreesAtReturn();
            ret->cleanups.clear();
            for (const auto& place : ret->autoFrees) {
                auto* variable = lookupCleanupVariable(place);
                ret->cleanups.push_back({
                    place, cleanupActionForType(variable ? variable->type : nullptr),
                    variable ? variable->type : nullptr});
            }
        }
        if (mErrors.size() != errorsBeforeReturnValidation) ok = false;
        return {ok, false};
}

OwnershipChecker::FlowResult OwnershipChecker::checkMatchStmt(MatchStmt* match) {
        if (match->matchedType &&
            luna::ownership::isMoveOnly(
                defaultUsageForType(match->matchedType)) &&
            !dynamic_cast<MoveExpr*>(match->scrutinee.get())) {
            error("match on a move-only value requires explicit `match move ...`",
                  match->line, match->col);
            return false;
        }
        if (!checkExpr(match->scrutinee.get())) return false;

        const CheckerState before = captureState();
        std::optional<CheckerState> merged;
        bool anyFallsThrough = false;
        for (auto& arm : match->arms) {
            restoreState(before);
            enterScope();
            for (size_t index = 0;
                 index < arm.bindings.size() &&
                 index < arm.bindingTypes.size(); ++index) {
                const auto usage =
                    index < arm.bindingUsages.size()
                        ? arm.bindingUsages[index]
                        : defaultUsageForType(
                              arm.bindingTypes[index]);
                define(arm.bindings[index], arm.bindingTypes[index],
                       false, usage);
            }

            FlowResult armResult = checkBlock(arm.body.get());
            if (!armResult.ok) {
                exitScope();
                restoreState(before);
                return false;
            }
            if (armResult.fallsThrough) {
                validateLinearScope();
                if (!mErrors.empty()) {
                    exitScope();
                    restoreState(before);
                    return false;
                }
                for (const auto& name : collectFreesAtScopeExit()) {
                    auto cleanup = std::make_unique<FreeStmt>();
                    cleanup->isImplicit = true;
                    cleanup->operand =
                        std::make_unique<IdentifierExpr>(name);
                    auto* variable = lookup(name);
                    cleanup->action = cleanupActionForType(
                        variable ? variable->type : nullptr);
                    arm.body->stmts.push_back(std::move(cleanup));
                }
            }
            exitScope();
            const CheckerState armState = captureState();

            if (!armResult.fallsThrough) continue;
            anyFallsThrough = true;
            if (!merged) {
                merged = armState;
            } else {
                restoreState(*merged);
                if (!mergeFallthroughStates(
                        before, *merged, armState, match)) {
                    restoreState(before);
                    return false;
                }
                merged = captureState();
            }
        }
        if (anyFallsThrough && merged) {
            restoreState(*merged);
            return {true, true};
        }
        restoreState(before);
        return {true, false};
}

OwnershipChecker::FlowResult OwnershipChecker::checkForStmt(ForStmt* loop) {
        std::optional<Place> consumedMaterializedRecipe;
        bool materializedRecipeOwnsSource = false;
        TypePtr materializedRecipeSourceType;
        std::function<std::optional<Place>(Expr*)>
            materializedRecipe =
                [&](Expr* expression)
                    -> std::optional<Place> {
            if (auto place =
                    extractPlace(expression)) {
                auto* variable =
                    lookup(place->root);
                if (variable &&
                    variable->type &&
                    variable->type->kind ==
                        TypeKind::Iterator)
                    return place;
            }
            auto* call =
                dynamic_cast<CallExpr*>(
                    expression);
            auto* member = call
                ? dynamic_cast<FieldAccessExpr*>(
                      call->callee.get())
                : nullptr;
            return member
                ? materializedRecipe(
                      member->object.get())
                : std::nullopt;
        };
        if (auto recipe =
                materializedRecipe(
                    loop->iterable.get())) {
            auto* variable = lookup(recipe->root);
            consumedMaterializedRecipe = recipe;
            materializedRecipeOwnsSource =
                variable &&
                variable->materializedIteratorOwnsSource;
            materializedRecipeSourceType =
                variable
                    ? variable->
                        materializedIteratorSourceType
                    : nullptr;
            if (!consume(*recipe,
                         "materialized iterator consumption"))
                return false;
        } else if (!loop->recipeStateName.empty()) {
            if (!checkExpr(loop->iterable.get()))
                return false;
            std::function<Expr*(Expr*)>
                consumingSource =
                    [&](Expr* expression) -> Expr* {
                auto* call =
                    dynamic_cast<CallExpr*>(
                        expression);
                if (!call)
                    return expression;
                auto* member =
                    dynamic_cast<FieldAccessExpr*>(
                        call->callee.get());
                if (!member) return nullptr;
                if (call->iteratorOp ==
                    IteratorOp::IntoIter)
                    return member->object.get();
                return consumingSource(
                    member->object.get());
            };
            Expr* sourceExpression =
                consumingSource(
                    loop->iterable.get());
            auto source =
                extractPlace(sourceExpression);
            auto* sourceVariable = source
                ? lookup(source->root) : nullptr;
            if (sourceVariable &&
                luna::ownership::mustConsume(
                    sourceVariable->usage)) {
                error("linear consuming array iterator "
                      "state cannot be hidden from explicit "
                      "consumption",
                      loop->line, loop->col);
                return false;
            }
            if (!source ||
                !consume(*source,
                         "consuming array iteration")) {
                if (!source)
                    error("move-only consuming array iteration "
                          "requires a local source binding",
                          loop->line, loop->col);
                return false;
            }
        } else if (!loop->protocolIntoSymbol.empty()) {
            auto source = extractPlace(
                loop->iterable.get());
            if (!source) {
                error("implicit IntoIterator currently requires a "
                      "local source binding",
                      loop->line, loop->col);
                return false;
            }
            // `for item in source` is the ownership syntax for the Core
            // IntoIterator contract.  The conversion takes source by value;
            // requiring an additional written `move` would defeat the
            // language-level desugaring.
            if (!consume(*source,
                         "implicit IntoIterator conversion"))
                return false;
        } else if (!checkExpr(loop->iterable.get())) {
            return false;
        }
        enterScope();
        // Iterator recipes are ephemeral, but a borrow made by their source
        // must remain live for the complete loop body.
        std::function<CallExpr*(Expr*)> sourceCall = [&](Expr* expression) -> CallExpr* {
            auto* call = dynamic_cast<CallExpr*>(expression);
            if (!call) return nullptr;
            if (call->iteratorOp == IteratorOp::Iter ||
                call->iteratorOp == IteratorOp::IterMut ||
                call->iteratorOp == IteratorOp::IntoIter)
                return call;
            auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
            return member ? sourceCall(member->object.get()) : nullptr;
        };
        bool sourceLoanAcquired = false;
        if (auto* source = sourceCall(loop->iterable.get())) {
            auto* member = dynamic_cast<FieldAccessExpr*>(source->callee.get());
            if (member && (source->iteratorOp == IteratorOp::Iter ||
                           source->iteratorOp == IteratorOp::IterMut)) {
                if (auto place = extractPlace(member->object.get())) {
                    if (!acquireLoan(*place, source->iteratorOp == IteratorOp::IterMut)) {
                        exitScope();
                        return false;
                    }
                    sourceLoanAcquired = true;
                }
            }
        }
        if (!sourceLoanAcquired) {
            if (auto place = extractPlace(loop->iterable.get())) {
                auto* variable = lookup(place->root);
                const bool protocolLoan =
                    !loop->protocolNextSymbol.empty() &&
                    loop->protocolIntoSymbol.empty();
                const bool sliceLoan =
                    variable && variable->type &&
                    variable->type->kind == TypeKind::Slice;
                if (protocolLoan || sliceLoan) {
                    if (!acquireLoan(*place, protocolLoan)) {
                        exitScope();
                        return false;
                    }
                }
            }
        }
        const CheckerState before = captureState();
        if (!loop->recipeStateName.empty()) {
            TypePtr stateType =
                loop->recipeSourceType;
            define(loop->recipeStateName,
                   stateType, true,
                   defaultUsageForType(stateType));
        }
        if (consumedMaterializedRecipe &&
            materializedRecipeOwnsSource) {
            define(
                consumedMaterializedRecipe->root,
                materializedRecipeSourceType,
                true,
                defaultUsageForType(
                    materializedRecipeSourceType));
        }
        if (!loop->protocolIntoSymbol.empty()) {
            TypePtr stateType = loop->protocolIteratorType
                ? loop->protocolIteratorType : TyUnknown;
            const auto stateUsage =
                defaultUsageForType(stateType);
            if (luna::ownership::mustConsume(stateUsage)) {
                error("implicit IntoIterator result has linear type '" +
                      stateType->toString() +
                      "' and cannot be hidden from explicit consumption",
                      loop->line, loop->col);
                exitScope();
                return false;
            }
            define(loop->protocolStateName, stateType,
                   typeRequiresCleanup(stateType),
                   stateUsage);
            auto* state = lookup(
                loop->protocolStateName);
            loop->protocolStateNeedsCleanup =
                state && state->isHeapAllocated;
            if (loop->protocolStateNeedsCleanup)
                loop->protocolStateCleanup =
                    cleanupActionForType(state->type);
        }
        define(loop->varName,
               loop->elementType ? loop->elementType : TyI32,
               false, loop->bindingUsage);
        FlowResult bodyResult = checkBlock(loop->body.get());
        CheckerState after = captureState();
        if (bodyResult.ok && bodyResult.fallsThrough) {
            auto* item = lookup(loop->varName);
            if (item && item->state == OwnState::Valid) {
                if (luna::ownership::mustConsume(item->usage)) {
                    error("Linear iterator item '" + loop->varName +
                          "' must be consumed on every loop iteration",
                          loop->line, loop->col);
                    bodyResult.ok = false;
                } else if (item->isHeapAllocated) {
                    // The binding is reinitialized by every successful
                    // `next`.  Its normal-path cleanup therefore belongs at
                    // the end of the loop body, not after the complete loop.
                    auto cleanup = std::make_unique<FreeStmt>();
                    cleanup->isImplicit = true;
                    cleanup->operand =
                        std::make_unique<IdentifierExpr>(
                            loop->varName);
                    cleanup->action =
                        cleanupActionForType(item->type);
                    loop->body->stmts.push_back(
                        std::move(cleanup));
                }
            }
        }
        // The iteration binding is fresh on every trip and is not an outer
        // loop invariant.  Returning paths already recorded its cleanup;
        // fall-through paths either consumed it or received the cleanup above.
        if (!after.scopes.empty())
            after.scopes.back().erase(loop->varName);
        if (!after.scopes.empty() &&
            !loop->protocolStateName.empty())
            after.scopes.back().erase(
                loop->protocolStateName);
        if (!after.scopes.empty() &&
            !loop->recipeStateName.empty())
            after.scopes.back().erase(
                loop->recipeStateName);
        if (!after.scopes.empty() &&
            consumedMaterializedRecipe &&
            materializedRecipeOwnsSource)
            after.scopes.back().erase(
                consumedMaterializedRecipe->root);
        restoreState(before);
        bool ok = bodyResult.ok;
        if (ok && bodyResult.fallsThrough)
            ok = loopPreservesOuterState(before, after, loop);
        exitScope();
        return {ok, true};
}
