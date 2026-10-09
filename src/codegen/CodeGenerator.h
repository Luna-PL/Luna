#pragma once

#include "diagnostics/Diagnostic.h"

#include "../moonir/MoonIR.h"
#include "CGHelpers.h"
#ifdef LUNA_PRIVATE_REF_JIT_TEST
#include "NativeOwnedResultFacts.h"
#endif
#include <cstdint>
#include <functional>
#include <llvm/ExecutionEngine/ExecutionEngine.h>
#include <llvm/ExecutionEngine/Orc/AbsoluteSymbols.h>
#include <llvm/ExecutionEngine/Orc/EPCDynamicLibrarySearchGenerator.h>
#include <llvm/ExecutionEngine/Orc/LLJIT.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/Support/TargetSelect.h>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace luna::driver {
struct NativeExportSpec;
}
namespace luna::runtime {
class RuntimeOwnedResultHandle;
}

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
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    // Bound at private materialization, before any loaded entry can look up code.
    std::vector<uint8_t> mPrivateRefUnitApplyEntryRecord;
    std::optional<luna::codegen::NativeOwnedResultEntryProof>
        mPrivateOwnedResultEntryProof;
#endif
};

#ifdef LUNA_PRIVATE_REF_JIT_TEST
// Test-only loaded entry. A call keeps the JIT code lease alive even when the
// caller releases its original materialization reference.
class LunaPrivateRefUnitApplyLoadedEntry {
public:
    int32_t call(const void* parentContext, const void* borrowedRef) const;
    const std::vector<uint8_t>& entryRecord() const { return record_; }

private:
    friend class CodeGenerator;
    std::shared_ptr<LunaJitModule> lease_;
    const void* entry_ = nullptr;
    std::vector<uint8_t> record_;
};

// Test-only loaded Ref/Result host adapter. It supplies a Runtime-issued code
// lease token and stable owner cell to the verified six-argument candidate
// entry; failure injection remains on a separate test entry.
class LunaPrivateRefResultLoadedEntry {
public:
    int32_t call(const void* parentContext, const void* borrowedRef,
                 uint32_t* tagOutput, int32_t* scalarOutput,
                 luna::runtime::RuntimeOwnedResultHandle& ownerOutput,
                 bool failAdoptionForTest = false) const;

private:
    friend class CodeGenerator;
    std::shared_ptr<LunaJitModule> lease_;
    const void* entry_ = nullptr;
    const void* injectionEntry_ = nullptr;
    const void* drop_ = nullptr;
};
#endif

enum class LunaOptimizationLevel { O0, O2, O3 };

// Initialize the immutable LLVM native-target registries used by Luna. This is
// safe to call repeatedly and lets fresh REPL workers pay the one-time host
// cost before they publish readiness without eagerly loading device targets.
void initializeLunaLLVMTargets();

// Device code-object targets are compiler inputs. Runtime backend selection is
// deliberately separate and remains owned by LUNA_GPU_BACKEND in Runtime.cpp.
// The host simulator form is always available for every reachable kernel.
struct LunaGpuTargetConfig {
    bool emitPTX = false;
    std::string cudaArchitecture = "sm_52";
    bool emitHSACO = false;
    std::string rocmArchitecture = "gfx1101";
};

struct LunaJitRunResult {
    bool executed = false;
    int exitCode = 1;
    std::string error;
    uint64_t materializationMicroseconds = 0;
    uint64_t lookupMicroseconds = 0;
    uint64_t executionMicroseconds = 0;
    uint64_t cleanupMicroseconds = 0;
};

class CodeGenerator {
public:
    CodeGenerator(const std::string& moduleName);
    ~CodeGenerator();

    bool generate(moon::Module* module);
    void setOptimizationLevel(LunaOptimizationLevel level) { mOptimizationLevel = level; }
    void setGpuTargets(LunaGpuTargetConfig targets);

    // JIT: compile and run while keeping infrastructure failure distinct from
    // a successfully executed program whose main() returns a non-zero code.
    LunaJitRunResult jitRun();
    // Internal evolution adapter: consume the generated LLVM module into a
    // retained ORC session whose lifetime can be held by a generation lease.
    std::shared_ptr<LunaJitModule> materializeJitModule(std::string& error);

