#include "CodeGenerator.h"
#include "../diagnostics/Diagnostic.h"

// ─── Shared helpers ─────────────────────────────────────────────────

llvm::Value* CodeGenerator::coerceCallArgument(llvm::Value* value, llvm::Type* target) {
    if (!value || !target || value->getType() == target) return value;
    if (value->getType()->isIntegerTy() && target->isIntegerTy())
        return mBuilder->CreateIntCast(value, target, true, "abiarg");
    if (value->getType()->isPointerTy() && target->isPointerTy())
        return mBuilder->CreateBitCast(value, target, "abiarg");
    return value;
}

TypePtr CodeGenerator::resolveType(const moon::TypeRef& reference) {
    return mTypeMaterializer
        ? mTypeMaterializer->materialize(reference) : nullptr;
}

TypePtr CodeGenerator::allocationTypeForExpr(moon::Expr* expr) {
    if (!expr) return nullptr;
    if (auto* move = dynamic_cast<moon::MoveExpr*>(expr))
        return allocationTypeForExpr(move->operand.get());
    if (auto* borrow = dynamic_cast<moon::BorrowExpr*>(expr))
        return allocationTypeForExpr(borrow->operand.get());
    if (auto* id = dynamic_cast<moon::IdentifierExpr*>(expr)) {
        auto found = mLocalTypes.find(id->name);
        if (found != mLocalTypes.end()) return found->second;
    }
    return resolveType(expr->type);
}

llvm::AllocaInst* CodeGenerator::createEntryBlockAlloca(
    llvm::Function* func, llvm::Type* type, const std::string& name) {
    llvm::IRBuilder<> tmpBuilder(&func->getEntryBlock(), func->getEntryBlock().begin());
    return tmpBuilder.CreateAlloca(type, nullptr, name);
}

void CodeGenerator::error(const std::string& msg) {
    mErrors.push_back(diagnostic::format("codegen", msg, "", 0, 0,
                                         "this is usually caused by an earlier invalid declaration or unsupported construct"));
}

size_t CodeGenerator::fieldIndex(const TypePtr& type, const std::string& field) const {
    if (!type) return static_cast<size_t>(-1);
    for (size_t i = 0; i < type->fields.size(); ++i)
        if (type->fields[i].name == field) return i;
    return static_cast<size_t>(-1);
}
