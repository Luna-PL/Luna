#include "CodeGenerator.h"

#include <cstdlib>
#include <cstring>
#include <iterator>
#include <optional>

#include <llvm/ADT/SmallString.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/IntrinsicsAMDGPU.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
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

using moon::AddrOfExpr;
using moon::BorrowExpr;
using moon::Expr;
using moon::FunctionDecl;
using moon::IdentifierExpr;
using moon::LaunchExpr;
using moon::MoveExpr;

namespace {

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
    auto pipeline = passBuilder.buildPerModuleDefaultPipeline(
        llvm::OptimizationLevel::O3);
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
    static constexpr const char hostTarget[] =
        "host-x86_64-unknown-linux-gnu-";
    const std::string deviceTarget =
        "hipv4-amdgcn-amd-amdhsa--" + architecture;

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
bool dumpHsacoIfRequested(const std::string& hsaco,
                          const std::string& symbol,
                          const std::string& architecture,
                          std::string& error) {
    const char* requestedDirectory = std::getenv("LUNA_GPU_DUMP_HSACO");
    if (!requestedDirectory || !*requestedDirectory) return true;

    llvm::SmallString<256> outputPath(requestedDirectory);
    llvm::sys::path::append(
        outputPath, "luna-" + architecture + "-" + symbol + ".hsaco");
    std::error_code streamError;
    llvm::raw_fd_ostream output(
        outputPath, streamError, llvm::sys::fs::OF_None);
    if (streamError) {
        error = "could not write requested HSACO dump '" +
                outputPath.str().str() + "': " + streamError.message();
        return false;
    }
    output.write(hsaco.data(), hsaco.size());
    output.flush();
    if (output.has_error()) {
        error = "could not finish requested HSACO dump '" +
                outputPath.str().str() + "'";
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
            auto* gep =
                llvm::dyn_cast<llvm::GetElementPtrInst>(&instruction);
            auto* cast = gep
                ? llvm::dyn_cast<llvm::AddrSpaceCastInst>(
                      gep->getPointerOperand())
                : nullptr;
            if (cast && cast->getSrcAddressSpace() == 1 &&
                cast->getDestAddressSpace() == 0)
                candidates.push_back(gep);
        }
    }
    for (auto* genericGep : candidates) {
        auto* cast = llvm::cast<llvm::AddrSpaceCastInst>(
            genericGep->getPointerOperand());
        llvm::IRBuilder<> builder(genericGep);
        llvm::SmallVector<llvm::Value*, 4> indices;
        for (auto& index : genericGep->indices())
            indices.push_back(index.get());
        auto* globalGep = llvm::cast<llvm::GetElementPtrInst>(
            builder.CreateGEP(
                genericGep->getSourceElementType(),
                cast->getPointerOperand(),
                indices,
                genericGep->getName() + ".global"));
        globalGep->setIsInBounds(genericGep->isInBounds());

        std::vector<llvm::User*> users;
        for (auto* user : genericGep->users()) users.push_back(user);
        bool onlyDirectMemoryUses = true;
        for (auto* user : users) {
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(user)) {
                if (load->getPointerOperand() == genericGep) {
                    load->setOperand(
                        load->getPointerOperandIndex(), globalGep);
                    continue;
                }
            } else if (auto* store =
                           llvm::dyn_cast<llvm::StoreInst>(user)) {
                if (store->getPointerOperand() == genericGep) {
                    store->setOperand(
                        store->getPointerOperandIndex(), globalGep);
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
            auto* cast =
                llvm::dyn_cast<llvm::AddrSpaceCastInst>(&instruction);
            if (cast && cast->use_empty()) unusedCasts.push_back(cast);
        }
    }
    for (auto* cast : unusedCasts) cast->eraseFromParent();
}

} // namespace

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
#if LLVM_VERSION_MAJOR >= 22
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        llvm::Triple(targetTriple), targetError);
#else
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        targetTriple, targetError);
#endif
    if (!target) {
        error("CUDA backend requires LLVM NVPTX support: " + targetError);
        return false;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 22
        llvm::Triple(targetTriple), mGpuTargets.cudaArchitecture, "", options,
#else
        targetTriple, mGpuTargets.cudaArchitecture, "", options,
#endif
        llvm::Reloc::PIC_, std::nullopt,
        llvm::CodeGenOptLevel::Aggressive));
    if (!machine) {
        error("could not create LLVM NVPTX target machine for kernel '" +
              kernel->name + "'");
        return false;
    }

    auto deviceModule =
        std::make_unique<llvm::Module>("cuda." + symbol, *mCtx);
#if LLVM_VERSION_MAJOR >= 22
    deviceModule->setTargetTriple(llvm::Triple(targetTriple));
#else
    deviceModule->setTargetTriple(targetTriple);
