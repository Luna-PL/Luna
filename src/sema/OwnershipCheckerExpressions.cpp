#include "../diagnostics/Diagnostic.h"
#include "OwnershipChecker.h"
#include <algorithm>
#include <functional>
#include <limits>
#include <unordered_set>

bool OwnershipChecker::checkLaunchExpr(LaunchExpr* launch) {
    if (mValidatingManyContinuation) {
        error("a multi-shot fragment cannot launch asynchronous work because its continuation may "
              "be replayed",
              launch->line, launch->col);
        return false;
    }
    for (auto& arg : launch->args) {
        bool isInFlightBorrow = false;
        if (auto* borrow = dynamic_cast<BorrowExpr*>(arg.get())) {
            if (auto* id = dynamic_cast<IdentifierExpr*>(borrow->operand.get())) {
                for (const auto& resource : launch->inFlightResources) {
                    if (resource.first == id->name) {
                        isInFlightBorrow = true;
                        break;
                    }
                }
            }
        }
        if (!isInFlightBorrow && !checkExpr(arg.get())) return false;
    }
    for (const auto& resource : launch->inFlightResources) {
        if (!beginInFlightBorrow(resource.first, resource.second)) return false;
    }
    return true;
}

bool OwnershipChecker::checkCallExpr(CallExpr* call) {
    if (call->iteratorOp != IteratorOp::None) {
        // Method syntax stores the recipe receiver in the field-access
        // callee. It is an expression dependency, not a callable value.
        if (auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get())) {
            if (!checkExpr(member->object.get())) return false;
        } else if (call->iteratorOp == IteratorOp::Range) {
            // `range` has an identifier callee and only its bounds matter.
        } else if (!checkExpr(call->callee.get())) {
            return false;
        }
        if (call->iteratorOp == IteratorOp::Fold && call->iteratorOutputType &&
            luna::ownership::isMoveOnly(defaultUsageForType(call->iteratorOutputType)) &&
            !call->args.empty()) {
            Expr* initial = call->args.front().get();
            auto* transfer = dynamic_cast<MoveExpr*>(initial);
            Expr* sourceExpression = transfer ? transfer->operand.get() : initial;
            auto source = extractPlace(sourceExpression);
            if (source && !transfer) {
                error("move-only fold accumulator '" + renderPlace(*source) +
                          "' must be moved explicitly",
                      call->line, call->col);
                return false;
            }
            auto* sourceVariable = source ? lookup(source->root) : nullptr;
            if (sourceVariable && luna::ownership::mustConsume(sourceVariable->usage)) {
                error("linear fold accumulator cannot "
                      "be hidden in affine replacement "
                      "state",
                      call->line, call->col);
                return false;
            }
        }
        for (auto& arg : call->args)
            if (!checkExpr(arg.get())) return false;
        const bool terminal =
            call->iteratorOp == IteratorOp::Fold || call->iteratorOp == IteratorOp::ForEach ||
            call->iteratorOp == IteratorOp::Count || call->iteratorOp == IteratorOp::Collect;
        if (terminal) {
            std::function<std::optional<Place>(Expr*)> materializedRecipe =
                [&](Expr* expression) -> std::optional<Place> {
                if (auto place = extractPlace(expression)) {
                    auto* variable = lookup(place->root);
                    if (variable && variable->type && variable->type->kind == TypeKind::Iterator)
                        return place;
                }
                auto* nested = dynamic_cast<CallExpr*>(expression);
                auto* nestedMember =
                    nested ? dynamic_cast<FieldAccessExpr*>(nested->callee.get()) : nullptr;
                return nestedMember ? materializedRecipe(nestedMember->object.get()) : std::nullopt;
            };
            auto* terminalMember = dynamic_cast<FieldAccessExpr*>(call->callee.get());
            auto recipe =
                terminalMember ? materializedRecipe(terminalMember->object.get()) : std::nullopt;
            if (recipe && !consume(*recipe, "materialized iterator terminal")) return false;
        }
        if (!call->iteratorRecipeStateName.empty()) {
            std::function<Expr*(Expr*)> consumingSource = [&](Expr* expression) -> Expr* {
                auto* sourceCall = dynamic_cast<CallExpr*>(expression);
                if (!sourceCall) return nullptr;
                auto* sourceMember = dynamic_cast<FieldAccessExpr*>(sourceCall->callee.get());
                if (!sourceMember) return nullptr;
                if (sourceCall->iteratorOp == IteratorOp::IntoIter)
                    return sourceMember->object.get();
                return consumingSource(sourceMember->object.get());
            };
            auto* member = dynamic_cast<FieldAccessExpr*>(call->callee.get());
            Expr* sourceExpression = member ? consumingSource(member->object.get()) : nullptr;
            auto source = extractPlace(sourceExpression);
            auto* sourceVariable = source ? lookup(source->root) : nullptr;
            if (sourceVariable && luna::ownership::mustConsume(sourceVariable->usage)) {
                error("linear iterator terminal state "
                      "cannot be hidden from explicit "
                      "consumption",
                      call->line, call->col);
                return false;
            }
            if (!source || !consume(*source, "move-only iterator terminal")) return false;
        }
        return true;
    }
    if (!checkExpr(call->callee.get())) return false;
    if (auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
        callee &&
        (callee->name == "clone" || callee->name == "is_ok" || callee->name == "is_err")) {
        return call->args.size() == 1 && checkExpr(call->args.front().get());
    }
    SymbolInfo* calleeInfo = nullptr;
    if (auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
        if (mSymTable && !call->resolvedSymbolName.empty())
            calleeInfo = mSymTable->lookupLinkage(call->resolvedSymbolName);
        if (!calleeInfo) calleeInfo = mSymTable ? mSymTable->lookup(callee->name) : nullptr;
    }
    for (size_t index = 0; index < call->args.size(); ++index) {
        auto& arg = call->args[index];
        if (!dynamic_cast<MoveExpr*>(arg.get())) {
            if (auto place = extractPlace(arg.get())) {
                auto* var = lookup(place->root);
                const bool owningParameter =
                    calleeInfo && index < calleeInfo->paramContracts.size() &&
                    calleeInfo->paramContracts[index].relation == luna::ownership::Relation::Owned;
                if (var && luna::ownership::isMoveOnly(var->usage) &&
                    (owningParameter || !calleeInfo)) {
                    error(std::string(luna::ownership::usageName(var->usage)) + " value '" +
                          renderPlace(*place) +
                          "' must be moved explicitly when passed to an owning call");
                    return false;
                }
            }
        }
        if (!checkExpr(arg.get())) return false;
    }
    return true;
}

