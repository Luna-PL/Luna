#include "OwnershipChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <functional>
#include <algorithm>
#include <unordered_set>

OwnershipChecker::OwnershipChecker() {
    enterScope();
}

bool OwnershipChecker::check(Program* program, SymbolTable& symTable) {
    mSymTable = &symTable;
    mFragments.clear();
    mApplyScopes.clear();
    mSlotScopes.clear();
    mApplyScopes.emplace_back();
    mSlotScopes.emplace_back();
    for (auto& declaration : program->declarations) {
        if (auto* fragment = dynamic_cast<FragmentDecl*>(declaration.get())) {
            mFragments[fragment->name] = fragment;
            if (!fragment->generatedSymbolName.empty())
                mFragments[fragment->generatedSymbolName] = fragment;
        }
    }
    for (auto& declaration : program->declarations) {
        if (auto* function = dynamic_cast<FunctionDecl*>(declaration.get())) {
            if (function->isExtern) continue;
            if (!checkFunction(function)) return false;
        } else if (auto* impl = dynamic_cast<ImplDecl*>(declaration.get())) {
            for (auto& method : impl->methods)
                if (!checkFunction(method.get())) return false;
        }
    }
    return mErrors.empty();
}

bool OwnershipChecker::checkFunction(FunctionDecl* decl) {
    setDiagnosticLocation(decl);
    enterScope();
    mApplyScopes.emplace_back();
    mSlotScopes.emplace_back();

    for (auto& param : decl->params) {
        TypePtr type = param.inferredType ? param.inferredType
                                          : resolveType(param.type.get(), {});
        bool isReference = type && type->kind == TypeKind::Reference;
        // Function parameters are non-owning views by default.  A nominal
        // type lowers to a pointer, but that pointer still belongs to the
        // caller unless the interface explicitly models a linear transfer.
        // Treating every heap-shaped parameter as locally owned caused
        // ordinary calls such as read_x(point) to free the caller's value on
        // return, followed by a second free in the caller.
        const bool explicitUsage = param.hasExplicitUsage || param.isLinear ||
            dynamic_cast<LinearTypeAST*>(param.type.get()) ||
            dynamic_cast<AffineTypeAST*>(param.type.get());
        const auto syntaxUsage = usageFromTypeAST(param.type.get());
        const auto usage = explicitUsage
            ? (param.isLinear ? luna::ownership::Usage::Linear
                              : (syntaxUsage == luna::ownership::Usage::Copy
                                  ? param.usage : syntaxUsage))
            : defaultUsageForType(type);
        const auto contract = parameterContractFor(type, usage, explicitUsage);
        define(param.name, type, false, contract.usage, contract.relation,
               isReference, isReference && type->isMutable);
        // An unqualified heap-shaped parameter is a borrowed view. Explicit
        // affine/linear syntax transfers ownership to the callee, so an
        // affine parameter needs ordinary return-path cleanup and a linear
        // parameter must be consumed before exit.
        mScopes.back()[param.name].isHeapAllocated =
            contract.relation == luna::ownership::Relation::Owned &&
            typeRequiresCleanup(type);
    }

    FlowResult bodyResult;
    if (decl->body) bodyResult = checkBlock(decl->body.get());
    if (!bodyResult.ok) {
        exitScope();
        mApplyScopes.pop_back();
        mSlotScopes.pop_back();
        return false;
    }

    releaseLoansInCurrentScope();
    if (bodyResult.fallsThrough) validateLinearScope();

    if (decl->body && bodyResult.fallsThrough) {
        for (auto& name : collectFreesAtScopeExit()) {
            auto freeStmt = std::make_unique<FreeStmt>();
            freeStmt->isImplicit = true;
            freeStmt->operand = std::make_unique<IdentifierExpr>(name);
            auto* variable = lookup(name);
            freeStmt->action = cleanupActionForType(
                variable ? variable->type : nullptr);
            decl->body->stmts.push_back(std::move(freeStmt));
        }
    }

    exitScope();
    mApplyScopes.pop_back();
    mSlotScopes.pop_back();
    return mErrors.empty();
}

