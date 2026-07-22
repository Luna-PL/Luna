#pragma once

#include "CGHelpers.h"
#include "../moonir/MoonIR.h"
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Verifier.h>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/Support/TargetSelect.h>
#include <array>
#include <string>
#include <vector>
#include <unordered_map>

enum class LunaOptimizationLevel { O0, O2, O3 };

class CodeGenerator {
public:
    CodeGenerator(const std::string& moduleName);
    ~CodeGenerator();

    bool generate(moon::Module* module);
    void setOptimizationLevel(LunaOptimizationLevel level) { mOptimizationLevel = level; }

    // JIT: compile and run, returning main()'s exit code
    int jitRun();

    // AOT: emit object file
    bool emitObjectFile(const std::string& outputPath);

    const std::vector<std::string>& errors() const { return mErrors; }

private:
    struct ContinuationFrame {
        llvm::AllocaInst* storage = nullptr;
        llvm::StructType* llvmType = nullptr;
        llvm::BasicBlock* returnDispatch = nullptr;
        llvm::Type* returnType = nullptr;
    };

    void generateFunctionBody(moon::FunctionDecl* decl);
    void generateStmt(moon::Stmt* stmt, llvm::Function* func = nullptr);
    void generateBlock(moon::BlockStmt* block, llvm::Function* func);
    void generateStructuredContinuation(moon::BlockStmt* continuation, llvm::Function* func);
    void generateSlotInvoke(moon::SlotInvokeStmt* slot, llvm::Function* func);
    void generateFragmentInline(moon::FragmentDecl* fragment, moon::SlotInvokeStmt* slot,
                                llvm::Function* func);
    void generateDynamicFragmentDispatch(const std::vector<moon::FragmentDecl*>& candidates,
                                         moon::SlotInvokeStmt* slot, llvm::Function* func);
    std::array<llvm::Value*, 4> generateExternalFragmentInvocation(
        moon::SlotInvokeStmt* slot, llvm::Function* func, llvm::Value* selected);
    llvm::Value* generateExpr(moon::Expr* expr);
    llvm::Value* generateLaunch(moon::LaunchExpr* launch);
    llvm::Value* generateDeviceBufferPointer(moon::Expr* expr);
    llvm::Value* generateHostRawPointer(moon::Expr* expr);
    void emitRuntimeDescriptors();
    void emitGpuOperationFailureCheck(llvm::Value* operationSucceeded,
                                      llvm::Function* func);
    llvm::Value* coerceCallArgument(llvm::Value* value, llvm::Type* target);
    bool emitKernelPTX(moon::FunctionDecl* kernel);
    bool emitKernelHSACO(moon::FunctionDecl* kernel);

    // Helpers
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);
    llvm::Value* getOrCreatePrintf();
    size_t fieldIndex(const TypePtr& type, const std::string& field) const;

    void error(const std::string& msg);

    std::unique_ptr<llvm::LLVMContext> mCtx;
    std::unique_ptr<llvm::Module> mModule;
    std::unique_ptr<llvm::IRBuilder<>> mBuilder;
    std::unique_ptr<CGHelpers> mHelpers;

    moon::Module* mProgram = nullptr;

    // Current state
    std::unordered_map<std::string, llvm::AllocaInst*> mLocals;
    std::unordered_map<std::string, TypePtr> mLocalTypes;
    // Exclusive upper bounds proven from local initializers, used only to
    // remove redundant safe-array checks. Any assignment invalidates a bound.
    std::unordered_map<std::string, uint64_t> mLocalKnownUpperBounds;
    llvm::Function* mCurrentFunc = nullptr;
    bool mCurrentFunctionIsKernel = false;
    std::unordered_map<std::string, llvm::Function*> mFunctions;
    std::unordered_map<std::string, moon::FragmentDecl*> mFragments;
    std::unordered_map<std::string, moon::FragmentDecl*> mSlotDefaults;
    std::vector<std::unordered_map<std::string, moon::FragmentDecl*>> mApplyScopes;
    std::vector<std::unordered_map<std::string, std::vector<moon::FragmentDecl*>>> mDynamicApplyScopes;
    moon::BlockStmt* mCurrentSlotContinuation = nullptr;
    llvm::BasicBlock* mCurrentFragmentExit = nullptr;
    std::vector<ContinuationFrame> mContinuationFrames;
    unsigned mContinuationFrameCounter = 0;
    // A return in the fragment body ends that fragment. Slot-continuation
    // returns clear this target and use the structured CPS frame instead.
    llvm::BasicBlock* mCurrentFragmentReturn = nullptr;
    // Exact generated kernel symbol -> PTX source, emitted only for the CUDA
    // backend. The simulator deliberately has no NVPTX dependency.
    std::unordered_map<std::string, std::string> mKernelPTX;
    // Exact generated kernel symbol -> linked HSACO wrapped in the Clang HIP
    // module bundle accepted by HIP's Module API.
    std::unordered_map<std::string, std::string> mKernelHSACO;

    std::vector<std::string> mErrors;
    // Keep O0 as the Alpha default.  Experimental source constructs such as
    // dynamic fragments can opt in to the standard LLVM speed pipelines once
    // their behavior is covered by parity tests.
    LunaOptimizationLevel mOptimizationLevel = LunaOptimizationLevel::O0;
    bool mHasRtAlloc = false;
    bool mHasRtFree = false;
    bool mHasPrintf = false;
};