#endif
    deviceModule->setDataLayout(machine->createDataLayout());
    auto* deviceFunction = llvm::Function::Create(
        source->second->getFunctionType(),
        llvm::Function::ExternalLinkage,
        symbol,
        deviceModule.get());
    deviceFunction->setCallingConv(llvm::CallingConv::PTX_Kernel);

    llvm::ValueToValueMapTy valueMap;
    if (auto* sourceTrap = mModule->getFunction("llvm.trap"))
        valueMap[sourceTrap] = llvm::Intrinsic::getOrInsertDeclaration(
            deviceModule.get(), llvm::Intrinsic::trap);
    auto destinationArgument = deviceFunction->arg_begin();
    for (auto& sourceArgument : source->second->args()) {
        destinationArgument->setName(sourceArgument.getName());
        valueMap[&sourceArgument] = &*destinationArgument;
        ++destinationArgument;
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(
        deviceFunction, source->second, valueMap,
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
        error("could not locate the generated index slot for CUDA kernel '" +
              kernel->name + "'");
        return false;
    }
    llvm::IRBuilder<> deviceBuilder(*mCtx);
    deviceBuilder.SetInsertPoint(
        indexParameterStore->getParent(),
        std::next(indexParameterStore->getIterator()));
    auto indexIntrinsic = [&](const char* name) {
        auto intrinsic = deviceModule->getOrInsertFunction(
            name, llvm::FunctionType::get(mHelpers->i32Ty(), false));
        return deviceBuilder.CreateCall(
            intrinsic, {}, "cuda.index.reg");
    };
    auto* block = indexIntrinsic("llvm.nvvm.read.ptx.sreg.ctaid.x");
    auto* blockWidth = indexIntrinsic("llvm.nvvm.read.ptx.sreg.ntid.x");
    auto* thread = indexIntrinsic("llvm.nvvm.read.ptx.sreg.tid.x");
    auto* globalIndex = deviceBuilder.CreateAdd(
        deviceBuilder.CreateMul(
            block, blockWidth, "cuda.block.base"),
        thread,
        "cuda.index");
    deviceBuilder.CreateStore(
        globalIndex, indexParameterStore->getPointerOperand());

    if (llvm::verifyFunction(*deviceFunction, &llvm::errs())) {
        error("generated invalid LLVM IR while preparing CUDA kernel '" +
              kernel->name + "'");
        return false;
    }
    auto* annotations =
        deviceModule->getOrInsertNamedMetadata("nvvm.annotations");
    annotations->addOperand(llvm::MDNode::get(*mCtx, {
        llvm::ValueAsMetadata::get(deviceFunction),
        llvm::MDString::get(*mCtx, "kernel"),
        llvm::ConstantAsMetadata::get(
            llvm::ConstantInt::get(mHelpers->i32Ty(), 1))
    }));
    optimizeDeviceModule(*deviceModule);

    llvm::SmallString<0> ptx;
    llvm::raw_svector_ostream stream(ptx);
    llvm::legacy::PassManager passManager;
    if (machine->addPassesToEmitFile(
            passManager, stream, nullptr,
            llvm::CodeGenFileType::AssemblyFile)) {
        error("LLVM NVPTX backend cannot emit PTX for kernel '" +
              kernel->name + "'");
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
        error("cannot emit AMDGPU code object for unknown kernel '" +
              kernel->name + "'");
        return false;
    }

    constexpr const char* targetTriple = "amdgcn-amd-amdhsa";
    const std::string& architecture = mGpuTargets.rocmArchitecture;
    std::string targetError;
#if LLVM_VERSION_MAJOR >= 22
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        llvm::Triple(targetTriple), targetError);
#else
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        targetTriple, targetError);
#endif
    if (!target) {
        error("ROCm backend requires LLVM AMDGPU support: " + targetError);
        return false;
    }
    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 22
        llvm::Triple(targetTriple), architecture, "", options,
#else
        targetTriple, architecture, "", options,
#endif
        llvm::Reloc::PIC_, std::nullopt,
        llvm::CodeGenOptLevel::Aggressive));
    if (!machine) {
        error("could not create LLVM AMDGPU target machine for architecture '" +
              architecture + "'");
        return false;
    }

    auto deviceModule =
        std::make_unique<llvm::Module>("rocm." + symbol, *mCtx);
#if LLVM_VERSION_MAJOR >= 22
    deviceModule->setTargetTriple(llvm::Triple(targetTriple));
#else
    deviceModule->setTargetTriple(targetTriple);
