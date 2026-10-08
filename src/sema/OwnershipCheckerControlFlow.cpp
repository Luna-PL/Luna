#include "OwnershipChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <functional>
#include <algorithm>
#include <limits>
#include <unordered_set>

OwnershipChecker::FlowResult OwnershipChecker::checkBlock(BlockStmt* block) {
    enterScope();
    mApplyScopes.emplace_back();
    FlowResult result;
    for (auto& stmt : block->stmts) {
        result = checkStmt(stmt.get());
        if (!result.ok) {
            releaseLoansInCurrentScope();
            exitScope();
            mApplyScopes.pop_back();
            return {false, result.fallsThrough};
        }
        // Statements after a return are unreachable.  In particular, they
        // must not be treated as another ownership path: doing so produces
        // false use-after-move/free errors and invalid branch merges.
        if (!result.fallsThrough) break;
    }

    // References created in this block cannot be used after the block. End
    // their loans before deciding which owned values may be freed.
    releaseLoansInCurrentScope();
    // A terminating path was validated at its `return` statement across all
    // active scopes.  There is no fall-through state to validate here.
    if (result.fallsThrough) validateLinearScope();
    if (!mErrors.empty()) {
        exitScope();
        mApplyScopes.pop_back();
        return {false, result.fallsThrough};
    }

    std::vector<std::string> frees = collectFreesAtScopeExit();
    std::vector<std::unique_ptr<Stmt>> freeStmts;
    for (const auto& name : frees) {
        auto freeStmt = std::make_unique<FreeStmt>();
        freeStmt->isImplicit = true;
        freeStmt->operand = std::make_unique<IdentifierExpr>(name);
        auto* variable = lookup(name);
        freeStmt->action = cleanupActionForType(
            variable ? variable->type : nullptr);
        if (variable)
            freeStmt->inactiveFields = inactiveCleanupFields(*variable);
        freeStmts.push_back(std::move(freeStmt));
    }

    // Return-path cleanup is recorded on each ReturnStmt while that precise
    // control-flow state is available.  The block cleanup below is only for
    // actual fall-through; appending it after a terminating statement would
    // be unreachable and cannot release a nested-return path.
    if (result.fallsThrough) {
        auto& statements = block->stmts;
        for (const auto& freeStmt : freeStmts) {
            auto* original = dynamic_cast<FreeStmt*>(freeStmt.get());
            auto copy = std::make_unique<FreeStmt>();
            copy->isImplicit = true;
            copy->action = original ? original->action
                                    : luna::ownership::CleanupAction::Deallocate;
            if (original) copy->inactiveFields = original->inactiveFields;
            if (original && original->operand) {
                if (auto* id = dynamic_cast<IdentifierExpr*>(original->operand.get()))
                    copy->operand = std::make_unique<IdentifierExpr>(id->name);
            }
            statements.push_back(std::move(copy));
        }
    }

    exitScope();
    mApplyScopes.pop_back();
    return {mErrors.empty(), result.fallsThrough};
}

