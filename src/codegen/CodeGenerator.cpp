#include "CodeGenerator.h"
#include "../diagnostics/Diagnostic.h"
#include "../runtime/Runtime.h"
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <optional>
#include <iterator>
#include <llvm/ADT/SmallString.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/Path.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ModuleUtils.h>

#include <llvm/TargetParser/Host.h>
// BitcodeWriter no longer needed (using text IR output)

using moon::AbortStmt;
using moon::AddrOfExpr;
using moon::ApplyStmt;
using moon::ArrayLiteralExpr;
using moon::AssignExpr;
using moon::AwaitStmt;
using moon::BinaryExpr;
using moon::BlockStmt;
using moon::BoolLiteralExpr;
using moon::BorrowExpr;
using moon::CallExpr;
using moon::DerefExpr;
using moon::DynamicSelectExpr;
using moon::Expr;
using moon::ExprStmt;
using moon::FieldAccessExpr;
using moon::FloatLiteralExpr;
using moon::ForStmt;
using moon::FragmentCardinality;
using moon::FragmentDecl;
using moon::FragmentKind;
using moon::FreeStmt;
using moon::FunctionDecl;
using moon::HeapAllocExpr;
using moon::IdentifierExpr;
using moon::IfStmt;
using moon::IndexExpr;
using moon::IntLiteralExpr;
using moon::ImplDecl;
using moon::LambdaExpr;
using moon::LaunchExpr;
using moon::LetStmt;
using moon::MoveExpr;
using moon::Operator;
using moon::ResumeStmt;
using moon::ReturnStmt;
using moon::SlotDeclStmt;
using moon::SlotInvokeStmt;
using moon::Stmt;
using moon::StringLiteralExpr;
using moon::UnaryExpr;
using moon::VariantConstructExpr;
using moon::WhileStmt;

static void initializeLLVM() {
    static bool initialized = false;
    if (!initialized) {
        llvm::InitializeNativeTarget();
        llvm::InitializeNativeTargetAsmPrinter();
        llvm::InitializeNativeTargetAsmParser();
        llvm::InitializeAllTargets();
        llvm::InitializeAllTargetMCs();
        llvm::InitializeAllAsmPrinters();
        initialized = true;
    }
}

namespace {

// A non-negative bit-mask index is statically bounded: x & mask is always in
// [0, mask], even when x is signed. The map contains exclusive upper bounds
// for simple locals whose initializers established such a range. Preserve
// runtime checks for every other expression so safety is never inferred from
// an incomplete range proof.
std::optional<uint64_t> knownArrayIndexUpperBound(
    const Expr* expression,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds) {
    if (const auto* literal = dynamic_cast<const IntLiteralExpr*>(expression)) {
        if (literal->value < 0) return std::nullopt;
        return static_cast<uint64_t>(literal->value) + 1;
    }
    if (const auto* identifier = dynamic_cast<const IdentifierExpr*>(expression)) {
        auto it = knownUpperBounds.find(identifier->name);
        if (it != knownUpperBounds.end()) return it->second;
    }
    const auto* binary = dynamic_cast<const BinaryExpr*>(expression);
    if (!binary || binary->op != Operator::BitAnd) return std::nullopt;
    const auto* mask = dynamic_cast<const IntLiteralExpr*>(binary->rhs.get());
    if (!mask || mask->value < 0) return std::nullopt;
    return static_cast<uint64_t>(mask->value) + 1;
}

bool isProvablySafeArrayIndex(
    const Expr* expression,
    uint64_t length,
    const std::unordered_map<std::string, uint64_t>& knownUpperBounds) {
    auto upperBound = knownArrayIndexUpperBound(expression, knownUpperBounds);
    return upperBound && *upperBound <= length;
}

uint64_t stableRuntimeId(const std::string& text) {
    uint64_t hash = 1469598103934665603ULL;
    for (unsigned char byte : text) {
        hash ^= byte;
        hash *= 1099511628211ULL;
    }
    return hash;
}

void appendLittleEndianU64(std::string& output, uint64_t value) {
    for (unsigned int byte = 0; byte < 8; ++byte)
        output.push_back(static_cast<char>((value >> (byte * 8)) & 0xff));
}

// Kernel bodies are cloned before host-module optimization runs, so they need
// their own middle-end pipeline. Without it, AMDGPU/NVPTX receive host-style
// entry allocas and address-space casts that the host O2/O3 pass later removes
// but the device code object never sees.
void optimizeDeviceModule(llvm::Module& module) {
    llvm::LoopAnalysisManager loopAnalyses;
    llvm::FunctionAnalysisManager functionAnalyses;
    llvm::CGSCCAnalysisManager cgsccAnalyses;
    llvm::ModuleAnalysisManager moduleAnalyses;
    llvm::PassBuilder passBuilder;
    passBuilder.registerModuleAnalyses(moduleAnalyses);
    passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
    passBuilder.registerFunctionAnalyses(functionAnalyses);
    passBuilder.registerLoopAnalyses(loopAnalyses);
    passBuilder.crossRegisterProxies(loopAnalyses, functionAnalyses,
                                     cgsccAnalyses, moduleAnalyses);
    auto pipeline = passBuilder.buildPerModuleDefaultPipeline(llvm::OptimizationLevel::O3);
    pipeline.run(module, moduleAnalyses);
}

// HIP's Module API accepts the legacy Clang offload bundle when it is passed
// a pointer in memory. Keep the wrapper deliberately small: an empty host
// image plus the single architecture-specific HSA code object. The HSA image
// must begin on a 4 KiB boundary, matching HIP's own device-only output.
std::string makeHipModuleBundle(const std::string& hsaco,
                                const std::string& architecture) {
    constexpr uint64_t imageAlignment = 4096;
    static constexpr const char magic[] = "__CLANG_OFFLOAD_BUNDLE__";
    static constexpr const char hostTarget[] = "host-x86_64-unknown-linux-gnu-";
    const std::string deviceTarget = "hipv4-amdgcn-amd-amdhsa--" + architecture;

    std::string bundle;
    bundle.append(magic, sizeof(magic) - 1);
    appendLittleEndianU64(bundle, 2); // empty host image + one HIP image
    appendLittleEndianU64(bundle, imageAlignment);
    appendLittleEndianU64(bundle, 0);
    appendLittleEndianU64(bundle, sizeof(hostTarget) - 1);
    bundle.append(hostTarget, sizeof(hostTarget) - 1);
    appendLittleEndianU64(bundle, imageAlignment);
    appendLittleEndianU64(bundle, hsaco.size());
    appendLittleEndianU64(bundle, deviceTarget.size());
    bundle.append(deviceTarget);
    bundle.resize(imageAlignment, '\0');
    bundle.append(hsaco);
    return bundle;
}

// Keep generated code objects inspectable without changing the normal AOT
// artifact or leaving temporary linker files behind. This is intentionally an
// opt-in compiler diagnostic: it lets performance investigations compare the
// final ISA against HIP/Clang output, which is more reliable than comparing
// source-shaped LLVM IR.
bool dumpHsacoIfRequested(const std::string& hsaco, const std::string& symbol,
                          const std::string& architecture, std::string& error) {
    const char* requestedDirectory = std::getenv("LUNA_GPU_DUMP_HSACO");
    if (!requestedDirectory || !*requestedDirectory) return true;

    llvm::SmallString<256> outputPath(requestedDirectory);
    llvm::sys::path::append(outputPath, "luna-" + architecture + "-" + symbol + ".hsaco");
    std::error_code streamError;
    llvm::raw_fd_ostream output(outputPath, streamError, llvm::sys::fs::OF_None);
    if (streamError) {
        error = "could not write requested HSACO dump '" + outputPath.str().str() +
                "': " + streamError.message();
        return false;
    }
    output.write(hsaco.data(), hsaco.size());
    output.flush();
    if (output.has_error()) {
        error = "could not finish requested HSACO dump '" + outputPath.str().str() + "'";
        return false;
    }
    return true;
}

// Host-shaped kernel IR uses generic pointers so the CPU simulator can call
// the same function. An HSA kernel ABI, however, receives device-buffer
// parameters in AMDGPU's global address space (1). Keep that ABI on the
// cloned entry point, then redirect the direct load/store GEPs emitted for
// Luna's initial device built-ins to the global pointer. Otherwise LLVM must
// select conservative flat memory instructions for every device-buffer access.
void lowerDirectDeviceMemoryToGlobal(llvm::Function& function) {
    std::vector<llvm::GetElementPtrInst*> candidates;
    for (auto& block : function) {
        for (auto& instruction : block) {
            auto* gep = llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
            auto* cast = gep ? llvm::dyn_cast<llvm::AddrSpaceCastInst>(gep->getPointerOperand())
                             : nullptr;
            if (cast && cast->getSrcAddressSpace() == 1 && cast->getDestAddressSpace() == 0)
                candidates.push_back(gep);
        }
    }
    for (auto* genericGep : candidates) {
        auto* cast = llvm::cast<llvm::AddrSpaceCastInst>(genericGep->getPointerOperand());
        llvm::IRBuilder<> builder(genericGep);
        llvm::SmallVector<llvm::Value*, 4> indices;
        for (auto& index : genericGep->indices()) indices.push_back(index.get());
        auto* globalGep = llvm::cast<llvm::GetElementPtrInst>(builder.CreateGEP(
            genericGep->getSourceElementType(), cast->getPointerOperand(), indices,
            genericGep->getName() + ".global"));
        globalGep->setIsInBounds(genericGep->isInBounds());

        std::vector<llvm::User*> users;
        for (auto* user : genericGep->users()) users.push_back(user);
        bool onlyDirectMemoryUses = true;
        for (auto* user : users) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                if (load->getPointerOperand() == genericGep) {
                    load->setOperand(load->getPointerOperandIndex(), globalGep);
                    continue;
                }
            } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(user)) {
                if (store->getPointerOperand() == genericGep) {
                    store->setOperand(store->getPointerOperandIndex(), globalGep);
                    continue;
                }
            }
            onlyDirectMemoryUses = false;
            break;
        }
        if (onlyDirectMemoryUses) genericGep->eraseFromParent();
    }

    std::vector<llvm::AddrSpaceCastInst*> unusedCasts;
    for (auto& block : function) {
        for (auto& instruction : block) {
            auto* cast = llvm::dyn_cast<llvm::AddrSpaceCastInst>(&instruction);
            if (cast && cast->use_empty()) unusedCasts.push_back(cast);
        }
    }
    for (auto* cast : unusedCasts) cast->eraseFromParent();
}

} // namespace

CodeGenerator::CodeGenerator(const std::string& moduleName)
    : mCtx(std::make_unique<llvm::LLVMContext>())
    , mModule(std::make_unique<llvm::Module>(moduleName, *mCtx))
    , mBuilder(std::make_unique<llvm::IRBuilder<>>(*mCtx))
    , mHelpers(std::make_unique<CGHelpers>(*mCtx)) {
    initializeLLVM();
}

CodeGenerator::~CodeGenerator() = default;

bool CodeGenerator::generate(moon::Module* program) {
    mProgram = program;
    mFunctions.clear();
    mFragments.clear();
    mSlotDefaults.clear();
    mApplyScopes.clear();
    mDynamicApplyScopes.clear();
    mKernelPTX.clear();
    mKernelHSACO.clear();
    for (auto& decl : program->declarations) {
        if (auto* fragment = dynamic_cast<FragmentDecl*>(decl.get())) {
            const std::string key = fragment->generatedSymbolName.empty()
                ? fragment->name : fragment->generatedSymbolName;
            mFragments[key] = fragment;
            if (key == fragment->name) mFragments[fragment->name] = fragment;
        }
    }

    auto declareFunc = [&](FunctionDecl* f) {
        if (f->isSelector) return;
        if (f->isKernel && !f->isCodegenReachable) return;
        if (!f->typeParams.empty() && !f->isTemplateInstance) return;
        std::vector<llvm::Type*> paramLLVMTypes;
        for (auto& p : f->params) {
            paramLLVMTypes.push_back(mHelpers->toLLVMType(p.type));
        }
        llvm::Type* retLLVMType = f->returnType
            ? mHelpers->toLLVMType(f->returnType)
            : mHelpers->voidTy();
        auto funcType = llvm::FunctionType::get(retLLVMType, paramLLVMTypes, false);
        // A package's ABI is its explicit export list. `main` remains visible
        // as the executable entry point, while other private declarations are
        // kept local to the combined LLVM module.
        const bool visible = !program->isPackage || f->isExported ||
                             f->isExtern || f->name == "main";
        const auto linkage = visible ? llvm::Function::ExternalLinkage
                                     : llvm::Function::InternalLinkage;
        const std::string internalName = f->generatedSymbolName.empty()
            ? f->name : f->generatedSymbolName;
        const std::string symbolName = f->linkName.empty() ? internalName : f->linkName;
        auto* function = llvm::Function::Create(
            funcType, linkage, symbolName, mModule.get());
        mFunctions[internalName] = function;
        if (internalName == f->name) mFunctions[f->name] = function;
    };

    auto generateBodies = [&](bool kernels) {
        for (auto& decl : program->declarations) {
            if (auto* function = dynamic_cast<FunctionDecl*>(decl.get())) {
                if (!function->isSelector &&
                    (!function->isKernel || function->isCodegenReachable) &&
                    function->isKernel == kernels &&
                    (function->typeParams.empty() || function->isTemplateInstance))
                    generateFunctionBody(function);
            }
            if (auto* impl = dynamic_cast<ImplDecl*>(decl.get())) {
                for (auto& method : impl->methods) {
                    if (!method->isSelector &&
                        (!method->isKernel || method->isCodegenReachable) &&
                        method->isKernel == kernels &&
                        (method->typeParams.empty() || method->isTemplateInstance))
                        generateFunctionBody(method.get());
                }
            }
        }
    };

    // Pass 1: create all function declarations (resolve forward references)
    for (auto& decl : program->declarations) {
        if (auto* f = dynamic_cast<FunctionDecl*>(decl.get())) declareFunc(f);
        if (auto* i = dynamic_cast<ImplDecl*>(decl.get())) {
            for (auto& m : i->methods) declareFunc(m.get());
        }
    }

    emitRuntimeDescriptors();

    // Pass 2: generate kernels first. The target-specific code object must
    // exist before host launch expressions are lowered, otherwise an AOT
    // executable would embed the temporary empty-device-module placeholder.
    generateBodies(true);

    // Device code-object targets are explicit compiler inputs. Runtime backend
    // selection must never silently alter an AOT/JIT artifact.
    if (mGpuTargets.emitPTX) {
        for (auto& decl : program->declarations) {
            if (auto* function = dynamic_cast<FunctionDecl*>(decl.get())) {
                if (function->isKernel && function->isCodegenReachable &&
                    !emitKernelPTX(function)) return false;
            }
        }
    }
    if (mGpuTargets.emitHSACO) {
        for (auto& decl : program->declarations) {
            if (auto* function = dynamic_cast<FunctionDecl*>(decl.get())) {
                if (function->isKernel && function->isCodegenReachable &&
                    !emitKernelHSACO(function)) return false;
            }
        }
    }

    // Pass 3: lower host functions only after their launch sites can embed
    // the PTX/HSACO produced above.
    generateBodies(false);

    auto verifyHostModule = [this](const std::string& suffix) {
        std::string verifierOutput;
        llvm::raw_string_ostream verifierStream(verifierOutput);
        if (llvm::verifyModule(*mModule, &verifierStream)) {
            verifierStream.flush();
            error("generated invalid host LLVM IR" + suffix + ": " + verifierOutput);
            return true;
        }
        return false;
    };
    if (mErrors.empty() && verifyHostModule("")) return false;

    if (mErrors.empty() && mOptimizationLevel != LunaOptimizationLevel::O0) {
        llvm::LoopAnalysisManager loopAnalyses;
        llvm::FunctionAnalysisManager functionAnalyses;
        llvm::CGSCCAnalysisManager cgsccAnalyses;
        llvm::ModuleAnalysisManager moduleAnalyses;
        llvm::PassBuilder passBuilder;
        passBuilder.registerModuleAnalyses(moduleAnalyses);
        passBuilder.registerCGSCCAnalyses(cgsccAnalyses);
        passBuilder.registerFunctionAnalyses(functionAnalyses);
        passBuilder.registerLoopAnalyses(loopAnalyses);
        passBuilder.crossRegisterProxies(loopAnalyses, functionAnalyses,
                                         cgsccAnalyses, moduleAnalyses);
        const llvm::OptimizationLevel level =
            mOptimizationLevel == LunaOptimizationLevel::O3
                ? llvm::OptimizationLevel::O3
                : llvm::OptimizationLevel::O2;
        auto pipeline = passBuilder.buildPerModuleDefaultPipeline(level);
        pipeline.run(*mModule, moduleAnalyses);
        if (verifyHostModule(" after optimization")) return false;
    }
    return mErrors.empty();
}

