#pragma once

#include "diagnostics/Diagnostic.h"

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
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <unordered_map>

namespace luna::driver { struct NativeExportSpec; }

// Internal ownership boundary for ORC materializations. Keeping this object
// alive keeps every address returned by lookup() executable; destroying it
// tears down the JIT session and invalidates those addresses.
class LunaJitModule {
public:
    ~LunaJitModule();

    LunaJitModule(const LunaJitModule&) = delete;
    LunaJitModule& operator=(const LunaJitModule&) = delete;
    const void* lookup(const std::string& symbol, std::string& error) const;

private:
    friend class CodeGenerator;
    LunaJitModule();
    struct Impl;
    std::unique_ptr<Impl> mImpl;
};

enum class LunaOptimizationLevel { O0, O2, O3 };

// Device code-object targets are compiler inputs. Runtime backend selection is
// deliberately separate and remains owned by LUNA_GPU_BACKEND in Runtime.cpp.
// The host simulator form is always available for every reachable kernel.
struct LunaGpuTargetConfig {
    bool emitPTX = false;
    std::string cudaArchitecture = "sm_52";
    bool emitHSACO = false;
    std::string rocmArchitecture = "gfx1101";
};

class CodeGenerator {
public:
    CodeGenerator(const std::string& moduleName);
    ~CodeGenerator();

    bool generate(moon::Module* module);
    void setOptimizationLevel(LunaOptimizationLevel level) { mOptimizationLevel = level; }
    void setGpuTargets(LunaGpuTargetConfig targets) { mGpuTargets = std::move(targets); }

    // JIT: compile and run, returning main()'s exit code
    int jitRun();
    // Internal evolution adapter: consume the generated LLVM module into a
    // retained ORC session whose lifetime can be held by a generation lease.
    std::shared_ptr<LunaJitModule> materializeJitModule(std::string& error);

    // AOT: emit object file
    bool emitObjectFile(const std::string& outputPath);
    bool emitNativeProofPlaceholder(const std::vector<uint8_t>& record);
    bool emitNativeLibraryDescriptor(
        const std::string& packageId, const std::string& packageVersion,
        const std::string& targetAbi, const std::string& compilerIdentity,
        const std::vector<luna::driver::NativeExportSpec>& exports);

    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

private:
    struct IteratorStep {
        IteratorOp op = IteratorOp::None;
        moon::Expr* argument = nullptr;
        TypePtr inputType;
        TypePtr outputType;
    };

    struct IteratorPlan {
        moon::Expr* source = nullptr;
        TypePtr sourceType;
        TypePtr itemType;
        IteratorMode mode = IteratorMode::Copy;
        moon::Expr* rangeStart = nullptr;
        moon::Expr* rangeEnd = nullptr;
        std::string ownedStateName;
        std::string materializedName;
        std::vector<IteratorStep> steps;
    };

    struct RuntimeIteratorStep {
        IteratorStep description;
        llvm::Value* value = nullptr;
        llvm::AllocaInst* remaining = nullptr;
    };

    struct MaterializedIterator {
        IteratorPlan plan;
        llvm::Value* sourceData = nullptr;
        llvm::Value* limit = nullptr;
        llvm::AllocaInst* indexStorage = nullptr;
        llvm::AllocaInst* sourceDropFlags = nullptr;
        bool ownsSource = false;
        std::vector<RuntimeIteratorStep> steps;
    };