bool OwnershipChecker::checkVariantConstruct(VariantConstructExpr* variant) {
    const TypeVariant* selected = nullptr;
    if (variant->constructedType) {
        for (const auto& candidate : variant->constructedType->variants) {
            if (candidate.name == variant->variantName) {
                selected = &candidate;
                break;
            }
        }
    }
    for (size_t index = 0; index < variant->args.size(); ++index) {
        auto& arg = variant->args[index];
        if (selected && index < selected->fields.size() &&
            luna::ownership::isMoveOnly(defaultUsageForType(selected->fields[index])) &&
            !dynamic_cast<MoveExpr*>(arg.get())) {
            error("move-only payload for variant '" + variant->variantName +
                  "' must be moved explicitly");
            return false;
        }
        if (!checkExpr(arg.get())) return false;
    }
    return true;
}

bool OwnershipChecker::checkRecordLiteral(RecordLiteralExpr* record) {
    for (auto& field : record->fields) {
        TypePtr fieldType;
        if (record->recordType) {
            for (const auto& candidate : record->recordType->fields) {
                if (candidate.name == field.name) {
                    fieldType = candidate.type;
                    break;
                }
            }
        }
        if (fieldType && luna::ownership::isMoveOnly(defaultUsageForType(fieldType)) &&
            !dynamic_cast<MoveExpr*>(field.value.get())) {
            error("move-only record field '" + field.name + "' must be moved explicitly");
            return false;
        }
        if (!checkExpr(field.value.get())) return false;
    }
    return true;
}