void CodeGenerator::emitRuntimeDescriptors() {
    if (!mProgram || !mProgram->features.runtime) return;

    auto* i8 = llvm::Type::getInt8Ty(*mCtx);
    auto* i32 = mHelpers->i32Ty();
    auto* i64 = llvm::Type::getInt64Ty(*mCtx);
    auto* ptr = llvm::cast<llvm::PointerType>(mHelpers->ptrTy());
    auto* metadataValueType = llvm::StructType::create(*mCtx, "moon.metadata.value");
    metadataValueType->setBody({i8, i64, ptr});
    auto* metadataInstanceType = llvm::StructType::create(
        *mCtx, "moon.metadata.instance");
    metadataInstanceType->setBody({ptr, i64, ptr, i8});
    auto* descriptorType = llvm::StructType::create(
        *mCtx, "moon.declaration.descriptor");
    descriptorType->setBody({i32, ptr, ptr, ptr, i8, i8, i64, ptr, ptr});

    std::unordered_map<std::string, llvm::Constant*> strings;
    auto cString = [&](const std::string& text) -> llvm::Constant* {
        auto found = strings.find(text);
        if (found != strings.end()) return found->second;
        auto* initializer = llvm::ConstantDataArray::getString(*mCtx, text, true);
        std::ostringstream name;
        name << "__moon_string_" << std::hex << stableRuntimeId(text);
        auto* global = new llvm::GlobalVariable(
            *mModule, initializer->getType(), true,
            llvm::GlobalValue::PrivateLinkage, initializer, name.str());
        global->setUnnamedAddr(llvm::GlobalValue::UnnamedAddr::Global);
        auto* zero = llvm::ConstantInt::get(i32, 0);
        llvm::Constant* indices[] = {zero, zero};
        auto* address = llvm::ConstantExpr::getInBoundsGetElementPtr(
            initializer->getType(), global, indices);
        strings.emplace(text, address);
        return address;
    };

    std::vector<llvm::GlobalValue*> retainedGlobals;
    std::vector<llvm::Constant*> descriptorPointers;
    for (const auto& record : mProgram->declarationTable) {
        std::vector<const moon::MetadataInstance*> retainedMetadata;
        for (const auto& metadata : record.metadata) {
            if (metadata.retention != moon::Retention::CompileTime)
                retainedMetadata.push_back(&metadata);
        }
        if (record.retention == moon::Retention::CompileTime &&
            retainedMetadata.empty())
            continue;

        std::ostringstream suffixStream;
        suffixStream << std::hex << stableRuntimeId(record.id);
        const std::string suffix = suffixStream.str();
        std::vector<llvm::Constant*> metadataConstants;
        for (size_t metadataIndex = 0;
             metadataIndex < retainedMetadata.size(); ++metadataIndex) {
            const auto& metadata = *retainedMetadata[metadataIndex];
            std::vector<llvm::Constant*> valueConstants;
            for (const auto& value : metadata.values) {
                uint8_t kind = 0;
                uint64_t payload = 0;
                llvm::Constant* text = llvm::ConstantPointerNull::get(ptr);
                if (auto* integer = std::get_if<int64_t>(&value)) {
                    payload = static_cast<uint64_t>(*integer);
                } else if (auto* floating = std::get_if<double>(&value)) {
                    kind = 1;
                    static_assert(sizeof(payload) == sizeof(*floating));
                    std::memcpy(&payload, floating, sizeof(payload));
                } else if (auto* boolean = std::get_if<bool>(&value)) {
                    kind = 2;
                    payload = *boolean ? 1 : 0;
                } else {
                    kind = 3;
                    text = cString(std::get<std::string>(value));
                }
                valueConstants.push_back(llvm::ConstantStruct::get(
                    metadataValueType,
                    {llvm::ConstantInt::get(i8, kind),
                     llvm::ConstantInt::get(i64, payload), text}));
            }

            llvm::Constant* valuesPointer = llvm::ConstantPointerNull::get(ptr);
            if (!valueConstants.empty()) {
                auto* arrayType = llvm::ArrayType::get(
                    metadataValueType, valueConstants.size());
                auto* array = llvm::ConstantArray::get(arrayType, valueConstants);
                auto* valuesGlobal = new llvm::GlobalVariable(
                    *mModule, arrayType, true, llvm::GlobalValue::PrivateLinkage,
                    array, "__moon_meta_values_" + suffix + "_" +
                           std::to_string(metadataIndex));
                valuesPointer = valuesGlobal;
            }
            metadataConstants.push_back(llvm::ConstantStruct::get(
                metadataInstanceType,
                {cString(metadata.schemaId),
                 llvm::ConstantInt::get(i64, metadata.values.size()),
                 valuesPointer,
                 llvm::ConstantInt::get(
                     i8, static_cast<uint8_t>(metadata.retention))}));
        }

        llvm::Constant* metadataPointer = llvm::ConstantPointerNull::get(ptr);
        if (!metadataConstants.empty()) {
            auto* arrayType = llvm::ArrayType::get(
                metadataInstanceType, metadataConstants.size());
            auto* array = llvm::ConstantArray::get(arrayType, metadataConstants);
            auto* metadataGlobal = new llvm::GlobalVariable(
                *mModule, arrayType, true, llvm::GlobalValue::PrivateLinkage,
                array, "__moon_metadata_" + suffix);
            metadataPointer = metadataGlobal;
        }

        llvm::Constant* entry = llvm::ConstantPointerNull::get(ptr);
        auto function = mFunctions.find(record.linkageName);
        if (function != mFunctions.end()) entry = function->second;
        auto* descriptor = llvm::ConstantStruct::get(
            descriptorType,
            {llvm::ConstantInt::get(i32, 1),
             cString(record.id), cString(record.familyId),
             cString(record.linkageName),
             llvm::ConstantInt::get(i8, static_cast<uint8_t>(record.kind)),
             llvm::ConstantInt::get(i8, static_cast<uint8_t>(record.retention)),
             llvm::ConstantInt::get(i64, retainedMetadata.size()),
             metadataPointer, entry});
        auto* descriptorGlobal = new llvm::GlobalVariable(
            *mModule, descriptorType, true, llvm::GlobalValue::InternalLinkage,
            descriptor, "__moon_descriptor_" + suffix);
        descriptorGlobal->setSection(".moon.runtime.descriptor");
        retainedGlobals.push_back(descriptorGlobal);
        descriptorPointers.push_back(descriptorGlobal);
    }

    if (descriptorPointers.empty()) return;
    auto* pointerArrayType = llvm::ArrayType::get(ptr, descriptorPointers.size());
    auto* pointerArray = llvm::ConstantArray::get(pointerArrayType, descriptorPointers);
    auto* registryType = llvm::StructType::get(i64, pointerArrayType);
    auto* registryValue = llvm::ConstantStruct::get(
        registryType,
        {llvm::ConstantInt::get(i64, descriptorPointers.size()), pointerArray});
    std::ostringstream registryName;
    registryName << "__moon_runtime_registry_" << std::hex
                 << stableRuntimeId(mProgram->name);
    auto* registry = new llvm::GlobalVariable(
        *mModule, registryType, true, llvm::GlobalValue::ExternalLinkage,
        registryValue, registryName.str());
    registry->setSection(".moon.runtime.registry");
    retainedGlobals.push_back(registry);
    llvm::appendToCompilerUsed(*mModule, retainedGlobals);
}

// ─── Function generation ───────────────────────────────────────────

