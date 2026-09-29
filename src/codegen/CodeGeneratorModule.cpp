#include "CodeGenerator.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/Verifier.h"

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

bool matchesPrivateRefContextEffect(
    const moon::Module& program, const FunctionDecl& function) {
    const auto effects = moon::computeFragmentContextEffects(program);
    const auto found = effects.find(moon::fragmentContextEffectKey(
        {function.symbolId, function.contractId}));
    const bool inferred = found != effects.end() && found->second;
    return function.requiresFragmentContext == inferred;
}

} // namespace

bool CodeGenerator::verifyPrivateRuntimeFragmentRefUnitIngress(
    moon::Module& program, FunctionDecl& function, std::string& failure) {
    // Never reuse the publishing CodeGenerator. Even a successful proof is
    // destroyed here, so its raw-pointer body cannot reach emitObjectFile or
    // materializeJitModule while the source/container gates remain closed.
    CodeGenerator proof("private.ref.ingress.proof");
    proof.mProgram = &program;
    proof.mTypeMaterializer =
        std::make_unique<moon::TypeMaterializer>(program);
    if (function.generatedSymbolName.empty()) {
        failure = "function has no generated symbol";
        return false;
    }
    // The module-level verifier intentionally rejects all source Refs today.
    // Validate the sealed CFG on its own before asking codegen to traverse it.
    moon::Verifier cfgVerifier;
    if (!function.controlFlow || !function.controlFlow->sealed ||
        function.body || function.isExtern ||
        !cfgVerifier.verify(*function.controlFlow, program)) {
        failure = "function has no independently verified sealed CFG";
        return false;
    }
    const bool privateRefApply =
        !function.controlFlow->runtimeRefApplies.empty();
    if (!matchesPrivateRefContextEffect(program, function)) {
        failure = "Ref entry context effect differs from the sealed CFG fixed point";
        return false;
    }
    std::vector<llvm::Type*> parameters;
    if (function.requiresFragmentContext)
        parameters.push_back(proof.mHelpers->ptrTy());
    parameters.push_back(proof.mHelpers->ptrTy());
    auto* body = llvm::Function::Create(
        llvm::FunctionType::get(proof.mHelpers->voidTy(), parameters, false),
        llvm::Function::InternalLinkage,
        function.generatedSymbolName, *proof.mModule);
    proof.mFunctions[function.generatedSymbolName] = body;
    proof.mPrivateRefApplyEnabled = privateRefApply;
    proof.generateFunctionBody(&function);
    if (!proof.mErrors.empty()) {
        failure = proof.mErrors.front().message;
        return false;
    }
    if (privateRefApply) {
        std::string flowError;
        const auto flow = moon::planRuntimeRefApplyFlow(
            *function.controlFlow, flowError);
        if (!flow) {
            failure = "verified Ref apply body lost its context flow: " + flowError;
            return false;
        }
        size_t derivations = 0, contextDrops = 0, dispatches = 0;
        llvm::CallInst* deriveCall = nullptr;
        llvm::CallInst* dropCall = nullptr;
        llvm::CallInst* dispatchCall = nullptr;
        std::vector<llvm::CallInst*> refDrops;
        for (auto& block : *body)
            for (auto& instruction : block)
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    call && call->getCalledFunction()) {
                    const auto name = call->getCalledFunction()->getName();
                    if (name ==
                        "luna_compiler_fragment_context_override_from_ref") {
                        ++derivations;
                        deriveCall = call;
                    } else if (name ==
                        "luna_compiler_fragment_context_drop") {
                        ++contextDrops;
                        dropCall = call;
                    } else if (name ==
                        "luna_runtime_fragment_dispatch_v1") {
                        ++dispatches;
                        dispatchCall = call;
                    } else if (name ==
                        "luna_runtime_fragment_ref_drop_v1") {
                        refDrops.push_back(call);
                    }
                }
        const auto* dispatchContext = dispatchCall
            ? llvm::dyn_cast<llvm::LoadInst>(dispatchCall->getArgOperand(0))
            : nullptr;
        if (!body->hasInternalLinkage() || derivations != 1 ||
            contextDrops != 1 || dispatches != 1 || !deriveCall ||
            !dropCall || !dispatchContext ||
            deriveCall->getArgOperand(0) != body->getArg(0) ||
            deriveCall->getArgOperand(4) != dropCall->getArgOperand(0) ||
            dispatchContext->getPointerOperand() !=
                deriveCall->getArgOperand(4)) {
            failure = "generated Ref apply body lacks one connected context lifetime";
            return false;
        }
        if (!flow->terminals.empty()) {
            llvm::BasicBlock* returnBlock = nullptr;
            const auto blockName = "cfg." +
                std::to_string(flow->terminals.front().block.value);
            for (auto& block : *body)
                if (block.getName() == blockName) returnBlock = &block;
            auto* returnInst = returnBlock
                ? llvm::dyn_cast<llvm::ReturnInst>(
                    returnBlock->getTerminator())
                : nullptr;
            if (!returnInst || dropCall->getParent() != returnInst->getParent() ||
                !dropCall->comesBefore(returnInst)) {
                failure = "early Ref apply return does not release its context";
                return false;
            }
            const auto* borrowed = llvm::dyn_cast<llvm::LoadInst>(
                deriveCall->getArgOperand(1));
            if (!borrowed) {
                failure = "early Ref apply did not borrow a local Ref cell";
                return false;
            }
            for (const auto* refDrop : refDrops)
                if (refDrop->getParent() == returnInst->getParent() &&
                    refDrop->getArgOperand(0) == borrowed->getPointerOperand() &&
                    !dropCall->comesBefore(refDrop)) {
                    failure = "early Ref apply released its Ref before its context";
                    return false;
                }
        }
        std::string invalidIR;
        llvm::raw_string_ostream stream(invalidIR);
        if (llvm::verifyModule(*proof.mModule, &stream)) {
            stream.flush();
            failure = "generated Ref apply LLVM IR is invalid: " + invalidIR;
            return false;
        }
        failure.clear();
        return true;
    }
    auto* wrapper = proof.mHelpers->emitRuntimeFragmentRefUnitIngressWrapper(
        *proof.mModule, *body, program, function,
        "__private_ref_ingress_proof");
    if (!wrapper || body->empty() || !body->hasInternalLinkage() ||
        !wrapper->hasInternalLinkage()) {
        failure = "frozen signature/CFG did not pair with the generated body";
        return false;
    }
    size_t bodyDropCalls = 0;
    size_t wrapperBodyCalls = 0;
    for (auto& block : *body)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                call && call->getCalledFunction() &&
                call->getCalledFunction()->getName() ==
                    "luna_runtime_fragment_ref_drop_v1")
                ++bodyDropCalls;
    for (auto& block : *wrapper)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                call && call->getCalledFunction() == body)
                ++wrapperBodyCalls;
    const bool owned = function.params.front().relation ==
        luna::ownership::Relation::Owned;
    if (wrapperBodyCalls != 1 ||
        (owned ? bodyDropCalls == 0 : bodyDropCalls != 0)) {
        failure = "generated body has no matching Ref Drop behavior";
        return false;
    }
    std::string invalidIR;
    llvm::raw_string_ostream stream(invalidIR);
    if (llvm::verifyModule(*proof.mModule, &stream)) {
        stream.flush();
        failure = "generated body/wrapper LLVM IR is invalid: " + invalidIR;
        return false;
    }
    failure.clear();
    return true;
}