OwnershipChecker::FlowResult OwnershipChecker::checkStmt(Stmt* stmt) {
    setDiagnosticLocation(stmt);
    if (auto* declaration = dynamic_cast<SlotDeclStmt*>(stmt)) {
        if (mSlotScopes.back().count(declaration->name)) {
            error("duplicate slot declaration '" + declaration->name + "'", declaration->line,
                  declaration->col);
            return false;
        }
        mSlotScopes.back()[declaration->name] = declaration;
        return true;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) return checkSlotInvoke(slot);
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        const std::string& fragmentName =
            apply->resolvedFragmentName.empty() ? apply->fragmentName : apply->resolvedFragmentName;
        auto fragment = mFragments.find(fragmentName);
        if (fragment == mFragments.end()) {
            error("unknown fragment '" + apply->fragmentName + "' in apply", apply->line,
                  apply->col);
            return false;
        }
        if (apply->body) {
            mApplyScopes.emplace_back();
            mApplyScopes.back()[apply->slotName] = fragment->second;
            bool ok = checkBlock(apply->body.get()).ok;
            mApplyScopes.pop_back();
            return ok;
        }
        mApplyScopes.back()[apply->slotName] = fragment->second;
        return true;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mCurrentSlotContinuation) {
            error("`resume()` may only appear in an applied fragment", stmt->line, stmt->col);
            return false;
        }
        if (mValidatingManyContinuation) return true;
        const bool savedCheckingContinuation = mCheckingSlotContinuation;
        mCheckingSlotContinuation = true;
        FlowResult continuation = checkBlock(mCurrentSlotContinuation);
        mCheckingSlotContinuation = savedCheckingContinuation;
        return continuation;
    }
    if (auto* abort = dynamic_cast<AbortStmt*>(stmt)) return checkAbortStmt(abort);
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        auto* id = dynamic_cast<IdentifierExpr*>(await->event.get());
        auto* event = id ? lookup(id->name) : nullptr;
        if (!event || !event->isGpuEvent) {
            error("`await` requires a named launch event", await->line, await->col);
            return false;
        }
        if (event->state != OwnState::Valid) {
            error("launch event '" + id->name + "' has already been consumed", id->line, id->col);
            return false;
        }
        finishEvent(event);
        return consume(event, "await");
    }
    if (auto* let = dynamic_cast<LetStmt*>(stmt)) return checkLetStmt(let);
    if (auto* expression = dynamic_cast<ExprStmt*>(stmt)) {
        if (dynamic_cast<LaunchExpr*>(expression->expr.get())) {
            error("a launch event must be bound and awaited; write `let done = launch ...; await "
                  "done;`",
                  expression->line, expression->col);
            return false;
        }
        if (auto* call = dynamic_cast<CallExpr*>(expression->expr.get());
            call && luna::ownership::isMoveOnly(call->returnUsage)) {
            error("owning result of FFI call must be bound to a variable and consumed "
                  "(all move-only results require an explicit owner)",
                  expression->line, expression->col);
            return false;
        }
        const size_t loanCount = mLoansInScope.back().size();
        bool ok = checkExpr(expression->expr.get());
        while (mLoansInScope.back().size() > loanCount) {
            releaseLoan(mLoansInScope.back().back());
            mLoansInScope.back().pop_back();
        }
        return ok;
    }
    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt)) return checkReturnStmt(ret);
    if (auto* conditional = dynamic_cast<IfStmt*>(stmt)) {
        if (!checkExpr(conditional->cond.get())) return false;

        const CheckerState before = captureState();
        FlowResult thenResult = checkBlock(conditional->thenBlock.get());
        const CheckerState thenState = captureState();
        if (!thenResult.ok) {
            restoreState(before);
            return false;
        }

        restoreState(before);
        FlowResult elseResult =
            conditional->elseBranch ? checkStmt(conditional->elseBranch.get()) : FlowResult{};
        const CheckerState elseState = captureState();
        if (!elseResult.ok) {
            restoreState(before);
            return false;
        }

        // Only paths that can reach the next statement participate in the
        // merge.  A returning branch has already been validated at its own
        // scope exit and cannot cause a false conflict after this `if`.
        if (thenResult.fallsThrough && elseResult.fallsThrough) {
            if (!mergeFallthroughStates(before, thenState, elseState, conditional)) return false;
            return {true, true};
        }
        if (thenResult.fallsThrough) {
            restoreState(thenState);
            return {true, true};
        }
        if (elseResult.fallsThrough) {
            restoreState(elseState);
            return {true, true};
        }
        restoreState(before);
        return {true, false};
    }
    if (auto* match = dynamic_cast<MatchStmt*>(stmt)) return checkMatchStmt(match);
    if (auto* loop = dynamic_cast<WhileStmt*>(stmt)) {
        if (!checkExpr(loop->cond.get())) return false;
        const CheckerState before = captureState();
        FlowResult bodyResult = checkBlock(loop->body.get());
        const CheckerState after = captureState();
        restoreState(before);
        if (!bodyResult.ok) return false;
        if (bodyResult.fallsThrough && !loopPreservesOuterState(before, after, loop)) return false;
        return {true, true};
    }
    if (auto* loop = dynamic_cast<ForStmt*>(stmt)) return checkForStmt(loop);
    if (auto* freeStmt = dynamic_cast<FreeStmt*>(stmt)) {
        auto* id = dynamic_cast<IdentifierExpr*>(freeStmt->operand.get());
        auto* var = id ? lookup(id->name) : nullptr;
        if (!var) {
            error("Cannot free undefined variable");
            return false;
        }
        if (!var->isHeapAllocated) {
            error("Cannot free non-heap variable '" + id->name + "'");
            return false;
        }
        if (isDeviceBuffer(var->type)) {
            error("device buffer '" + id->name + "' must be released with `gpu_free(move " +
                      id->name + ")`",
                  id->line, id->col);
            return false;
        }
        if (!consume(var, "free")) return false;
        freeStmt->action = cleanupActionForType(var->type);
        var->state = OwnState::Freed;
        return true;
    }
    if (auto* nested = dynamic_cast<BlockStmt*>(stmt)) return checkBlock(nested);
    return true;
}

