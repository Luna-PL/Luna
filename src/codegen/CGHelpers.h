#pragma once

#include "../core/TypeSystem.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <unordered_map>
#include <string>

class CGHelpers {
public:
    explicit CGHelpers(llvm::LLVMContext& ctx);

    llvm::Type* toLLVMType(const TypePtr& type) const;
    llvm::Type* i32Ty() const { return llvm::Type::getInt32Ty(mCtx); }
    llvm::Type* i64Ty() const { return llvm::Type::getInt64Ty(mCtx); }
    llvm::Type* f32Ty() const { return llvm::Type::getFloatTy(mCtx); }
    llvm::Type* f64Ty() const { return llvm::Type::getDoubleTy(mCtx); }
    llvm::Type* boolTy() const { return llvm::Type::getInt1Ty(mCtx); }
    llvm::Type* voidTy() const { return llvm::Type::getVoidTy(mCtx); }
    llvm::Type* ptrTy() const { return llvm::PointerType::get(mCtx, 0); }
    llvm::Type* sizeTy() const { return llvm::Type::getInt64Ty(mCtx); }

    llvm::LLVMContext& context() { return mCtx; }

private:
    llvm::LLVMContext& mCtx;
};

// Map a Luna type kind to its size in bytes
uint64_t typeSize(const TypePtr& type);