void CodeGenerator::generateFunctionBody(FunctionDecl* decl) {
    const std::string internalName = decl->generatedSymbolName.empty()
        ? decl->name : decl->generatedSymbolName;
    auto functionIt = mFunctions.find(internalName);
    auto func = functionIt != mFunctions.end()
        ? functionIt->second : mModule->getFunction(internalName);
    if (!func) return;

    llvm::Type* retLLVMType = decl->returnType
        ? mHelpers->toLLVMType(decl->returnType)
        : mHelpers->voidTy();

    if (!decl->body || decl->isExtern) return;

    mCurrentFunc = func;
    const bool savedKernelMode = mCurrentFunctionIsKernel;
    mCurrentFunctionIsKernel = decl->isKernel;
    mLocals.clear();
    mLocalTypes.clear();
    mLocalKnownUpperBounds.clear();
    mSlotDefaults.clear();
    mCurrentSlotContinuation = nullptr;
    mContinuationFrames.clear();
    mContinuationFrameCounter = 0;
    mCurrentFragmentReturn = nullptr;

    auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
    mBuilder->SetInsertPoint(entryBB);

    // GPU init is only needed when kernels exist. Pure-CPU programs must never
    // reference rt_gpu_* symbols so they materialise on every platform (incl.
    // Windows CI).  When kernels ARE present, the generated main performs its
    // own runtime check so AOT binaries exit clearly when the backend is
    // misconfigured, rather than crashing on a null device pointer.
    if (decl->name == "main" && mProgram && mProgram->features.kernel) {
        auto initialize = mModule->getOrInsertFunction(
            "rt_gpu_initialize", mHelpers->i32Ty());
        auto reportFailure = mModule->getOrInsertFunction(
            "rt_gpu_report_initialization_error", mHelpers->voidTy());
        auto* initialized = mBuilder->CreateCall(initialize, {}, "gpu.initialized");
        auto* readyBB = llvm::BasicBlock::Create(*mCtx, "gpu.ready", func);
        auto* failedBB = llvm::BasicBlock::Create(*mCtx, "gpu.failed", func);
        auto* succeeded = mBuilder->CreateICmpNE(
            initialized, llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.ready.result");
        mBuilder->CreateCondBr(succeeded, readyBB, failedBB);

        mBuilder->SetInsertPoint(failedBB);
        mBuilder->CreateCall(reportFailure);
        if (retLLVMType == mHelpers->voidTy()) mBuilder->CreateRetVoid();
        else if (retLLVMType->isIntegerTy())
            mBuilder->CreateRet(llvm::ConstantInt::get(retLLVMType, 1));
        else
            mBuilder->CreateRet(llvm::Constant::getNullValue(retLLVMType));

        mBuilder->SetInsertPoint(readyBB);
    }

    for (size_t i = 0; i < decl->params.size(); ++i) {
        auto& p = decl->params[i];
        auto* argVal = func->getArg(i);
        auto* alloca = createEntryBlockAlloca(func, argVal->getType(), p.name);
        mBuilder->CreateStore(argVal, alloca);
        mLocals[p.name] = alloca;
        mLocalTypes[p.name] = p.type;
    }

    generateBlock(decl->body.get(), func);

    if (retLLVMType == mHelpers->voidTy() && !mBuilder->GetInsertBlock()->getTerminator()) {
        mBuilder->CreateRetVoid();
    }

    mCurrentFunc = nullptr;
    mCurrentFunctionIsKernel = savedKernelMode;
}

// ─── Statement generation ──────────────────────────────────────────

void CodeGenerator::generateStmt(Stmt* stmt, llvm::Function* func) {
    if (auto* bs = dynamic_cast<BlockStmt*>(stmt)) {
        generateBlock(bs, func);
        return;
    }
    if (auto* ls = dynamic_cast<LetStmt*>(stmt)) {
        llvm::Value* initVal = generateExpr(ls->initializer.get());
        llvm::Type* valType;
        mLocalKnownUpperBounds.erase(ls->name);

        if (dynamic_cast<HeapAllocExpr*>(ls->initializer.get())) {
            auto* varPtr = createEntryBlockAlloca(func, initVal->getType(), ls->name);
            mBuilder->CreateStore(initVal, varPtr);
            mLocals[ls->name] = varPtr;
            if (auto* heap = dynamic_cast<HeapAllocExpr*>(ls->initializer.get()))
                mLocalTypes[ls->name] = heap->allocatedType;
        } else {
            valType = initVal->getType();
            if (valType->isVoidTy()) valType = mHelpers->i32Ty();
            auto* alloca = createEntryBlockAlloca(func, valType, ls->name);
            mBuilder->CreateStore(initVal, alloca);
            mLocals[ls->name] = alloca;
            // Inferred aggregate types (notably array literals) have no AST
            // annotation. Semantic analysis has already canonicalised them in
            // the symbol table; preserve that information for GEP lowering.
            if (ls->type) mLocalTypes[ls->name] = ls->type;
        }
        if (auto upperBound = knownArrayIndexUpperBound(
                ls->initializer.get(), mLocalKnownUpperBounds))
            mLocalKnownUpperBounds[ls->name] = *upperBound;
        return;
    }
    if (auto* slot = dynamic_cast<SlotDeclStmt*>(stmt)) {
        if (!slot->defaultFragment.empty()) {
            const std::string& key = slot->resolvedDefaultFragmentName.empty()
                ? slot->defaultFragment : slot->resolvedDefaultFragmentName;
            auto fragment = mFragments.find(key);
            if (fragment == mFragments.end())
                error("unknown default fragment '" + slot->defaultFragment + "' for slot '" + slot->name + "'");
            else
                mSlotDefaults[slot->name] = fragment->second;
        }
        return;
    }
    if (auto* slot = dynamic_cast<SlotInvokeStmt*>(stmt)) {
        generateSlotInvoke(slot, func);
        return;
    }
    if (auto* apply = dynamic_cast<ApplyStmt*>(stmt)) {
        const std::string& key = apply->resolvedFragmentName.empty()
            ? apply->fragmentName : apply->resolvedFragmentName;
        auto fragment = mFragments.find(key);
        if (fragment == mFragments.end()) {
            error("unknown fragment '" + apply->fragmentName + "' in apply");
            return;
        }
        if (apply->isDynamic) {
            std::vector<FragmentDecl*> candidates{fragment->second};
            for (const auto& resolved : apply->resolvedAlternativeFragmentNames) {
                auto alternative = mFragments.find(resolved);
                if (alternative == mFragments.end()) {
                    error("unknown dynamic fragment candidate '" + resolved + "'");
                    return;
                }
                candidates.push_back(alternative->second);
            }
            if (apply->body) {
                mApplyScopes.emplace_back();
                mDynamicApplyScopes.emplace_back();
                mApplyScopes.back()[apply->slotName] = fragment->second;
                mDynamicApplyScopes.back()[apply->slotName] = std::move(candidates);
                generateBlock(apply->body.get(), func);
                mDynamicApplyScopes.pop_back();
                mApplyScopes.pop_back();
            } else if (!mApplyScopes.empty()) {
                mApplyScopes.back()[apply->slotName] = fragment->second;
                if (mDynamicApplyScopes.empty()) mDynamicApplyScopes.emplace_back();
                mDynamicApplyScopes.back()[apply->slotName] = std::move(candidates);
            }
            return;
        }
        if (apply->body) {
            mApplyScopes.emplace_back();
            mApplyScopes.back()[apply->slotName] = fragment->second;
            generateBlock(apply->body.get(), func);
            mApplyScopes.pop_back();
        } else if (!mApplyScopes.empty()) {
            mApplyScopes.back()[apply->slotName] = fragment->second;
        }
        return;
    }
    if (dynamic_cast<ResumeStmt*>(stmt)) {
        if (!mCurrentSlotContinuation) {
            error("resume() reached code generation without an active slot");
            return;
        }
        generateStructuredContinuation(mCurrentSlotContinuation, func);
        return;
    }
    if (dynamic_cast<AbortStmt*>(stmt)) {
        if (!mCurrentFragmentExit) {
            error("abort() reached code generation without an active fragment");
            return;
        }
        mBuilder->CreateBr(mCurrentFragmentExit);
        return;
    }
    if (auto* await = dynamic_cast<AwaitStmt*>(stmt)) {
        // The simulator completes a launch before returning its event.  A
        // device launch or synchronization can fail, however, and must never
        // be mistaken for a completed event.  Make await the explicit runtime
        // error boundary for both backends.
        if (await->event) {
            auto* event = coerceCallArgument(generateExpr(await->event.get()), mHelpers->i32Ty());
            auto wait = mModule->getOrInsertFunction(
                "rt_gpu_await_event", mHelpers->i32Ty(), mHelpers->i32Ty());
            auto* completed = mBuilder->CreateCall(wait, {event}, "gpu.awaited");
            emitGpuOperationFailureCheck(completed, func);
        }
        return;
    }
    if (auto* rs = dynamic_cast<ReturnStmt*>(stmt)) {
        llvm::Value* retVal = rs->value ? generateExpr(rs->value.get()) : nullptr;
        // Ownership checking records the heap values that are still live on
        // this exact return path.  Emitting cleanup here (rather than at a
        // surrounding block's textual end) also covers returns nested in
        // conditionals and loops without double-freeing values already moved
        // or explicitly freed by that path.
        for (const auto& name : rs->autoFrees) {
            IdentifierExpr cleanup;
            cleanup.name = name;
            llvm::Value* ptr = generateExpr(&cleanup);
            // Only heap-owning values lower to pointers. Stack arrays and
            // borrowed slice fat pointers have no destructor in this phase.
            if (!ptr->getType()->isPointerTy()) continue;
            auto rtFree = mModule->getOrInsertFunction(
                "rt_free", mHelpers->voidTy(), mHelpers->ptrTy());
            mBuilder->CreateCall(rtFree, {mBuilder->CreateBitCast(ptr, mHelpers->ptrTy())});
        }
        if (mCurrentFragmentReturn) {
            // A fragment return ends only the fragment. It does not return
            // from the enclosing Luna function and it does not enter the
            // slot continuation.
            mBuilder->CreateBr(mCurrentFragmentReturn);
            return;
        }
        if (!mContinuationFrames.empty()) {
            // A return in a slot continuation must not return from the LLVM
            // block that happens to contain it. Store the result in the
            // explicit frame and transfer to its return dispatcher. The
            // context's post-resume path is therefore reachable only after a
            // normal continuation completion.
            const auto& frame = mContinuationFrames.back();
            auto* status = mBuilder->CreateStructGEP(frame.llvmType,
                                                     frame.storage, 0,
                                                     "continuation.status");
            if (retVal && frame.returnType && !frame.returnType->isVoidTy()) {
                retVal = coerceCallArgument(retVal, frame.returnType);
                auto* result = mBuilder->CreateStructGEP(frame.llvmType,
                                                         frame.storage, 1,
                                                         "continuation.result");
                mBuilder->CreateStore(retVal, result);
            }
            mBuilder->CreateStore(llvm::ConstantInt::get(
                                      llvm::Type::getInt8Ty(*mCtx), 1), status);
            mBuilder->CreateBr(frame.returnDispatch);
            return;
        }
        if (rs->value) {
            mBuilder->CreateRet(retVal);
        } else {
            mBuilder->CreateRetVoid();
        }
        return;
    }
    if (auto* es = dynamic_cast<ExprStmt*>(stmt)) {
        generateExpr(es->expr.get());
        return;
    }
    if (auto* is_ = dynamic_cast<IfStmt*>(stmt)) {
        llvm::Value* condVal = generateExpr(is_->cond.get());
        condVal = mBuilder->CreateICmpNE(
            condVal, llvm::ConstantInt::get(mHelpers->boolTy(), 0), "ifcond");

        auto* thenBB = llvm::BasicBlock::Create(*mCtx, "then", func);
        // Keep the merge block detached until a branch actually needs it.
        // If both branches return, inserting an empty merge block would leave
        // invalid host IR and make code after the if spuriously reachable.
        auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "ifmerge");
        llvm::BasicBlock* elseBB = nullptr;

        if (is_->elseBranch) {
            elseBB = llvm::BasicBlock::Create(*mCtx, "else");
            mBuilder->CreateCondBr(condVal, thenBB, elseBB);
        } else {
            mBuilder->CreateCondBr(condVal, thenBB, mergeBB);
        }

        mBuilder->SetInsertPoint(thenBB);
        if (auto* thenBlock = dynamic_cast<BlockStmt*>(is_->thenBlock.get())) {
            generateBlock(thenBlock, func);
        }
        const bool thenFallsThrough = !mBuilder->GetInsertBlock()->getTerminator();
        if (thenFallsThrough)
            mBuilder->CreateBr(mergeBB);

        bool elseFallsThrough = true;
        if (elseBB) {
            elseBB->insertInto(func, nullptr);
            mBuilder->SetInsertPoint(elseBB);
            generateStmt(is_->elseBranch.get(), func);
            elseFallsThrough = !mBuilder->GetInsertBlock()->getTerminator();
            if (elseFallsThrough)
                mBuilder->CreateBr(mergeBB);
        }

        const bool mergeIsNeeded = !is_->elseBranch || thenFallsThrough || elseFallsThrough;

        if (mergeIsNeeded) {
            mergeBB->insertInto(func, nullptr);
            mBuilder->SetInsertPoint(mergeBB);
        } else {
            // Neither branch reaches a continuation.  Leave the builder on a
            // terminated block so generateBlock stops before unreachable
            // statements, and destroy the unused detached merge block.
            delete mergeBB;
            mBuilder->SetInsertPoint(elseBB);
        }
        return;
    }
    if (auto* ws = dynamic_cast<WhileStmt*>(stmt)) {
        auto* condBB = llvm::BasicBlock::Create(*mCtx, "whilecond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*mCtx, "whilebody");
        auto* exitBB = llvm::BasicBlock::Create(*mCtx, "whileexit");

        mBuilder->CreateBr(condBB);
        mBuilder->SetInsertPoint(condBB);
        llvm::Value* condVal = generateExpr(ws->cond.get());
        condVal = mBuilder->CreateICmpNE(
            condVal, llvm::ConstantInt::get(mHelpers->boolTy(), 0), "whilecond_val");
        mBuilder->CreateCondBr(condVal, bodyBB, exitBB);

        exitBB->insertInto(func, nullptr);
        bodyBB->insertInto(func, exitBB);
        mBuilder->SetInsertPoint(bodyBB);
        generateBlock(ws->body.get(), func);
        if (!mBuilder->GetInsertBlock()->getTerminator())
            mBuilder->CreateBr(condBB);

        mBuilder->SetInsertPoint(exitBB);
        return;
    }
    if (auto* fs = dynamic_cast<ForStmt*>(stmt)) {
        // Simple fixed-iteration for-loop: iterates 10 times
        // Create counter on stack
        auto* counterAlloca = createEntryBlockAlloca(func, mHelpers->i32Ty(), fs->varName + "_cnt");
        mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->i32Ty(), 0), counterAlloca);
        mLocals[fs->varName] = counterAlloca;

        auto* condBB = llvm::BasicBlock::Create(*mCtx, "forcond", func);
        auto* bodyBB = llvm::BasicBlock::Create(*mCtx, "forbody");
        auto* exitBB = llvm::BasicBlock::Create(*mCtx, "forexit");

        mBuilder->CreateBr(condBB);
        mBuilder->SetInsertPoint(condBB);
        llvm::Value* cntVal = mBuilder->CreateLoad(mHelpers->i32Ty(), counterAlloca, "cnt");
        llvm::Value* condVal = mBuilder->CreateICmpSLT(cntVal,
            llvm::ConstantInt::get(mHelpers->i32Ty(), 10), "forcond_val");
        mBuilder->CreateCondBr(condVal, bodyBB, exitBB);

        exitBB->insertInto(func, nullptr);
        bodyBB->insertInto(func, exitBB);
        mBuilder->SetInsertPoint(bodyBB);
        generateBlock(fs->body.get(), func);
        // Increment counter
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            llvm::Value* cnt2 = mBuilder->CreateLoad(mHelpers->i32Ty(), counterAlloca, "cnt2");
            llvm::Value* inc = mBuilder->CreateAdd(cnt2,
                llvm::ConstantInt::get(mHelpers->i32Ty(), 1), "inc");
            mBuilder->CreateStore(inc, counterAlloca);
            mBuilder->CreateBr(condBB);
        }

        mBuilder->SetInsertPoint(exitBB);
        mLocals.erase(fs->varName);
        return;
    }
    if (auto* freeStmt = dynamic_cast<FreeStmt*>(stmt)) {
        llvm::Value* ptr = generateExpr(freeStmt->operand.get());
        auto rtFree = mModule->getOrInsertFunction(
            "rt_free", mHelpers->voidTy(), mHelpers->ptrTy());
        mBuilder->CreateCall(rtFree, { mBuilder->CreateBitCast(ptr, mHelpers->ptrTy()) });
        return;
    }
}

void CodeGenerator::generateBlock(BlockStmt* block, llvm::Function* func) {
    mApplyScopes.emplace_back();
    mDynamicApplyScopes.emplace_back();
    for (auto& stmt : block->stmts) {
        generateStmt(stmt.get(), func);
        if (mBuilder->GetInsertBlock()->getTerminator()) break;
    }
    mApplyScopes.pop_back();
    mDynamicApplyScopes.pop_back();
}

void CodeGenerator::generateStructuredContinuation(BlockStmt* continuation,
                                                    llvm::Function* func) {
    llvm::Type* returnType = mCurrentFunc ? mCurrentFunc->getReturnType()
                                          : mHelpers->voidTy();
    std::vector<llvm::Type*> frameFields{
        llvm::Type::getInt8Ty(*mCtx) // 0 = normal, 1 = function return
    };
    if (!returnType->isVoidTy()) frameFields.push_back(returnType);

    auto* frameType = llvm::StructType::create(
        *mCtx, frameFields,
        "luna.continuation.frame." + std::to_string(mContinuationFrameCounter++));
    auto* storage = createEntryBlockAlloca(func, frameType, "continuation.frame");
    auto* status = mBuilder->CreateStructGEP(frameType, storage, 0,
                                             "continuation.status");
    mBuilder->CreateStore(llvm::ConstantInt::get(
                              llvm::Type::getInt8Ty(*mCtx), 0), status);

    auto* continuationEntry = llvm::BasicBlock::Create(
        *mCtx, "continuation.entry", func);
    auto* normalExit = llvm::BasicBlock::Create(
        *mCtx, "continuation.normal", func);
    auto* returnDispatch = llvm::BasicBlock::Create(
        *mCtx, "continuation.return.dispatch", func);
    auto* returnValue = llvm::BasicBlock::Create(
        *mCtx, "continuation.return", func);
    auto* invalidState = llvm::BasicBlock::Create(
        *mCtx, "continuation.invalid", func);

    ContinuationFrame frame{storage, frameType, returnDispatch, returnType};
    mBuilder->CreateBr(continuationEntry);
    mBuilder->SetInsertPoint(continuationEntry);
    auto* savedFragmentReturn = mCurrentFragmentReturn;
    mCurrentFragmentReturn = nullptr;
    mContinuationFrames.push_back(frame);
    generateBlock(continuation, func);
    mContinuationFrames.pop_back();
    mCurrentFragmentReturn = savedFragmentReturn;

    if (!mBuilder->GetInsertBlock()->getTerminator()) {
        auto* normalStatus = mBuilder->CreateStructGEP(
            frameType, storage, 0, "continuation.status");
        mBuilder->CreateStore(llvm::ConstantInt::get(
                                  llvm::Type::getInt8Ty(*mCtx), 0), normalStatus);
        mBuilder->CreateBr(normalExit);
    }

    mBuilder->SetInsertPoint(returnDispatch);
    auto* dispatchStatus = mBuilder->CreateLoad(
        llvm::Type::getInt8Ty(*mCtx),
        mBuilder->CreateStructGEP(frameType, storage, 0, "continuation.status"),
        "continuation.state");
    auto* isReturn = mBuilder->CreateICmpEQ(
        dispatchStatus, llvm::ConstantInt::get(llvm::Type::getInt8Ty(*mCtx), 1),
        "continuation.is_return");
    mBuilder->CreateCondBr(isReturn, returnValue, invalidState);

    mBuilder->SetInsertPoint(returnValue);
    if (returnType->isVoidTy()) {
        mBuilder->CreateRetVoid();
    } else {
        auto* result = mBuilder->CreateLoad(
            returnType,
            mBuilder->CreateStructGEP(frameType, storage, 1,
                                      "continuation.result"),
            "continuation.return.value");
        mBuilder->CreateRet(result);
    }

    mBuilder->SetInsertPoint(invalidState);
    auto abort = mModule->getOrInsertFunction("abort", mHelpers->voidTy());
    mBuilder->CreateCall(abort);
    mBuilder->CreateUnreachable();

    mBuilder->SetInsertPoint(normalExit);
}

void CodeGenerator::generateSlotInvoke(SlotInvokeStmt* slot, llvm::Function* func) {
    for (auto it = mDynamicApplyScopes.rbegin(); it != mDynamicApplyScopes.rend(); ++it) {
        auto dynamic = it->find(slot->name);
        if (dynamic != it->end()) {
            generateDynamicFragmentDispatch(dynamic->second, slot, func);
            return;
        }
    }
    FragmentDecl* fragment = nullptr;
    for (auto it = mApplyScopes.rbegin(); it != mApplyScopes.rend(); ++it) {
        auto applied = it->find(slot->name);
        if (applied != it->end()) {
            fragment = applied->second;
            break;
        }
    }
    if (!fragment && !slot->defaultFragment.empty()) {
        const std::string& key = slot->resolvedDefaultFragmentName.empty()
            ? slot->defaultFragment : slot->resolvedDefaultFragmentName;
        auto found = mFragments.find(key);
        if (found != mFragments.end()) fragment = found->second;
    }
    if (!fragment) {
        auto fallback = mSlotDefaults.find(slot->name);
        if (fallback != mSlotDefaults.end()) fragment = fallback->second;
    }
    if (!fragment) {
        generateBlock(slot->continuation.get(), func);
        return;
    }
    generateFragmentInline(fragment, slot, func);
}

