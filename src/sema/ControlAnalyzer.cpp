#include "ControlAnalyzer.h"

#include "SemanticContext.h"
#include "SemanticAnalysisSupport.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include <algorithm>
#include <set>
#include <utility>

void ControlAnalyzer::declareSlot(SlotDecl* decl) {
    const std::string sourceKey = mContext.sourceDeclarationKey(decl->name);
    if (mContext.mSlotScopes.front().count(sourceKey)) {
        mContext.error("duplicate slot declaration '" + decl->name + "'", decl->line, decl->col);
        return;
    }
    TypeVec params;
    std::vector<luna::ownership::Contract> contracts;
    for (auto& param : decl->params) {
        if (!param.type) {
            mContext.error("module-level slot parameter '" + param.name + "' requires a type", decl->line, decl->col);
            params.push_back(TyUnknown);
            contracts.push_back({});
        } else {
            param.inferredType = mContext.declaredType(param.type.get(), {});
            params.push_back(param.inferredType);
            const bool explicitUsage = param.hasExplicitUsage || param.isLinear ||
                dynamic_cast<LinearTypeAST*>(param.type.get()) != nullptr ||
                dynamic_cast<AffineTypeAST*>(param.type.get()) != nullptr;
            const auto requestedUsage = param.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage ? param.usage
                                 : defaultUsageForType(param.inferredType));
            const auto contract = parameterContractFor(
                param.inferredType, requestedUsage, explicitUsage);
            param.relation = contract.relation;
            param.usage = contract.usage;
            contracts.push_back(contract);
        }
    }
    ControlContextAccess::SlotInfo info;
    info.declaration = decl;
    info.name = decl->name;
    info.acceptedKind = decl->acceptedKind;
    info.acceptedCardinality = decl->acceptedCardinality;
    info.paramTypes = params;
    info.paramContracts = contracts;
    for (const auto& param : decl->params) info.paramNames.push_back(param.name);
    info.defaultFragment = decl->defaultFragment;
    info.structuralType = Type::makeSlot(
        params, TyUnit, false,
        info.acceptedKind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        contracts);
    const std::string symbolName = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    info.structuralType->identityMode = luna::types::IdentityMode::Nominal;
    info.structuralType->nominalId = nominalDeclarationIdentity(
        mContext.mProgram, "slot", symbolName, decl);
    info.structuralType->name = decl->name;
    info.structuralType->declarationLinkageName = symbolName;
    decl->structuralType = info.structuralType;
    mContext.mSlotScopes.front().emplace(sourceKey, info);

    SymbolInfo symbol;
    symbol.kind = SymbolKind::Slot;
    symbol.type = decl->structuralType;
    if (!mContext.mSymTable.defineAtRoot(sourceKey, symbol))
        mContext.error("slot name '" + decl->name + "' conflicts with an existing declaration", decl->line, decl->col);
}

void ControlAnalyzer::analyzeSlotDecl(SlotDeclStmt* stmt) {
    mContext.error("local slot declarations are not part of Luna 0.3; declare the slot at module level",
                   stmt->line, stmt->col);
}

void ControlAnalyzer::finalizeSlot(SlotDecl* decl) {
    if (!decl || decl->defaultFragment.empty()) return;
    auto* fragment = selectFragment(decl->defaultFragment, decl);
    if (!fragment) return;
    const std::string slotSymbol = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    if (fragment->resolvedTargetSlotName != slotSymbol) {
        mContext.error("default fragment '" + fragment->name +
            "' nominally targets a different slot than '" + decl->name + "'",
            decl->line, decl->col);
        return;
    }
    decl->resolvedDefaultFragmentName =
        fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
    const std::string key = mContext.sourceDeclarationKey(decl->name);
    auto found = mContext.mSlotScopes.front().find(key);
    if (found != mContext.mSlotScopes.front().end())
        found->second.resolvedDefaultFragmentName =
            decl->resolvedDefaultFragmentName;
}