bool OwnershipChecker::checkLambda(LambdaExpr* lambda) {
    setDiagnosticLocation(lambda);
    const size_t errorsBefore = mErrors.size();

    auto savedScopes = std::move(mScopes);
    auto savedLoans = std::move(mLoansInScope);
    auto savedApplyScopes = std::move(mApplyScopes);
    auto savedSlotScopes = std::move(mSlotScopes);
    auto savedUnavailableCaptures =
        std::move(mUnavailableLambdaCaptures);
    auto* savedSlotContinuation = mCurrentSlotContinuation;
    const bool savedManyContinuation =
        mValidatingManyContinuation;
    const bool savedCheckingContinuation =
        mCheckingSlotContinuation;
    auto* savedAbortExits =
        mCurrentFragmentAbortExits;
    const size_t savedFragmentScopeBase =
        mCurrentFragmentScopeBase;
    const size_t savedFragmentApplyBase =
        mCurrentFragmentApplyBase;
    const size_t savedFragmentSlotBase =
        mCurrentFragmentSlotBase;

    mUnavailableLambdaCaptures =
        savedUnavailableCaptures;
    for (const auto& scope : savedScopes)
        for (const auto& [name, _] : scope)
            mUnavailableLambdaCaptures.insert(name);

    mScopes.clear();
    mLoansInScope.clear();
    mApplyScopes.clear();
    mSlotScopes.clear();
    enterScope();
    mApplyScopes.emplace_back();
    mSlotScopes.emplace_back();
    mCurrentSlotContinuation = nullptr;
    mValidatingManyContinuation = false;
    mCheckingSlotContinuation = false;
    mCurrentFragmentAbortExits = nullptr;
    mCurrentFragmentScopeBase = 0;
    mCurrentFragmentApplyBase = 0;
    mCurrentFragmentSlotBase = 0;

    for (auto& param : lambda->params) {
        TypePtr type = param.inferredType
            ? param.inferredType
            : resolveType(param.type.get(), {});
        const bool isReference =
            type && type->kind == TypeKind::Reference;
        const bool explicitUsage =
            param.hasExplicitUsage || param.isLinear ||
            dynamic_cast<LinearTypeAST*>(param.type.get()) ||
            dynamic_cast<AffineTypeAST*>(param.type.get());
        const auto syntaxUsage =
            usageFromTypeAST(param.type.get());
        const auto usage = explicitUsage
            ? (param.isLinear
                   ? luna::ownership::Usage::Linear
                   : (syntaxUsage ==
                              luna::ownership::Usage::Copy
                          ? param.usage
                          : syntaxUsage))
            : defaultUsageForType(type);
        const auto contract =
            parameterContractFor(
                type, usage, explicitUsage);
        define(param.name, type, false,
               contract.usage, contract.relation,
               isReference,
               isReference && type->isMutable);
        mScopes.back()[param.name].isHeapAllocated =
            contract.relation ==
                luna::ownership::Relation::Owned &&
            typeRequiresCleanup(type);
    }

    FlowResult bodyResult;
    if (lambda->body)
        bodyResult = checkBlock(lambda->body.get());

    releaseLoansInCurrentScope();
    if (bodyResult.ok && bodyResult.fallsThrough)
        validateLinearScope();
    if (lambda->body && bodyResult.ok &&
        bodyResult.fallsThrough) {
        for (const auto& name :
             collectFreesAtScopeExit()) {
            auto cleanup =
                std::make_unique<FreeStmt>();
            cleanup->isImplicit = true;
            cleanup->operand =
                std::make_unique<IdentifierExpr>(name);
            auto* variable = lookup(name);
            cleanup->action = cleanupActionForType(
                variable ? variable->type : nullptr);
            lambda->body->stmts.push_back(
                std::move(cleanup));
        }
    }

    exitScope();
    mApplyScopes.pop_back();
    mSlotScopes.pop_back();

    mScopes = std::move(savedScopes);
    mLoansInScope = std::move(savedLoans);
    mApplyScopes = std::move(savedApplyScopes);
    mSlotScopes = std::move(savedSlotScopes);
    mUnavailableLambdaCaptures =
        std::move(savedUnavailableCaptures);
    mCurrentSlotContinuation =
        savedSlotContinuation;
    mValidatingManyContinuation =
        savedManyContinuation;
    mCheckingSlotContinuation =
        savedCheckingContinuation;
    mCurrentFragmentAbortExits =
        savedAbortExits;
    mCurrentFragmentScopeBase =
        savedFragmentScopeBase;
    mCurrentFragmentApplyBase =
        savedFragmentApplyBase;
    mCurrentFragmentSlotBase =
        savedFragmentSlotBase;

    return bodyResult.ok &&
           mErrors.size() == errorsBefore;
}

OwnershipChecker::FlowResult OwnershipChecker::checkBlock(BlockStmt* block) {
    enterScope();
    mApplyScopes.emplace_back();
    mSlotScopes.emplace_back();
    FlowResult result;
    for (auto& stmt : block->stmts) {
        result = checkStmt(stmt.get());
        if (!result.ok) {
            releaseLoansInCurrentScope();
            exitScope();
            mApplyScopes.pop_back();
            mSlotScopes.pop_back();
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
        mSlotScopes.pop_back();
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
            if (original && original->operand) {
                if (auto* id = dynamic_cast<IdentifierExpr*>(original->operand.get()))
                    copy->operand = std::make_unique<IdentifierExpr>(id->name);
            }
            statements.push_back(std::move(copy));
        }
    }

    exitScope();
    mApplyScopes.pop_back();
    mSlotScopes.pop_back();
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
    auto lookupDefault = [this, slot](const std::string& name) -> FragmentDecl* {
        if (!slot->defaultFragment.empty()) {
            auto direct = mFragments.find(slot->defaultFragment);
            if (direct != mFragments.end()) return direct->second;
        }
        for (auto it = mSlotScopes.rbegin(); it != mSlotScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found == it->end() || !found->second || found->second->defaultFragment.empty()) continue;
            auto fragment = mFragments.find(found->second->defaultFragment);
            if (fragment != mFragments.end()) return fragment->second;
        }
        return nullptr;
    };

    FragmentDecl* fragment = lookupApplied(slot->name);
    if (!fragment) fragment = lookupDefault(slot->name);

    if (slot->usesDynamicDispatch) {
        if (slot->resolvedDynamicFragmentNames.empty()) {
            error("dynamic slot '" + slot->name + "' has no resolved fragment candidates",
                  slot->line, slot->col);
            return false;
        }
        const CheckerState before = captureState();
        CheckerState merged;
        bool haveMerged = false;
        bool ok = true;
        for (const auto& name : slot->resolvedDynamicFragmentNames) {
            auto candidate = mFragments.find(name);
            if (candidate == mFragments.end()) {
                error("dynamic slot '" + slot->name + "' references unknown fragment '" + name + "'",
                      slot->line, slot->col);
                ok = false;
                continue;
            }
            const bool multiShot = candidate->second->cardinality == FragmentCardinality::Many;
            if (multiShot) {
                error("dynamic slot '" + slot->name + "' cannot use multi-shot fragment '" +
                      candidate->second->name + "' in the initial runtime ABI", slot->line, slot->col);
                ok = false;
                continue;
            }
            restoreState(before);
            FlowResult candidateResult = checkFragment(candidate->second, slot, multiShot);
            if (!candidateResult.ok) {
                ok = false;
                continue;
            }
            if (!candidateResult.fallsThrough) continue;
            const CheckerState after = captureState();
            if (!haveMerged) {
                merged = after;
                haveMerged = true;
                continue;
            }
            bool same = merged.scopes.size() == after.scopes.size() &&
                        merged.loans.size() == after.loans.size();
            if (same) {
                for (size_t index = 0; index < merged.scopes.size() && same; ++index) {
                    if (merged.scopes[index].size() != after.scopes[index].size()) {
                        same = false;
                        break;
                    }
                    for (const auto& [variable, prior] : merged.scopes[index]) {
                        auto current = after.scopes[index].find(variable);
                        if (current == after.scopes[index].end() || !sameVarState(prior, current->second)) {
                            same = false;
                            break;
                        }
                    }
                    if (index < merged.loans.size() &&
                        !sameLoanState(merged.loans[index], after.loans[index]))
                        same = false;
                }
            }
            if (!same) {
                error("dynamic fragments bound to slot '" + slot->name +
                      "' leave different ownership or borrow states; dynamic alternatives must be effect-compatible",
                      slot->line, slot->col);
                ok = false;
            }
        }
        if (ok && haveMerged) restoreState(merged);
        else restoreState(before);
        return {ok && haveMerged, haveMerged};
    }

    if (!fragment) return checkBlock(slot->continuation.get());

    const bool multiShot = fragment->cardinality == FragmentCardinality::Many;
    if (!multiShot) return checkFragment(fragment, slot, false);

    // Multi-shot continuations are replayed from the same frame. Check one
    // execution on a snapshot, then reject any captured ownership transition.
    const auto savedScopes = mScopes;
    const auto savedLoans = mLoansInScope;
    const size_t errorsBefore = mErrors.size();
    bool continuationOk = checkBlock(slot->continuation.get()).ok;
    const bool consumes = continuationConsumesCapturedState(savedScopes);
    mScopes = savedScopes;
    mLoansInScope = savedLoans;
    if (consumes) {
        error("slot '" + slot->name + "' continuation consumes or frees captured state and cannot be resumed more than once",
              slot->line, slot->col);
        continuationOk = false;
    }
    // Do not emit duplicate diagnostics from the validation-only traversal.
    if (mErrors.size() > errorsBefore && !continuationOk) {
        // Preserve real continuation diagnostics while ensuring the fragment
        // itself is still checked with resume treated as a replay marker.
    }
    FlowResult fragmentResult = checkFragment(fragment, slot, true);
    return {continuationOk && fragmentResult.ok, fragmentResult.fallsThrough};
}

