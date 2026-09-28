#pragma once

#include "../core/TypeSystem.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Type.h>
#include <unordered_map>
#include <string>

namespace moon { struct DeclarationRef; }

enum class RuntimeFragmentRefIngressMode { Borrowed, Owned };

class CGHelpers {
public:
    explicit CGHelpers(llvm::LLVMContext& ctx);

    llvm::Type* toLLVMType(const TypePtr& type) const;
    llvm::CallInst* emitRuntimeFragmentRefDrop(
        llvm::IRBuilder<>& builder, llvm::Module& module,
        llvm::Value* carrierCell) const;
    llvm::Value* emitRuntimeFragmentRefTake(
        llvm::IRBuilder<>& builder, llvm::Value* carrierCell) const;
    llvm::CallInst* emitRuntimeFragmentRefBorrowCheck(
        llvm::IRBuilder<>& builder, llvm::Module& module,
        llvm::Value* reference, const moon::DeclarationRef& target) const;
    llvm::CallInst* emitRuntimeFragmentRefOwnedTransfer(
        llvm::IRBuilder<>& builder, llvm::Module& module,
        llvm::Value* sourceCell, llvm::Value* destinationCell,
        const moon::DeclarationRef& target) const;
    // Status-returning host entry only: success continues to bodyEntry;
    // failure returns the unchanged native status before entering user code.
    llvm::CallInst* emitRuntimeFragmentRefBorrowIngressGate(
        llvm::IRBuilder<>& builder, llvm::Module& module,
        llvm::Value* reference, const moon::DeclarationRef& target,
        llvm::BasicBlock* bodyEntry) const;
    llvm::CallInst* emitRuntimeFragmentRefOwnedIngressGate(
        llvm::IRBuilder<>& builder, llvm::Module& module,
        llvm::Value* sourceCell, llvm::Value* destinationCell,
        const moon::DeclarationRef& target,
        llvm::BasicBlock* bodyEntry) const;
    // Internal one-Ref, unit-result wrapper. The body must already obey its
    // verified borrow/ownership contract; no source ABI is published here.
    llvm::Function* emitRuntimeFragmentRefUnitIngressWrapper(
        llvm::Module& module, llvm::Function& body,
        const moon::DeclarationRef& target,
        RuntimeFragmentRefIngressMode mode,
        bool hasFragmentContext,
        const std::string& name) const;
    llvm::Type* i32Ty() const { return llvm::Type::getInt32Ty(mCtx); }
    llvm::Type* i64Ty() const { return llvm::Type::getInt64Ty(mCtx); }
    llvm::Type* f32Ty() const { return llvm::Type::getFloatTy(mCtx); }
    llvm::Type* f64Ty() const { return llvm::Type::getDoubleTy(mCtx); }
    llvm::Type* boolTy() const { return llvm::Type::getInt1Ty(mCtx); }
    llvm::Type* voidTy() const { return llvm::Type::getVoidTy(mCtx); }
    llvm::Type* ptrTy() const { return llvm::PointerType::get(mCtx, 0); }
    llvm::Type* sizeTy() const { return llvm::Type::getInt64Ty(mCtx); }
    llvm::StructType* deviceBufferTy() const {
        return llvm::StructType::get(mCtx, {ptrTy(), sizeTy()});
    }

    llvm::LLVMContext& context() { return mCtx; }

private:
    llvm::LLVMContext& mCtx;
};

// Map a Luna type kind to its size in bytes
uint64_t typeSize(const TypePtr& type);
uint64_t typeAlignment(const TypePtr& type);