void ControlAnalyzer::analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    const std::string slotKey = mContext.sourceDeclarationKey(stmt->name);
    ControlContextAccess::SlotInfo* declared = nullptr;
    for (auto it = mContext.mSlotScopes.rbegin();
         it != mContext.mSlotScopes.rend(); ++it) {
        auto found = it->find(slotKey);
        if (found != it->end()) { declared = &found->second; break; }
    }
    if (!declared) {
        mContext.error("unknown module-level slot '" + stmt->name + "'",
                       stmt->line, stmt->col);
        return;
    }
    auto lookupApplied = [this](const std::string& name) -> FragmentDecl* {
        for (auto it = mContext.mApplyScopes.rbegin(); it != mContext.mApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    };
    const auto& active = *declared;
    stmt->acceptedKind = active.acceptedKind;
    stmt->acceptedCardinality = active.acceptedCardinality;
    stmt->structuralType = active.structuralType;
    stmt->resolvedParamNames = active.paramNames;
    stmt->defaultFragment = active.defaultFragment;
    if (stmt->args.size() != active.paramTypes.size()) {
        mContext.error("slot '" + stmt->name + "' expects " +
              std::to_string(active.paramTypes.size()) + " arguments, got " +
              std::to_string(stmt->args.size()), stmt->line, stmt->col);
    }
    const size_t count = std::min(stmt->args.size(), active.paramTypes.size());
    for (size_t i = 0; i < count; ++i)
        mContext.constrain(mContext.analyzeExpr(stmt->args[i].get()),
            active.paramTypes[i], "argument " + std::to_string(i + 1) +
            " of slot '" + stmt->name + "'");
    const auto captures = mContext.mSymTable.visibleSymbols();
    mContext.analyzeBlock(stmt->continuation.get(), expectedReturn);

    FragmentDecl* fragment = lookupApplied(slotKey);
    if (!fragment && !active.defaultFragment.empty()) {
        fragment = selectFragment(active.defaultFragment, stmt);
    }
    if (fragment) {
        stmt->resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
    }
    if (!fragment) return; // no binding is an identity fragment: resume once
    if (fragment->resolvedTargetSlotName !=
        (active.declaration && !active.declaration->generatedSymbolName.empty()
             ? active.declaration->generatedSymbolName : active.name))
        mContext.error("fragment '" + fragment->name +
            "' nominally targets a different slot than '" + stmt->name + "'",
            stmt->line, stmt->col);
    analyzeFragmentForSlot(fragment, stmt->name, active.paramTypes,
                           active.paramContracts, captures);
}

void ControlAnalyzer::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    auto* fragment = selectFragment(stmt->fragmentName, stmt);
    if (!fragment) return;
    stmt->resolvedFragmentName = fragment->generatedSymbolName.empty()
        ? fragment->name : fragment->generatedSymbolName;
    stmt->slotName = fragment->targetSlotName;
    if (stmt->slotName.empty()) {
        mContext.error("fragment '" + fragment->name +
                       "' has no resolved nominal slot target",
                       stmt->line, stmt->col);
        return;
    }
    if (!stmt->body) {
        mContext.error("lexical `apply` requires a body", stmt->line, stmt->col);
        return;
    }
    enterSlotScope();
    const std::string slotKey = mContext.sourceDeclarationKey(
        fragment->targetSlotName);
    mContext.mApplyScopes.back()[slotKey] = fragment;
    mContext.analyzeBlock(stmt->body.get(), expectedReturn);
    exitSlotScope();
}