OwnershipChecker::FlowResult OwnershipChecker::checkSlotInvoke(SlotInvokeStmt* slot) {
    auto lookupApplied = [this](const std::string& name) -> FragmentDecl* {
        for (auto it = mApplyScopes.rbegin(); it != mApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    };
    FragmentDecl* fragment = lookupApplied(slot->name);

    if (!fragment) {
        // A runtime-selected Fragment may finish without resuming the
        // continuation. Its owner then continues after the Slot with the
        // pre-invocation ownership state, even if the continuation itself
        // always returns. If it does resume and complete normally, both
        // continuing paths must agree on ownership.
        const CheckerState before = captureState();
        FlowResult continuation = checkBlock(slot->continuation.get());
        if (!continuation.ok) {
            restoreState(before);
            return continuation;
        }
        if (!continuation.fallsThrough) {
            restoreState(before);
            return {true, true};
        }
        const CheckerState after = captureState();
        if (!mergeFallthroughStates(before, before, after, slot))
            return {false, true};
        return {true, true};
    }
    return checkFragment(fragment, slot);
}

OwnershipChecker::FlowResult OwnershipChecker::checkFragment(
    FragmentDecl* fragment, SlotInvokeStmt* slot) {
    const CheckerState before = captureState();
    auto* savedContinuation = mCurrentSlotContinuation;
    const bool savedCheckingContinuation = mCheckingSlotContinuation;
    auto* savedFragmentExits = mCurrentFragmentExits;
    const size_t savedScopeBase = mCurrentFragmentScopeBase;
    const size_t savedApplyBase = mCurrentFragmentApplyBase;
    std::vector<CheckerState> exits;
    mCurrentSlotContinuation = slot->continuation.get();
    mCheckingSlotContinuation = false;
    mCurrentFragmentExits = &exits;
    mCurrentFragmentScopeBase = before.scopes.size();
    mCurrentFragmentApplyBase = before.applyScopes.size();

    enterScope();
    for (auto& param : fragment->environmentParams) {
        TypePtr type = param.inferredType ? param.inferredType
                                          : resolveType(param.type.get(), {});
        define(param.name, type, false, luna::ownership::Usage::Copy,
               luna::ownership::Relation::SharedBorrow,
               type && type->kind == TypeKind::Reference,
               false);
    }
    for (auto& param : fragment->params) {
        TypePtr type = param.inferredType ? param.inferredType
                                           : resolveType(param.type.get(), {});
        bool isReference = type && type->kind == TypeKind::Reference;
        const auto usage = param.isLinear ? luna::ownership::Usage::Linear
                                         : param.usage;
        define(param.name, type, typeRequiresCleanup(type), usage,
               param.relation, isReference, isReference && type->isMutable);
    }
    FlowResult body = fragment->body ? checkBlock(fragment->body.get()) : FlowResult{};
    bool ok = body.ok;
    if (ok && body.fallsThrough) {
        CheckerState normal = captureState();
        normal.scopes.resize(before.scopes.size());
        normal.loans.resize(before.loans.size());
        normal.applyScopes.resize(before.applyScopes.size());
        exits.push_back(std::move(normal));
    }
    exitScope();

    mCurrentSlotContinuation = savedContinuation;
    mCheckingSlotContinuation = savedCheckingContinuation;
    mCurrentFragmentExits = savedFragmentExits;
    mCurrentFragmentScopeBase = savedScopeBase;
    mCurrentFragmentApplyBase = savedApplyBase;

    restoreState(before);
    if (!ok) return {false, false};
    // An empty exit set is a valid function-return path when the continuation
    // itself returned. It is not a fallthrough path and must not be merged
    // with ownership state that continues after the slot.
    if (exits.empty()) return {true, false};
    CheckerState merged = exits.front();
    for (size_t index = 1; index < exits.size(); ++index) {
        if (!mergeFallthroughStates(before, merged, exits[index], slot))
            return {false, false};
        merged = captureState();
    }
    restoreState(merged);
    // Fragment completion resumes after the slot invocation. A continuation
    // return, in contrast, leaves no fragment exit and terminates the owner.
    return {true, body.fallsThrough || !exits.empty()};
}

OwnershipChecker::CheckerState OwnershipChecker::captureState() const {
    return {mScopes, mLoansInScope, mApplyScopes};
}

void OwnershipChecker::restoreState(const CheckerState& state) {
    mScopes = state.scopes;
    mLoansInScope = state.loans;
    mApplyScopes = state.applyScopes;
}

bool OwnershipChecker::sameVarState(const VarInfo& left, const VarInfo& right) const {
    if (left.state != right.state ||
        left.sharedBorrows != right.sharedBorrows ||
        left.mutableBorrow != right.mutableBorrow ||
        left.inFlightReads != right.inFlightReads ||
        left.inFlightWrites != right.inFlightWrites ||
        left.isGpuEvent != right.isGpuEvent ||
        left.eventResources.size() != right.eventResources.size())
        return false;
    if (left.movedPlaces.size() != right.movedPlaces.size()) return false;
    std::vector<std::string> leftMoved;
    std::vector<std::string> rightMoved;
    for (const auto& place : left.movedPlaces) leftMoved.push_back(renderPlace(place));
    for (const auto& place : right.movedPlaces) rightMoved.push_back(renderPlace(place));
    std::sort(leftMoved.begin(), leftMoved.end());
    std::sort(rightMoved.begin(), rightMoved.end());
    if (leftMoved != rightMoved) return false;
    for (size_t i = 0; i < left.eventResources.size(); ++i) {
        if (renderPlace(left.eventResources[i].source) !=
                renderPlace(right.eventResources[i].source) ||
            left.eventResources[i].isMutable != right.eventResources[i].isMutable)
            return false;
    }
    return true;
}

bool OwnershipChecker::sameLoanState(const std::vector<Loan>& left,
                                     const std::vector<Loan>& right) const {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (renderPlace(left[i].source) != renderPlace(right[i].source) ||
            left[i].isMutable != right[i].isMutable)
            return false;
    }
    return true;
}

bool OwnershipChecker::sameApplyState(
    const std::vector<std::unordered_map<std::string, FragmentDecl*>>& left,
    const std::vector<std::unordered_map<std::string, FragmentDecl*>>& right) const {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].size() != right[i].size()) return false;
        for (const auto& [name, fragment] : left[i]) {
            auto found = right[i].find(name);
            if (found == right[i].end() || found->second != fragment) return false;
        }
    }
    return true;
}

