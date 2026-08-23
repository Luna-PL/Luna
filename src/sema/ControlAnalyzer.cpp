#include "ControlAnalyzer.h"

#include "SemanticContext.h"
#include "../core/TypeRelations.h"
#include "../parser/AST.h"
#include <algorithm>
#include <set>
#include <utility>

void ControlAnalyzer::analyzeSlotDecl(SlotDeclStmt* stmt) {
    if (mContext.mSlotScopes.back().count(stmt->name)) {
        mContext.error("duplicate slot declaration '" + stmt->name + "'", stmt->line, stmt->col);
        return;
    }
    TypeVec params;
    std::vector<luna::ownership::Contract> contracts;
    for (auto& param : stmt->params) {
        if (!param.type) {
            mContext.error("explicit slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
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
    info.name = stmt->name;
    info.acceptedKind = stmt->acceptedKind;
    info.acceptedCardinality = stmt->acceptedCardinality;
    info.isDynamic = stmt->isDynamic;
    info.paramTypes = params;
    info.paramContracts = contracts;
    for (const auto& param : stmt->params) info.paramNames.push_back(param.name);
    info.defaultFragment = stmt->defaultFragment;
    if (!info.defaultFragment.empty()) {
        if (auto* fragment = selectFragment(info.defaultFragment, stmt)) {
            if (fragment->kind != info.acceptedKind ||
                fragment->cardinality != info.acceptedCardinality)
                mContext.error("default binding for slot '" + stmt->name +
                      "' has the wrong interceptor/context or once/many contract", stmt->line, stmt->col);
            info.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            stmt->resolvedDefaultFragmentName = info.resolvedDefaultFragmentName;
        }
    }
    info.structuralType = Type::makeSlot(
        params, TyUnit, info.acceptedCardinality == FragmentCardinality::Many,
        info.acceptedKind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        contracts);
    stmt->structuralType = info.structuralType;
    mContext.mSlotScopes.back().emplace(stmt->name, info);

    SymbolInfo symbol;
    symbol.kind = SymbolKind::Slot;
    symbol.type = stmt->structuralType;
    if (!mContext.mSymTable.define(stmt->name, symbol))
        mContext.error("slot name '" + stmt->name + "' conflicts with an existing binding", stmt->line, stmt->col);
}

void ControlAnalyzer::analyzeSlotInvoke(SlotInvokeStmt* stmt, TypePtr expectedReturn) {
    if (stmt->isDynamic) {
        mContext.error("dynamic slot must be a separate declaration with an explicit interface; write `dynamic slot name(value: Type);`",
              stmt->line, stmt->col);
        return;
    }
    auto lookupSlot = [this](const std::string& name) -> ControlContextAccess::SlotInfo* {
        for (auto it = mContext.mSlotScopes.rbegin(); it != mContext.mSlotScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };
    auto lookupApplied = [this](const std::string& name) -> FragmentDecl* {
        for (auto it = mContext.mApplyScopes.rbegin(); it != mContext.mApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return found->second;
        }
        return nullptr;
    };
    auto lookupDynamicApplied = [this](const std::string& name) -> const std::vector<FragmentDecl*>* {
        for (auto it = mContext.mDynamicApplyScopes.rbegin(); it != mContext.mDynamicApplyScopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        return nullptr;
    };

    ControlContextAccess::SlotInfo active;
    if (stmt->isImplicitCapture) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.isImplicitCapture = true;
        active.defaultFragment = stmt->defaultFragment;
        active.structuralType = Type::makeSlot(
            {}, TyUnit, active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context);
        if (!mContext.mSlotScopes.back().emplace(stmt->name, active).second) {
            mContext.error("duplicate implicit slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = mContext.mSlotScopes.back().at(stmt->name);
    } else if (!stmt->interfaceParams.empty()) {
        active.name = stmt->name;
        active.acceptedKind = stmt->acceptedKind;
        active.acceptedCardinality = stmt->acceptedCardinality;
        active.defaultFragment = stmt->defaultFragment;
        for (auto& param : stmt->interfaceParams) {
            if (!param.type) {
                mContext.error("inline slot parameter '" + param.name + "' requires a type", stmt->line, stmt->col);
                active.paramTypes.push_back(TyUnknown);
                continue;
            }
            auto parameterType = mContext.declaredType(param.type.get(), {});
            param.inferredType = parameterType;
            active.paramTypes.push_back(parameterType);
            const bool explicitUsage = param.hasExplicitUsage || param.isLinear ||
                dynamic_cast<LinearTypeAST*>(param.type.get()) != nullptr ||
                dynamic_cast<AffineTypeAST*>(param.type.get()) != nullptr;
            const auto requestedUsage = param.isLinear
                ? luna::ownership::Usage::Linear
                : (explicitUsage ? param.usage : defaultUsageForType(parameterType));
            const auto contract = parameterContractFor(
                parameterType, requestedUsage, explicitUsage);
            param.relation = contract.relation;
            param.usage = contract.usage;
            active.paramContracts.push_back(contract);
            active.paramNames.push_back(param.name);
            auto* captured = mContext.mSymTable.lookup(param.name);
            if (!captured) mContext.error("inline slot parameter '" + param.name + "' has no matching local binding");
            else mContext.constrain(captured->type, parameterType,
                           "inline slot parameter '" + param.name + "'");
        }
        active.structuralType = Type::makeSlot(
            active.paramTypes, TyUnit,
            active.acceptedCardinality == FragmentCardinality::Many,
            active.acceptedKind == FragmentKind::Interceptor
                ? ContinuationKind::Interceptor : ContinuationKind::Context,
            active.paramContracts);
        mContext.mSlotScopes.back()[stmt->name] = active;
    } else {
        auto* declared = lookupSlot(stmt->name);
        if (!declared) {
            mContext.error("unknown slot '" + stmt->name + "'", stmt->line, stmt->col);
            return;
        }
        active = *declared;
        stmt->acceptedKind = active.acceptedKind;
        stmt->acceptedCardinality = active.acceptedCardinality;
        if (stmt->args.size() != active.paramTypes.size()) {
            mContext.error("slot '" + stmt->name + "' expects " +
                  std::to_string(active.paramTypes.size()) + " arguments, got " +
                  std::to_string(stmt->args.size()), stmt->line, stmt->col);
        }
        const size_t count = std::min(stmt->args.size(), active.paramTypes.size());
        for (size_t i = 0; i < count; ++i)
            mContext.constrain(mContext.analyzeExpr(stmt->args[i].get()), active.paramTypes[i],
                      "argument " + std::to_string(i + 1) + " of slot '" + stmt->name + "'");
    }

    stmt->structuralType = active.structuralType;
    stmt->resolvedParamNames = active.paramNames;
    const auto captures = mContext.mSymTable.visibleSymbols();
    mContext.analyzeBlock(stmt->continuation.get(), expectedReturn);

    FragmentDecl* fragment = lookupApplied(stmt->name);
    const std::vector<FragmentDecl*>* dynamicFragments = lookupDynamicApplied(stmt->name);
    if (dynamicFragments && !dynamicFragments->empty()) {
        stmt->usesDynamicDispatch = true;
        stmt->resolvedDynamicFragmentNames.clear();
        FragmentDecl* contract = nullptr;
        for (auto* candidate : *dynamicFragments) {
            if (!candidate) continue;
            if (candidate->kind != active.acceptedKind ||
                candidate->cardinality != active.acceptedCardinality)
                mContext.error("dynamic candidate '" + candidate->name +
                      "' does not match slot '" + stmt->name + "' contract",
                      stmt->line, stmt->col);
            if (!contract) contract = candidate;
            else if (candidate->kind != contract->kind ||
                     candidate->cardinality != contract->cardinality) {
                mContext.error("all dynamic candidates must declare the same interceptor/context and once/many contract",
                      stmt->line, stmt->col);
            }
            stmt->resolvedDynamicFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
            analyzeFragmentForSlot(candidate, stmt->name, active.paramTypes,
                                   active.paramContracts, captures);
        }
        return;
    }
    if (!fragment && !active.defaultFragment.empty()) {
        fragment = selectFragment(active.defaultFragment, stmt);
    }
    if (fragment) {
        active.resolvedDefaultFragmentName = fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
        stmt->resolvedDefaultFragmentName = active.resolvedDefaultFragmentName;
    }
    if (!fragment) return; // no binding is an identity fragment: resume once
    if (fragment->kind != active.acceptedKind ||
        fragment->cardinality != active.acceptedCardinality)
        mContext.error("fragment '" + fragment->name + "' does not match slot '" + stmt->name +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    analyzeFragmentForSlot(fragment, stmt->name, active.paramTypes,
                           active.paramContracts, captures);
}

void ControlAnalyzer::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    auto* fragment = selectFragment(stmt->fragmentName, stmt);
    if (!fragment) return;
    stmt->resolvedFragmentName = fragment->generatedSymbolName.empty()
        ? fragment->name : fragment->generatedSymbolName;
    ControlContextAccess::SlotInfo* knownSlot = nullptr;
    for (auto it = mContext.mSlotScopes.rbegin(); it != mContext.mSlotScopes.rend(); ++it) {
        auto found = it->find(stmt->slotName);
        if (found != it->end()) { knownSlot = &found->second; break; }
    }
    auto matchesContract = [](const FragmentDecl* candidate, const ControlContextAccess::SlotInfo* slot) {
        return !slot || (candidate->kind == slot->acceptedKind &&
                         candidate->cardinality == slot->acceptedCardinality);
    };
    if (!matchesContract(fragment, knownSlot))
        mContext.error("fragment '" + fragment->name + "' does not match slot '" + stmt->slotName +
              "' interceptor/context and once/many contract", stmt->line, stmt->col);
    if (stmt->isDynamic) {
        ControlContextAccess::SlotInfo* slot = knownSlot;
        if (!slot) {
            mContext.error("dynamic apply requires a previously declared dynamic slot '" + stmt->slotName + "'",
                  stmt->line, stmt->col);
            return;
        }
        if (!slot->isDynamic) {
            mContext.error("slot '" + stmt->slotName + "' is static; declare it with `dynamic slot` before dynamic apply",
                  stmt->line, stmt->col);
            return;
        }
        auto requireRuntimeCandidate = [this, stmt](const FragmentDecl* candidate) {
            if (candidate && candidate->retention == RetentionKind::CompileTime)
                mContext.error("dynamic apply candidate '" + candidate->name +
                      "' must be declared `runtime` or `dynamic`",
                      stmt->line, stmt->col);
        };
        requireRuntimeCandidate(fragment);
        std::vector<FragmentDecl*> candidates{fragment};
        for (const auto& name : stmt->alternativeFragmentNames) {
            auto* candidate = selectFragment(name, stmt);
            if (!candidate) continue;
            requireRuntimeCandidate(candidate);
            if (!matchesContract(candidate, slot))
                mContext.error("dynamic candidate '" + candidate->name + "' does not match slot '" +
                      stmt->slotName + "' contract", stmt->line, stmt->col);
            candidates.push_back(candidate);
            stmt->resolvedAlternativeFragmentNames.push_back(
                candidate->generatedSymbolName.empty() ? candidate->name : candidate->generatedSymbolName);
        }
        if (stmt->body) {
            enterSlotScope();
            mContext.mApplyScopes.back()[stmt->slotName] = fragment;
            mContext.mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
            mContext.analyzeBlock(stmt->body.get(), expectedReturn);
            exitSlotScope();
        } else {
            mContext.mApplyScopes.back()[stmt->slotName] = fragment;
            mContext.mDynamicApplyScopes.back()[stmt->slotName] = std::move(candidates);
        }
        return;
    }
    if (stmt->body) {
        enterSlotScope();
        mContext.mApplyScopes.back()[stmt->slotName] = fragment;
        mContext.analyzeBlock(stmt->body.get(), expectedReturn);
        exitSlotScope();
    } else {
        mContext.mApplyScopes.back()[stmt->slotName] = fragment;
    }
}

void ControlAnalyzer::analyzeFragmentForSlot(
    FragmentDecl* fragment, const std::string& slotName, const TypeVec& parameterTypes,
    const std::vector<luna::ownership::Contract>& parameterContracts,
    const std::unordered_map<std::string, SymbolInfo>& captures) {
    if (fragment->params.size() > parameterTypes.size()) {
        mContext.error("fragment '" + fragment->name + "' requires " +
              std::to_string(fragment->params.size()) + " parameters, but slot '" + slotName +
              " provides " + std::to_string(parameterTypes.size()));
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
    // A fragment may leave parameter types implicit until it is bound to a
    // typed slot. Rebuild its own declaration shape from the now-resolved
    // prefix rather than copying the entire slot interface into the fragment.
    TypeVec fragmentParameterTypes;
    std::vector<luna::ownership::Contract> fragmentParameterContracts;
    for (const auto& parameter : fragment->params) {
        fragmentParameterTypes.push_back(parameter.inferredType);
        fragmentParameterContracts.push_back(
            {parameter.relation, parameter.usage});
    }
    fragment->structuralType = Type::makeFragment(
        std::move(fragmentParameterTypes), TyUnit, isMany,
        fragment->kind == FragmentKind::Interceptor
            ? ContinuationKind::Interceptor : ContinuationKind::Context,
        std::move(fragmentParameterContracts));
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
    mContext.mDynamicApplyScopes.emplace_back();
}

void ControlAnalyzer::exitSlotScope() {
    if (mContext.mSlotScopes.size() > 1) mContext.mSlotScopes.pop_back();
    if (mContext.mApplyScopes.size() > 1) mContext.mApplyScopes.pop_back();
    if (mContext.mDynamicApplyScopes.size() > 1) mContext.mDynamicApplyScopes.pop_back();
}

FragmentDecl* ControlAnalyzer::selectFragment(
    const std::string& name, const ASTNode* useSite) {
    auto fragment = mContext.mFragments.find(mContext.sourceDeclarationKey(name));
    if (fragment != mContext.mFragments.end()) return fragment->second;
    mContext.error("unknown fragment '" + name + "'", useSite->line, useSite->col);
    return nullptr;
}