void CodeGenerator::generateDynamicFragmentDispatch(
    const std::vector<FragmentDecl*>& candidates, SlotInvokeStmt* slot, llvm::Function* func) {
    if (candidates.empty()) {
        error("dynamic slot '" + slot->name + "' has no fragment candidates");
        return;
    }

    auto select = mModule->getOrInsertFunction(
        "rt_dynamic_fragment_select", mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto matches = mModule->getOrInsertFunction(
        "rt_dynamic_fragment_matches", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto* slotName = mBuilder->CreateGlobalString(slot->name, "dynamic.slot");
    const std::string fallbackName = candidates.front()->name;
    auto* fallback = mBuilder->CreateGlobalString(fallbackName, "dynamic.fallback");
    auto* selected = mBuilder->CreateCall(select, {slotName, fallback}, "dynamic.fragment");

    auto* merge = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.merge");
    bool reachesMerge = false;
    for (size_t index = 0; index < candidates.size(); ++index) {
        auto* candidateBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.candidate", func);
        auto* nextBlock = index + 1 < candidates.size()
            ? llvm::BasicBlock::Create(*mCtx, "dynamic.apply.next", func)
            : llvm::BasicBlock::Create(*mCtx, "dynamic.apply.unknown", func);
        auto* candidateName = mBuilder->CreateGlobalString(candidates[index]->name, "dynamic.candidate");
        auto* isCandidate = mBuilder->CreateICmpNE(
            mBuilder->CreateCall(matches, {selected, candidateName}, "dynamic.matches"),
            llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "dynamic.selected");
        mBuilder->CreateCondBr(isCandidate, candidateBlock, nextBlock);

        mBuilder->SetInsertPoint(candidateBlock);
        generateFragmentInline(candidates[index], slot, func);
        if (!mBuilder->GetInsertBlock()->getTerminator()) {
            mBuilder->CreateBr(merge);
            reachesMerge = true;
        }

        mBuilder->SetInsertPoint(nextBlock);
    }

    // A name not present in the statically linked candidate set may still be
    // supplied by an external v1 plugin.  The plugin path is intentionally a
    // separate ABI: it receives only explicit argument addresses and returns
    // continue/abort.  It never receives the generated stack continuation.
    auto* externalCandidate = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external", func);
    auto* externalFailure = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.failure", func);
    auto externalInvocation = generateExternalFragmentInvocation(slot, func, selected);
    auto invoke = mModule->getOrInsertFunction(
        "rt_fragment_plugin_invoke", mHelpers->i32Ty(),
        mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy(), mHelpers->ptrTy());
    auto* pluginAction = mBuilder->CreateCall(invoke, externalInvocation, "external.fragment.action");
    auto* continueBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.continue", func);
    auto* abortBlock = llvm::BasicBlock::Create(*mCtx, "dynamic.apply.external.abort", func);
    auto* isContinue = mBuilder->CreateICmpEQ(
        pluginAction, llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_CONTINUE),
        "external.fragment.continue");
    mBuilder->CreateCondBr(isContinue, continueBlock, externalCandidate);

    mBuilder->SetInsertPoint(externalCandidate);
    auto* isAbort = mBuilder->CreateICmpEQ(
        pluginAction, llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_ABORT),
        "external.fragment.abort");
    mBuilder->CreateCondBr(isAbort, abortBlock, externalFailure);
    mBuilder->SetInsertPoint(continueBlock);
    generateBlock(slot->continuation.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator()) {
        mBuilder->CreateBr(merge);
        reachesMerge = true;
    }
    mBuilder->SetInsertPoint(abortBlock);
    mBuilder->CreateBr(merge);
    reachesMerge = true;
    mBuilder->SetInsertPoint(externalFailure);
    auto reportPluginFailure = mModule->getOrInsertFunction(
        "rt_fragment_plugin_report_error_and_abort", mHelpers->voidTy());
    mBuilder->CreateCall(reportPluginFailure);
    mBuilder->CreateUnreachable();
    if (reachesMerge) {
        merge->insertInto(func, nullptr);
        mBuilder->SetInsertPoint(merge);
    } else {
        delete merge;
    }
}

static std::string externalFragmentContract(const SlotInvokeStmt* slot) {
    std::string contract = "luna.slot." + slot->name + ".";
    contract += slot->acceptedKind == FragmentKind::Interceptor ? "interceptor" : "context";
    contract += slot->acceptedCardinality == FragmentCardinality::Many ? ".many" : ".once";
    if (slot->structuralType) {
        for (const auto& parameter : slot->structuralType->paramTypes)
            contract += "." + (parameter ? parameter->toString() : "unknown");
    }
    return contract + ".v1";
}

std::array<llvm::Value*, 4> CodeGenerator::generateExternalFragmentInvocation(
    SlotInvokeStmt* slot, llvm::Function* func, llvm::Value* selected) {
    auto* slotName = mBuilder->CreateGlobalString(slot->name, "external.slot");
    auto* contract = mBuilder->CreateGlobalString(externalFragmentContract(slot), "external.contract");
    auto* invocationType = llvm::StructType::get(
        *mCtx, {mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->sizeTy()});
    auto* invocationStorage = createEntryBlockAlloca(func, invocationType, "external.invocation");

    llvm::Value* argumentArray = llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(mHelpers->ptrTy()));
    if (!slot->args.empty()) {
        auto* arrayType = llvm::ArrayType::get(mHelpers->ptrTy(), slot->args.size());
        auto* arrayStorage = createEntryBlockAlloca(func, arrayType, "external.args");
        for (size_t i = 0; i < slot->args.size(); ++i) {
            auto* value = generateExpr(slot->args[i].get());
            auto* valueStorage = createEntryBlockAlloca(func, value->getType(), "external.arg");
            mBuilder->CreateStore(value, valueStorage);
            auto* element = mBuilder->CreateInBoundsGEP(
                arrayType, arrayStorage,
                {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                 llvm::ConstantInt::get(mHelpers->i32Ty(), i)}, "external.arg.ptr");
            mBuilder->CreateStore(valueStorage, element);
        }
        argumentArray = mBuilder->CreateInBoundsGEP(
            arrayType, arrayStorage,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "external.args.ptr");
    }
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->i32Ty(), LUNA_FRAGMENT_PLUGIN_ABI_V1),
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 0));
    mBuilder->CreateStore(argumentArray,
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 1));
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->sizeTy(), slot->args.size()),
                          mBuilder->CreateStructGEP(invocationType, invocationStorage, 2));

    // Return the four C-call arguments; the selected string is installed by
    // generateDynamicFragmentDispatch immediately before the call.
    return {slotName, selected, contract, invocationStorage};
}

void CodeGenerator::generateFragmentInline(FragmentDecl* fragment, SlotInvokeStmt* slot,
                                            llvm::Function* func) {
    auto savedLocals = mLocals;
    auto savedTypes = mLocalTypes;
    auto savedUpperBounds = mLocalKnownUpperBounds;
    auto* savedContinuation = mCurrentSlotContinuation;
    auto* savedFragmentExit = mCurrentFragmentExit;
    auto* savedFragmentReturn = mCurrentFragmentReturn;
    auto* fragmentExit = llvm::BasicBlock::Create(*mCtx, "fragment.exit", func);

    for (size_t i = 0; i < fragment->params.size(); ++i) {
        llvm::Value* value = nullptr;
        TypePtr type;
        if (i < slot->args.size()) {
            value = generateExpr(slot->args[i].get());
            type = fragment->params[i].type;
        } else if (i < slot->resolvedParamNames.size()) {
            auto outer = mLocals.find(slot->resolvedParamNames[i]);
            if (outer != mLocals.end()) {
                value = mBuilder->CreateLoad(outer->second->getAllocatedType(), outer->second,
                                             fragment->params[i].name + ".slotarg");
                auto outerType = mLocalTypes.find(slot->resolvedParamNames[i]);
                if (outerType != mLocalTypes.end()) type = outerType->second;
            }
        }
        if (!value) {
            error("cannot materialize argument " + std::to_string(i + 1) +
                  " for fragment '" + fragment->name + "'");
            mLocals = std::move(savedLocals);
            mLocalTypes = std::move(savedTypes);
            mLocalKnownUpperBounds = std::move(savedUpperBounds);
            return;
        }
        auto* alloca = createEntryBlockAlloca(func, value->getType(),
                                              "__fragment_" + fragment->params[i].name);
        mBuilder->CreateStore(value, alloca);
        mLocals[fragment->params[i].name] = alloca;
        mLocalTypes[fragment->params[i].name] = type;
    }

    mCurrentSlotContinuation = slot->continuation.get();
    mCurrentFragmentExit = fragmentExit;
    mCurrentFragmentReturn = fragmentExit;
    generateBlock(fragment->body.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator() &&
        fragment->kind == FragmentKind::Interceptor)
        generateBlock(slot->continuation.get(), func);
    if (!mBuilder->GetInsertBlock()->getTerminator()) mBuilder->CreateBr(fragmentExit);
    mBuilder->SetInsertPoint(fragmentExit);
    mCurrentSlotContinuation = savedContinuation;
    mCurrentFragmentExit = savedFragmentExit;
    mCurrentFragmentReturn = savedFragmentReturn;
    mLocals = std::move(savedLocals);
    mLocalTypes = std::move(savedTypes);
    mLocalKnownUpperBounds = std::move(savedUpperBounds);
}

// ─── Expression generation ─────────────────────────────────────────