bool OwnershipChecker::checkExpr(Expr* expr) {
    if (!expr) return true;
    setDiagnosticLocation(expr);
    if (auto* lambda = dynamic_cast<LambdaExpr*>(expr)) {
        if (!checkLambda(lambda)) return false;
        // Affine/Linear captures move the outer binding into the closure
        // environment (C016 CL010). Copy captures do not consume their
        // source. Consume each move-only capture in the enclosing scope so
        // the outer binding is unavailable after this expression.
        for (const auto& capture : lambda->captures) {
            auto* source = lookup(capture);
            if (!source) continue;
            if (source->usage == luna::ownership::Usage::Copy) continue;
            if (!consume({capture, {}}, "closure capture")) return false;
        }
        return true;
    }
    if (auto* selection = dynamic_cast<SelectExpr*>(expr)) {
        for (auto& arg : selection->selectorArgs) {
            if (!checkExpr(arg.get())) return false;
        }
        return true;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return checkLaunchExpr(launch);
    if (auto* propagation = dynamic_cast<TryExpr*>(expr)) {
        if (auto place = extractPlace(propagation->operand.get())) {
            if (!consume(*place, "error propagation")) return false;
        } else if (!checkExpr(propagation->operand.get())) {
            return false;
        }
        propagation->cleanups.clear();
        for (const auto& place : collectFreesAtReturn()) {
            auto* variable = lookupCleanupVariable(place);
            propagation->cleanups.push_back(
                {place, cleanupActionForType(variable ? variable->type : nullptr),
                 variable ? variable->type : nullptr});
        }
        return true;
    }
    if (auto* move = dynamic_cast<MoveExpr*>(expr)) {
        if (auto place = extractPlace(move->operand.get())) return consume(*place, "move");
        return checkExpr(move->operand.get());
    }
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expr)) {
        if (auto place = extractPlace(borrow->operand.get()))
            return acquireLoan(*place, borrow->isMutable);
        return checkExpr(borrow->operand.get());
    }
    if (auto* address = dynamic_cast<AddrOfExpr*>(expr)) {
        if (auto place = extractPlace(address->operand.get()))
            return acquireLoan(*place, address->isMutable);
        return checkExpr(address->operand.get());
    }
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto* var = lookup(id->name);
        if (!var) {
            return true; // functions are resolved by semantic analysis
        }
        if (!isPlaceAvailable({id->name, {}}, "use")) return false;
        if (var->isGpuEvent) {
            error("launch event '" + id->name + "' must be consumed with `await`", id->line,
                  id->col);
            return false;
        }
        if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
            error("device buffer '" + id->name +
                      "' is in flight; await its launch event before accessing it",
                  id->line, id->col);
            return false;
        }
        return true;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr))
        return checkExpr(binary->lhs.get()) && checkExpr(binary->rhs.get());
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) return checkExpr(unary->operand.get());
    if (auto* call = dynamic_cast<CallExpr*>(expr)) return checkCallExpr(call);
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr))
        return checkVariantConstruct(variant);
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr)) return checkRecordLiteral(record);
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        for (auto& element : array->elements) {
            if (array->elementType &&
                luna::ownership::isMoveOnly(defaultUsageForType(array->elementType)) &&
                !dynamic_cast<MoveExpr*>(element.get())) {
                error("move-only array elements must be moved "
                      "explicitly into the array");
                return false;
            }
            if (!checkExpr(element.get())) return false;
        }
        return true;
    }
    if (auto* assignment = dynamic_cast<AssignExpr*>(expr)) {
        if (!checkExpr(assignment->rhs.get())) return false;
        if (!checkWriteTarget(assignment->lhs.get())) return false;
        return true;
    }
    if (auto* deref = dynamic_cast<DerefExpr*>(expr)) return checkExpr(deref->operand.get());
    if (auto* heap = dynamic_cast<HeapAllocExpr*>(expr)) return checkExpr(heap->initializer.get());
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr))
        return extractPlace(field).has_value() ? isPlaceAvailable(*extractPlace(field), "use")
                                               : checkExpr(field->object.get());
    if (auto* index = dynamic_cast<IndexExpr*>(expr)) {
        if (!checkExpr(index->index.get())) return false;
        if (auto place = extractPlace(index)) return isPlaceAvailable(*place, "use");
        return checkExpr(index->object.get());
    }
    return true;
}