    void generateFunctionBody(moon::FunctionDecl* decl);
    void generateControlFlowBody(
        moon::ControlFlowGraph& graph, llvm::Function* func,
        llvm::BasicBlock* abiEntry);
    llvm::Value* generateExpr(moon::Expr* expr);
    // Literal expression emitters. Split out from generateExpr so each AST
    // node has one home; behavior is unchanged.
    llvm::Value* generateIntLiteral(moon::IntLiteralExpr* expr);
    llvm::Value* generateFloatLiteral(moon::FloatLiteralExpr* expr);
    llvm::Value* generateStringLiteral(moon::StringLiteralExpr* expr);
    llvm::Value* generateBoolLiteral(moon::BoolLiteralExpr* expr);
    llvm::Value* generateUnitLiteral(moon::UnitExpr* expr);
    llvm::Value* generateArrayLiteral(moon::ArrayLiteralExpr* expr);
    // Value-access expression emitters.
    llvm::Value* generateIdentifier(moon::IdentifierExpr* expr);
    llvm::Value* generateDynamicSelect(moon::DynamicSelectExpr* expr);
    llvm::Value* generateFieldAccess(moon::FieldAccessExpr* expr);
    llvm::Value* generateSliceLength(moon::SliceLengthExpr* expr);
    llvm::Value* generateIndex(moon::IndexExpr* expr);
    // Arithmetic expression emitters.
    llvm::Value* generateBinary(moon::BinaryExpr* expr);
    llvm::Value* generateUnary(moon::UnaryExpr* expr);
    // Construct expression emitters.
    llvm::Value* generateVariantConstruct(moon::VariantConstructExpr* expr);
    llvm::Value* generateResultConstruct(moon::ResultConstructExpr* expr);
    llvm::Value* generateRecordLiteral(moon::RecordLiteralExpr* expr);
    llvm::Value* generateInitAllocation(moon::InitAllocationExpr* expr);
    llvm::Value* generateHeapAlloc(moon::HeapAllocExpr* expr);
    // Call/launch expression emitters.
    llvm::Value* generateCall(moon::CallExpr* expr);
    // Control-flow and ownership expression emitters.
    llvm::Value* generateTry(moon::TryExpr* expr);
    llvm::Value* generateAssign(moon::AssignExpr* expr);
    llvm::Value* generateMove(moon::MoveExpr* expr);
    llvm::Value* generateBorrow(moon::BorrowExpr* expr);
    llvm::Value* generateDeref(moon::DerefExpr* expr);
    llvm::Value* generateAddrOf(moon::AddrOfExpr* expr);
    llvm::Value* generateLambda(moon::LambdaExpr* expr);
    llvm::Value* generateEnvLoad(moon::EnvLoadExpr* expr);
    llvm::Value* generateMakeClosure(moon::MakeClosureExpr* expr);
    bool buildIteratorPlan(moon::Expr* expr, IteratorPlan& plan);
    bool materializeIteratorBinding(
        const std::string& name,
        const IteratorPlan& plan);
    void emitIteratorPipeline(const IteratorPlan& plan,
                              const std::function<void(llvm::Value*)>& consume,
                              const std::function<void()>& prepareTerminal = {});
    llvm::Value* generateIteratorTerminal(moon::CallExpr* call);
    llvm::Value* emitCallableInvocation(llvm::Value* callable,
                                        const TypePtr& callableType,
                                        llvm::ArrayRef<llvm::Value*> arguments,
                                        llvm::Type* returnType,
                                        const std::string& name);
    llvm::Value* generateLaunch(moon::LaunchExpr* launch);
    llvm::Value* generateDeviceBufferPointer(moon::Expr* expr);
    llvm::Value* generateHostRawPointer(moon::Expr* expr);
    void emitRuntimeDescriptors();
    void emitGpuOperationFailureCheck(llvm::Value* operationSucceeded,
                                      llvm::Function* func);
    llvm::Value* coerceCallArgument(llvm::Value* value, llvm::Type* target);
    TypePtr resolveType(const moon::TypeRef& reference);
    const moon::DeclarationRecord* resolveDeclaration(
        const moon::DeclarationRef& reference) const;
    llvm::Function* resolveFunction(
        const moon::DeclarationRef& reference) const;
    TypePtr allocationTypeForExpr(moon::Expr* expr);
    void emitLunaDeallocation(llvm::Value* pointer, const TypePtr& type);
    void emitCleanup(const std::string& place,
                     luna::ownership::CleanupAction action);
    void emitCanonicalCleanup(const moon::CleanupRecord& cleanup);
    void emitMaterializedIteratorCleanup(const std::string& name);
    llvm::Value* packResultPayload(llvm::Value* value, const TypePtr& type,
                                   const TypePtr& resultType);
    llvm::Value* unpackResultPayload(llvm::Value* bits, const TypePtr& type,
                                     uint64_t byteOffset = 0);
    void emitResourceContentsCleanup(llvm::Value* value, const TypePtr& type,
                                     const std::string& label);
    void emitOwnedPayloadCleanup(llvm::Value* value, const TypePtr& type,
                                 const std::string& label);
    llvm::Function* getOrCreateDropCallback(const TypePtr& type);
    bool emitKernelPTX(moon::FunctionDecl* kernel);
    bool emitKernelHSACO(moon::FunctionDecl* kernel);

    // Helpers
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func,
                                              llvm::Type* type,
                                              const std::string& name);
    size_t fieldIndex(const TypePtr& type, const std::string& field) const;

    void error(const std::string& msg);

    std::unique_ptr<llvm::LLVMContext> mCtx;
    std::unique_ptr<llvm::Module> mModule;
    std::unique_ptr<llvm::IRBuilder<>> mBuilder;
    std::unique_ptr<CGHelpers> mHelpers;

    moon::Module* mProgram = nullptr;
    std::unique_ptr<moon::TypeMaterializer> mTypeMaterializer;

    // Current state
    std::unordered_map<std::string, llvm::AllocaInst*> mLocals;
    std::unordered_map<std::string, TypePtr> mLocalTypes;
    // Canonical bodies are keyed exclusively by LocalId. Diagnostic names
    // may shadow and are never backend identities.
    std::vector<llvm::AllocaInst*> mCanonicalLocals;
    std::vector<TypePtr> mCanonicalLocalTypes;
    // Hidden consuming-array iterator states use one initialization bit per
    // element. ArrayDrop consults these bits on normal and early exits.
    std::unordered_map<std::string, llvm::AllocaInst*> mArrayDropFlags;
    std::unordered_map<std::string, MaterializedIterator>
        mMaterializedIterators;
    // Exclusive upper bounds proven from local initializers, used only to
    // remove redundant safe-array checks. Any assignment invalidates a bound.
    std::unordered_map<std::string, uint64_t> mLocalKnownUpperBounds;
    llvm::Function* mCurrentFunc = nullptr;
    bool mCurrentFunctionIsKernel = false;
    std::unordered_map<std::string, llvm::Function*> mFunctions;
    std::unordered_map<std::string, llvm::Function*> mDropCallbacks;
    // Exact generated kernel symbol -> PTX source, emitted only for the CUDA
    // backend. The simulator deliberately has no NVPTX dependency.
    std::unordered_map<std::string, std::string> mKernelPTX;
    // Exact generated kernel symbol -> linked HSACO wrapped in the Clang HIP
    // module bundle accepted by HIP's Module API.
    std::unordered_map<std::string, std::string> mKernelHSACO;

    std::vector<diagnostic::Diagnostic> mErrors;
    // Keep O0 as the Alpha default.  Experimental source constructs such as
    // dynamic fragments can opt in to the standard LLVM speed pipelines once
    // their behavior is covered by parity tests.
    LunaOptimizationLevel mOptimizationLevel = LunaOptimizationLevel::O0;
    LunaGpuTargetConfig mGpuTargets;
};