    // AOT: retain inspectable textual IR and emit the native linker input.
    bool emitObjectFile(const std::string& outputPath);
    bool emitNativeObjectFile(const std::string& outputPath);
    bool emitNativeProofPlaceholder(const std::vector<uint8_t>& record);
    bool emitNativeLibraryDescriptor(const std::string& packageId,
                                     const std::string& packageVersion,
                                     const std::string& targetAbi,
                                     const std::string& compilerIdentity,
                                     const std::vector<luna::driver::NativeExportSpec>& exports);

    const std::vector<diagnostic::Diagnostic>& errors() const { return mErrors; }

#ifdef LUNA_PRIVATE_REF_JIT_TEST
    // Test-target-only escape hatch. The caller must retain the returned JIT
    // lease; it does not publish a source Ref ABI or a container artifact.
    static std::shared_ptr<LunaJitModule>
    materializePrivateRuntimeFragmentRefApplyForTest(
        moon::Module& program, moon::FunctionDecl& function,
        std::string& failure,
        std::vector<uint8_t>* entryRecord = nullptr);
    // Validates the pointer-free test record against current frozen source
    // facts. This is neither a Native descriptor nor a public entry ABI.
    static bool validatePrivateRuntimeFragmentRefApplyEntryRecordForTest(
        const moon::Module& program, const moon::FunctionDecl& function,
        const std::vector<uint8_t>& entryRecord, std::string& failure);
    static std::unique_ptr<LunaPrivateRefUnitApplyLoadedEntry>
    loadPrivateRuntimeFragmentRefApplyEntryForTest(
        const moon::Module& program, const moon::FunctionDecl& function,
        const std::vector<uint8_t>& entryRecord,
        std::shared_ptr<LunaJitModule> executable, std::string& failure);
    static std::unique_ptr<LunaPrivateRefResultLoadedEntry>
    loadPrivateRuntimeFragmentRefResultEntryForTest(
        const moon::Module& program, const moon::FunctionDecl& function,
        std::shared_ptr<LunaJitModule> executable, std::string& failure);
#endif

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
    // Proof-only lowering of one constrained Ref entry into a temporary LLVM
    // module. Ordinary proofs destroy it before returning. The test-target-only
    // hook may consume that same verified module into a private JIT lease;
    // neither path publishes a source Ref ABI or runtime descriptor.
    static bool verifyPrivateRuntimeFragmentRefUnitIngress(
        moon::Module& program, moon::FunctionDecl& function,
        std::string& failure
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        , std::shared_ptr<LunaJitModule>* executable = nullptr,
        std::vector<uint8_t>* entryRecord = nullptr
#endif
    );
    // Pass-through owned Ref returns are lowered only inside a disposable
    // proof module; no host return carrier or public function is emitted.
    static bool verifyPrivateRuntimeFragmentRefOwnedReturn(
        moon::Module& program, moon::FunctionDecl& function,
        std::string& failure);
    void generateControlFlowBody(moon::ControlFlowGraph& graph, llvm::Function* func,
                                 llvm::BasicBlock* abiEntry,
                                 size_t hiddenParameterCount);
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
    llvm::Value* generateFieldAccess(moon::FieldAccessExpr* expr);
    llvm::Value* generateSliceLength(moon::SliceLengthExpr* expr);
    llvm::Value* generateIndex(moon::IndexExpr* expr);
    llvm::Value* emitCheckedArrayIndex(llvm::Value* index,
                                       llvm::Value* length,
                                       const std::string& label);
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
    bool materializeIteratorBinding(const std::string& name, const IteratorPlan& plan);
    void emitIteratorPipeline(const IteratorPlan& plan,
                              const std::function<void(llvm::Value*)>& consume,
                              const std::function<void()>& prepareTerminal = {});
    llvm::Value* generateIteratorTerminal(moon::CallExpr* call);
    llvm::Value* emitCallableInvocation(llvm::Value* callable, const TypePtr& callableType,
                                        llvm::ArrayRef<llvm::Value*> arguments,
                                        llvm::Type* returnType, const std::string& name);
    llvm::Value* generateLaunch(moon::LaunchExpr* launch);
    llvm::Value* generateDeviceBufferValue(moon::Expr* expr);
    llvm::Value* emitDeviceBufferIndexCheck(llvm::Value* index, llvm::Value* length);
    llvm::Value* generateHostRawPointer(moon::Expr* expr);
    void emitRuntimeDescriptors();
    void emitGpuOperationFailureCheck(llvm::Value* operationSucceeded, llvm::Function* func);
    llvm::Value* coerceCallArgument(llvm::Value* value, llvm::Type* target);
    TypePtr resolveType(const moon::TypeRef& reference);
    const moon::DeclarationRecord* resolveDeclaration(const moon::DeclarationRef& reference) const;
    const moon::FunctionDecl* resolveFunctionDeclaration(
        const moon::DeclarationRef& reference) const;
    llvm::Function* resolveFunction(const moon::DeclarationRef& reference) const;
    TypePtr allocationTypeForExpr(moon::Expr* expr);
    void emitLunaDeallocation(llvm::Value* pointer, const TypePtr& type);
    void emitCleanup(const std::string& place, luna::ownership::CleanupAction action);
    void emitCanonicalCleanup(const moon::CleanupRecord& cleanup);
    void emitMaterializedIteratorCleanup(const std::string& name);
    llvm::Value* packResultPayload(llvm::Value* value, const TypePtr& type,
                                   const TypePtr& resultType);
    llvm::Value* unpackResultPayload(llvm::Value* bits, const TypePtr& type,
                                     uint64_t byteOffset = 0);
    void emitResourceContentsCleanup(llvm::Value* value, const TypePtr& type,
                                     const std::string& label);
    void emitOwnedPayloadCleanup(llvm::Value* value, const TypePtr& type, const std::string& label);
    llvm::Function* getOrCreateDropCallback(const TypePtr& type);
    bool emitKernelPTX(moon::FunctionDecl* kernel);
    bool emitKernelHSACO(moon::FunctionDecl* kernel);