std::string OwnershipChecker::describeControlFlowDifference(
    const std::string& name, const VarInfo& left, const VarInfo& right,
    const char* construct) const {
    const bool consumedOnOnePath =
        (left.state == OwnState::Valid) != (right.state == OwnState::Valid);
    if (consumedOnOnePath) {
        if (left.isGpuEvent || right.isGpuEvent)
            return "launch event '" + name + "' is awaited or moved on only some paths through `" +
                   construct + "`; every path that continues must leave it in the same state";
        if (luna::ownership::mustConsume(left.usage) ||
            luna::ownership::mustConsume(right.usage))
            return "linear resource '" + name + "' is consumed on only some paths through `" +
                   construct + "`; consume it before the branch or on every path that continues";
        if (left.isHeapAllocated || right.isHeapAllocated)
            return "owned heap value '" + name + "' is freed or moved on only some paths through `" +
                   construct + "`; make ownership consistent before continuing";
    }
    if (left.sharedBorrows != right.sharedBorrows || left.mutableBorrow != right.mutableBorrow)
        return "borrow state of '" + name + "' differs across paths through `" + construct +
               "`; a borrow may not escape only one branch";
    if (left.inFlightReads != right.inFlightReads || left.inFlightWrites != right.inFlightWrites)
        return "in-flight state of device buffer '" + name + "' differs across paths through `" +
               construct + "`; await the launch event on every continuing path";
    return "ownership state of '" + name + "' differs across paths through `" + construct +
           "`; every path that continues must agree";
}

bool OwnershipChecker::mergeFallthroughStates(const CheckerState& before,
                                              const CheckerState& left,
                                              const CheckerState& right,
                                              const ASTNode* controlFlow) {
    const char* construct =
        dynamic_cast<const SlotInvokeStmt*>(controlFlow) ? "slot" : "if";
    if (before.scopes.size() != left.scopes.size() ||
        before.scopes.size() != right.scopes.size()) {
        error("internal ownership-state mismatch while merging control-flow paths",
              controlFlow ? controlFlow->line : 0, controlFlow ? controlFlow->col : 0);
        restoreState(before);
        return false;
    }

    CheckerState merged = left;
    bool ok = true;
    for (size_t i = 0; i < before.scopes.size(); ++i) {
        for (const auto& [name, prior] : before.scopes[i]) {
            auto leftVar = left.scopes[i].find(name);
            auto rightVar = right.scopes[i].find(name);
            if (leftVar == left.scopes[i].end() || rightVar == right.scopes[i].end()) {
                error("ownership binding '" + name + "' is not available on every control-flow path",
                      controlFlow ? controlFlow->line : 0, controlFlow ? controlFlow->col : 0);
                ok = false;
                continue;
            }
            if (!sameVarState(leftVar->second, rightVar->second)) {
                error(describeControlFlowDifference(name, leftVar->second, rightVar->second, construct),
                      controlFlow ? controlFlow->line : 0, controlFlow ? controlFlow->col : 0);
                ok = false;
                continue;
            }
            merged.scopes[i][name] = leftVar->second;
        }
    }

    bool sameLoans = left.loans.size() == right.loans.size();
    if (sameLoans) {
        for (size_t i = 0; i < left.loans.size(); ++i) {
            if (!sameLoanState(left.loans[i], right.loans[i])) {
                sameLoans = false;
                break;
            }
        }
    }
    if (ok && (!sameLoans || !sameApplyState(left.applyScopes, right.applyScopes))) {
        error(std::string("lexical borrow or apply state differs across paths through `") + construct + "`; "
              "such state must remain branch-local", controlFlow ? controlFlow->line : 0,
              controlFlow ? controlFlow->col : 0);
        ok = false;
    }

    if (ok) restoreState(merged);
    else restoreState(before);
    return ok;
}

bool OwnershipChecker::loopPreservesOuterState(const CheckerState& before,
                                               const CheckerState& after,
                                               const ASTNode* loop) {
    if (before.scopes.size() != after.scopes.size()) {
        error("internal ownership-state mismatch while checking loop",
              loop ? loop->line : 0, loop ? loop->col : 0);
        return false;
    }
    bool ok = true;
    for (size_t i = 0; i < before.scopes.size(); ++i) {
        for (const auto& [name, prior] : before.scopes[i]) {
            auto current = after.scopes[i].find(name);
            if (current == after.scopes[i].end() || !sameVarState(prior, current->second)) {
                error("loop body changes ownership, borrow, or in-flight state of '" + name +
                      "'; because a loop may run zero or many times, consume or await outer "
                      "resources outside the loop", loop ? loop->line : 0,
                      loop ? loop->col : 0);
                ok = false;
            }
        }
    }
    bool sameLoans = before.loans.size() == after.loans.size();
    if (sameLoans) {
        for (size_t i = 0; i < before.loans.size(); ++i) {
            if (!sameLoanState(before.loans[i], after.loans[i])) {
                sameLoans = false;
                break;
            }
        }
    }
    if (ok && (!sameLoans || !sameApplyState(before.applyScopes, after.applyScopes))) {
        error("loop body changes a lexical apply binding; make that binding local to the loop body",
              loop ? loop->line : 0, loop ? loop->col : 0);
        ok = false;
    }
    return ok;
}