llvm::Value* CodeGenerator::generateExpr(Expr* expr) {
    if (auto* il = dynamic_cast<IntLiteralExpr*>(expr)) {
        return llvm::ConstantInt::get(mHelpers->i32Ty(), il->value, true);
    }
    if (auto* fl = dynamic_cast<FloatLiteralExpr*>(expr)) {
        return llvm::ConstantFP::get(mHelpers->f64Ty(), fl->value);
    }
    if (auto* sl = dynamic_cast<StringLiteralExpr*>(expr)) {
        auto* gvar = mBuilder->CreateGlobalString(sl->value, "str");
        return mBuilder->CreateGEP(
            gvar->getValueType(), gvar,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "strptr");
    }
    if (auto* bl = dynamic_cast<BoolLiteralExpr*>(expr)) {
        return llvm::ConstantInt::get(mHelpers->boolTy(), bl->value ? 1 : 0);
    }
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto it = mLocals.find(id->name);
        if (it != mLocals.end()) {
            auto* alloca = it->second;
            return mBuilder->CreateLoad(alloca->getAllocatedType(), alloca, id->name);
        }
        auto function = mFunctions.find(id->name);
        if (function != mFunctions.end()) return function->second;
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    if (auto* selection = dynamic_cast<DynamicSelectExpr*>(expr)) {
        auto* opaquePointerType = llvm::cast<llvm::PointerType>(mHelpers->ptrTy());
        std::vector<llvm::Value*> filterValues;
        for (auto& argument : selection->filterArguments)
            filterValues.push_back(generateExpr(argument.get()));

        llvm::Value* selected = llvm::ConstantPointerNull::get(opaquePointerType);
        llvm::Value* matchCount = llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        for (const auto& candidate : selection->candidates) {
            llvm::Value* matches = llvm::ConstantInt::getTrue(*mCtx);
            for (size_t index = 0; index < filterValues.size(); ++index) {
                llvm::Value* actual = filterValues[index];
                const auto& expectedValue = candidate.metadataValues[index];
                llvm::Value* equal = nullptr;
                if (auto* integer = std::get_if<int64_t>(&expectedValue)) {
                    if (!actual->getType()->isIntegerTy()) {
                        error("dynamic selector integer metadata type mismatch");
                        return llvm::ConstantPointerNull::get(opaquePointerType);
                    }
                    auto* expected = llvm::ConstantInt::get(
                        actual->getType(), static_cast<uint64_t>(*integer), true);
                    equal = mBuilder->CreateICmpEQ(actual, expected, "dynamic.meta.eq");
                } else if (auto* floating = std::get_if<double>(&expectedValue)) {
                    if (!actual->getType()->isFloatingPointTy()) {
                        error("dynamic selector floating metadata type mismatch");
                        return llvm::ConstantPointerNull::get(opaquePointerType);
                    }
                    auto* expected = llvm::ConstantFP::get(actual->getType(), *floating);
                    equal = mBuilder->CreateFCmpOEQ(actual, expected, "dynamic.meta.eq");
                } else if (auto* boolean = std::get_if<bool>(&expectedValue)) {
                    if (!actual->getType()->isIntegerTy()) {
                        error("dynamic selector boolean metadata type mismatch");
                        return llvm::ConstantPointerNull::get(opaquePointerType);
                    }
                    auto* expected = llvm::ConstantInt::get(
                        actual->getType(), *boolean ? 1 : 0);
                    equal = mBuilder->CreateICmpEQ(actual, expected, "dynamic.meta.eq");
                } else {
                    const auto& string = std::get<std::string>(expectedValue);
                    if (!actual->getType()->isPointerTy()) {
                        error("dynamic selector string metadata type mismatch");
                        return llvm::ConstantPointerNull::get(opaquePointerType);
                    }
                    auto* storage = mBuilder->CreateGlobalString(
                        string, "dynamic.meta.string");
                    auto* expected = mBuilder->CreateInBoundsGEP(
                        storage->getValueType(), storage,
                        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                         llvm::ConstantInt::get(mHelpers->i32Ty(), 0)});
                    auto compare = mModule->getOrInsertFunction(
                        "strcmp", mHelpers->i32Ty(), mHelpers->ptrTy(),
                        mHelpers->ptrTy());
                    auto* compared = mBuilder->CreateCall(
                        compare, {actual, expected}, "dynamic.meta.strcmp");
                    equal = mBuilder->CreateICmpEQ(
                        compared, llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                        "dynamic.meta.eq");
                }
                matches = mBuilder->CreateAnd(matches, equal, "dynamic.meta.all");
            }
            auto function = mFunctions.find(candidate.linkageName);
            if (function == mFunctions.end()) {
                error("dynamic select candidate '" + candidate.linkageName +
                      "' has no generated function");
                return llvm::ConstantPointerNull::get(opaquePointerType);
            }
            selected = mBuilder->CreateSelect(matches, function->second, selected,
                                              "dynamic.selected");
            matchCount = mBuilder->CreateAdd(
                matchCount, mBuilder->CreateZExt(matches, mHelpers->i32Ty()),
                "dynamic.match.count");
        }

        auto* valid = mBuilder->CreateICmpEQ(
            matchCount, llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
            "dynamic.select.unique");
        auto* success = llvm::BasicBlock::Create(
            *mCtx, "dynamic.select.success", mCurrentFunc);
        auto* failure = llvm::BasicBlock::Create(
            *mCtx, "dynamic.select.failure", mCurrentFunc);
        mBuilder->CreateCondBr(valid, success, failure);
        mBuilder->SetInsertPoint(failure);
        auto abort = mModule->getOrInsertFunction("abort", mHelpers->voidTy());
        mBuilder->CreateCall(abort);
        mBuilder->CreateUnreachable();
        mBuilder->SetInsertPoint(success);
        return selected;
    }
    if (auto* bin = dynamic_cast<BinaryExpr*>(expr)) {
        llvm::Value* lhs = generateExpr(bin->lhs.get());
        if (bin->op == Operator::LogicalAnd || bin->op == Operator::LogicalOr) {
            // `&&` and `||` are control-flow operators, not integer bitwise
            // aliases.  Lower them through a small CFG so the right-hand
            // expression is evaluated only when it is semantically needed.
            auto* originBB = mBuilder->GetInsertBlock();
            auto* rhsBB = llvm::BasicBlock::Create(*mCtx, "logic.rhs", mCurrentFunc);
            auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "logic.merge", mCurrentFunc);
            if (bin->op == Operator::LogicalAnd)
                mBuilder->CreateCondBr(lhs, rhsBB, mergeBB);
            else
                mBuilder->CreateCondBr(lhs, mergeBB, rhsBB);

            mBuilder->SetInsertPoint(rhsBB);
            llvm::Value* rhs = generateExpr(bin->rhs.get());
            auto* rhsEndBB = mBuilder->GetInsertBlock();
            if (!rhsEndBB->getTerminator()) mBuilder->CreateBr(mergeBB);

            mBuilder->SetInsertPoint(mergeBB);
            auto* result = mBuilder->CreatePHI(mHelpers->boolTy(), 2,
                                               bin->op == Operator::LogicalAnd
                                                   ? "and.shortcircuit"
                                                   : "or.shortcircuit");
            result->addIncoming(llvm::ConstantInt::get(
                mHelpers->boolTy(), bin->op == Operator::LogicalAnd ? 0 : 1), originBB);
            result->addIncoming(rhs, rhsEndBB);
            return result;
        }
        llvm::Value* rhs = generateExpr(bin->rhs.get());
        switch (bin->op) {
            case Operator::Add:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFAdd(lhs, rhs, "addtmp")
                    : mBuilder->CreateAdd(lhs, rhs, "addtmp");
            case Operator::Subtract:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFSub(lhs, rhs, "subtmp")
                    : mBuilder->CreateSub(lhs, rhs, "subtmp");
            case Operator::Multiply:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFMul(lhs, rhs, "multmp")
                    : mBuilder->CreateMul(lhs, rhs, "multmp");
            case Operator::Divide:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFDiv(lhs, rhs, "divtmp")
                    : mBuilder->CreateSDiv(lhs, rhs, "divtmp");
            case Operator::Remainder:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFRem(lhs, rhs, "remtmp")
                    : mBuilder->CreateSRem(lhs, rhs, "remtmp");
            case Operator::BitAnd:
                return mBuilder->CreateAnd(lhs, rhs, "bitandtmp");
            case Operator::BitOr:
                return mBuilder->CreateOr(lhs, rhs, "bitortmp");
            case Operator::BitXor:
                return mBuilder->CreateXor(lhs, rhs, "bitxortmp");
            case Operator::ShiftLeft:
                return mBuilder->CreateShl(lhs, rhs, "shltmp");
            case Operator::ShiftRight:
                return mBuilder->CreateAShr(lhs, rhs, "shrtmp");
            case Operator::Equal:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpOEQ(lhs, rhs, "eqtmp")
                    : mBuilder->CreateICmpEQ(lhs, rhs, "eqtmp");
            case Operator::NotEqual:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpONE(lhs, rhs, "neqtmp")
                    : mBuilder->CreateICmpNE(lhs, rhs, "neqtmp");
            case Operator::Less:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpOLT(lhs, rhs, "lttmp")
                    : mBuilder->CreateICmpSLT(lhs, rhs, "lttmp");
            case Operator::LessEqual:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpOLE(lhs, rhs, "letmp")
                    : mBuilder->CreateICmpSLE(lhs, rhs, "letmp");
            case Operator::Greater:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpOGT(lhs, rhs, "gttmp")
                    : mBuilder->CreateICmpSGT(lhs, rhs, "gttmp");
            case Operator::GreaterEqual:
                return lhs->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFCmpOGE(lhs, rhs, "getmp")
                    : mBuilder->CreateICmpSGE(lhs, rhs, "getmp");
            default: break;
        }
    }
    if (auto* un = dynamic_cast<UnaryExpr*>(expr)) {
        llvm::Value* op = generateExpr(un->operand.get());
        switch (un->op) {
            case Operator::Negate:
                return op->getType()->isFloatingPointTy()
                    ? mBuilder->CreateFNeg(op, "negtmp")
                    : mBuilder->CreateNeg(op, "negtmp");
            case Operator::LogicalNot:
                return mBuilder->CreateNot(op, "nottmp");
            case Operator::BitNot:
                return mBuilder->CreateNot(op, "bitnottmp");
            case Operator::Dereference: {
                auto* ptrTy = llvm::PointerType::get(*mCtx, 0);
                auto* ptr = mBuilder->CreateBitCast(op, ptrTy);
                return mBuilder->CreateLoad(mHelpers->i32Ty(), ptr, "dereftmp");
            }
            default: break;
        }
    }
    if (auto* variant = dynamic_cast<VariantConstructExpr*>(expr)) {
        // Enum values use an opaque heap representation for now. The
        // semantic layer has already checked the nominal enum and payload;
        // pattern matching/tag lowering can build on this allocation later.
        auto rtMalloc = mModule->getOrInsertFunction(
            "rt_malloc", mHelpers->ptrTy(), mHelpers->sizeTy());
        auto sizeVal = llvm::ConstantInt::get(
            mHelpers->sizeTy(), typeSize(variant->constructedType));
        for (auto& arg : variant->args) generateExpr(arg.get());
        return mBuilder->CreateCall(rtMalloc, {sizeVal}, "enumalloc");
    }
    if (auto* field = dynamic_cast<FieldAccessExpr*>(expr)) {
        TypePtr objectType;
        if (auto* id = dynamic_cast<IdentifierExpr*>(field->object.get())) {
            auto it = mLocalTypes.find(id->name);
            if (it != mLocalTypes.end()) objectType = it->second;
        }
        if (objectType && objectType->kind == TypeKind::Reference)
            objectType = objectType->inner;
        size_t index = fieldIndex(objectType, field->field);
        if (!objectType || index == static_cast<size_t>(-1))
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);

        llvm::Value* object = generateExpr(field->object.get());
        uint64_t offset = 0;
        for (size_t i = 0; i < index; ++i)
            offset += typeSize(objectType->fields[i].type);
        auto* bytePtr = mBuilder->CreateGEP(
            llvm::Type::getInt8Ty(*mCtx), object,
            llvm::ConstantInt::get(mHelpers->sizeTy(), offset), "fieldptr");
        auto fieldType = mHelpers->toLLVMType(objectType->fields[index].type);
        auto* typedPtr = mBuilder->CreateBitCast(
            bytePtr, llvm::PointerType::get(*mCtx, 0), "typedfieldptr");
        return mBuilder->CreateLoad(fieldType, typedPtr, field->field);
    }
    if (auto* array = dynamic_cast<ArrayLiteralExpr*>(expr)) {
        TypePtr arrayType = Type::makeArray(array->elementType, array->elements.size());
        auto* llvmArray = llvm::cast<llvm::ArrayType>(mHelpers->toLLVMType(arrayType));
        llvm::Value* result = llvm::UndefValue::get(llvmArray);
        for (size_t i = 0; i < array->elements.size(); ++i)
            result = mBuilder->CreateInsertValue(result, generateExpr(array->elements[i].get()),
                                                  {static_cast<unsigned>(i)}, "array.init");
        return result;
    }
    if (auto* index = dynamic_cast<IndexExpr*>(expr)) {
        auto* id = dynamic_cast<IdentifierExpr*>(index->object.get());
        if (!id || !mLocals.count(id->name)) {
            error("safe array indexing currently requires a local array binding");
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        }
        TypePtr arrayType = mLocalTypes[id->name];
        if (!arrayType || (arrayType->kind != TypeKind::Array && arrayType->kind != TypeKind::Slice))
            return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
        if (arrayType->kind == TypeKind::Slice) {
            auto* slice = generateExpr(index->object.get());
            auto* length = mBuilder->CreateExtractValue(slice, {1}, "slice.length");
            auto* checked = mBuilder->CreateCall(mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()), {coerceCallArgument(generateExpr(index->index.get()), mHelpers->i32Ty()), length}, "slice.index");
            auto* data = mBuilder->CreateExtractValue(slice, {0}, "slice.data");
            auto* ptr = mBuilder->CreateGEP(mHelpers->toLLVMType(arrayType->inner), data, checked, "slice.element");
            return mBuilder->CreateLoad(mHelpers->toLLVMType(arrayType->inner), ptr, "slice.load");
        }
        auto* rawIndex = coerceCallArgument(
            generateExpr(index->index.get()), mHelpers->i32Ty());
        llvm::Value* checked = rawIndex;
        if (!isProvablySafeArrayIndex(index->index.get(), arrayType->arrayLength,
                                      mLocalKnownUpperBounds))
            checked = mBuilder->CreateCall(mModule->getOrInsertFunction(
                "rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()),
                {rawIndex, llvm::ConstantInt::get(mHelpers->sizeTy(), arrayType->arrayLength)},
                "array.index");
        auto* elementPtr = mBuilder->CreateInBoundsGEP(
            mLocals[id->name]->getAllocatedType(), mLocals[id->name],
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked}, "array.element");
        return mBuilder->CreateLoad(mHelpers->toLLVMType(arrayType->inner), elementPtr, "array.load");
    }
    if (auto* launch = dynamic_cast<LaunchExpr*>(expr)) return generateLaunch(launch);
    if (auto* call = dynamic_cast<CallExpr*>(expr)) {
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get()); calleeId && calleeId->name == "slice" && call->args.size() == 3) {
            auto* source = generateExpr(call->args[0].get());
            auto* start = coerceCallArgument(generateExpr(call->args[1].get()), mHelpers->i32Ty());
            auto* end = coerceCallArgument(generateExpr(call->args[2].get()), mHelpers->i32Ty());
            // Semantic analysis guarantees a borrowed local array. Its extent is recovered from that binding.
            uint64_t length = 0;
            if (auto* b = dynamic_cast<BorrowExpr*>(call->args[0].get())) if (auto* id = dynamic_cast<IdentifierExpr*>(b->operand.get())) length = mLocalTypes[id->name]->arrayLength;
            auto* checkedStart = mBuilder->CreateCall(mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()), {start, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.start");
            auto* checkedEnd = mBuilder->CreateCall(mModule->getOrInsertFunction("rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()), {end, llvm::ConstantInt::get(mHelpers->sizeTy(), length + 1)}, "slice.end");
            auto* valid = mBuilder->CreateICmpSLE(checkedStart, checkedEnd, "slice.order");
            auto* ok = llvm::BasicBlock::Create(*mCtx, "slice.ok", mCurrentFunc); auto* bad = llvm::BasicBlock::Create(*mCtx, "slice.bad", mCurrentFunc);
            mBuilder->CreateCondBr(valid, ok, bad); mBuilder->SetInsertPoint(bad); mBuilder->CreateCall(mModule->getOrInsertFunction("abort", mHelpers->voidTy())); mBuilder->CreateUnreachable(); mBuilder->SetInsertPoint(ok);
            llvm::Value* data = source;
            if (auto* b = dynamic_cast<BorrowExpr*>(call->args[0].get())) {
                if (auto* id = dynamic_cast<IdentifierExpr*>(b->operand.get()); id && mLocals.count(id->name))
                    data = mBuilder->CreateInBoundsGEP(mLocals[id->name]->getAllocatedType(), mLocals[id->name],
                        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checkedStart}, "slice.data");
            }
            auto* sliceTy = llvm::StructType::get(*mCtx, {mHelpers->ptrTy(), mHelpers->sizeTy()}); llvm::Value* value = llvm::UndefValue::get(sliceTy);
            value = mBuilder->CreateInsertValue(value, data, {0}); return mBuilder->CreateInsertValue(value, mBuilder->CreateSExtOrTrunc(mBuilder->CreateSub(checkedEnd, checkedStart), mHelpers->sizeTy()), {1});
        }
        if (call->compileTimeValue) {
            if (auto* integer = std::get_if<int64_t>(&*call->compileTimeValue))
                return llvm::ConstantInt::get(mHelpers->i32Ty(), *integer, true);
            if (auto* floating = std::get_if<double>(&*call->compileTimeValue))
                return llvm::ConstantFP::get(mHelpers->f64Ty(), *floating);
            if (auto* boolean = std::get_if<bool>(&*call->compileTimeValue))
                return llvm::ConstantInt::get(mHelpers->boolTy(), *boolean ? 1 : 0);
            if (auto* string = std::get_if<std::string>(&*call->compileTimeValue)) {
                auto* global = mBuilder->CreateGlobalString(*string, "ctstr");
                return mBuilder->CreateGEP(
                    global->getValueType(), global,
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
                     llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "ctstrptr");
            }
        }
        // Device built-ins use the runtime boundary on the host. Kernel bodies
        // retain direct element operations so that the same LLVM function can
        // be cloned to PTX; the CPU simulator invokes that host form directly.
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            if (calleeId->name == "gpu_alloc_i32" && call->args.size() == 1) {
                auto alloc = mModule->getOrInsertFunction(
                    "rt_gpu_alloc_i32", mHelpers->ptrTy(), mHelpers->sizeTy());
                auto* count = coerceCallArgument(generateExpr(call->args[0].get()),
                                                  mHelpers->sizeTy());
                return mBuilder->CreateCall(alloc, {count}, "devicealloc");
            }
            if (calleeId->name == "gpu_free" && call->args.size() == 1) {
                auto free = mModule->getOrInsertFunction(
                    "rt_gpu_free", mHelpers->voidTy(), mHelpers->ptrTy());
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                mBuilder->CreateCall(free, {buffer});
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_load_i32" && call->args.size() == 2) {
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                auto* index = coerceCallArgument(generateExpr(call->args[1].get()),
                                                  mHelpers->i32Ty());
                if (mCurrentFunctionIsKernel) {
                    auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index,
                                                        "deviceelem");
                    return mBuilder->CreateLoad(mHelpers->i32Ty(), element, "deviceload");
                }
                auto load = mModule->getOrInsertFunction(
                    "rt_gpu_load_i32", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->i32Ty());
                return mBuilder->CreateCall(load, {buffer, index}, "deviceload");
            }
            if (calleeId->name == "gpu_store_i32" && call->args.size() == 3) {
                auto* buffer = generateDeviceBufferPointer(call->args[0].get());
                auto* index = coerceCallArgument(generateExpr(call->args[1].get()),
                                                  mHelpers->i32Ty());
                auto* value = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                if (mCurrentFunctionIsKernel) {
                    auto* element = mBuilder->CreateGEP(mHelpers->i32Ty(), buffer, index,
                                                        "deviceelem");
                    mBuilder->CreateStore(value, element);
                } else {
                    auto store = mModule->getOrInsertFunction(
                        "rt_gpu_store_i32", mHelpers->voidTy(), mHelpers->ptrTy(),
                        mHelpers->i32Ty(), mHelpers->i32Ty());
                    mBuilder->CreateCall(store, {buffer, index, value});
                }
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_copy_from_host_i32" && call->args.size() == 3) {
                auto* destination = generateDeviceBufferPointer(call->args[0].get());
                auto* source = generateHostRawPointer(call->args[1].get());
                auto* count = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                auto copy = mModule->getOrInsertFunction(
                    "rt_gpu_copy_from_host_i32", mHelpers->i32Ty(), mHelpers->ptrTy(),
                    mHelpers->ptrTy(), mHelpers->i32Ty());
                auto* copied = mBuilder->CreateCall(copy, {destination, source, count}, "gpu.uploaded");
                emitGpuOperationFailureCheck(copied, mCurrentFunc);
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
            if (calleeId->name == "gpu_copy_to_host_i32" && call->args.size() == 3) {
                auto* destination = generateHostRawPointer(call->args[0].get());
                auto* source = generateDeviceBufferPointer(call->args[1].get());
                auto* count = coerceCallArgument(generateExpr(call->args[2].get()),
                                                  mHelpers->i32Ty());
                auto copy = mModule->getOrInsertFunction(
                    "rt_gpu_copy_to_host_i32", mHelpers->i32Ty(), mHelpers->ptrTy(),
                    mHelpers->ptrTy(), mHelpers->i32Ty());
                auto* copied = mBuilder->CreateCall(copy, {destination, source, count}, "gpu.downloaded");
                emitGpuOperationFailureCheck(copied, mCurrentFunc);
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
        }

        // Handle print built-in (maps to printf)
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            if (calleeId->name == "print" && !call->args.empty()) {
                auto printfFn = mModule->getOrInsertFunction("printf",
                    llvm::FunctionType::get(mHelpers->i32Ty(), {mHelpers->ptrTy()}, true));
                for (auto& arg : call->args) {
                    llvm::Value* argVal = generateExpr(arg.get());
                    if (argVal->getType()->isIntegerTy(32)) {
                        auto* fmt = mBuilder->CreateGlobalString("%d\n", "printfmt");
                        mBuilder->CreateCall(printfFn, {fmt, argVal});
                    } else {
                        auto* fmt = mBuilder->CreateGlobalString("%s\n", "printfmt");
                        mBuilder->CreateCall(printfFn, {fmt, argVal});
                    }
                }
                return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
            }
        }

        // Try direct function call by name
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            llvm::Function* callee = nullptr;
            const std::string& symbolName = call->resolvedSymbolName.empty()
                ? calleeId->name : call->resolvedSymbolName;
            auto functionIt = mFunctions.find(symbolName);
            if (functionIt != mFunctions.end()) callee = functionIt->second;
            else callee = mModule->getFunction(symbolName);
            if (callee) {
                
                std::vector<llvm::Value*> args;
                for (size_t i = 0; i < call->args.size(); ++i) {
                    auto* value = generateExpr(call->args[i].get());
                    if (i < callee->getFunctionType()->getNumParams())
                        value = coerceCallArgument(
                            value, callee->getFunctionType()->getParamType(i));
                    args.push_back(value);
                }
                
                return mBuilder->CreateCall(
                    callee, args,
                    callee->getReturnType()->isVoidTy() ? "" : "calltmp");
            }

            
        }
        // Every non-direct callable uses its resolved MoonIR function type.
        // This covers closures, statically selected bindings, and dynamic
        // selector results without baking an i32-only ABI into LLVM lowering.
        TypePtr callableType = call->callee ? call->callee->type : nullptr;
        if (auto* calleeId = dynamic_cast<IdentifierExpr*>(call->callee.get())) {
            auto localType = mLocalTypes.find(calleeId->name);
            if ((!callableType || callableType->kind != TypeKind::Function) &&
                localType != mLocalTypes.end())
                callableType = localType->second;
        }
        if (callableType && callableType->kind == TypeKind::Function) {
            llvm::Value* functionPointer = generateExpr(call->callee.get());
            std::vector<llvm::Type*> parameterTypes;
            std::vector<llvm::Value*> arguments;
            for (size_t index = 0; index < callableType->paramTypes.size(); ++index)
                parameterTypes.push_back(mHelpers->toLLVMType(callableType->paramTypes[index]));
            for (size_t index = 0; index < call->args.size(); ++index) {
                llvm::Value* argument = generateExpr(call->args[index].get());
                if (index < parameterTypes.size())
                    argument = coerceCallArgument(argument, parameterTypes[index]);
                arguments.push_back(argument);
            }
            auto* returnType = mHelpers->toLLVMType(callableType->returnType);
            auto* functionType = llvm::FunctionType::get(
                returnType, parameterTypes, false);
            return mBuilder->CreateCall(
                functionType, functionPointer, arguments,
                returnType->isVoidTy() ? "" : "indirect.call");
        }
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }
    if (auto* ha = dynamic_cast<HeapAllocExpr*>(expr)) {
        uint64_t sz = typeSize(ha->allocatedType);
        auto* sizeVal = llvm::ConstantInt::get(mHelpers->sizeTy(), sz);
        auto rtMalloc = mModule->getOrInsertFunction(
            "rt_malloc", mHelpers->ptrTy(), mHelpers->sizeTy());
        llvm::Value* ptr = mBuilder->CreateCall(rtMalloc, {sizeVal}, "heapalloc");

        // Initialize: store constructor args at the malloc'd pointer
        if (auto* initCall = dynamic_cast<CallExpr*>(ha->initializer.get())) {
            uint64_t offset = 0;
            for (size_t i = 0; i < initCall->args.size(); ++i) {
                auto& arg = initCall->args[i];
                llvm::Value* argVal = generateExpr(arg.get());
                if (i > 0 && ha->allocatedType &&
                    (ha->allocatedType->kind == TypeKind::Struct ||
                     ha->allocatedType->kind == TypeKind::Record)) {
                    offset = 0;
                    for (size_t j = 0; j < i && j < ha->allocatedType->fields.size(); ++j)
                        offset += typeSize(ha->allocatedType->fields[j].type);
                }
                auto* basePtr = ptr;
                if (offset != 0)
                    basePtr = mBuilder->CreateGEP(
                        llvm::Type::getInt8Ty(*mCtx), ptr,
                        llvm::ConstantInt::get(mHelpers->sizeTy(), offset), "fieldinit");
                auto* typedPtr = mBuilder->CreateBitCast(
                    basePtr, llvm::PointerType::get(*mCtx, 0), "typedptr");
                mBuilder->CreateStore(argVal, typedPtr);
            }
        }
        return ptr;
    }
    if (auto* as = dynamic_cast<AssignExpr*>(expr)) {
        llvm::Value* rhs = generateExpr(as->rhs.get());
        if (auto* index = dynamic_cast<IndexExpr*>(as->lhs.get())) {
            auto* id = dynamic_cast<IdentifierExpr*>(index->object.get());
            if (id && mLocals.count(id->name) && mLocalTypes[id->name] &&
                mLocalTypes[id->name]->kind == TypeKind::Array) {
                TypePtr arrayType = mLocalTypes[id->name];
                auto* rawIndex = coerceCallArgument(
                    generateExpr(index->index.get()), mHelpers->i32Ty());
                llvm::Value* checked = rawIndex;
                if (!isProvablySafeArrayIndex(index->index.get(), arrayType->arrayLength,
                                              mLocalKnownUpperBounds))
                    checked = mBuilder->CreateCall(mModule->getOrInsertFunction(
                        "rt_array_index_or_abort", mHelpers->i32Ty(), mHelpers->i32Ty(), mHelpers->sizeTy()),
                        {rawIndex, llvm::ConstantInt::get(mHelpers->sizeTy(), arrayType->arrayLength)},
                        "array.index");
                auto* elementPtr = mBuilder->CreateInBoundsGEP(
                    mLocals[id->name]->getAllocatedType(), mLocals[id->name],
                    {llvm::ConstantInt::get(mHelpers->i32Ty(), 0), checked}, "array.element");
                mBuilder->CreateStore(rhs, elementPtr);
                return rhs;
            }
        }
        if (auto* id = dynamic_cast<IdentifierExpr*>(as->lhs.get())) {
            auto it = mLocals.find(id->name);
            if (it != mLocals.end()) {
                llvm::Value* result = rhs;
                if (as->op != Operator::Assign) {
                    llvm::Value* lhs = mBuilder->CreateLoad(
                        it->second->getAllocatedType(), it->second, id->name + ".old");
                    switch (as->op) {
                        case Operator::AddAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFAdd(lhs, rhs, "addeqtmp")
                                : mBuilder->CreateAdd(lhs, rhs, "addeqtmp"); break;
                        case Operator::SubtractAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFSub(lhs, rhs, "subeqtmp")
                                : mBuilder->CreateSub(lhs, rhs, "subeqtmp"); break;
                        case Operator::MultiplyAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFMul(lhs, rhs, "muleqtmp")
                                : mBuilder->CreateMul(lhs, rhs, "muleqtmp"); break;
                        case Operator::DivideAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFDiv(lhs, rhs, "diveqtmp")
                                : mBuilder->CreateSDiv(lhs, rhs, "diveqtmp"); break;
                        case Operator::RemainderAssign:
                            result = lhs->getType()->isFloatingPointTy()
                                ? mBuilder->CreateFRem(lhs, rhs, "remeqtmp")
                                : mBuilder->CreateSRem(lhs, rhs, "remeqtmp"); break;
                        case Operator::BitAndAssign: result = mBuilder->CreateAnd(lhs, rhs, "andeqtmp"); break;
                        case Operator::BitOrAssign: result = mBuilder->CreateOr(lhs, rhs, "oreqtmp"); break;
                        case Operator::BitXorAssign: result = mBuilder->CreateXor(lhs, rhs, "xoreqtmp"); break;
                        case Operator::ShiftLeftAssign: result = mBuilder->CreateShl(lhs, rhs, "shleqtmp"); break;
                        case Operator::ShiftRightAssign: result = mBuilder->CreateAShr(lhs, rhs, "shreqtmp"); break;
                        default: break;
                    }
                }
                mBuilder->CreateStore(result, it->second);
                mLocalKnownUpperBounds.erase(id->name);
                return result;
            }
        }
        return rhs;
    }
    if (auto* mv = dynamic_cast<MoveExpr*>(expr)) {
        return generateExpr(mv->operand.get());
    }
    if (auto* bw = dynamic_cast<BorrowExpr*>(expr)) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(bw->operand.get())) {
            auto it = mLocals.find(id->name);
            if (it != mLocals.end()) return it->second;
        }
        return generateExpr(bw->operand.get());
    }
    if (auto* dr = dynamic_cast<DerefExpr*>(expr)) {
        llvm::Value* op = generateExpr(dr->operand.get());
        auto* ptr = mBuilder->CreateBitCast(op, llvm::PointerType::get(*mCtx, 0));
        return mBuilder->CreateLoad(mHelpers->i32Ty(), ptr, "deref");
    }
    if (auto* ad = dynamic_cast<AddrOfExpr*>(expr)) {
        if (auto* id = dynamic_cast<IdentifierExpr*>(ad->operand.get())) {
            auto it = mLocals.find(id->name);
            if (it != mLocals.end()) return it->second;
        }
        return generateExpr(ad->operand.get());
    }
    if (auto* le = dynamic_cast<LambdaExpr*>(expr)) {
        // Generate a hidden function for the lambda body
        static int lambdaCount = 0;
        std::string lambdaName = "__lambda_" + std::to_string(lambdaCount++);
        if (!le->identitySuffix.empty()) lambdaName += "__" + le->identitySuffix;

        // Build LLVM function for lambda
        std::vector<llvm::Type*> paramTypes;
        for (auto& p : le->params)
            paramTypes.push_back(mHelpers->toLLVMType(p.type));
        llvm::Type* retTy = le->returnType
            ? mHelpers->toLLVMType(le->returnType) : mHelpers->i32Ty();
        auto funcTy = llvm::FunctionType::get(retTy, paramTypes, false);
        auto func = llvm::Function::Create(
            funcTy, llvm::Function::InternalLinkage, lambdaName, mModule.get());

        // Save caller state (including insert point)
        auto savedFunc = mCurrentFunc;
        auto savedLocals = std::move(mLocals);
        auto savedLocalTypes = std::move(mLocalTypes);
        auto savedUpperBounds = std::move(mLocalKnownUpperBounds);
        auto savedContinuationFrames = std::move(mContinuationFrames);
        const unsigned savedContinuationFrameCounter = mContinuationFrameCounter;
        auto savedIP = mBuilder->saveIP();
        mLocals.clear();
        mLocalTypes.clear();
        mLocalKnownUpperBounds.clear();
        mContinuationFrames.clear();
        mContinuationFrameCounter = 0;
        mCurrentFunc = func;

        auto entryBB = llvm::BasicBlock::Create(*mCtx, "entry", func);
        mBuilder->SetInsertPoint(entryBB);

        // Allocate params
        size_t idx = 0;
        for (auto& arg : func->args()) {
            arg.setName(le->params[idx].name);
            auto* alloca = createEntryBlockAlloca(func, arg.getType(), le->params[idx].name);
            mBuilder->CreateStore(&arg, alloca);
            mLocals[le->params[idx].name] = alloca;
            mLocalTypes[le->params[idx].name] = le->params[idx].type;
            idx++;
        }

        if (le->body) generateBlock(le->body.get(), func);
        if (!mBuilder->GetInsertBlock()->getTerminator()) mBuilder->CreateRet(llvm::ConstantInt::get(retTy, 0));

        // Restore state (including insert point)
        mCurrentFunc = savedFunc;
        mLocals = std::move(savedLocals);
        mLocalTypes = std::move(savedLocalTypes);
        mLocalKnownUpperBounds = std::move(savedUpperBounds);
        mContinuationFrames = std::move(savedContinuationFrames);
        mContinuationFrameCounter = savedContinuationFrameCounter;
        mBuilder->restoreIP(savedIP);
        return func;
    }

    return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
}