#endif
    deviceModule->setDataLayout(machine->createDataLayout());
    llvm::SmallVector<llvm::Type*, 8> deviceParameterTypes;
    for (llvm::Type* parameter :
         source->second->getFunctionType()->params()) {
        // The initial device ABI only permits device-buffer references for
        // pointer parameters. Represent them as HSA global pointers.
        deviceParameterTypes.push_back(parameter->isPointerTy()
            ? llvm::PointerType::get(*mCtx, 1) : parameter);
    }
    auto* deviceFunctionType = llvm::FunctionType::get(
        source->second->getReturnType(), deviceParameterTypes, false);
    auto* deviceFunction = llvm::Function::Create(
        deviceFunctionType,
        llvm::Function::ExternalLinkage,
        symbol,
        deviceModule.get());
    deviceFunction->setCallingConv(llvm::CallingConv::AMDGPU_KERNEL);

    llvm::ValueToValueMapTy valueMap;
    if (auto* sourceTrap = mModule->getFunction("llvm.trap"))
        valueMap[sourceTrap] = llvm::Intrinsic::getOrInsertDeclaration(
            deviceModule.get(), llvm::Intrinsic::trap);
    auto* abiBlock =
        llvm::BasicBlock::Create(*mCtx, "abi", deviceFunction);
    llvm::IRBuilder<> abiBuilder(abiBlock);
    auto destinationArgument = deviceFunction->arg_begin();
    for (auto& sourceArgument : source->second->args()) {
        destinationArgument->setName(sourceArgument.getName());
        llvm::Value* mappedArgument = &*destinationArgument;
        if (mappedArgument->getType() != sourceArgument.getType())
            mappedArgument = abiBuilder.CreateAddrSpaceCast(
                mappedArgument,
                sourceArgument.getType(),
                sourceArgument.getName() + ".generic");
        valueMap[&sourceArgument] = mappedArgument;
        ++destinationArgument;
    }
    llvm::SmallVector<llvm::ReturnInst*, 4> returns;
    llvm::CloneFunctionInto(
        deviceFunction, source->second, valueMap,
        llvm::CloneFunctionChangeType::DifferentModule, returns);
    if (std::next(deviceFunction->begin()) == deviceFunction->end()) {
        error("could not clone ROCm kernel body for '" +
              kernel->name + "'");
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
            if (auto* alloca =
                    llvm::dyn_cast<llvm::AllocaInst>(&instruction))
                hostAllocas.push_back(alloca);
        }
    }
    for (auto* hostAlloca : hostAllocas) {
        llvm::IRBuilder<> allocaBuilder(hostAlloca);
        auto* privateAlloca = allocaBuilder.CreateAlloca(
            hostAlloca->getAllocatedType(),
            5,
            hostAlloca->getArraySize(),
            hostAlloca->getName() + ".private");
        privateAlloca->setAlignment(hostAlloca->getAlign());
        auto* genericPointer = allocaBuilder.CreateAddrSpaceCast(
            privateAlloca,
            hostAlloca->getType(),
            hostAlloca->getName() + ".generic");
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
                auto* store =
                    llvm::dyn_cast<llvm::StoreInst>(&instruction);
                if (store && store->getValueOperand() == indexParameter) {
                    indexParameterStore = store;
                    break;
                }
            }
            if (indexParameterStore) break;
        }
    }
    if (!indexParameterStore) {
        error("could not locate the generated index slot for ROCm kernel '" +
              kernel->name + "'");
        return false;
    }
    llvm::IRBuilder<> deviceBuilder(*mCtx);
    deviceBuilder.SetInsertPoint(
        indexParameterStore->getParent(),
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
        workgroup,
        llvm::ConstantInt::get(mHelpers->i32Ty(), 256),
        "amdgpu.group.base");
    auto* globalIndex = deviceBuilder.CreateAdd(
        workgroupBase, workitem, "amdgpu.index");
    deviceBuilder.CreateStore(
        globalIndex, indexParameterStore->getPointerOperand());

    if (llvm::verifyFunction(*deviceFunction, &llvm::errs())) {
        error("generated invalid LLVM IR while preparing ROCm kernel '" +
              kernel->name + "'");
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
        error("generated invalid optimized LLVM IR while preparing ROCm "
              "kernel '" + kernel->name + "'");
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
        error("could not create a temporary AMDGPU object file: " +
              fileError.message());
        return false;
    }
    fileError = llvm::sys::fs::createTemporaryFile(
        "luna-amdgpu-kernel", "hsaco", linkedPath);
    if (fileError) {
        cleanup();
        error("could not create a temporary HSA code-object file: " +
              fileError.message());
        return false;
    }

    llvm::raw_fd_ostream stream(
        objectPath, fileError, llvm::sys::fs::OF_None);
    if (fileError) {
        cleanup();
        error("could not write the temporary AMDGPU object file: " +
              fileError.message());
        return false;
    }
    llvm::legacy::PassManager passManager;
    if (machine->addPassesToEmitFile(
            passManager, stream, nullptr,
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
        error("ROCm code-object emission requires LLVM's 'ld.lld' linker "
              "in PATH");
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
    auto linkedObject =
        llvm::MemoryBuffer::getFile(linkedPath, false, false);
    if (!linkedObject) {
        cleanup();
        error("could not read linked HSA code object for kernel '" +
              kernel->name + "': " +
              linkedObject.getError().message());
        return false;
    }
    const std::string linkedHsaco((*linkedObject)->getBuffer().str());
    cleanup();
    std::string dumpError;
    if (!dumpHsacoIfRequested(
            linkedHsaco, symbol, architecture, dumpError)) {
        error(dumpError);
        return false;
    }
    mKernelHSACO.emplace(
        symbol, makeHipModuleBundle(linkedHsaco, architecture));
    return true;
}
