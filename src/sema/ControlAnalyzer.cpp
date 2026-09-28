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
            if (decl->isExported &&
                contract.usage != luna::ownership::Usage::Copy)
                mContext.error("exported slot '" + decl->name +
                    "' currently requires Copy parameter contracts; parameter '" +
                    param.name + "' is move-only",
                    decl->line, decl->col);
        }
    }
    ControlContextAccess::SlotInfo info;
    info.declaration = decl;
    info.name = decl->name;
    info.paramTypes = params;
    info.paramContracts = contracts;
    for (const auto& param : decl->params) info.paramNames.push_back(param.name);
    info.structuralType = Type::makeSlot(
        params, TyUnit, contracts);
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

void ControlAnalyzer::finalizeSlot(SlotDecl* decl) {
    (void)decl;
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
    stmt->structuralType = active.structuralType;
    stmt->resolvedParamNames = active.paramNames;
    stmt->resolvedSlotName =
        active.declaration && !active.declaration->generatedSymbolName.empty()
        ? active.declaration->generatedSymbolName : active.name;
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
    mContext.analyzeBlock(stmt->continuation.get(), expectedReturn);

    FragmentDecl* fragment = lookupApplied(slotKey);
    if (fragment) {
        stmt->resolvedFragmentName = fragment->generatedSymbolName.empty()
            ? fragment->name : fragment->generatedSymbolName;
    }
    if (!fragment) return; // no binding is an identity fragment: resume once
    if (fragment->resolvedTargetSlotName !=
        stmt->resolvedSlotName)
        mContext.error("fragment '" + fragment->name +
            "' nominally targets a different slot than '" + stmt->name + "'",
            stmt->line, stmt->col);
    analyzeFragmentForSlot(fragment, stmt->name, active.paramTypes,
                           active.paramContracts);
}

