#include "CodeGenerator.h"

#include <llvm/Config/llvm-config.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <memory>
#include <optional>

using moon::FunctionDecl;
using moon::ImplDecl;

namespace {

std::unique_ptr<llvm::TargetMachine> createHostOptimizationTarget(
    llvm::Module& module, LunaOptimizationLevel level, std::string& error) {
    const std::string targetTriple = llvm::sys::getProcessTriple();
#if LLVM_VERSION_MAJOR >= 22
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        llvm::Triple(targetTriple), error);
    module.setTargetTriple(llvm::Triple(targetTriple));
#else
    const llvm::Target* target = llvm::TargetRegistry::lookupTarget(
        targetTriple, error);
    module.setTargetTriple(targetTriple);
#endif
    if (!target) return nullptr;

    llvm::TargetOptions options;
    std::unique_ptr<llvm::TargetMachine> machine(target->createTargetMachine(
#if LLVM_VERSION_MAJOR >= 22
        llvm::Triple(targetTriple), "generic", "", options,
#else
        targetTriple, "generic", "", options,
#endif
        llvm::Reloc::PIC_, std::nullopt,
        level == LunaOptimizationLevel::O3
            ? llvm::CodeGenOptLevel::Aggressive
            : llvm::CodeGenOptLevel::Default));
    if (!machine) {
        error = "could not create the LLVM host target machine";
        return nullptr;
    }
    module.setDataLayout(machine->createDataLayout());
    return machine;
}

} // namespace

bool CodeGenerator::generate(moon::Module* program) {
    mProgram = program;
    mHostTargetMachine.reset();
    mTypeMaterializer = std::make_unique<moon::TypeMaterializer>(*program);
    mFunctions.clear();
    mDropCallbacks.clear();
    mKernelPTX.clear();
    mKernelHSACO.clear();

    auto declareFunc = [&](FunctionDecl* f) {
        if (f->isSelector) return;
        if (f->isKernel && !f->isCodegenReachable) return;
        if (!f->typeParams.empty() && !f->isTemplateInstance) return;
        std::vector<llvm::Type*> paramLLVMTypes;
        for (auto& p : f->params) {
            const TypePtr type = resolveType(p.type);
            if (f->isKernel && type && type->kind == TypeKind::Reference &&
                type->inner && type->inner->kind == TypeKind::DeviceBuffer) {
                // Native GPU ABIs handle scalar parameters predictably. Keep
                // the bounds-carrying source value explicit as (data, length)
                // instead of relying on target-specific aggregate lowering.
                paramLLVMTypes.push_back(mHelpers->ptrTy());
                paramLLVMTypes.push_back(mHelpers->sizeTy());
            } else {
                paramLLVMTypes.push_back(mHelpers->toLLVMType(type));
            }
        }
        const TypePtr returnType = resolveType(f->returnType);
        llvm::Type* retLLVMType = returnType
            ? mHelpers->toLLVMType(returnType)
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
        if (returnType && returnType->kind == TypeKind::Never)
            function->addFnAttr(llvm::Attribute::NoReturn);
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
        std::string targetError;
        mHostTargetMachine = createHostOptimizationTarget(
            *mModule, mOptimizationLevel, targetError);
        if (!mHostTargetMachine) {
            error("cannot configure target-aware host optimization: " +
                  targetError);
            return false;
        }
        llvm::LoopAnalysisManager loopAnalyses;
        llvm::FunctionAnalysisManager functionAnalyses;
        llvm::CGSCCAnalysisManager cgsccAnalyses;
        llvm::ModuleAnalysisManager moduleAnalyses;
        // Supplying the target machine is what makes TTI available to the
        // vectorizer and loop cost model. Without it, JIT code is optimized
        // generically and AOT only recovers after clang runs a second O2/O3
        // middle-end pipeline over the emitted IR.
        llvm::PassBuilder passBuilder(mHostTargetMachine.get());
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
    }

    // Runtime's lightweight default profile already owns allocation and
    // console output. Install the heavier application profile only for input,
    // filesystem, direct host-service access, or the currently conservative
    // GPU application boundary. In particular, print-only programs must not
    // pull the file registry into their native artifact.
    bool needsApplicationHost = mProgram && mProgram->features.kernel;
    for (const auto& function : *mModule) {
        if (function.use_empty()) continue;
        const llvm::StringRef name = function.getName();
        if (name == "rt_console_read_v1" ||
            name == "rt_console_read_line_lossy_v1" ||
            name.starts_with("rt_file_") || name.starts_with("rt_path_") ||
            name == "rt_remove_file_v1" || name == "rt_create_directory_v1" ||
            name == "rt_host_services_v1") {
            needsApplicationHost = true;
            break;
        }
    }
    if (needsApplicationHost) {
        if (auto* mainFunction = mModule->getFunction("main");
            mainFunction && !mainFunction->empty()) {
            auto installApplicationHost = mModule->getOrInsertFunction(
                "rt_install_application_host_services_v1", mHelpers->i32Ty());
            llvm::IRBuilder<> entryBuilder(&*mainFunction->getEntryBlock().getFirstInsertionPt());
            entryBuilder.CreateCall(installApplicationHost);
        }
    }
    if (mOptimizationLevel != LunaOptimizationLevel::O0 &&
        verifyHostModule(" after optimization"))
        return false;
    return mErrors.empty();
}