OwnershipChecker::FlowResult OwnershipChecker::checkFragment(
    FragmentDecl* fragment, SlotInvokeStmt* slot, bool multiShot) {
    const CheckerState before = captureState();
    auto* savedContinuation = mCurrentSlotContinuation;
    const bool savedManyValidation = mValidatingManyContinuation;
    const bool savedCheckingContinuation = mCheckingSlotContinuation;
    auto* savedAbortExits = mCurrentFragmentAbortExits;
    const size_t savedScopeBase = mCurrentFragmentScopeBase;
    const size_t savedApplyBase = mCurrentFragmentApplyBase;
    const size_t savedSlotBase = mCurrentFragmentSlotBase;
    std::vector<CheckerState> exits;
    mCurrentSlotContinuation = slot->continuation.get();
    mValidatingManyContinuation = multiShot;
    mCheckingSlotContinuation = false;
    mCurrentFragmentAbortExits = &exits;
    mCurrentFragmentScopeBase = before.scopes.size();
    mCurrentFragmentApplyBase = before.applyScopes.size();
    mCurrentFragmentSlotBase = before.slotScopes.size();

    enterScope();
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
    if (ok && body.fallsThrough && fragment->kind == FragmentKind::Interceptor) {
        FlowResult continuation = checkBlock(slot->continuation.get());
        ok = continuation.ok;
        body.fallsThrough = continuation.fallsThrough;
    }
    if (ok && body.fallsThrough) {
        CheckerState normal = captureState();
        normal.scopes.resize(before.scopes.size());
        normal.loans.resize(before.loans.size());
        normal.applyScopes.resize(before.applyScopes.size());
        normal.slotScopes.resize(before.slotScopes.size());
        exits.push_back(std::move(normal));
    }
    exitScope();

    mCurrentSlotContinuation = savedContinuation;
    mValidatingManyContinuation = savedManyValidation;
    mCheckingSlotContinuation = savedCheckingContinuation;
    mCurrentFragmentAbortExits = savedAbortExits;
    mCurrentFragmentScopeBase = savedScopeBase;
    mCurrentFragmentApplyBase = savedApplyBase;
    mCurrentFragmentSlotBase = savedSlotBase;

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
    // An abort exits the fragment but deliberately resumes the code after
    // the slot invocation. A continuation return, in contrast, leaves no
    // fragment exit and terminates the enclosing function.
    return {true, body.fallsThrough || !exits.empty()};
}

bool OwnershipChecker::continuationConsumesCapturedState(
    const std::vector<std::unordered_map<std::string, VarInfo>>& before) const {
    const size_t scopeCount = std::min(before.size(), mScopes.size());
    for (size_t i = 0; i < scopeCount; ++i) {
        for (const auto& [name, prior] : before[i]) {
            auto current = mScopes[i].find(name);
            if (current == mScopes[i].end()) continue;
            if (prior.state == OwnState::Valid && current->second.state != OwnState::Valid)
                return true;
        }
    }
    return false;
}

OwnershipChecker::CheckerState OwnershipChecker::captureState() const {
    return {mScopes, mLoansInScope, mApplyScopes, mSlotScopes};
}

