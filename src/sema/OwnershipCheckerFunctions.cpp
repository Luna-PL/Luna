#include "OwnershipChecker.h"
#include "../diagnostics/Diagnostic.h"
#include <functional>
#include <algorithm>
#include <limits>
#include <unordered_set>

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

    mUnavailableLambdaCaptures = savedUnavailableCaptures;
    for (const auto& scope : savedScopes)
        for (const auto& [name, info] : scope)
            mUnavailableLambdaCaptures[name] = info.usage;

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

    // Captures become owned locals inside the lambda scope. Copy captures
    // are value copies (C016 CL003); Affine/Linear captures are moved into
    // the environment, so they keep their outer usage and cleanup obligation
    // (C016 CL010). Borrowed (Reference) captures remain a later slice.
    bool capturesValid = true;
    for (const auto& capture : lambda->captures) {
        const auto found = mUnavailableLambdaCaptures.find(capture);
        if (found == mUnavailableLambdaCaptures.end()) continue;
        VarInfo info;
        info.name = capture;
        info.usage = found->second;
        info.relation = luna::ownership::Relation::Owned;
        for (auto it = savedScopes.rbegin();
             it != savedScopes.rend(); ++it) {
            const auto foundOuter = it->find(capture);
            if (foundOuter != it->end()) {
                info.type = foundOuter->second.type;
                info.isHeapAllocated = foundOuter->second.isHeapAllocated;
                if (info.type &&
                    info.type->kind == TypeKind::Reference) {
                    error("lambda capture of borrowed binding '" +
                          capture + "' is not yet supported "
                          "(C016 CL005)",
                          lambda->line, lambda->col);
                    capturesValid = false;
                }
                break;
            }
        }
        mScopes.back()[capture] = std::move(info);
    }

    FlowResult bodyResult;
    if (capturesValid && lambda->body)
        bodyResult = checkBlock(lambda->body.get());

    releaseLoansInCurrentScope();
    if (capturesValid && bodyResult.ok && bodyResult.fallsThrough)
        validateLinearScope();
    if (capturesValid && lambda->body && bodyResult.ok &&
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

    return capturesValid && bodyResult.ok &&
           mErrors.size() == errorsBefore;
}