bool CodeGenerator::verifyPrivateRuntimeFragmentRefOwnedReturn(
    moon::Module& program, FunctionDecl& function, std::string& failure) {
    // This deliberately proves only the direct affine parameter round-trip.
    // The host output carrier, failure protocol and publication are separate.
    const auto* result = program.findType(function.returnType);
    const auto* parameter = program.findType(function.params.front().type);
    const auto* record = program.findDeclarationById(function.declarationId);
    const auto* callable = record ? program.findType(record->type) : nullptr;
    if (!program.typeTableSealed || !result || !parameter ||
        result->kind != TypeKind::RuntimeFragmentRef ||
        parameter->kind != TypeKind::RuntimeFragmentRef ||
        result->id != parameter->id ||
        !program.resolveRuntimeFragmentRefTarget(result->id) ||
        result->sysmeta.resource.management !=
            luna::sysmeta::ResourceManagement::Unique ||
        result->sysmeta.resource.releaseDomain !=
            luna::sysmeta::ReleaseDomain::Executable ||
        result->sysmeta.resource.lifetime !=
            luna::sysmeta::ResourceLifetime::Lexical ||
        result->sysmeta.resource.relation != luna::ownership::Relation::Owned ||
        result->sysmeta.resource.usage != luna::ownership::Usage::Affine ||
        !result->sysmeta.resource.cleanupRequired ||
        !result->sysmeta.resource.needsDrop ||
        result->sysmeta.resource.cleanup !=
            luna::ownership::CleanupAction::Drop ||
        !result->sysmeta.capability.hostOnly ||
        function.params.front().relation != luna::ownership::Relation::Owned ||
        function.params.front().usage != luna::ownership::Usage::Affine ||
        function.returnUsage != luna::ownership::Usage::Affine ||
        function.returnsLinear || function.requiresFragmentContext ||
        function.isExtern || function.isKernel || function.isSelector ||
        !function.typeParams.empty() || function.body ||
        function.generatedSymbolName.empty() || !function.linkName.empty() ||
        !record || record->kind != moon::DeclarationKind::Function ||
        record->symbolId != function.symbolId ||
        record->contractId != function.contractId ||
        record->linkageName != function.generatedSymbolName ||
        record->canonicalContract != moon::canonicalContract(*record) ||
        record->contractId != luna::identity::contractIdFromCanonical(
            record->canonicalContract) ||
        record->sysmeta.identity.symbol != record->symbolId ||
        record->sysmeta.identity.contract != record->contractId ||
        !callable || callable->kind != TypeKind::Function ||
        callable->parameterTypeIds.size() != 1 ||
        callable->parameterTypeIds.front() != parameter->id ||
        callable->returnTypeId != result->id ||
        callable->parameterContracts.size() != 1 ||
        callable->parameterContracts.front() != luna::ownership::Contract{
            luna::ownership::Relation::Owned, luna::ownership::Usage::Affine} ||
        callable->returnContract != luna::ownership::Contract{
            luna::ownership::Relation::Owned, luna::ownership::Usage::Affine}) {
        failure = "owned Ref return has no matching frozen callable";
        return false;
    }
    if (!function.controlFlow || !function.controlFlow->sealed) {
        failure = "owned Ref return has no sealed CFG";
        return false;
    }
    moon::Verifier cfgVerifier;
    if (!cfgVerifier.verify(*function.controlFlow, program)) {
        failure = "owned Ref return CFG failed independent verification";
        return false;
    }
    if (!function.controlFlow->runtimeRefApplies.empty()) {
        failure = "Ref apply context override is not executable";
        return false;
    }
    if (!matchesPrivateRefContextEffect(program, function)) {
        failure = "owned Ref return context effect differs from the sealed CFG fixed point";
        return false;
    }
    const moon::LocalRecord* parameterLocal = nullptr;
    for (const auto& local : function.controlFlow->locals) {
        if (local.kind != moon::LocalKind::Parameter) continue;
        if (parameterLocal) {
            failure = "owned Ref return has multiple CFG parameters";
            return false;
        }
        parameterLocal = &local;
    }
    if (!parameterLocal || parameterLocal->scope !=
            function.controlFlow->rootScope ||
        parameterLocal->name != function.params.front().name ||
        parameterLocal->type != function.params.front().type ||
        parameterLocal->relation != luna::ownership::Relation::Owned ||
        parameterLocal->usage != luna::ownership::Usage::Affine) {
        failure = "owned Ref return CFG parameter differs from declaration";
        return false;
    }
    size_t parameterCleanups = 0;
    for (const auto& cleanup : function.controlFlow->cleanups) {
        if (cleanup.place.root != parameterLocal->id) continue;
        ++parameterCleanups;
        if (!cleanup.place.projections.empty() || cleanup.guard ||
            cleanup.scope != function.controlFlow->rootScope ||
            cleanup.type != parameterLocal->type ||
            cleanup.kind != moon::CleanupKind::Value ||
            cleanup.action != luna::ownership::CleanupAction::Drop) {
            failure = "owned Ref return has a noncanonical parameter cleanup";
            return false;
        }
    }
    if (parameterCleanups != 1) {
        failure = "owned Ref return has no unique parameter Drop cleanup";
        return false;
    }
    size_t directReturns = 0;
    for (const auto& block : function.controlFlow->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Return) continue;
        const auto* identifier = dynamic_cast<const moon::IdentifierExpr*>(
            block.terminator.operand.get());
        if (!identifier || identifier->local != parameterLocal->id) {
            failure = "owned Ref return is not a direct parameter transfer";
            return false;
        }
        ++directReturns;
    }
    if (!directReturns) {
        failure = "owned Ref return has no direct return path";
        return false;
    }

    CodeGenerator proof("private.ref.owned.return.proof");
    proof.mProgram = &program;
    proof.mTypeMaterializer =
        std::make_unique<moon::TypeMaterializer>(program);
    auto* body = llvm::Function::Create(
        llvm::FunctionType::get(proof.mHelpers->ptrTy(),
                                {proof.mHelpers->ptrTy()}, false),
        llvm::Function::InternalLinkage,
        function.generatedSymbolName, *proof.mModule);
    proof.mFunctions[function.generatedSymbolName] = body;
    proof.generateFunctionBody(&function);
    if (!proof.mErrors.empty() || body->empty()) {
        failure = proof.mErrors.empty()
            ? "owned Ref return generated no body"
            : proof.mErrors.front().message;
        return false;
    }
    size_t returnedHandles = 0;
    for (auto& block : *body) {
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(
                    &instruction); call &&
                (!call->getCalledFunction() ||
                 call->getCalledFunction()->getName() !=
                     "luna_runtime_fragment_ref_drop_v1")) {
                failure = "owned Ref round-trip body contains a callback";
                return false;
            }
        auto* returned = llvm::dyn_cast_or_null<llvm::ReturnInst>(
            block.getTerminator());
        if (!returned) continue;
        auto* taken = llvm::dyn_cast_or_null<llvm::LoadInst>(
            returned->getReturnValue());
        if (!taken || !llvm::isa<llvm::AllocaInst>(
                taken->getPointerOperand())) {
            failure = "owned Ref return did not take a local carrier";
            return false;
        }
        auto* cell = taken->getPointerOperand();
        bool parameterStored = false;
        bool carrierCleared = false;
        for (auto& candidateBlock : *body)
            for (auto& instruction : candidateBlock)
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(
                        &instruction); store && store->getPointerOperand() == cell &&
                    store->getValueOperand() == body->getArg(0))
                    parameterStored = true;
        for (auto& instruction : block) {
            if (&instruction == taken) {
                carrierCleared = false;
            } else if (auto* store = llvm::dyn_cast<llvm::StoreInst>(
                           &instruction); store &&
                       store->getPointerOperand() == cell &&
                       llvm::isa<llvm::ConstantPointerNull>(
                           store->getValueOperand())) {
                carrierCleared = true;
            }
        }
        if (!parameterStored || !carrierCleared) {
            failure = "owned Ref return did not clear its parameter carrier";
            return false;
        }
        ++returnedHandles;
    }
    if (!body->hasInternalLinkage() || returnedHandles != directReturns) {
        failure = "generated owned Ref return does not match CFG return paths";
        return false;
    }
    auto* wrapper = proof.mHelpers->emitRuntimeFragmentRefOwnedReturnWrapper(
        *proof.mModule, *body, program, function,
        "__private_ref_owned_return_proof");
    if (!wrapper || !wrapper->hasInternalLinkage()) {
        failure = "generated owned Ref body has no matching host carrier wrapper";
        return false;
    }
    size_t bodyCalls = 0;
    size_t transferCalls = 0;
    size_t failureDrops = 0;
    for (auto& block : *wrapper)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                call && call->getCalledFunction()) {
                const auto* callee = call->getCalledFunction();
                if (callee == body) ++bodyCalls;
                else if (callee->getName() ==
                         "luna_runtime_fragment_ref_transfer_v1")
                    ++transferCalls;
                else if (callee->getName() ==
                         "luna_runtime_fragment_ref_drop_v1")
                    ++failureDrops;
            }
    if (bodyCalls != 1 || transferCalls != 2 || failureDrops != 1) {
        failure = "host return carrier has no single-owner transfer path";
        return false;
    }
    std::string invalidIR;
    llvm::raw_string_ostream stream(invalidIR);
    if (llvm::verifyModule(*proof.mModule, &stream)) {
        stream.flush();
        failure = "generated owned Ref return LLVM IR is invalid: " + invalidIR;
        return false;
    }
    failure.clear();
    return true;
}