llvm::Value* CodeGenerator::generateDeviceBufferPointer(Expr* expr) {
    if (auto* move = dynamic_cast<MoveExpr*>(expr))
        return generateDeviceBufferPointer(move->operand.get());
    if (auto* borrow = dynamic_cast<BorrowExpr*>(expr))
        return generateDeviceBufferPointer(borrow->operand.get());
    if (auto* address = dynamic_cast<AddrOfExpr*>(expr))
        return generateDeviceBufferPointer(address->operand.get());
    if (auto* id = dynamic_cast<IdentifierExpr*>(expr)) {
        auto local = mLocals.find(id->name);
        if (local != mLocals.end()) {
            TypePtr type;
            auto typed = mLocalTypes.find(id->name);
            if (typed != mLocalTypes.end()) type = typed->second;
            auto* value = mBuilder->CreateLoad(local->second->getAllocatedType(), local->second,
                                               id->name + ".devicearg");
            // A kernel reference parameter contains the caller's buffer slot.
            // In host functions dereference that slot once more to obtain the
            // actual allocation. Kernel IR instead uses the direct device
            // pointer ABI, which is also the form supplied by the CUDA Driver.
            if (type && type->kind == TypeKind::Reference && type->inner &&
                type->inner->kind == TypeKind::DeviceBuffer && !mCurrentFunctionIsKernel)
                return mBuilder->CreateLoad(mHelpers->ptrTy(), value, id->name + ".deviceptr");
            return value;
        }
    }
    return generateExpr(expr);
}

llvm::Value* CodeGenerator::generateHostRawPointer(Expr* expr) {
    // A bulk transfer accepts &raw<i32> / &mut raw<i32>.  The reference is
    // to a local that stores the foreign pointer, so load once to recover the
    // actual host-memory address rather than passing the address of its slot.
    Expr* operand = expr;
    if (auto* borrow = dynamic_cast<BorrowExpr*>(operand)) operand = borrow->operand.get();
    else if (auto* address = dynamic_cast<AddrOfExpr*>(operand)) operand = address->operand.get();
    if (auto* id = dynamic_cast<IdentifierExpr*>(operand)) {
        auto local = mLocals.find(id->name);
        if (local != mLocals.end())
            return mBuilder->CreateLoad(local->second->getAllocatedType(), local->second,
                                        id->name + ".hostptr");
    }
    return generateExpr(expr);
}

void CodeGenerator::emitGpuOperationFailureCheck(llvm::Value* operationSucceeded,
                                                  llvm::Function* func) {
    auto* succeeded = mBuilder->CreateICmpNE(
        operationSucceeded, llvm::ConstantInt::get(operationSucceeded->getType(), 0),
        "gpu.operation.ok");
    auto* failedBB = llvm::BasicBlock::Create(*mCtx, "gpu.operation.failed", func);
    auto* continuedBB = llvm::BasicBlock::Create(*mCtx, "gpu.operation.continue", func);
    mBuilder->CreateCondBr(succeeded, continuedBB, failedBB);

    mBuilder->SetInsertPoint(failedBB);
    auto report = mModule->getOrInsertFunction(
        "rt_gpu_report_operation_error_and_abort", mHelpers->voidTy());
    mBuilder->CreateCall(report);
    mBuilder->CreateUnreachable();

    mBuilder->SetInsertPoint(continuedBB);
}