void ControlAnalyzer::analyzeApply(ApplyStmt* stmt, TypePtr expectedReturn) {
    // A local Ref shadows a fragment declaration with the same spelling.
    // Treat it as a borrowed, exact-Slot lexical override, not as a
    // fragment constructor with a second environment argument list.
    auto* operand = mContext.mSymTable.lookup(stmt->fragmentName);
    const auto operandType = operand && operand->kind == SymbolKind::Variable
        ? mContext.resolved(operand->type) : nullptr;
    if (operandType && operandType->kind == TypeKind::RuntimeFragmentRef) {
        stmt->runtimeRefOperand = true;
        if (!stmt->body) {
            mContext.error("lexical `apply` requires a body", stmt->line, stmt->col);
            return;
        }
        if (!stmt->environmentArgs.empty()) {
            mContext.error("RuntimeFragmentRef apply does not accept environment arguments",
                           stmt->line, stmt->col);
            return;
        }
        const auto target = mContext.resolved(operandType->inner);
        ControlContextAccess::SlotInfo* selected = nullptr;
        if (target && target->kind == TypeKind::Slot)
            for (auto& [key, slot] : mContext.mSlotScopes.front()) {
                (void)key;
                if (slot.structuralType &&
                    slot.structuralType->nominalId == target->nominalId &&
                    luna::types::isAbiCompatible(slot.structuralType, target)) {
                    selected = &slot;
                    break;
                }
            }
        if (!selected || !selected->declaration ||
            !selected->declaration->isExported) {
            mContext.error("RuntimeFragmentRef apply requires its exact exported Slot",
                           stmt->line, stmt->col);
            return;
        }
        stmt->slotName = selected->name;
        enterSlotScope();
        // Null deliberately masks an outer static binding. The selected Ref
        // will supply the runtime binding once lowering is implemented.
        mContext.mApplyScopes.back()[
            mContext.sourceDeclarationKey(selected->name)] = nullptr;
        mContext.analyzeBlock(stmt->body.get(), expectedReturn);
        exitSlotScope();
        return;
    }
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
    if (stmt->environmentArgs.size() != fragment->environmentParams.size()) {
        mContext.error("fragment '" + fragment->name + "' expects " +
                       std::to_string(fragment->environmentParams.size()) +
                       " environment arguments, got " +
                       std::to_string(stmt->environmentArgs.size()),
                       stmt->line, stmt->col);
    }
    const size_t environmentCount = std::min(
        stmt->environmentArgs.size(), fragment->environmentParams.size());
    for (size_t index = 0; index < environmentCount; ++index) {
        mContext.constrain(
            mContext.analyzeExpr(stmt->environmentArgs[index].get()),
            fragment->environmentParams[index].inferredType,
            "environment argument " + std::to_string(index + 1) +
                " of fragment '" + fragment->name + "'");
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
    const std::vector<luna::ownership::Contract>& parameterContracts) {
    if (fragment->params.size() != parameterTypes.size()) {
        mContext.error("fragment '" + fragment->name + "' must bind all " +
              std::to_string(parameterTypes.size()) + " parameters of slot '" +
              slotName + "'");
        return;
    }

    if (std::find(mAnalyzingFragments.begin(), mAnalyzingFragments.end(),
                  fragment) != mAnalyzingFragments.end()) {
        mContext.error("recursive static fragment composition re-enters '" +
            fragment->name + "'", fragment->line, fragment->col);
        return;
    }
    mAnalyzingFragments.push_back(fragment);

    ControlContextAccess::SlotInfo context;
    context.name = slotName;
    context.paramTypes = parameterTypes;
    context.paramContracts = parameterContracts;
    context.structuralType = Type::makeSlot(
        parameterTypes, TyUnit, parameterContracts);
    const ControlContextAccess::SlotInfo* savedSlot =
        mContext.mCurrentFragmentSlot;
    FragmentDecl* savedFragment = mContext.mCurrentFragmentDecl;
    TypePtr savedReturnType = mContext.mCurrentReturnType;
    mContext.mCurrentFragmentSlot = &context;
    mContext.mCurrentFragmentDecl = fragment;
    mContext.mCurrentReturnType = TyUnit;

    // A fragment declaration is not a lexical closure. Only module symbols,
    // explicit environment parameters, and Slot invocation parameters are
    // visible in its body.
    mContext.mSymTable.enterIsolatedScope();
    for (auto& parameter : fragment->environmentParams) {
        SymbolInfo info;
        info.kind = SymbolKind::Variable;
        info.type = parameter.inferredType;
        info.usage = luna::ownership::Usage::Copy;
        info.relation = luna::ownership::Relation::SharedBorrow;
        mContext.mSymTable.define(parameter.name, info);
    }
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
    mContext.analyzeBlock(fragment->body.get(), TyUnit);
    mContext.mSymTable.exitIsolatedScope();
    mContext.mCurrentReturnType = savedReturnType;

    struct ControlPaths {
        std::set<int> active{0};
    };
    std::function<ControlPaths(const BlockStmt*, const std::set<int>&)> analyzePaths;
    std::function<ControlPaths(const Stmt*, const std::set<int>&)> analyzeStmtPaths;
    auto mergePaths = [](ControlPaths left, const ControlPaths& right) {
        left.active.insert(right.active.begin(), right.active.end());
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
        if (dynamic_cast<const ReturnStmt*>(stmt)) {
            out.active.clear(); return out;
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
            // therefore makes a single-shot Fragment path potentially resume
            // more than once.
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
        }
        return paths;
    };
    const ControlPaths control = analyzePaths(fragment->body.get(), {0});
    for (int resumes : control.active) {
        if (resumes > 1) {
            mContext.error("single-shot fragment '" + fragment->name +
                  "' may resume its continuation at most once; a path with no `resume;` discards it");
            break;
        }
    }

    mContext.mCurrentFragmentSlot = savedSlot;
    mContext.mCurrentFragmentDecl = savedFragment;
    mAnalyzingFragments.pop_back();
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