void OwnershipChecker::restoreState(const CheckerState& state) {
    mScopes = state.scopes;
    mLoansInScope = state.loans;
    mApplyScopes = state.applyScopes;
    mSlotScopes = state.slotScopes;
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

bool OwnershipChecker::sameSlotState(
    const std::vector<std::unordered_map<std::string, SlotDeclStmt*>>& left,
    const std::vector<std::unordered_map<std::string, SlotDeclStmt*>>& right) const {
    if (left.size() != right.size()) return false;
    for (size_t i = 0; i < left.size(); ++i) {
        if (left[i].size() != right[i].size()) return false;
        for (const auto& [name, slot] : left[i]) {
            auto found = right[i].find(name);
            if (found == right[i].end() || found->second != slot) return false;
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
                error(describeControlFlowDifference(name, leftVar->second, rightVar->second, "if"),
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
    if (ok && (!sameLoans || !sameApplyState(left.applyScopes, right.applyScopes) ||
               !sameSlotState(left.slotScopes, right.slotScopes))) {
        error("lexical borrow, slot, or apply state differs across paths through `if`; "
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
    if (ok && (!sameLoans || !sameApplyState(before.applyScopes, after.applyScopes) ||
               !sameSlotState(before.slotScopes, after.slotScopes))) {
        error("loop body changes a lexical slot or apply binding; make that binding local to the loop body",
              loop ? loop->line : 0, loop ? loop->col : 0);
        ok = false;
    }
    return ok;
}

OwnershipChecker::FlowResult OwnershipChecker::checkStmt(Stmt* stmt) {
    setDiagnosticLocation(stmt);
    if (auto* declaration = dynamic_cast<SlotDeclStmt*>(stmt)) {
        if (mSlotScopes.back().count(declaration->name)) {
            error("duplicate slot declaration '" + declaration->name + "'", declaration->line, declaration->col);
            return false;
        }
        mSlotScopes.back()[declaration->name] = declaration;
        return true;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) return checkSlotInvoke(slot);
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        const std::string& fragmentName = apply->resolvedFragmentName.empty()
            ? apply->fragmentName : apply->resolvedFragmentName;
        auto fragment = mFragments.find(fragmentName);
        if (fragment == mFragments.end()) {
            error("unknown fragment '" + apply->fragmentName + "' in apply", apply->line, apply->col);
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
    if (auto* abort = dynamic_cast<AbortStmt*>(stmt)) {
        if (!mCurrentFragmentAbortExits) {
            error("`abort()` may only appear in an applied interceptor or context",
                  stmt->line, stmt->col);
            return false;
        }
        bool ok = true;
        for (size_t index = mCurrentFragmentScopeBase; index < mScopes.size(); ++index) {
            for (const auto& [name, info] : mScopes[index]) {
                if (luna::ownership::mustConsume(info.usage) &&
                    info.state == OwnState::Valid) {
                    error("Linear variable '" + name +
                          "' must be consumed before aborting the fragment",
                          stmt->line, stmt->col);
                    ok = false;
                }
            }
        }
        if (ok) {
            abort->autoFrees = collectFreesAtFragmentExit();
            abort->cleanups.clear();
            for (const auto& place : abort->autoFrees) {
                auto* variable = lookup(place);
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
    if (auto* let = dynamic_cast<LetStmt*>(stmt)) {
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
    if (auto* expression = dynamic_cast<ExprStmt*>(stmt)) {
        if (dynamic_cast<LaunchExpr*>(expression->expr.get())) {
            error("a launch event must be bound and awaited; write `let done = launch ...; await done;`",
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
    if (auto* ret = dynamic_cast<ReturnStmt*>(stmt)) {
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
                              stmt->line, stmt->col);
                        ok = false;
                    }
                }
            }
            if (ok) {
                ret->autoFrees = collectFreesAtFragmentExit();
                ret->cleanups.clear();
                for (const auto& place : ret->autoFrees) {
                    auto* variable = lookup(place);
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
                auto* variable = lookup(place);
                ret->cleanups.push_back({
                    place, cleanupActionForType(variable ? variable->type : nullptr),
                    variable ? variable->type : nullptr});
            }
        }
        if (mErrors.size() != errorsBeforeReturnValidation) ok = false;
        return {ok, false};
    }
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
        FlowResult elseResult = conditional->elseBranch
            ? checkStmt(conditional->elseBranch.get())
            : FlowResult{};
        const CheckerState elseState = captureState();
        if (!elseResult.ok) {
            restoreState(before);
            return false;
        }

        // Only paths that can reach the next statement participate in the
        // merge.  A returning branch has already been validated at its own
        // scope exit and cannot cause a false conflict after this `if`.
        if (thenResult.fallsThrough && elseResult.fallsThrough) {
            if (!mergeFallthroughStates(before, thenState, elseState, conditional))
                return false;
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
    if (auto* match = dynamic_cast<MatchStmt*>(stmt)) {
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
    if (auto* loop = dynamic_cast<WhileStmt*>(stmt)) {
        if (!checkExpr(loop->cond.get())) return false;
        const CheckerState before = captureState();
        FlowResult bodyResult = checkBlock(loop->body.get());
        const CheckerState after = captureState();
        restoreState(before);
        if (!bodyResult.ok) return false;
        if (bodyResult.fallsThrough && !loopPreservesOuterState(before, after, loop))
            return false;
        return {true, true};
    }
    if (auto* loop = dynamic_cast<ForStmt*>(stmt)) {
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
                  id->name + ")`", id->line, id->col);
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
    if (auto* lambda =
            dynamic_cast<LambdaExpr*>(expr))
        return checkLambda(lambda);
    if (auto* selection = dynamic_cast<SelectExpr*>(expr)) {
        for (auto& arg : selection->selectorArgs) {
            if (!checkExpr(arg.get())) return false;
        }
        return true;
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) {
        if (mValidatingManyContinuation) {
            error("a multi-shot fragment cannot launch asynchronous work because its continuation may be replayed",
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
    if (auto* propagation = dynamic_cast<TryExpr*>(expr)) {
        if (auto place = extractPlace(propagation->operand.get())) {
            if (!consume(*place, "error propagation")) return false;
        } else if (!checkExpr(propagation->operand.get())) {
            return false;
        }
        propagation->cleanups.clear();
        for (const auto& place : collectFreesAtReturn()) {
            auto* variable = lookup(place);
            propagation->cleanups.push_back({
                place,
                cleanupActionForType(variable ? variable->type : nullptr),
                variable ? variable->type : nullptr
            });
        }
        return true;
    }
    if (auto* move = dynamic_cast<MoveExpr*>(expr)) {
        if (auto place = extractPlace(move->operand.get()))
            return consume(*place, "move");
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
            if (mUnavailableLambdaCaptures.count(
                    id->name)) {
                error("lambda capture of local '" +
                      id->name +
                      "' is reserved until closure "
                      "environment layout is implemented",
                      id->line, id->col);
                return false;
            }
            return true; // functions are resolved by semantic analysis
        }
        if (!isPlaceAvailable({id->name, {}}, "use")) return false;
        if (var->isGpuEvent) {
            error("launch event '" + id->name + "' must be consumed with `await`", id->line, id->col);
            return false;
        }
        if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
            error("device buffer '" + id->name +
                  "' is in flight; await its launch event before accessing it", id->line, id->col);
            return false;
        }
        return true;
    }
    if (auto* binary = dynamic_cast<BinaryExpr*>(expr))
        return checkExpr(binary->lhs.get()) && checkExpr(binary->rhs.get());
    if (auto* unary = dynamic_cast<UnaryExpr*>(expr))
        return checkExpr(unary->operand.get());
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
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
            if (call->iteratorOp ==
                    IteratorOp::Fold &&
                call->iteratorOutputType &&
                luna::ownership::isMoveOnly(
                    defaultUsageForType(
                        call->iteratorOutputType)) &&
                !call->args.empty()) {
                Expr* initial =
                    call->args.front().get();
                auto* transfer =
                    dynamic_cast<MoveExpr*>(
                        initial);
                Expr* sourceExpression = transfer
                    ? transfer->operand.get()
                    : initial;
                auto source =
                    extractPlace(sourceExpression);
                if (source && !transfer) {
                    error("move-only fold accumulator '" +
                          renderPlace(*source) +
                          "' must be moved explicitly",
                          call->line, call->col);
                    return false;
                }
                auto* sourceVariable = source
                    ? lookup(source->root) : nullptr;
                if (sourceVariable &&
                    luna::ownership::mustConsume(
                        sourceVariable->usage)) {
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
                call->iteratorOp ==
                    IteratorOp::Fold ||
                call->iteratorOp ==
                    IteratorOp::ForEach ||
                call->iteratorOp ==
                    IteratorOp::Count ||
                call->iteratorOp ==
                    IteratorOp::Collect;
            if (terminal) {
                std::function<std::optional<Place>(
                    Expr*)> materializedRecipe =
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
                    auto* nested =
                        dynamic_cast<CallExpr*>(
                            expression);
                    auto* nestedMember = nested
                        ? dynamic_cast<
                              FieldAccessExpr*>(
                              nested->callee.get())
                        : nullptr;
                    return nestedMember
                        ? materializedRecipe(
                              nestedMember->
                                  object.get())
                        : std::nullopt;
                };
                auto* terminalMember =
                    dynamic_cast<FieldAccessExpr*>(
                        call->callee.get());
                auto recipe = terminalMember
                    ? materializedRecipe(
                          terminalMember->
                              object.get())
                    : std::nullopt;
                if (recipe &&
                    !consume(
                        *recipe,
                        "materialized iterator terminal"))
                    return false;
            }
            if (!call->iteratorRecipeStateName.empty()) {
                std::function<Expr*(Expr*)>
                    consumingSource =
                        [&](Expr* expression) -> Expr* {
                    auto* sourceCall =
                        dynamic_cast<CallExpr*>(
                            expression);
                    if (!sourceCall)
                        return nullptr;
                    auto* sourceMember =
                        dynamic_cast<FieldAccessExpr*>(
                            sourceCall->callee.get());
                    if (!sourceMember) return nullptr;
                    if (sourceCall->iteratorOp ==
                        IteratorOp::IntoIter)
                        return sourceMember->object.get();
                    return consumingSource(
                        sourceMember->object.get());
                };
                auto* member =
                    dynamic_cast<FieldAccessExpr*>(
                        call->callee.get());
                Expr* sourceExpression = member
                    ? consumingSource(
                          member->object.get())
                    : nullptr;
                auto source =
                    extractPlace(sourceExpression);
                auto* sourceVariable = source
                    ? lookup(source->root) : nullptr;
                if (sourceVariable &&
                    luna::ownership::mustConsume(
                        sourceVariable->usage)) {
                    error("linear iterator terminal state "
                          "cannot be hidden from explicit "
                          "consumption",
                          call->line, call->col);
                    return false;
                }
                if (!source ||
                    !consume(*source,
                             "move-only iterator terminal"))
                    return false;
            }
            return true;
        }
        if (!checkExpr(call->callee.get())) return false;
        if (auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get());
            callee && (callee->name == "clone" ||
                       callee->name == "is_ok" ||
                       callee->name == "is_err")) {
            return call->args.size() == 1 &&
                   checkExpr(call->args.front().get());
        }
        SymbolInfo* calleeInfo = nullptr;
        if (auto* callee = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            if (mSymTable && !call->resolvedSymbolName.empty())
                calleeInfo = mSymTable->lookupLinkage(call->resolvedSymbolName);
            if (!calleeInfo)
                calleeInfo = mSymTable ? mSymTable->lookup(callee->name) : nullptr;
        }
        for (size_t index = 0; index < call->args.size(); ++index) {
            auto& arg = call->args[index];
            if (!dynamic_cast<MoveExpr*>(arg.get())) {
                if (auto place = extractPlace(arg.get())) {
                    auto* var = lookup(place->root);
                    const bool owningParameter = calleeInfo &&
                        index < calleeInfo->paramContracts.size() &&
                        calleeInfo->paramContracts[index].relation ==
                            luna::ownership::Relation::Owned;
                    if (var && luna::ownership::isMoveOnly(var->usage) &&
                        (owningParameter || !calleeInfo)) {
                        error(std::string(luna::ownership::usageName(var->usage)) +
                              " value '" + renderPlace(*place) +
                              "' must be moved explicitly when passed to an owning call");
                        return false;
                    }
                }
            }
            if (!checkExpr(arg.get())) return false;
        }
        return true;
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr)) {
        const TypeVariant* selected = nullptr;
        if (variant->constructedType) {
            for (const auto& candidate :
                 variant->constructedType->variants) {
                if (candidate.name == variant->variantName) {
                    selected = &candidate;
                    break;
                }
            }
        }
        for (size_t index = 0; index < variant->args.size(); ++index) {
            auto& arg = variant->args[index];
            if (selected && index < selected->fields.size() &&
                luna::ownership::isMoveOnly(
                    defaultUsageForType(selected->fields[index])) &&
                !dynamic_cast<MoveExpr*>(arg.get())) {
                error("move-only payload for variant '" +
                      variant->variantName +
                      "' must be moved explicitly");
                return false;
            }
            if (!checkExpr(arg.get())) return false;
        }
        return true;
    }
    if (auto* record = dynamic_cast<RecordLiteralExpr*>(expr)) {
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
            if (fieldType &&
                luna::ownership::isMoveOnly(
                    defaultUsageForType(fieldType)) &&
                !dynamic_cast<MoveExpr*>(field.value.get())) {
                error("move-only record field '" + field.name +
                      "' must be moved explicitly");
                return false;
            }
            if (!checkExpr(field.value.get())) return false;
        }
        return true;
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        for (auto& element : array->elements) {
            if (array->elementType &&
                luna::ownership::isMoveOnly(
                    defaultUsageForType(
                        array->elementType)) &&
                !dynamic_cast<MoveExpr*>(
                    element.get())) {
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
    if (auto* deref = dynamic_cast<DerefExpr*>(expr))
        return checkExpr(deref->operand.get());
    if (auto* heap = dynamic_cast<HeapAllocExpr*>(expr))
        return checkExpr(heap->initializer.get());
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr))
        return extractPlace(field).has_value()
            ? isPlaceAvailable(*extractPlace(field), "use")
            : checkExpr(field->object.get());
    if (auto* index = dynamic_cast<IndexExpr*>(expr)) {
        if (!checkExpr(index->index.get())) return false;
        if (auto place = extractPlace(index)) return isPlaceAvailable(*place, "use");
        return checkExpr(index->object.get());
    }
    return true;
}

void OwnershipChecker::enterScope() {
    mScopes.emplace_back();
    mLoansInScope.emplace_back();
}

void OwnershipChecker::exitScope() {
    releaseLoansInCurrentScope();
    mLoansInScope.pop_back();
    if (mScopes.size() > 1) mScopes.pop_back();
}

void OwnershipChecker::releaseLoansInCurrentScope() {
    auto& loans = mLoansInScope.back();
    while (!loans.empty()) {
        releaseLoan(loans.back());
        loans.pop_back();
    }
}

std::optional<OwnershipChecker::Place> OwnershipChecker::extractPlace(Expr* expr) const {
    if (!expr) return std::nullopt;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) return Place{id->name, {}};
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr)) {
        auto place = extractPlace(field->object.get());
        if (!place) return std::nullopt;
        place->components.push_back("." + field->field);
        return place;
    }
    if (auto* index = dynamic_cast<IndexExpr*>(expr)) {
        auto place = extractPlace(index->object.get());
        if (!place) return std::nullopt;
        if (auto* literal = dynamic_cast<IntLiteralExpr*>(index->index.get()))
            place->components.push_back("[" + std::to_string(literal->value) + "]");
        else
            place->components.push_back("[*]");
        return place;
    }
    if (auto* dereference = dynamic_cast<DerefExpr*>(expr)) {
        auto place = extractPlace(dereference->operand.get());
        if (!place) return std::nullopt;
        place->components.push_back("*");
        return place;
    }
    return std::nullopt;
}

std::string OwnershipChecker::renderProjection(const Place& place) const {
    std::string result;
    for (const auto& component : place.components) result += component;
    return result;
}

std::string OwnershipChecker::renderPlace(const Place& place) const {
    return place.root + renderProjection(place);
}

bool OwnershipChecker::placesOverlap(const Place& left, const Place& right) const {
    if (left.root != right.root) return false;
    const size_t common = std::min(left.components.size(), right.components.size());
    for (size_t index = 0; index < common; ++index) {
        const auto& a = left.components[index];
        const auto& b = right.components[index];
        const bool wildcard = a == "[*]" || b == "[*]";
        if (a != b && !wildcard) return false;
    }
    return true;
}

bool OwnershipChecker::hasConflictingLoan(const Place& place, bool forMutation) const {
    for (const auto& scope : mLoansInScope) {
        for (const auto& loan : scope) {
            if (!placesOverlap(place, loan.source)) continue;
            if (forMutation || loan.isMutable) return true;
        }
    }
    return false;
}

bool OwnershipChecker::isPlaceAvailable(const Place& place, const std::string& action) {
    auto* var = lookup(place.root);
    if (!var) return true;
    if (var->state == OwnState::Moved) {
        error(action + " after move of '" + renderPlace(place) + "'");
        return false;
    }
    if (var->state == OwnState::Freed) {
        error(action + " after free of '" + renderPlace(place) + "'");
        return false;
    }
    for (const auto& moved : var->movedPlaces) {
        if (placesOverlap(place, moved)) {
            error(action + " of moved place '" + renderPlace(place) + "'");
            return false;
        }
    }
    if (hasConflictingLoan(place, false)) {
        error("Cannot " + action + " '" + renderPlace(place) +
              "' while an overlapping place is mutably borrowed");
        return false;
    }
    return true;
}

bool OwnershipChecker::acquireLoan(const Place& place, bool isMutable) {
    auto* var = lookup(place.root);
    if (!var) {
        error("Borrow of undefined place '" + renderPlace(place) + "'");
        return false;
    }
    if (!isPlaceAvailable(place, "borrow")) return false;
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot borrow device buffer '" + place.root + "' while a launch is in flight");
        return false;
    }
    if (isMutable && var->isReference && !var->isMutableReference) {
        error("Cannot mutably borrow through a shared reference '" + place.root + "'");
        return false;
    }
    if (hasConflictingLoan(place, isMutable)) {
        error("Cannot " + std::string(isMutable ? "mutably " : "") + "borrow '" +
              renderPlace(place) + "' while an overlapping place is borrowed");
        return false;
    }
    if (isMutable) var->mutableBorrow = true;
    else ++var->sharedBorrows;
    mLoansInScope.back().push_back({place, isMutable});
    return true;
}

bool OwnershipChecker::beginInFlightBorrow(const std::string& name, bool isMutable) {
    auto* var = lookup(name);
    if (!var) {
        error("launch borrows undefined device buffer '" + name + "'");
        return false;
    }
    if (!isDeviceBuffer(var->type)) {
        error("launch resource '" + name + "' is not a device buffer");
        return false;
    }
    if (var->state != OwnState::Valid) {
        error("Cannot launch with invalid device buffer '" + name + "'");
        return false;
    }
    if (var->sharedBorrows > 0 || var->mutableBorrow ||
        var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot launch with device buffer '" + name +
              "' while it is borrowed or already in flight");
        return false;
    }
    if (isMutable) ++var->inFlightWrites;
    else ++var->inFlightReads;
    return true;
}

void OwnershipChecker::finishEvent(VarInfo* event) {
    if (!event) return;
    for (const auto& resource : event->eventResources) {
        auto* buffer = lookup(resource.source.root);
        if (!buffer) continue;
        if (resource.isMutable) {
            if (buffer->inFlightWrites > 0) --buffer->inFlightWrites;
        } else if (buffer->inFlightReads > 0) {
            --buffer->inFlightReads;
        }
    }
    event->eventResources.clear();
}

void OwnershipChecker::releaseLoan(const Loan& loan) {
    auto* var = lookup(loan.source.root);
    if (!var) return;
    if (loan.isMutable) var->mutableBorrow = false;
    else if (var->sharedBorrows > 0) --var->sharedBorrows;
}

bool OwnershipChecker::consume(VarInfo* var, const std::string& action) {
    return var && consume(Place{var->name, {}}, action);
}

bool OwnershipChecker::allDirectFieldsMoved(const VarInfo& var) const {
    if (!var.type || var.type->fields.empty()) return false;
    for (const auto& field : var.type->fields) {
        const Place direct{var.name, {"." + field.name}};
        bool covered = false;
        for (const auto& moved : var.movedPlaces) {
            if (moved.components.size() <= direct.components.size() &&
                placesOverlap(direct, moved)) {
                covered = true;
                break;
            }
        }
        if (!covered) return false;
    }
    return true;
}

TypePtr OwnershipChecker::typeOfPlace(const Place& place) const {
    auto* self = const_cast<OwnershipChecker*>(this);
    auto* variable = self->lookup(place.root);
    TypePtr current = variable ? variable->type : nullptr;
    for (const auto& component : place.components) {
        if (!current) return nullptr;
        if (!component.empty() && component.front() == '.') {
            const std::string fieldName = component.substr(1);
            TypePtr next;
            for (const auto& field : current->fields)
                if (field.name == fieldName) { next = field.type; break; }
            current = next;
        } else if (!component.empty() && component.front() == '[') {
            current = current->inner;
        } else if (component == "*") {
            current = current->inner;
        }
    }
    return current;
}

bool OwnershipChecker::consume(const Place& place, const std::string& action) {
    auto* var = lookup(place.root);
    if (!var) {
        error(action + " of undefined place '" + renderPlace(place) + "'");
        return false;
    }
    if (!isPlaceAvailable(place, action)) return false;
    if (!place.components.empty() &&
        defaultUsageForType(typeOfPlace(place)) == luna::ownership::Usage::Copy)
        return true;
    if (!place.components.empty() && var->type &&
        var->type->kind == TypeKind::Record) {
        error("partial move from anonymous record '" + place.root +
              "' is not yet supported; move the whole record");
        return false;
    }
    // A reference binding owns no referent, but its local handle still has a
    // usage contract. Moving that complete handle consumes the binding while
    // the lexical loan remains attached to the source scope. Projections or
    // unqualified borrowed views of heap-shaped values cannot use this path.
    const bool consumesReferenceHandle =
        var->isReference && place.components.empty();
    if (var->relation != luna::ownership::Relation::Owned &&
        !consumesReferenceHandle) {
        error("Cannot " + action + " borrowed place '" + renderPlace(place) +
              "'; declare an owning affine or linear parameter to transfer it");
        return false;
    }
    if (hasConflictingLoan(place, true)) {
        error("Cannot " + action + " '" + renderPlace(place) +
              "' while an overlapping place is borrowed");
        return false;
    }
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot " + action + " device buffer '" + place.root +
              "' while a launch is in flight");
        return false;
    }
    // Moving a Copy value is observationally a copy. Affine and linear values
    // transfer ownership and invalidate precisely the selected place.
    if (var->usage == luna::ownership::Usage::Copy) return true;
    if (place.components.empty()) var->state = OwnState::Moved;
    else {
        var->movedPlaces.push_back(place);
        if (allDirectFieldsMoved(*var)) var->state = OwnState::Moved;
    }
    return true;
}

bool OwnershipChecker::checkWriteTarget(Expr* expr) {
    if (auto* index = dynamic_cast<IndexExpr*>(expr))
        if (!checkExpr(index->index.get())) return false;
    auto place = extractPlace(expr);
    if (!place) return checkExpr(expr);
    auto* var = lookup(place->root);
    if (!var) {
        error("Assignment to undefined place '" + renderPlace(*place) + "'");
        return false;
    }
    if (!isPlaceAvailable(*place, "assignment")) return false;
    if (hasConflictingLoan(*place, true)) {
        error("Cannot assign to '" + place->root +
              "' while it is borrowed (overlapping place '" +
              renderPlace(*place) + "')");
        return false;
    }
    if (var->inFlightReads > 0 || var->inFlightWrites > 0) {
        error("Cannot assign to device buffer '" + place->root + "' while a launch is in flight");
        return false;
    }
    if (luna::ownership::isMoveOnly(var->usage) && place->components.empty()) {
        error("Cannot overwrite " + std::string(luna::ownership::usageName(var->usage)) +
              " variable '" + place->root +
              "' without consuming its current value");
        return false;
    }
    return true;
}

luna::ownership::Usage OwnershipChecker::usageFromTypeAST(const TypeAST* ast) const {
    if (dynamic_cast<const LinearTypeAST*>(ast)) return luna::ownership::Usage::Linear;
    if (dynamic_cast<const AffineTypeAST*>(ast)) return luna::ownership::Usage::Affine;
    return luna::ownership::Usage::Copy;
}

bool OwnershipChecker::isReferenceExpr(Expr* expr) {
    if (dynamic_cast<BorrowExpr*>(expr) || dynamic_cast<AddrOfExpr*>(expr)) return true;
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto* var = lookup(id->name);
        return var && var->isReference;
    }
    return false;
}

OwnershipChecker::VarInfo* OwnershipChecker::lookup(const std::string& name) {
    for (auto it = mScopes.rbegin(); it != mScopes.rend(); ++it) {
        auto found = it->find(name);
        if (found != it->end()) return &found->second;
    }
    return nullptr;
}

void OwnershipChecker::define(const std::string& name, TypePtr type, bool isHeap,
                              luna::ownership::Usage usage,
                              luna::ownership::Relation relation,
                              bool isReference, bool isMutableReference) {
    VarInfo info;
    info.name = name;
    info.type = type;
    info.isHeapAllocated = relation == luna::ownership::Relation::Owned &&
        (isHeap || typeRequiresCleanup(type));
    info.usage = (isDeviceBuffer(type) || isEvent(type))
        ? luna::ownership::Usage::Linear
        : (usage == luna::ownership::Usage::Copy && info.isHeapAllocated
            ? luna::ownership::Usage::Affine : usage);
    info.relation = relation;
    info.isReference = isReference;
    info.isMutableReference = isMutableReference;
    info.isGpuEvent = isEvent(type);
    mScopes.back()[name] = info;
}

std::vector<std::string> OwnershipChecker::collectFreesAtScopeExit() {
    std::vector<std::string> frees;
    for (auto& [name, info] : mScopes.back()) {
        if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
            info.state == OwnState::Valid)
            frees.push_back(name);
    }
    return frees;
}

std::vector<std::string> OwnershipChecker::collectFreesAtReturn() const {
    std::vector<std::string> frees;
    // Exit order is innermost to outermost, matching lexical destruction.
    for (auto scope = mScopes.rbegin(); scope != mScopes.rend(); ++scope) {
        for (const auto& [name, info] : *scope) {
            if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
                info.state == OwnState::Valid)
                frees.push_back(name);
        }
    }
    return frees;
}