llvm::Value* CodeGenerator::generateLaunch(LaunchExpr* launch) {
    llvm::Function* callee = nullptr;
    const std::string& symbol = launch->resolvedKernelName.empty()
        ? launch->kernelName : launch->resolvedKernelName;
    auto known = mFunctions.find(symbol);
    if (known != mFunctions.end()) callee = known->second;
    else callee = mModule->getFunction(symbol);
    if (!callee || !mCurrentFunc) {
        error("cannot lower launch of unknown kernel '" + launch->kernelName + "'");
        return llvm::ConstantInt::get(mHelpers->i32Ty(), 0);
    }

    auto* counter = createEntryBlockAlloca(mCurrentFunc, mHelpers->i32Ty(), "launch.index");
    auto* threads = coerceCallArgument(generateExpr(launch->threads.get()), mHelpers->i32Ty());
    mBuilder->CreateStore(llvm::ConstantInt::get(mHelpers->i32Ty(), 0), counter);

    // Both CUDA's Driver API and HIP's Module API receive an array of
    // addresses, not an array of values. Buffer borrows already are addresses
    // of host-side buffer slots; scalar launch values are materialized into
    // entry-block slots here.
    std::vector<llvm::Value*> driverParameters;
    driverParameters.push_back(counter);
    for (size_t i = 0; i < launch->args.size(); ++i) {
        llvm::Value* value = generateExpr(launch->args[i].get());
        const size_t parameterIndex = i + 1;
        llvm::Type* parameterType = parameterIndex < callee->getFunctionType()->getNumParams()
            ? callee->getFunctionType()->getParamType(parameterIndex) : value->getType();
        if (parameterType->isPointerTy() && value->getType()->isPointerTy()) {
            driverParameters.push_back(value);
        } else {
            value = coerceCallArgument(value, parameterType);
            auto* slot = createEntryBlockAlloca(mCurrentFunc, value->getType(), "launch.scalar");
            mBuilder->CreateStore(value, slot);
            driverParameters.push_back(slot);
        }
    }
    auto* parameterArrayType = llvm::ArrayType::get(mHelpers->ptrTy(), driverParameters.size());
    auto* parameterArray = createEntryBlockAlloca(mCurrentFunc, parameterArrayType, "launch.params");
    for (size_t i = 0; i < driverParameters.size(); ++i) {
        auto* destination = mBuilder->CreateInBoundsGEP(
            parameterArrayType, parameterArray,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), i)}, "launch.param");
        mBuilder->CreateStore(driverParameters[i], destination);
    }
    auto* parameterStart = mBuilder->CreateInBoundsGEP(
        parameterArrayType, parameterArray,
        {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
         llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "launch.paramstart");

    const auto ptx = mKernelPTX.find(symbol);
    const std::string ptxSource = ptx == mKernelPTX.end() ? "" : ptx->second;
    auto* ptxValue = mBuilder->CreateGlobalString(ptxSource, "kernel.ptx");
    const auto hsaco = mKernelHSACO.find(symbol);
    llvm::Value* hsacoValue = nullptr;
    llvm::Value* hsacoSize = nullptr;
    if (hsaco == mKernelHSACO.end() || hsaco->second.empty()) {
        hsacoValue = mBuilder->CreateGlobalString("", "kernel.hsaco.empty");
        hsacoSize = llvm::ConstantInt::get(mHelpers->i64Ty(), 0);
    } else {
        const llvm::StringRef hsacoSource(hsaco->second.data(), hsaco->second.size());
        auto* hsacoData = llvm::ConstantDataArray::getString(*mCtx, hsacoSource, false);
        auto* hsacoGlobal = new llvm::GlobalVariable(
            *mModule, hsacoData->getType(), true, llvm::GlobalValue::PrivateLinkage,
            hsacoData, "kernel.hsaco");
        hsacoValue = mBuilder->CreateInBoundsGEP(
            hsacoData->getType(), hsacoGlobal,
            {llvm::ConstantInt::get(mHelpers->i32Ty(), 0),
             llvm::ConstantInt::get(mHelpers->i32Ty(), 0)}, "kernel.hsaco.data");
        hsacoSize = llvm::ConstantInt::get(mHelpers->i64Ty(), hsaco->second.size());
    }
    auto* kernelName = mBuilder->CreateGlobalString(symbol, "kernel.name");
    auto cudaBackend = mModule->getOrInsertFunction(
        "rt_gpu_backend_is_cuda", mHelpers->i32Ty());
    auto* useCuda = mBuilder->CreateICmpNE(
        mBuilder->CreateCall(cudaBackend, {}, "gpu.backend"),
        llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.iscuda");
    auto rocmBackend = mModule->getOrInsertFunction(
        "rt_gpu_backend_is_rocm", mHelpers->i32Ty());
    auto* useRocm = mBuilder->CreateICmpNE(
        mBuilder->CreateCall(rocmBackend, {}, "gpu.backend"),
        llvm::ConstantInt::get(mHelpers->i32Ty(), 0), "gpu.isrocm");

    auto* cudaBB = llvm::BasicBlock::Create(*mCtx, "launch.cuda", mCurrentFunc);
    auto* backendBB = llvm::BasicBlock::Create(*mCtx, "launch.backend", mCurrentFunc);
    auto* rocmBB = llvm::BasicBlock::Create(*mCtx, "launch.rocm", mCurrentFunc);
    auto* condBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.cond", mCurrentFunc);
    auto* bodyBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.body", mCurrentFunc);
    auto* exitBB = llvm::BasicBlock::Create(*mCtx, "launch.sim.exit", mCurrentFunc);
    auto* mergeBB = llvm::BasicBlock::Create(*mCtx, "launch.merge", mCurrentFunc);
    mBuilder->CreateCondBr(useCuda, cudaBB, backendBB);

    mBuilder->SetInsertPoint(cudaBB);
    auto cudaLaunch = mModule->getOrInsertFunction(
        "rt_gpu_launch_ptx", mHelpers->i32Ty(), mHelpers->ptrTy(), mHelpers->ptrTy(),
        mHelpers->i32Ty(), mHelpers->ptrTy());
    auto* cudaEvent = mBuilder->CreateCall(cudaLaunch,
        {ptxValue, kernelName, threads, parameterStart}, "cuda.event");
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(backendBB);
    mBuilder->CreateCondBr(useRocm, rocmBB, condBB);

    mBuilder->SetInsertPoint(rocmBB);
    auto rocmLaunch = mModule->getOrInsertFunction(
        "rt_gpu_launch_hsaco", mHelpers->i32Ty(), mHelpers->ptrTy(),
        mHelpers->i64Ty(), mHelpers->ptrTy(), mHelpers->i32Ty(), mHelpers->ptrTy());
    auto* rocmEvent = mBuilder->CreateCall(rocmLaunch,
        {hsacoValue, hsacoSize, kernelName, threads, parameterStart}, "rocm.event");
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(condBB);
    auto* index = mBuilder->CreateLoad(mHelpers->i32Ty(), counter, "launch.index.value");
    auto* active = mBuilder->CreateICmpSLT(index, threads, "launch.active");
    mBuilder->CreateCondBr(active, bodyBB, exitBB);

    mBuilder->SetInsertPoint(bodyBB);
    std::vector<llvm::Value*> args;
    args.push_back(index);
    for (size_t i = 0; i < launch->args.size(); ++i) {
        const size_t paramIndex = i + 1;
        llvm::Value* value = nullptr;
        if (paramIndex < callee->getFunctionType()->getNumParams() &&
            callee->getFunctionType()->getParamType(paramIndex)->isPointerTy()) {
            // The CPU simulator calls the same kernel IR used for PTX, so it
            // passes the buffer pointer value rather than the address of the
            // host slot used only by CUDA's parameter-array ABI.
            value = generateDeviceBufferPointer(launch->args[i].get());
        } else {
            value = generateExpr(launch->args[i].get());
        }
        if (paramIndex < callee->getFunctionType()->getNumParams())
            value = coerceCallArgument(value, callee->getFunctionType()->getParamType(paramIndex));
        args.push_back(value);
    }
    mBuilder->CreateCall(callee, args);
    auto* next = mBuilder->CreateAdd(index, llvm::ConstantInt::get(mHelpers->i32Ty(), 1),
                                     "launch.next");
    mBuilder->CreateStore(next, counter);
    mBuilder->CreateBr(condBB);

    mBuilder->SetInsertPoint(exitBB);
    mBuilder->CreateBr(mergeBB);

    mBuilder->SetInsertPoint(mergeBB);
    auto* event = mBuilder->CreatePHI(mHelpers->i32Ty(), 3, "launch.event");
    event->addIncoming(cudaEvent, cudaBB);
    event->addIncoming(rocmEvent, rocmBB);
    // Event value 1 denotes a completed simulator dispatch. `await` remains
    // explicit in source and becomes event synchronization in either vendor
    // branch above.
    event->addIncoming(llvm::ConstantInt::get(mHelpers->i32Ty(), 1), exitBB);
    return event;
}

bool CodeGenerator::emitKernelPTX(FunctionDecl* kernel) {
    const std::string symbol = kernel->generatedSymbolName.empty()
        ? kernel->name : kernel->generatedSymbolName;
    auto source = mFunctions.find(symbol);
    if (source == mFunctions.end() || !source->second) {
        error("cannot emit PTX for unknown kernel '" + kernel->name + "'");
        return false;
    }

    constexpr const char* targetTriple = "nvptx64-nvidia-cuda";
    std::string targetError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        llvm::Triple(targetTriple), targetError);
    if (!target) {
        error("CUDA backend requires LLVM NVPTX support: " + targetError);
        return false;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        llvm::Triple(targetTriple), mGpuTargets.cudaArchitecture, "", options, llvm::Reloc::PIC_,
        std::nullopt, llvm::CodeGenOptLevel::Aggressive));
    if (!machine) {
        error("could not create LLVM NVPTX target machine for kernel '" + kernel->name + "'");
        return false;
    }

    auto deviceModule = std::make_unique<llvm::Module>("cuda." + symbol, *mCtx);
    deviceModule->setTargetTriple(llvm::Triple(targetTriple));
    deviceModule->setDataLayout(machine->createDataLayout());
    auto* deviceFunction = llvm::Function::Create(
        source->second->getFunctionType(), llvm::Function::ExternalLinkage,
        symbol, deviceModule.get());
    deviceFunction->setCallingConv(llvm::CallingConv::PTX_Kernel);

    llvm::ValueToValueMapTy valueMap;
    auto destinationArgument = deviceFunction->arg_begin();
    for (auto& sourceArgument : source->second->args()) {
        destinationArgument->setName(sourceArgument.getName());
        valueMap[&sourceArgument] = &*destinationArgument;
        ++destinationArgument;
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(deviceFunction, source->second, valueMap,
                            llvm::CloneFunctionChangeType::DifferentModule, returns);
    // CloneFunctionInto preserves the source function's ordinary host calling
    // convention, so set the device ABI after cloning rather than before it.
    deviceFunction->setCallingConv(llvm::CallingConv::PTX_Kernel);

    // The host simulator receives `index` as an explicit scalar and invokes
    // the kernel once per logical thread. CUDA instead launches a grid, so
    // replace the entry-slot value with blockIdx.x * blockDim.x + threadIdx.x
    // in the cloned PTX function while retaining the common source signature.
    llvm::StoreInst* indexParameterStore = nullptr;
    if (!deviceFunction->arg_empty()) {
        llvm::Argument* indexParameter = deviceFunction->getArg(0);
        for (auto& instruction : deviceFunction->getEntryBlock()) {
            auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
            if (store && store->getValueOperand() == indexParameter) {
                indexParameterStore = store;
                break;
            }
        }
    }
    if (!indexParameterStore) {
        error("could not locate the generated index slot for CUDA kernel '" + kernel->name + "'");
        return false;
    }
    llvm::IRBuilder<> deviceBuilder(*mCtx);
    deviceBuilder.SetInsertPoint(indexParameterStore->getParent(),
                                 std::next(indexParameterStore->getIterator()));
    auto indexIntrinsic = [&](const char* name) {
        auto intrinsic = deviceModule->getOrInsertFunction(
            name, llvm::FunctionType::get(mHelpers->i32Ty(), false));
        return deviceBuilder.CreateCall(intrinsic, {}, "cuda.index.reg");
    };
    auto* block = indexIntrinsic("llvm.nvvm.read.ptx.sreg.ctaid.x");
    auto* blockWidth = indexIntrinsic("llvm.nvvm.read.ptx.sreg.ntid.x");
    auto* thread = indexIntrinsic("llvm.nvvm.read.ptx.sreg.tid.x");
    auto* globalIndex = deviceBuilder.CreateAdd(
        deviceBuilder.CreateMul(block, blockWidth, "cuda.block.base"), thread, "cuda.index");
    deviceBuilder.CreateStore(globalIndex, indexParameterStore->getPointerOperand());

    if (llvm::verifyFunction(*deviceFunction, &llvm::errs())) {
        error("generated invalid LLVM IR while preparing CUDA kernel '" + kernel->name + "'");
        return false;
    }
    auto* annotations = deviceModule->getOrInsertNamedMetadata("nvvm.annotations");
    annotations->addOperand(llvm::MDNode::get(*mCtx, {
        llvm::ValueAsMetadata::get(deviceFunction),
        llvm::MDString::get(*mCtx, "kernel"),
        llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(mHelpers->i32Ty(), 1))
    }));
    optimizeDeviceModule(*deviceModule);

    llvm::SmallString<0> ptx;
    llvm::raw_svector_ostream stream(ptx);
    llvm::legacy::PassManager passManager;
    if (machine->addPassesToEmitFile(passManager, stream, nullptr,
                                     llvm::CodeGenFileType::AssemblyFile)) {
        error("LLVM NVPTX backend cannot emit PTX for kernel '" + kernel->name + "'");
        return false;
    }
    passManager.run(*deviceModule);
    mKernelPTX.emplace(symbol, std::string(ptx.str()));
    return true;
}