bool CodeGenerator::generate(moon::Module* program) {
    if (!program) {
        error("code generation has no MoonIR module");
        return false;
    }
    bool containsRef = false;
    for (const auto& type : program->typeTable) {
        if (type.kind != TypeKind::RuntimeFragmentRef) continue;
        containsRef = true;
        if (!program->resolveRuntimeFragmentRefTarget(type.id)) {
            error("RuntimeFragmentRef has no frozen nominal Slot/Contract target");
            return false;
        }
    }
    if (containsRef) {
        size_t provenEntries = 0;
        size_t provenReturns = 0;
        size_t provenApplies = 0;
        for (auto& declaration : program->declarations) {
            auto* function = dynamic_cast<FunctionDecl*>(declaration.get());
            if (!function || function->params.size() != 1) continue;
            const auto* parameter = program->findType(
                function->params.front().type);
            const auto* result = program->findType(function->returnType);
            if (!parameter || !result ||
                parameter->kind != TypeKind::RuntimeFragmentRef) continue;
            std::string failure;
            if (result->kind == TypeKind::Unit) {
                if (verifyPrivateRuntimeFragmentRefUnitIngress(
                        *program, *function, failure)) {
                    if (function->controlFlow &&
                        !function->controlFlow->runtimeRefApplies.empty())
                        ++provenApplies;
                    else
                        ++provenEntries;
                } else {
                    error("private RuntimeFragmentRef unit ingress proof failed for '" +
                          function->name + "': " + failure);
                }
            } else if (result->kind == TypeKind::RuntimeFragmentRef) {
                if (verifyPrivateRuntimeFragmentRefOwnedReturn(
                        *program, *function, failure)) {
                    ++provenReturns;
                } else {
                    error("private RuntimeFragmentRef owned return proof failed for '" +
                          function->name + "': " + failure);
                }
            } else {
                continue;
            }
        }
        error("RuntimeFragmentRef host ingress/return ABI is not implemented; "
              "raw-pointer function publication is blocked; " +
              std::to_string(provenEntries) +
              " private unit body/wrapper pair(s) and " +
              std::to_string(provenReturns) +
              " private owned return body/wrapper pair(s) verified and discarded" +
              (provenApplies ? "; " + std::to_string(provenApplies) +
                  " private Ref apply body(s) verified and discarded" : ""));
        return false;
    }
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
        if (f->requiresFragmentContext)
            paramLLVMTypes.push_back(mHelpers->ptrTy());
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