std::vector<std::string> OwnershipChecker::collectFreesAtFragmentExit() const {
    std::vector<std::string> frees;
    for (size_t index = mScopes.size(); index > mCurrentFragmentScopeBase; --index) {
        for (const auto& [name, info] : mScopes[index - 1]) {
            if (info.isHeapAllocated && !luna::ownership::mustConsume(info.usage) &&
                info.state == OwnState::Valid)
                frees.push_back(name);
        }
    }
    return frees;
}

void OwnershipChecker::validateLinearScope() {
    // An unawaited event is the root cause for each buffer it still holds in
    // flight. Report that actionable error once instead of adding a second,
    // derivative "not consumed" diagnostic for the same-scope buffer.
    std::unordered_set<std::string> resourcesHeldByUnawaitedEvents;
    for (const auto& [_, info] : mScopes.back()) {
        if (!info.isGpuEvent || info.state != OwnState::Valid) continue;
        for (const auto& resource : info.eventResources)
            resourcesHeldByUnawaitedEvents.insert(resource.source.root);
    }
    for (const auto& [name, info] : mScopes.back()) {
        if (luna::ownership::mustConsume(info.usage) && info.state == OwnState::Valid) {
            if (info.isGpuEvent)
                error("launch event '" + name + "' was not awaited before leaving its scope");
            else if (isDeviceBuffer(info.type) && resourcesHeldByUnawaitedEvents.count(name))
                continue;
            else
                error("Linear variable '" + name + "' was not consumed before leaving its scope");
        }
    }
}