bool CodeGenerator::emitKernelHSACO(FunctionDecl* kernel) {
    const std::string symbol = kernel->generatedSymbolName.empty()
        ? kernel->name : kernel->generatedSymbolName;
    auto source = mFunctions.find(symbol);
    if (source == mFunctions.end() || !source->second) {
        error("cannot emit AMDGPU code object for unknown kernel '" + kernel->name + "'");
        return false;
    }

    constexpr const char* targetTriple = "amdgcn-amd-amdhsa";
    const std::string& architecture = mGpuTargets.rocmArchitecture;
    std::string targetError;
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        llvm::Triple(targetTriple), targetError);
    if (!target) {
        error("ROCm backend requires LLVM AMDGPU support: " + targetError);
        return false;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
        llvm::Triple(targetTriple), architecture, "", options, llvm::Reloc::PIC_,
        std::nullopt, llvm::CodeGenOptLevel::Aggressive));
    if (!machine) {
        error("could not create LLVM AMDGPU target machine for architecture '" +
              architecture + "'");
        return false;
    }

    auto deviceModule = std::make_unique<llvm::Module>("rocm." + symbol, *mCtx);
    deviceModule->setTargetTriple(llvm::Triple(targetTriple));
    deviceModule->setDataLayout(machine->createDataLayout());
    llvm::SmallVector<llvm::Type*, 8> deviceParameterTypes;
    for (llvm::Type* parameter : source->second->getFunctionType()->params()) {
        // The initial device ABI only permits device-buffer references for
        // pointer parameters. Represent them as HSA global pointers.
        deviceParameterTypes.push_back(parameter->isPointerTy()
            ? llvm::PointerType::get(*mCtx, 1) : parameter);
    }
    auto* deviceFunctionType = llvm::FunctionType::get(
        source->second->getReturnType(), deviceParameterTypes, false);
    auto* deviceFunction = llvm::Function::Create(
        deviceFunctionType, llvm::Function::ExternalLinkage,
        symbol, deviceModule.get());
    deviceFunction->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);

    llvm::ValueToValueMapTy valueMap;
    auto* abiBlock = llvm::BasicBlock::Create(*mCtx, "abi", deviceFunction);
    llvm::IRBuilder<> abiBuilder(abiBlock);
    auto destinationArgument = deviceFunction->arg_begin();
    for (auto& sourceArgument : source->second->args()) {
        destinationArgument->setName(sourceArgument.getName());
        llvm::Value* mappedArgument = &*destinationArgument;
        if (mappedArgument->getType() != sourceArgument.getType())
            mappedArgument = abiBuilder.CreateAddrSpaceCast(
                mappedArgument, sourceArgument.getType(), sourceArgument.getName() + ".generic");
        valueMap[&sourceArgument] = mappedArgument;
        ++destinationArgument;
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(deviceFunction, source->second, valueMap,
                            llvm::CloneFunctionChangeType::DifferentModule, returns);
    if (std::next(deviceFunction->begin()) == deviceFunction->end()) {
        error("could not clone ROCm kernel body for '" + kernel->name + "'");
        return false;
    }
    abiBuilder.CreateBr(&*std::next(deviceFunction->begin()));
    // CloneFunctionInto preserves the source function's ordinary host calling
    // convention, so set the device ABI after cloning rather than before it.
    deviceFunction->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);
    // CloneFunctionInto also copies the source function attributes. Apply the
    // device-only ABI contract after cloning, otherwise these attributes are
    // silently overwritten and AMDGPU reserves unused hidden arguments.
    for (const char* attribute : {
             "amdgpu-no-hostcall-ptr",
             "amdgpu-no-queue-ptr",
             "amdgpu-no-default-queue",
             "amdgpu-no-completion-action",
             "amdgpu-no-heap-ptr",
             "amdgpu-no-multigrid-sync-arg",
             "amdgpu-no-implicitarg-ptr",
             "amdgpu-flat-work-group-size"}) {
        if (std::strcmp(attribute, "amdgpu-flat-work-group-size") == 0)
            deviceFunction->addFnAttr(attribute, "1,1024");
        else
            deviceFunction->addFnAttr(attribute);
    }
    deviceFunction->addFnAttr("uniform-work-group-size", "true");

    // Host IR uses the default stack address space. AMDGPU reserves that
    // space for generic pointers and requires function-local allocas in its
    // private address space (5). Keep the cloned source uses generic through
    // an explicit address-space cast; this is the canonical bridge that lets
    // the backend lower host-shaped temporary IR into an HSA kernel.
    std::vector<llvm::AllocaInst*> hostAllocas;
    for (auto& block : *deviceFunction) {
        for (auto& instruction : block) {
            if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
                hostAllocas.push_back(alloca);
        }
    }
    for (auto* hostAlloca : hostAllocas) {
        llvm::IRBuilder<> allocaBuilder(hostAlloca);
        auto* privateAlloca = allocaBuilder.CreateAlloca(
            hostAlloca->getAllocatedType(), 5, hostAlloca->getArraySize(),
            hostAlloca->getName() + ".private");
        privateAlloca->setAlignment(hostAlloca->getAlign());
        auto* genericPointer = allocaBuilder.CreateAddrSpaceCast(
            privateAlloca, hostAlloca->getType(), hostAlloca->getName() + ".generic");
        hostAlloca->replaceAllUsesWith(genericPointer);
        hostAlloca->eraseFromParent();
    }

    // The simulator provides source `index` explicitly. An HSA dispatch
    // derives it from the same one-dimensional 256-thread grid as CUDA.
    llvm::StoreInst* indexParameterStore = nullptr;
    if (!deviceFunction->arg_empty()) {
        llvm::Argument* indexParameter = deviceFunction->getArg(0);
        for (auto& block : *deviceFunction) {
            for (auto& instruction : block) {
                auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (store && store->getValueOperand() == indexParameter) {
                    indexParameterStore = store;
                    break;
                }
            }
            if (indexParameterStore) break;
        }
    }
    if (!indexParameterStore) {
        error("could not locate the generated index slot for ROCm kernel '" + kernel->name + "'");
        return false;
    }
    llvm::IRBuilder<> deviceBuilder(*mCtx);
    deviceBuilder.SetInsertPoint(indexParameterStore->getParent(),
                                 std::next(indexParameterStore->getIterator()));
    // Use LLVM's real AMDGPU intrinsic declarations rather than creating
    // ordinary functions whose names happen to match intrinsic names. The
    // declaration carries range, convergent, and memory-effect attributes
    // consumed by AMDGPU implicit-argument lowering. A plain declaration can
    // conservatively retain hostcall/queue/heap hidden arguments in the HSA
    // kernel descriptor, increasing SGPR pressure and changing the ABI.
    auto* workgroupIntrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        deviceModule.get(), llvm::Intrinsic::amdgcn_workgroup_id_x);
    auto* workitemIntrinsic = llvm::Intrinsic::getOrInsertDeclaration(
        deviceModule.get(), llvm::Intrinsic::amdgcn_workitem_id_x);
    auto* workgroup = deviceBuilder.CreateCall(
        workgroupIntrinsic, {}, "amdgpu.index.group");
    auto* workitem = deviceBuilder.CreateCall(
        workitemIntrinsic, {}, "amdgpu.index.item");
    auto* workgroupBase = deviceBuilder.CreateMul(
        workgroup, llvm::ConstantInt::get(mHelpers->i32Ty(), 256), "amdgpu.group.base");
    auto* globalIndex = deviceBuilder.CreateAdd(workgroupBase, workitem, "amdgpu.index");
    deviceBuilder.CreateStore(globalIndex, indexParameterStore->getPointerOperand());

    if (llvm::verifyFunction(*deviceFunction, &llvm::errs())) {
        error("generated invalid LLVM IR while preparing ROCm kernel '" + kernel->name + "'");
        return false;
    }
    // The first pass canonicalizes the cloned host-shaped body. The ABI
    // rewrite must happen after that pass: running another generic-pointer
    // cleanup after the rewrite can legally fold the global pointer back to a
    // flat pointer on AMDGPU. The final pass below is intentionally retained
    // only to clean up the rewritten global form.
    optimizeDeviceModule(*deviceModule);
    lowerDirectDeviceMemoryToGlobal(*deviceFunction);
    optimizeDeviceModule(*deviceModule);
    if (llvm::verifyModule(*deviceModule, &llvm::errs())) {
        error("generated invalid optimized LLVM IR while preparing ROCm kernel '" + kernel->name + "'");
        return false;
    }

    llvm::SmallString<128> objectPath;
    llvm::SmallString<128> linkedPath;
    const auto cleanup = [&] {
        llvm::sys::fs::remove(objectPath);
        llvm::sys::fs::remove(linkedPath);
    };
    std::error_code fileError = llvm::sys::fs::createTemporaryFile(
        "luna-amdgpu-kernel", "o", objectPath);
    if (fileError) {
        error("could not create a temporary AMDGPU object file: " + fileError.message());
        return false;
    }
    fileError = llvm::sys::fs::createTemporaryFile(
        "luna-amdgpu-kernel", "hsaco", linkedPath);
    if (fileError) {
        cleanup();
        error("could not create a temporary HSA code-object file: " + fileError.message());
        return false;
    }

    llvm::raw_fd_ostream stream(objectPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        cleanup();
        error("could not write the temporary AMDGPU object file: " + fileError.message());
        return false;
    }
    llvm::legacy::PassManager passManager;
    if (machine->addPassesToEmitFile(passManager, stream, nullptr,
                                     llvm::CodeGenFileType::ObjectFile)) {
        cleanup();
        error("LLVM AMDGPU backend cannot emit an HSA code object for kernel '" +
              kernel->name + "'");
        return false;
    }
    passManager.run(*deviceModule);
    stream.flush();

    // LLVM's target machine emits an ET_REL AMDGPU object. HIP loads a linked
    // ET_DYN HSA code object, so finish the device link with the LLD shipped
    // alongside the LLVM installation. This occurs only for ROCm/offline
    // AMDGPU builds, never for the portable simulator path.
    auto linker = llvm::sys::findProgramByName("ld.lld");
    if (!linker) {
        cleanup();
        error("ROCm code-object emission requires LLVM's 'ld.lld' linker in PATH");
        return false;
    }
    llvm::SmallVector<llvm::StringRef, 4> linkerArgs;
    linkerArgs.push_back(*linker);
    linkerArgs.push_back("-shared");
    linkerArgs.push_back(objectPath);
    linkerArgs.push_back("-o");
    linkerArgs.push_back(linkedPath);
    std::string linkerError;
    const int linkerResult = llvm::sys::ExecuteAndWait(
        *linker, linkerArgs, std::nullopt, {}, 0, 0, &linkerError);
    if (linkerResult != 0) {
        cleanup();
        error("LLD could not link AMDGPU kernel '" + kernel->name + "'" +
              (linkerError.empty() ? "" : ": " + linkerError));
        return false;
    }
    auto linkedObject = llvm::MemoryBuffer::getFile(linkedPath, false, false);
    if (!linkedObject) {
        cleanup();
        error("could not read linked HSA code object for kernel '" + kernel->name +
              "': " + linkedObject.getError().message());
        return false;
    }
    const std::string linkedHsaco((*linkedObject)->getBuffer().str());
    cleanup();
    std::string dumpError;
    if (!dumpHsacoIfRequested(linkedHsaco, symbol, architecture, dumpError)) {
        error(dumpError);
        return false;
    }
    mKernelHSACO.emplace(symbol, makeHipModuleBundle(linkedHsaco, architecture));
    return true;
}

llvm::Value* CodeGenerator::coerceCallArgument(llvm::Value* value, llvm::Type* target) {
    if (!value || !target || value->getType() == target) return value;
    if (value->getType()->isIntegerTy() && target->isIntegerTy())
        return mBuilder->CreateIntCast(value, target, true, "abiarg");
    if (value->getType()->isPointerTy() && target->isPointerTy())
        return mBuilder->CreateBitCast(value, target, "abiarg");
    return value;
}

// ─── Helpers ───────────────────────────────────────────────────────

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

// ─── JIT Execution ─────────────────────────────────────────────────

int CodeGenerator::jitRun() {
    using namespace llvm;
    using namespace llvm::orc;

    auto JIT = LLJITBuilder().create();
    if (!JIT) {
        llvm::errs() << "JIT: " << toString(JIT.takeError()) << "\n";
        return 1;
    }

    // Luna runtime calls are language ABI symbols, not ambient process
    // symbols. Register every referenced helper explicitly so JIT behavior
    // does not depend on ELF -rdynamic, Mach-O export policy, or Windows
    // __declspec(dllexport). System/user-library symbols remain available via
    // the target-process search generator below.
    SymbolMap runtimeSymbols;
    const auto exported = JITSymbolFlags::Exported;
    auto bindRuntime = [&](StringRef name, auto* address) {
        if (!mModule->getFunction(name)) return;
        runtimeSymbols[(*JIT)->mangleAndIntern(name)] =
            ExecutorSymbolDef::fromPtr(address, exported);
    };
    bindRuntime("rt_malloc", &rt_malloc);
    bindRuntime("rt_free", &rt_free);
    bindRuntime("rt_array_index_or_abort", &rt_array_index_or_abort);
    bindRuntime("rt_dynamic_fragment_select", &rt_dynamic_fragment_select);
    bindRuntime("rt_dynamic_fragment_matches", &rt_dynamic_fragment_matches);
    bindRuntime("rt_dynamic_fragment_report_unknown_and_abort",
                &rt_dynamic_fragment_report_unknown_and_abort);
    bindRuntime("rt_fragment_plugin_load", &rt_fragment_plugin_load);
    bindRuntime("rt_fragment_plugin_last_error", &rt_fragment_plugin_last_error);
    bindRuntime("rt_fragment_plugin_is_registered", &rt_fragment_plugin_is_registered);
    bindRuntime("rt_fragment_plugin_invoke", &rt_fragment_plugin_invoke);
    bindRuntime("rt_fragment_plugin_report_error_and_abort",
                &rt_fragment_plugin_report_error_and_abort);
    bindRuntime("rt_gpu_initialize", &rt_gpu_initialize);
    bindRuntime("rt_gpu_backend_name", &rt_gpu_backend_name);
    bindRuntime("rt_gpu_last_error", &rt_gpu_last_error);
    bindRuntime("rt_gpu_report_initialization_error",
                &rt_gpu_report_initialization_error);
    bindRuntime("rt_gpu_report_operation_error_and_abort",
                &rt_gpu_report_operation_error_and_abort);
    bindRuntime("rt_gpu_backend_is_cuda", &rt_gpu_backend_is_cuda);
    bindRuntime("rt_gpu_backend_is_rocm", &rt_gpu_backend_is_rocm);
    bindRuntime("rt_gpu_alloc_i32", &rt_gpu_alloc_i32);
    bindRuntime("rt_gpu_free", &rt_gpu_free);
    bindRuntime("rt_gpu_load_i32", &rt_gpu_load_i32);
    bindRuntime("rt_gpu_store_i32", &rt_gpu_store_i32);
    bindRuntime("rt_gpu_copy_from_host_i32", &rt_gpu_copy_from_host_i32);
    bindRuntime("rt_gpu_copy_to_host_i32", &rt_gpu_copy_to_host_i32);
    bindRuntime("rt_gpu_launch_ptx", &rt_gpu_launch_ptx);
    bindRuntime("rt_gpu_launch_hsaco", &rt_gpu_launch_hsaco);
    bindRuntime("rt_gpu_await_event", &rt_gpu_await_event);
    if (!runtimeSymbols.empty()) {
        if (auto err = (*JIT)->getMainJITDylib().define(
                absoluteSymbols(std::move(runtimeSymbols)))) {
            llvm::errs() << "JIT runtime symbols: " << toString(std::move(err)) << "\n";
            return 1;
        }
    }

    auto tsm = ThreadSafeModule(std::move(mModule), std::move(mCtx));
    auto err = (*JIT)->addIRModule(std::move(tsm));
    if (err) {
        llvm::errs() << "JIT: " << toString(std::move(err)) << "\n";
        return 1;
    }

    // Resolve libc and explicitly loaded user-library symbols. Luna's own
    // runtime symbols were defined above and never rely on this fallback.
    auto &ES = (*JIT)->getExecutionSession();
    auto dlsg = EPCDynamicLibrarySearchGenerator::GetForTargetProcess(ES);
    if (dlsg) {
        (*JIT)->getMainJITDylib().addGenerator(std::move(*dlsg));
    }

    auto mainSym = (*JIT)->lookup("main");
    if (!mainSym) {
        llvm::errs() << "JIT: " << toString(mainSym.takeError()) << "\n";
        return 1;
    }

    auto mainFn = jitTargetAddressToPointer<int(*)()>(mainSym->getValue());
    return mainFn();
}

// ─── AOT: Emit LLVM bitcode ────────────────────────────────────────

bool CodeGenerator::emitObjectFile(const std::string& outputPath) {
    auto targetTriple = llvm::sys::getProcessTriple();
    mModule->setTargetTriple(llvm::Triple(targetTriple));

    std::error_code ec;
    llvm::raw_fd_ostream dest(outputPath, ec, llvm::sys::fs::OF_None);
    if (ec) {
        error("Cannot open output file: " + ec.message());
        return false;
    }

    mModule->print(dest, nullptr); // text IR, avoids bitcode compat issues
    dest.flush();
    return true;
}