void ControlAnalyzer::analyzeFragmentForSlot(
    FragmentDecl* fragment, const std::string& slotName, const TypeVec& parameterTypes,
    const std::vector<luna::ownership::Contract>& parameterContracts,
    const std::unordered_map<std::string, SymbolInfo>& captures) {
    if (fragment->params.size() != parameterTypes.size()) {
        mContext.error("fragment '" + fragment->name + "' must bind all " +
              std::to_string(parameterTypes.size()) + " parameters of slot '" +
              slotName + "'");
        return;
    }

    ControlContextAccess::SlotInfo context;
    context.name = slotName;
    context.paramTypes = parameterTypes;
    context.paramContracts = parameterContracts;
    context.structuralType = Type::makeSlot(
        parameterTypes, TyUnit,
        fragment->cardinality == FragmentCardinality::Many,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        parameterContracts);
    const ControlContextAccess::SlotInfo* savedSlot =
        mContext.mCurrentFragmentSlot;
    FragmentDecl* savedFragment = mContext.mCurrentFragmentDecl;
    TypePtr savedReturnType = mContext.mCurrentReturnType;
    mContext.mCurrentFragmentSlot = &context;
    mContext.mCurrentFragmentDecl = fragment;
    mContext.mCurrentReturnType = TyUnit;

    mContext.mSymTable.enterScope();
    for (size_t i = 0; i < fragment->params.size(); ++i) {
        auto& param = fragment->params[i];
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = parameterTypes[i];
        if (param.type) mContext.constrain(mContext.declaredType(param.type.get(), {}), info.type,
                                  "parameter '" + param.name + "' of fragment '" + fragment->name + "'");
        if (i < parameterContracts.size()) {
            const luna::ownership::Contract fragmentContract{
                param.relation, param.usage};
            if (fragmentContract != parameterContracts[i]) {
                mContext.error("parameter '" + param.name + "' of fragment '" +
                      fragment->name + "' has ownership contract " +
                      std::string(luna::ownership::relationName(fragmentContract.relation)) +
                      "/" + std::string(luna::ownership::usageName(fragmentContract.usage)) +
                      ", but slot '" + slotName + "' requires " +
                      std::string(luna::ownership::relationName(
                          parameterContracts[i].relation)) +
                      "/" + std::string(luna::ownership::usageName(
                          parameterContracts[i].usage)));
            }
        }
        param.inferredType = info.type;
        mContext.mSymTable.define(param.name, info);
    }
    for (const auto& [name, info] : captures) {
        if (!mContext.mSymTable.hasInCurrentScope(name)) mContext.mSymTable.define(name, info);
    }
    mContext.analyzeBlock(fragment->body.get(), TyUnit);
    mContext.mSymTable.exitScope();
    mContext.mCurrentReturnType = savedReturnType;

    struct ControlPaths {
        std::set<int> active{0};
        bool aborted = false;
        bool returned = false;
        bool abortAfterResume = false;
    };
    std::function<ControlPaths(const BlockStmt*, const std::set<int>&)> analyzePaths;
    std::function<ControlPaths(const Stmt*, const std::set<int>&)> analyzeStmtPaths;
    auto mergePaths = [](ControlPaths left, const ControlPaths& right) {
        left.active.insert(right.active.begin(), right.active.end());
        left.aborted = left.aborted || right.aborted;
        left.returned = left.returned || right.returned;
        left.abortAfterResume = left.abortAfterResume || right.abortAfterResume;
        return left;
    };
    analyzeStmtPaths = [&](const Stmt* stmt, const std::set<int>& incoming) -> ControlPaths {
        ControlPaths out; out.active = incoming;
        if (!stmt) return out;
        if (dynamic_cast<const ResumeStmt*>(stmt)) {
            out.active.clear();
            for (int count : incoming) out.active.insert(std::min(count + 1, 2));
            return out;
        }
        if (dynamic_cast<const AbortStmt*>(stmt)) {
            for (int count : incoming) if (count > 0) out.abortAfterResume = true;
            out.active.clear(); out.aborted = true; return out;
        }
        if (dynamic_cast<const ReturnStmt*>(stmt)) {
            out.active.clear(); out.returned = true; return out;
        }
        if (auto* block = dynamic_cast<const BlockStmt*>(stmt))
            return analyzePaths(block, incoming);
        if (auto* branch = dynamic_cast<const IfStmt*>(stmt)) {
            ControlPaths thenPaths = analyzePaths(branch->thenBlock.get(), incoming);
            ControlPaths elsePaths;
            elsePaths.active = incoming;
            if (branch->elseBranch) elsePaths = analyzeStmtPaths(branch->elseBranch.get(), incoming);
            return mergePaths(std::move(thenPaths), elsePaths);
        }
        if (auto* match = dynamic_cast<const MatchStmt*>(stmt)) {
            ControlPaths paths;
            paths.active.clear();
            for (const auto& arm : match->arms)
                paths = mergePaths(
                    std::move(paths),
                    analyzePaths(arm.body.get(), incoming));
            return paths;
        }
        if (auto* loop = dynamic_cast<const WhileStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            // A loop may execute zero times or repeat. Any resume in its body
            // therefore makes a once-context path potentially multi-shot.
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        if (auto* loop = dynamic_cast<const ForStmt*>(stmt)) {
            ControlPaths body = analyzePaths(loop->body.get(), incoming);
            out = mergePaths(out, body);
            for (int before : incoming) for (int after : body.active)
                if (after > before) out.active.insert(2);
            return out;
        }
        return out;
    };
    analyzePaths = [&](const BlockStmt* block, const std::set<int>& incoming) {
        ControlPaths paths; paths.active = incoming;
        if (!block) return paths;
        for (const auto& statement : block->stmts) {
            if (paths.active.empty()) break;
            ControlPaths next = analyzeStmtPaths(statement.get(), paths.active);
            paths.active = std::move(next.active);
            paths.aborted = paths.aborted || next.aborted;
            paths.returned = paths.returned || next.returned;
            paths.abortAfterResume = paths.abortAfterResume || next.abortAfterResume;
        }
        return paths;
    };
    const ControlPaths control = analyzePaths(fragment->body.get(), {0});
    const bool isMany = fragment->cardinality == FragmentCardinality::Many;
    if (!isMany && control.abortAfterResume)
        mContext.error("single-shot context '" + fragment->name +
              "' cannot abort after resume(); the continuation has already been consumed");
    if (fragment->kind == FragmentKind::Context) {
        for (int resumes : control.active) {
            if (!isMany && resumes > 1) {
                mContext.error("single-shot context '" + fragment->name +
                      "' may resume its continuation at most once; a path with no `resume()` is an implicit abort");
                break;
            }
        }
    }
    if (isMany) {
        for (const auto& [name, info] : captures) {
            if (info.isLinear) {
                mContext.error("fragment '" + fragment->name + "' may resume slot '" + slotName +
                      "' more than once, but captured linear value '" + name + "' is not replayable");
            }
        }
    }

    mContext.mCurrentFragmentSlot = savedSlot;
    mContext.mCurrentFragmentDecl = savedFragment;
}

void ControlAnalyzer::enterSlotScope() {
    mContext.mSlotScopes.emplace_back();
    mContext.mApplyScopes.emplace_back();
}

void ControlAnalyzer::exitSlotScope() {
    if (mContext.mSlotScopes.size() > 1) mContext.mSlotScopes.pop_back();
    if (mContext.mApplyScopes.size() > 1) mContext.mApplyScopes.pop_back();
}

FragmentDecl* ControlAnalyzer::selectFragment(
    const std::string& name, const ASTNode* useSite) {
    auto fragment = mContext.mFragments.find(mContext.sourceDeclarationKey(name));
    if (fragment != mContext.mFragments.end()) return fragment->second;
    mContext.error("unknown fragment '" + name + "'", useSite->line, useSite->col);
    return nullptr;
}