void OwnershipChecker::validateLinearReturnPath() {
    // Returning exits every active lexical scope, not only the innermost
    // block that contains the `return`.  Checking the complete stack here
    // makes an early return as strict as ordinary fall-through scope exit.
    std::unordered_set<std::string> resourcesHeldByUnawaitedEvents;
    for (const auto& scope : mScopes) {
        for (const auto& [_, info] : scope) {
            if (!info.isGpuEvent || info.state != OwnState::Valid) continue;
            for (const auto& resource : info.eventResources)
                resourcesHeldByUnawaitedEvents.insert(resource.source.root);
        }
    }
    for (const auto& scope : mScopes) {
        for (const auto& [name, info] : scope) {
            if (!luna::ownership::mustConsume(info.usage) ||
                info.state != OwnState::Valid) continue;
            if (info.isGpuEvent)
                error("launch event '" + name + "' was not awaited before returning");
            else if (isDeviceBuffer(info.type) && resourcesHeldByUnawaitedEvents.count(name))
                continue;
            else
                error("Linear variable '" + name + "' was not consumed before returning");
        }
    }
}

bool OwnershipChecker::isDeviceBuffer(const TypePtr& type) const {
    return type && type->kind == TypeKind::DeviceBuffer;
}

bool OwnershipChecker::isEvent(const TypePtr& type) const {
    return type && type->kind == TypeKind::Event;
}

