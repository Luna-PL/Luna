#include "CodeGenerator.h"

#include <llvm/IR/Constants.h>

namespace {

std::string localName(const moon::LocalRecord& local) {
    return "local." + std::to_string(local.id.value) + "." + local.name;
}

} // namespace

void CodeGenerator::generateControlFlowBody(
    moon::ControlFlowGraph& graph, llvm::Function* func,
    llvm::BasicBlock* abiEntry) {
    mCanonicalLocals.assign(graph.locals.size(), nullptr);
    mCanonicalLocalTypes.assign(graph.locals.size(), nullptr);
    for (const auto& local : graph.locals) {
        TypePtr type = resolveType(local.type);
        llvm::Type* llvmType = type
            ? mHelpers->toLLVMType(type) : nullptr;
        if (!llvmType || llvmType->isVoidTy()) {
            error("canonical local '" + local.name +
                  "' has no storable LLVM type");
            continue;
        }
        auto* storage = createEntryBlockAlloca(
            func, llvmType, localName(local));
        mCanonicalLocals[local.id.value] = storage;
        mCanonicalLocalTypes[local.id.value] = std::move(type);
    }

    size_t parameterIndex = 0;
    for (const auto& local : graph.locals) {
        if (local.kind != moon::LocalKind::Parameter) continue;
        if (parameterIndex >= func->arg_size() ||
            !mCanonicalLocals[local.id.value]) {
            error("canonical parameter table disagrees with its LLVM function");
            ++parameterIndex;
            continue;
        }
        auto* argument = func->getArg(
            static_cast<unsigned>(parameterIndex++));
        mBuilder->CreateStore(
            coerceCallArgument(
                argument,
                mCanonicalLocals[local.id.value]->getAllocatedType()),
            mCanonicalLocals[local.id.value]);
    }
    if (parameterIndex != func->arg_size())
        error("canonical parameter table has the wrong LLVM arity");

    std::vector<llvm::BasicBlock*> blocks;
    blocks.reserve(graph.blocks.size());
    for (const auto& block : graph.blocks)
        blocks.push_back(llvm::BasicBlock::Create(
            *mCtx, "cfg." + std::to_string(block.id.value), func));
    if (graph.entry.empty() || graph.entry.value >= blocks.size()) {
        error("canonical CFG has no LLVM entry target");
        return;
    }
    if (!mBuilder->GetInsertBlock()->getTerminator())
        mBuilder->CreateBr(blocks[graph.entry.value]);

    const auto emitCleanups = [this, &graph](
        const std::vector<moon::CleanupId>& cleanups,
        const std::string& context) {
        for (const auto cleanup : cleanups) {
            const auto* record = graph.findCleanup(cleanup);
            if (!record) {
                error(context + " references no canonical cleanup row");
                continue;
            }
            emitCanonicalCleanup(*record);
        }
    };
    const auto emitEdge = [this, &blocks, &emitCleanups](
        const moon::ControlEdge& edge, const std::string& context) {
        if (edge.target.empty() || edge.target.value >= blocks.size()) {
            error(context + " references no LLVM block");
            return;
        }
        emitCleanups(edge.cleanups, context);
        mBuilder->CreateBr(blocks[edge.target.value]);
    };
    const auto edgeTarget = [this, func, &blocks, &emitCleanups](
        const moon::ControlEdge& edge,
        const std::string& context) -> llvm::BasicBlock* {
        if (edge.target.empty() || edge.target.value >= blocks.size()) {
            error(context + " references no LLVM block");
            return nullptr;
        }
        if (edge.cleanups.empty()) return blocks[edge.target.value];

        const auto saved = mBuilder->saveIP();
        auto* cleanupBlock = llvm::BasicBlock::Create(
            *mCtx, context, func);
        mBuilder->SetInsertPoint(cleanupBlock);
        emitCleanups(edge.cleanups, context);
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(blocks[edge.target.value]);
        mBuilder->restoreIP(saved);
        return cleanupBlock;
    };

    for (auto& block : graph.blocks) {
        mBuilder->SetInsertPoint(blocks[block.id.value]);
        for (auto& operation : block.operations) {
            if (auto* declaration =
                    dynamic_cast<moon::LetStmt*>(operation.get())) {
                if (declaration->local.empty() ||
                    declaration->local.value >= mCanonicalLocals.size() ||
                    !mCanonicalLocals[declaration->local.value]) {
                    error("canonical let has no LLVM local storage");
                    continue;
                }
                llvm::Value* value = generateExpr(
                    declaration->initializer.get());
                if (!value || mBuilder->GetInsertBlock()->getTerminator())
                    continue;
                auto* storage = mCanonicalLocals[declaration->local.value];
                mBuilder->CreateStore(
                    coerceCallArgument(value, storage->getAllocatedType()),
                    storage);
            } else if (auto* expression =
                           dynamic_cast<moon::ExprStmt*>(
                               operation.get())) {
                (void)generateExpr(expression->expr.get());
            } else {
                error("canonical CFG operation is outside the initial LLVM slice");
            }
            if (mBuilder->GetInsertBlock()->getTerminator()) break;
        }
        if (mBuilder->GetInsertBlock()->getTerminator()) continue;

        const auto& terminator = block.terminator;
        switch (terminator.kind) {
            case moon::TerminatorKind::Jump:
                emitEdge(terminator.primary, "canonical jump edge");
                break;
            case moon::TerminatorKind::Branch: {
                llvm::Value* condition = generateExpr(
                    terminator.operand.get());
                auto* primary = edgeTarget(
                    terminator.primary, "cfg.edge.true.cleanup");
                auto* secondary = edgeTarget(
                    terminator.secondary, "cfg.edge.false.cleanup");
                if (!condition || !primary || !secondary) {
                    error("canonical branch has no LLVM condition or target");
                    break;
                }
                mBuilder->CreateCondBr(condition, primary, secondary);
                break;
            }
            case moon::TerminatorKind::Return: {
                llvm::Type* returnType = func->getReturnType();
                if (returnType->isVoidTy()) {
                    if (terminator.operand)
                        (void)generateExpr(terminator.operand.get());
                    emitCleanups(
                        terminator.exitCleanups,
                        "canonical return cleanup");
                    if (!mBuilder->GetInsertBlock()->getTerminator())
                        mBuilder->CreateRetVoid();
                } else {
                    llvm::Value* value = generateExpr(
                        terminator.operand.get());
                    if (!value) {
                        error("canonical non-void return has no value");
                        break;
                    }
                    emitCleanups(
                        terminator.exitCleanups,
                        "canonical return cleanup");
                    mBuilder->CreateRet(
                        coerceCallArgument(value, returnType));
                }
                break;
            }
            case moon::TerminatorKind::Resume:
                emitEdge(terminator.primary, "canonical resume edge");
                break;
            case moon::TerminatorKind::Abort:
                emitEdge(terminator.primary, "canonical abort edge");
                break;
            case moon::TerminatorKind::Unreachable:
                mBuilder->CreateUnreachable();
                break;
            case moon::TerminatorKind::Switch:
                error("canonical switch lowering is not implemented");
                break;
            case moon::TerminatorKind::Invalid:
                error("canonical block has no terminator");
                break;
        }
    }

    mBuilder->SetInsertPoint(abiEntry);
}