    // Helpers
    llvm::AllocaInst* createEntryBlockAlloca(llvm::Function* func, llvm::Type* type,
                                             const std::string& name);
    size_t fieldIndex(const TypePtr& type, const std::string& field) const;

    void error(const std::string& msg);

    std::unique_ptr<llvm::LLVMContext> mCtx;
    std::unique_ptr<llvm::Module> mModule;
    // Reused by the optimization and native code-emission phases so one cold
    // AOT build does not configure the same host target twice.
    std::unique_ptr<llvm::TargetMachine> mHostTargetMachine;
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
    // A source-level kernel reference to device_buffer expands to the native
    // device pointer plus this hidden element-count parameter.
    std::vector<llvm::Value*> mCanonicalDeviceBufferLengths;
    // Hidden consuming-array iterator states use one initialization bit per
    // element. ArrayDrop consults these bits on normal and early exits.
    std::unordered_map<std::string, llvm::AllocaInst*> mArrayDropFlags;
    std::unordered_map<std::string, MaterializedIterator> mMaterializedIterators;
    // Exclusive upper bounds proven from local initializers, used only to
    // remove redundant safe-array checks. Any assignment invalidates a bound.
    std::unordered_map<std::string, uint64_t> mLocalKnownUpperBounds;
    llvm::Function* mCurrentFunc = nullptr;
    // Non-null only while lowering a function whose verified
    // requires_fragment_context effect added the hidden leading ABI argument.
    llvm::Value* mCurrentFragmentContext = nullptr;
    // Set only on a disposable private proof generator. Public generation
    // continues to reject Ref-apply CFGs at the module boundary.
    bool mPrivateRefApplyEnabled = false;
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
    // Keep O0 as the default; explicit optimization levels use the standard
    // LLVM speed pipelines covered by JIT/AOT parity tests.
    LunaOptimizationLevel mOptimizationLevel = LunaOptimizationLevel::O0;
    LunaGpuTargetConfig mGpuTargets;
};