void OwnershipChecker::error(const std::string& msg, int line, int col) {
    if (line <= 0) line = mDiagnosticLine;
    if (col <= 0) col = mDiagnosticCol;
    std::string hint;
    if (msg.find("after move") != std::string::npos)
        hint = "use the value before `move`, borrow it instead, or create a replacement value";
    else if (msg.find("in flight") != std::string::npos || msg.find("was not awaited") != std::string::npos)
        hint = "bind the launch result and call `await event` before reusing, moving, or freeing the device buffer";
    else if (msg.find("borrow") != std::string::npos)
        hint = "a value may have many shared borrows or one mutable borrow, but not both";
    else if (msg.find("Linear variable") != std::string::npos)
        hint = "consume it with `move`, pass it to an owning operation, or `free` it when it is heap-allocated";
    else if (msg.find("cannot be resumed more than once") != std::string::npos)
        hint = "make the fragment single-shot, or ensure the continuation does not consume, free, or mutate captured ownership state";
    else if (msg.find("free") != std::string::npos)
        hint = "only heap-allocated values may be explicitly freed";
    mErrors.push_back(diagnostic::format(
        "ownership", msg, mDiagnosticFile, line, col, hint,
        diagnostic::sourceLineFromFile(mDiagnosticFile, line)));
}

void OwnershipChecker::setDiagnosticLocation(const ASTNode* node) {
    if (!node) return;
    if (!node->sourcePath.empty()) mDiagnosticFile = node->sourcePath;
    if (node->line > 0) mDiagnosticLine = node->line;
    if (node->col > 0) mDiagnosticCol = node->col;
}
