#include "moonir/MoonIR.h"
#include "moonir/ContainerModel.h"
#include "moonir/ControlFlowBuilder.h"
#include "moonir/Lowering.h"
#include "moonir/Sealer.h"
#include "moonir/Verifier.h"
#include "codegen/CodeGenerator.h"
#include "diagnostics/Diagnostic.h"
#include "runtime/RuntimeDescriptor.h"
#include "runtime/RuntimeFragment.h"
#include "tooling/AnalysisSnapshot.h"
#include "moonir_canonical_test_support.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Verifier.h>

namespace canonical_test {

int runLoweredCompositionTests(SealingTestContext& context) {
    auto& cfgVerifier = context.cfgVerifier;
    auto& verifier = context.verifier;
    auto& reverse = context.reverseModule;
    const auto shortId = context.shortIteratorType;

    // The source spelling now resolves an exact nominal Slot, but must not
    // publish an executable Ref until ingress, dropGlue and wire validation
    // are connected. Test the actual frontend -> lowerer -> verifier path.
    auto sourceRefSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "slot checkpoint(value: i32);\n"
        "slot shadow(value: i32);\n"
        "fn observe(selected: RuntimeFragmentRef<checkpoint>) -> unit {}\n"
        "fn accept(selected: affine RuntimeFragmentRef<checkpoint>) -> unit {}\n"
        "fn transfer(selected: affine RuntimeFragmentRef<checkpoint>) "
        "-> affine RuntimeFragmentRef<checkpoint> { return selected; }\n",
        "<canonical-source-ref-gate>");
    if (!sourceRefSnapshot.success())
        return fail("frontend rejected a well-formed nominal source Ref type");
    moon::LunaLowerer sourceRefLowerer;
    auto sourceRefModule = sourceRefLowerer.lower(
        *sourceRefSnapshot.program(), *sourceRefSnapshot.symbolTable());
    if (!sourceRefModule || !sourceRefLowerer.errors().empty())
        return fail("source Ref type did not lower into private MoonIR preparation");
    moon::FunctionDecl* observe = nullptr;
    moon::FunctionDecl* accept = nullptr;
    moon::FunctionDecl* transfer = nullptr;
    const moon::SlotDecl* shadow = nullptr;
    for (auto& declaration : sourceRefModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get())) {
            if (function->name == "observe") observe = function;
            if (function->name == "accept") accept = function;
            if (function->name == "transfer") transfer = function;
        }
        if (const auto* slot = dynamic_cast<const moon::SlotDecl*>(
                declaration.get()); slot && slot->name == "shadow")
            shadow = slot;
    }
    if (!observe || !accept || !transfer || observe->params.size() != 1 ||
        accept->params.size() != 1 || !shadow)
        return fail("source Ref target fixture has no frozen function/Slot");
    const auto refId = observe->params.front().type;
    auto* refRecord = const_cast<moon::TypeRecord*>(
        sourceRefModule->findType(refId));
    const auto* slotType = refRecord
        ? sourceRefModule->findType(refRecord->innerTypeId) : nullptr;
    auto* slotRecord = slotType
        ? const_cast<moon::DeclarationRecord*>(
              sourceRefModule->findDeclarationById(
                  slotType->nominalDeclarationId)) : nullptr;
    const auto* shadowRecord = sourceRefModule->findDeclarationById(
        shadow->declarationId);
    const auto target = sourceRefModule->resolveRuntimeFragmentRefTarget(refId);
    if (!refRecord || !slotRecord || !shadowRecord || !target ||
        target->symbol != slotRecord->symbolId ||
        target->contract != slotRecord->contractId ||
        sourceRefModule->resolveRuntimeFragmentRefTarget(shadowRecord->type))
        return fail("frozen Ref target did not resolve one exact nominal Slot contract");
    sourceRefModule->typeTableSealed = false;
    if (sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("unsealed Ref type table supplied a runtime target");
    sourceRefModule->typeTableSealed = true;
    const auto originalInner = refRecord->innerTypeId;
    refRecord->innerTypeId = shadowRecord->type;
    if (sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("same-shaped Slot substitution retargeted a frozen Ref");
    refRecord->innerTypeId = originalInner;
    const auto originalContract = slotRecord->contractId;
    slotRecord->contractId = luna::identity::contractIdFromCanonical(
        "forged slot contract");
    if (sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("frozen Ref accepted a forged Slot contract");
    slotRecord->contractId = originalContract;
    const auto originalCanonicalContract = slotRecord->canonicalContract;
    slotRecord->canonicalContract = "forged slot contract";
    slotRecord->contractId = luna::identity::contractIdFromCanonical(
        slotRecord->canonicalContract);
    slotRecord->sysmeta.identity.contract = slotRecord->contractId;
    if (sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("frozen Ref accepted a self-consistent forged contract string");
    slotRecord->canonicalContract = originalCanonicalContract;
    slotRecord->contractId = originalContract;
    slotRecord->sysmeta.identity.contract = originalContract;
    const auto originalType = slotRecord->type;
    slotRecord->type = shadowRecord->type;
    if (sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("frozen Ref accepted a Slot declaration/type mismatch");
    slotRecord->type = originalType;
    if (!sourceRefModule->resolveRuntimeFragmentRefTarget(refId))
        return fail("frozen Ref target did not recover after rejected mutations");
    moon::Sealer sourceRefSealer;
    if (!sourceRefSealer.sealFunctionBodies(*sourceRefModule))
        return fail("source Ref functions did not seal ownership CFGs");
    if (verifier.verify(*sourceRefModule) ||
        !std::any_of(verifier.errors().begin(), verifier.errors().end(),
            [](const auto& error) {
                return error.message.find(
                    "RuntimeFragmentRef source import/dropGlue/wire ABI is not implemented") !=
                    std::string::npos;
            }))
        return fail("source Ref passed executable publication before its full bridge");
    CodeGenerator blockedRefCodegen("canonical-source-ref-codegen-gate");
    if (blockedRefCodegen.generate(sourceRefModule.get()) ||
        !std::any_of(blockedRefCodegen.errors().begin(),
            blockedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "raw-pointer function publication is blocked") !=
                    std::string::npos;
            }) ||
        !std::any_of(blockedRefCodegen.errors().begin(),
            blockedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "2 private unit body/wrapper pair(s) and 1 private owned "
                    "return body/wrapper pair(s) verified and discarded") !=
                    std::string::npos;
            }) ||
        std::any_of(blockedRefCodegen.errors().begin(),
            blockedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "private RuntimeFragmentRef unit ingress proof failed") !=
                    std::string::npos;
            }) ||
        std::any_of(blockedRefCodegen.errors().begin(),
            blockedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "private RuntimeFragmentRef owned return proof failed") !=
                    std::string::npos;
            }))
        return fail("direct codegen bypassed the unimplemented Ref host ingress ABI");
    const auto originalRelation = accept->params.front().relation;
    accept->params.front().relation = luna::ownership::Relation::SharedBorrow;
    CodeGenerator forgedRefCodegen("canonical-forged-source-ref-codegen-gate");
    const bool forgedPublished = forgedRefCodegen.generate(sourceRefModule.get());
    accept->params.front().relation = originalRelation;
    if (forgedPublished ||
        !std::any_of(forgedRefCodegen.errors().begin(),
            forgedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "private RuntimeFragmentRef unit ingress proof failed for 'accept'") !=
                    std::string::npos;
            }) ||
        !std::any_of(forgedRefCodegen.errors().begin(),
            forgedRefCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "raw-pointer function publication is blocked") !=
                    std::string::npos;
            }))
        return fail("forged Ref relation escaped or appeared proven by codegen");
    const auto originalReturnUsage = transfer->returnUsage;
    transfer->returnUsage = luna::ownership::Usage::Copy;
    CodeGenerator forgedReturnCodegen("canonical-forged-ref-return-gate");
    const bool forgedReturnPublished =
        forgedReturnCodegen.generate(sourceRefModule.get());
    transfer->returnUsage = originalReturnUsage;
    if (forgedReturnPublished ||
        !std::any_of(forgedReturnCodegen.errors().begin(),
            forgedReturnCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "private RuntimeFragmentRef owned return proof failed for 'transfer'") !=
                    std::string::npos;
            }) ||
        !std::any_of(forgedReturnCodegen.errors().begin(),
            forgedReturnCodegen.errors().end(), [](const auto& diagnostic) {
                return diagnostic.message.find(
                    "raw-pointer function publication is blocked") !=
                    std::string::npos;
            }))
        return fail("forged Ref return relation escaped or appeared proven");

    auto sourceApplySnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        "export slot checkpoint(value: i32);\n"
        "runtime fn host_entry(selected: RuntimeFragmentRef<checkpoint>) {\n"
        "  apply selected { checkpoint(1) {} }\n"
        "}\n",
        "<canonical-source-ref-apply-gate>");
    if (!sourceApplySnapshot.success())
        return fail("frontend rejected exact-Slot source Ref apply preparation");
    moon::LunaLowerer sourceApplyLowerer;
    auto sourceApplyModule = sourceApplyLowerer.lower(
        *sourceApplySnapshot.program(), *sourceApplySnapshot.symbolTable());
    if (sourceApplyModule && sourceApplyLowerer.errors().empty())
        return fail("source Ref apply escaped its closed executable lowering gate");
    if (!std::any_of(sourceApplyLowerer.errors().begin(),
                     sourceApplyLowerer.errors().end(),
                     [](const auto& diagnostic) {
                         return diagnostic.message.find(
                             "RuntimeFragmentRef apply needs the source context-override ABI") !=
                             std::string::npos;
                     }))
        return fail("source Ref apply gate lost its explicit ABI diagnostic");

    llvm::LLVMContext bridgeContext;
    CGHelpers bridgeHelpers(bridgeContext);
    llvm::Module bridgeModule("canonical.ref_ingress_preparation", bridgeContext);
    auto* bridgeFunction = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.voidTy(),
            {bridgeHelpers.ptrTy(), bridgeHelpers.ptrTy()}, false),
        llvm::Function::ExternalLinkage, "test_ref_ingress", bridgeModule);
    auto* bridgeEntry = llvm::BasicBlock::Create(
        bridgeContext, "entry", bridgeFunction);
    llvm::IRBuilder<> bridgeBuilder(bridgeEntry);
    auto* sourceCell = bridgeFunction->getArg(0);
    auto* destinationCell = bridgeFunction->getArg(1);
    auto* borrowed = bridgeBuilder.CreateLoad(
        bridgeHelpers.ptrTy(), sourceCell, "borrowed.ref");
    auto* borrowCheck = bridgeHelpers.emitRuntimeFragmentRefBorrowCheck(
        bridgeBuilder, bridgeModule, borrowed, *target);
    auto* ownedTransfer = bridgeHelpers.emitRuntimeFragmentRefOwnedTransfer(
        bridgeBuilder, bridgeModule, sourceCell, destinationCell, *target);
    if (bridgeHelpers.emitRuntimeFragmentRefBorrowCheck(
            bridgeBuilder, bridgeModule, borrowed, {}) ||
        bridgeHelpers.emitRuntimeFragmentRefOwnedTransfer(
            bridgeBuilder, bridgeModule, sourceCell, sourceCell, *target))
        return fail("LLVM Ref ingress accepted an incomplete target or aliased owner cells");
    bridgeBuilder.CreateRetVoid();
    const auto carriesIdentity = [](const llvm::CallInst* call,
                                    unsigned index, const std::string& value) {
        const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(
            call->getArgOperand(index));
        const auto* data = global
            ? llvm::dyn_cast<llvm::ConstantDataArray>(global->getInitializer())
            : nullptr;
        return data && data->isCString() && data->getAsCString() == value;
    };
    auto* borrowWrapper = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.i32Ty(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::ExternalLinkage, "test_borrow_ref_ingress", bridgeModule);
    auto* borrowEntry = llvm::BasicBlock::Create(
        bridgeContext, "entry", borrowWrapper);
    auto* borrowBody = llvm::BasicBlock::Create(
        bridgeContext, "body", borrowWrapper);
    bridgeBuilder.SetInsertPoint(borrowEntry);
    auto* borrowStatus = bridgeHelpers.emitRuntimeFragmentRefBorrowIngressGate(
        bridgeBuilder, bridgeModule, borrowWrapper->getArg(0),
        *target, borrowBody);
    bridgeBuilder.CreateRet(llvm::ConstantInt::get(bridgeHelpers.i32Ty(), 0));

    auto* ownedWrapper = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.i32Ty(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::ExternalLinkage, "test_owned_ref_ingress", bridgeModule);
    auto* ownedEntry = llvm::BasicBlock::Create(
        bridgeContext, "entry", ownedWrapper);
    auto* ownedBody = llvm::BasicBlock::Create(
        bridgeContext, "body", ownedWrapper);
    bridgeBuilder.SetInsertPoint(ownedEntry);
    auto* ownedCell = bridgeBuilder.CreateAlloca(bridgeHelpers.ptrTy());
    bridgeBuilder.CreateStore(
        llvm::ConstantPointerNull::get(
            llvm::cast<llvm::PointerType>(bridgeHelpers.ptrTy())), ownedCell);
    auto* ownedStatus = bridgeHelpers.emitRuntimeFragmentRefOwnedIngressGate(
        bridgeBuilder, bridgeModule, ownedWrapper->getArg(0),
        ownedCell, *target, ownedBody);
    bridgeHelpers.emitRuntimeFragmentRefDrop(
        bridgeBuilder, bridgeModule, ownedCell);
    bridgeBuilder.CreateRet(llvm::ConstantInt::get(bridgeHelpers.i32Ty(), 0));

    auto* wrongStatusWrapper = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.voidTy(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::ExternalLinkage, "test_invalid_ref_ingress", bridgeModule);
    auto* wrongStatusEntry = llvm::BasicBlock::Create(
        bridgeContext, "entry", wrongStatusWrapper);
    auto* wrongStatusBody = llvm::BasicBlock::Create(
        bridgeContext, "body", wrongStatusWrapper);
    bridgeBuilder.SetInsertPoint(wrongStatusEntry);
    if (bridgeHelpers.emitRuntimeFragmentRefBorrowIngressGate(
            bridgeBuilder, bridgeModule, wrongStatusWrapper->getArg(0),
            *target, wrongStatusBody) || !wrongStatusEntry->empty())
        return fail("LLVM Ref ingress accepted a wrapper without a status return");
    bridgeBuilder.CreateRetVoid();
    bridgeBuilder.SetInsertPoint(wrongStatusBody);
    bridgeBuilder.CreateRetVoid();

    auto* borrowedBodyFunction = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.voidTy(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::InternalLinkage,
        observe->generatedSymbolName, bridgeModule);
    bridgeBuilder.SetInsertPoint(llvm::BasicBlock::Create(
        bridgeContext, "entry", borrowedBodyFunction));
    bridgeBuilder.CreateRetVoid();
    auto* owningBodyFunction = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.voidTy(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::InternalLinkage,
        accept->generatedSymbolName, bridgeModule);
    bridgeBuilder.SetInsertPoint(llvm::BasicBlock::Create(
        bridgeContext, "entry", owningBodyFunction));
    auto* bodyOwner = bridgeBuilder.CreateAlloca(bridgeHelpers.ptrTy());
    bridgeBuilder.CreateStore(owningBodyFunction->getArg(0), bodyOwner);
    bridgeHelpers.emitRuntimeFragmentRefDrop(
        bridgeBuilder, bridgeModule, bodyOwner);
    bridgeBuilder.CreateRetVoid();
    auto* returningBodyFunction = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.ptrTy(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::InternalLinkage,
        transfer->generatedSymbolName, bridgeModule);
    bridgeBuilder.SetInsertPoint(llvm::BasicBlock::Create(
        bridgeContext, "entry", returningBodyFunction));
    bridgeBuilder.CreateRet(returningBodyFunction->getArg(0));
    auto* generatedBorrowWrapper =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *borrowedBodyFunction, *sourceRefModule, *observe,
            "borrowed_ref_host_entry");
    auto* generatedOwnedWrapper =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "owning_ref_host_entry");
    auto* generatedReturnWrapper =
        bridgeHelpers.emitRuntimeFragmentRefOwnedReturnWrapper(
            bridgeModule, *returningBodyFunction, *sourceRefModule, *transfer,
            "returning_ref_host_entry");
    if (bridgeHelpers.emitRuntimeFragmentRefOwnedReturnWrapper(
            bridgeModule, *returningBodyFunction, *sourceRefModule, *transfer,
            "returning_ref_host_entry"))
        return fail("LLVM Ref return wrapper accepted a duplicate name");
    returningBodyFunction->setLinkage(llvm::Function::ExternalLinkage);
    const bool externalReturnBodyAccepted =
        bridgeHelpers.emitRuntimeFragmentRefOwnedReturnWrapper(
            bridgeModule, *returningBodyFunction, *sourceRefModule, *transfer,
            "external_return_body") != nullptr;
    returningBodyFunction->setLinkage(llvm::Function::InternalLinkage);
    if (externalReturnBodyAccepted ||
        bridgeModule.getFunction("external_return_body") ||
        bridgeHelpers.emitRuntimeFragmentRefOwnedReturnWrapper(
            bridgeModule, *borrowedBodyFunction, *sourceRefModule, *transfer,
            "wrong_return_body"))
        return fail("LLVM Ref return wrapper accepted an invalid body ABI");
    const auto originalBridgeReturnUsage = transfer->returnUsage;
    transfer->returnUsage = luna::ownership::Usage::Copy;
    const bool forgedReturnWrapper =
        bridgeHelpers.emitRuntimeFragmentRefOwnedReturnWrapper(
            bridgeModule, *returningBodyFunction, *sourceRefModule, *transfer,
            "forged_return_wrapper") != nullptr;
    transfer->returnUsage = originalBridgeReturnUsage;
    if (forgedReturnWrapper || bridgeModule.getFunction("forged_return_wrapper"))
        return fail("LLVM Ref return wrapper accepted a forged return contract");
    if (bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *borrowedBodyFunction, *sourceRefModule, *observe,
            "borrowed_ref_host_entry"))
        return fail("LLVM Ref host wrapper accepted a duplicate name");
    auto* unrelatedBody = llvm::Function::Create(
        llvm::FunctionType::get(
            bridgeHelpers.voidTy(), {bridgeHelpers.ptrTy()}, false),
        llvm::Function::InternalLinkage, "unrelated_ref_body", bridgeModule);
    bridgeBuilder.SetInsertPoint(llvm::BasicBlock::Create(
        bridgeContext, "entry", unrelatedBody));
    bridgeBuilder.CreateRetVoid();
    if (bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *unrelatedBody, *sourceRefModule, *observe,
            "unrelated_ref_entry") ||
        bridgeModule.getFunction("unrelated_ref_entry"))
        return fail("LLVM Ref host wrapper accepted a different body symbol");
    const auto originalOwnedRelation = accept->params.front().relation;
    accept->params.front().relation = luna::ownership::Relation::SharedBorrow;
    const bool forgedRelationAccepted =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "forged_relation_ref_entry") != nullptr;
    accept->params.front().relation = originalOwnedRelation;
    auto& ownedCleanups = accept->controlFlow->cleanups;
    const auto ownedCleanup = std::find_if(
        ownedCleanups.begin(), ownedCleanups.end(),
        [accept](const moon::CleanupRecord& cleanup) {
            return cleanup.type == accept->params.front().type &&
                cleanup.place.projections.empty();
        });
    if (ownedCleanup == ownedCleanups.end())
        return fail("owned source Ref has no canonical parameter Drop");
    const auto originalCleanupAction = ownedCleanup->action;
    ownedCleanup->action = luna::ownership::CleanupAction::None;
    const bool forgedCleanupAccepted =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "forged_cleanup_ref_entry") != nullptr;
    ownedCleanup->action = originalCleanupAction;
    const auto* ownedDeclaration = sourceRefModule->findDeclarationById(
        accept->declarationId);
    auto* ownedCallable = ownedDeclaration
        ? const_cast<moon::TypeRecord*>(
              sourceRefModule->findType(ownedDeclaration->type)) : nullptr;
    if (!ownedCallable)
        return fail("owned source Ref has no frozen callable contract");
    const auto originalCallableCanonical = ownedCallable->canonicalType;
    ownedCallable->canonicalType += ";forged";
    const bool forgedCallableAccepted =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "forged_callable_ref_entry") != nullptr;
    ownedCallable->canonicalType = originalCallableCanonical;
    const auto originalRefCleanupRequired =
        refRecord->sysmeta.resource.cleanupRequired;
    refRecord->sysmeta.resource.cleanupRequired = false;
    const bool forgedRefResourceAccepted =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "forged_ref_resource_entry") != nullptr;
    refRecord->sysmeta.resource.cleanupRequired =
        originalRefCleanupRequired;
    const auto originalEffect = accept->requiresFragmentContext;
    accept->requiresFragmentContext = !originalEffect;
    const bool forgedEffectAccepted =
        bridgeHelpers.emitRuntimeFragmentRefUnitIngressWrapper(
            bridgeModule, *owningBodyFunction, *sourceRefModule, *accept,
            "forged_effect_ref_entry") != nullptr;
    accept->requiresFragmentContext = originalEffect;
    if (forgedRelationAccepted || forgedCleanupAccepted ||
        forgedCallableAccepted || forgedRefResourceAccepted ||
        forgedEffectAccepted ||
        bridgeModule.getFunction("forged_relation_ref_entry") ||
        bridgeModule.getFunction("forged_cleanup_ref_entry") ||
        bridgeModule.getFunction("forged_callable_ref_entry") ||
        bridgeModule.getFunction("forged_ref_resource_entry") ||
        bridgeModule.getFunction("forged_effect_ref_entry"))
        return fail("LLVM Ref host wrapper accepted forged source ownership facts");

    const auto findCallTo = [](llvm::Function* function,
                               llvm::Function* callee) -> llvm::CallInst* {
        if (!function) return nullptr;
        for (auto& block : *function) {
            for (auto& instruction : block) {
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    call && call->getCalledFunction() == callee)
                    return call;
            }
        }
        return nullptr;
    };
    auto* generatedReturnCall = findCallTo(
        generatedReturnWrapper, returningBodyFunction);
    std::vector<llvm::CallInst*> returnTransfers;
    llvm::CallInst* returnFailureDrop = nullptr;
    llvm::BasicBlock* returnOutputCheck = nullptr;
    llvm::BasicBlock* invalidCarrierBlock = nullptr;
    if (generatedReturnWrapper) {
        for (auto& block : *generatedReturnWrapper) {
            if (block.getName() == "ref.output.check")
                returnOutputCheck = &block;
            if (block.getName() == "ref.invalid.carrier")
                invalidCarrierBlock = &block;
            for (auto& instruction : block)
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    call && call->getCalledFunction()) {
                    if (call->getCalledFunction()->getName() ==
                        "luna_runtime_fragment_ref_transfer_v1")
                        returnTransfers.push_back(call);
                    if (call->getCalledFunction()->getName() ==
                        "luna_runtime_fragment_ref_drop_v1")
                        returnFailureDrop = call;
                }
        }
    }
    llvm::LoadInst* outputLoad = nullptr;
    if (returnOutputCheck && !returnOutputCheck->empty())
        outputLoad = llvm::dyn_cast<llvm::LoadInst>(
            &*returnOutputCheck->begin());
    auto* returnOutputBranch = returnOutputCheck
        ? llvm::dyn_cast_or_null<llvm::BranchInst>(
              returnOutputCheck->getTerminator()) : nullptr;
    auto* outputEmptyCondition =
        returnOutputBranch && returnOutputBranch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(
              returnOutputBranch->getCondition()) : nullptr;
    auto* returnEntryBranch = generatedReturnWrapper
        ? llvm::dyn_cast_or_null<llvm::BranchInst>(
              generatedReturnWrapper->getEntryBlock().getTerminator())
        : nullptr;
    auto* invalidCarrierReturn = invalidCarrierBlock
        ? llvm::dyn_cast_or_null<llvm::ReturnInst>(
              invalidCarrierBlock->getTerminator()) : nullptr;
    auto* invalidCarrierStatus = invalidCarrierReturn
        ? llvm::dyn_cast<llvm::ConstantInt>(
              invalidCarrierReturn->getReturnValue()) : nullptr;
    bool returnsBodyHandle = false;
    if (generatedReturnWrapper && generatedReturnCall &&
        returnTransfers.size() == 2)
        for (auto& block : *generatedReturnWrapper)
            for (auto& instruction : block)
                if (auto* store = llvm::dyn_cast<llvm::StoreInst>(
                        &instruction); store &&
                    store->getValueOperand() == generatedReturnCall &&
                    store->getPointerOperand() ==
                        returnTransfers[1]->getArgOperand(0))
                    returnsBodyHandle = true;
    auto* returnStatusBranch = returnTransfers.size() == 2
        ? llvm::dyn_cast_or_null<llvm::BranchInst>(
              returnTransfers[1]->getParent()->getTerminator()) : nullptr;
    auto* returnFailureReturn = returnFailureDrop
        ? llvm::dyn_cast_or_null<llvm::ReturnInst>(
              returnFailureDrop->getParent()->getTerminator()) : nullptr;
    if (!generatedReturnWrapper || !generatedReturnCall ||
        !generatedReturnWrapper->hasInternalLinkage() ||
        generatedReturnWrapper->arg_size() != 2 ||
        returnTransfers.size() != 2 || !returnFailureDrop ||
        returnTransfers[0]->getArgOperand(0) !=
            generatedReturnWrapper->getArg(0) ||
        returnTransfers[1]->getArgOperand(3) !=
            generatedReturnWrapper->getArg(1) ||
        !returnsBodyHandle ||
        returnFailureDrop->getArgOperand(0) !=
            returnTransfers[1]->getArgOperand(0) ||
        !returnStatusBranch || !returnStatusBranch->isConditional() ||
        returnStatusBranch->getSuccessor(1) !=
            returnFailureDrop->getParent() ||
        !returnFailureReturn ||
        returnFailureReturn->getReturnValue() != returnTransfers[1] ||
        !carriesIdentity(returnTransfers[0], 1, target->symbol.value) ||
        !carriesIdentity(returnTransfers[0], 2, target->contract.value) ||
        !carriesIdentity(returnTransfers[1], 1, target->symbol.value) ||
        !carriesIdentity(returnTransfers[1], 2, target->contract.value) ||
        !outputLoad || outputLoad->getPointerOperand() !=
            generatedReturnWrapper->getArg(1) ||
        !returnOutputBranch || !returnOutputBranch->isConditional() ||
        !outputEmptyCondition ||
        outputEmptyCondition->getPredicate() != llvm::CmpInst::ICMP_EQ ||
        outputEmptyCondition->getOperand(0) != outputLoad ||
        !llvm::isa<llvm::ConstantPointerNull>(
            outputEmptyCondition->getOperand(1)) ||
        !returnEntryBranch || !returnEntryBranch->isConditional() ||
        returnEntryBranch->getSuccessor(0) != returnOutputCheck ||
        returnEntryBranch->getSuccessor(1) != invalidCarrierBlock ||
        returnOutputBranch->getSuccessor(0) !=
            returnTransfers[0]->getParent() ||
        returnOutputBranch->getSuccessor(1) != invalidCarrierBlock ||
        !invalidCarrierStatus ||
        invalidCarrierStatus->getSExtValue() !=
            LUNA_RUNTIME_FRAGMENT_REF_INVALID_CARRIER_V1)
        return fail("LLVM Ref return wrapper lost its carrier/identity checks");
    auto* generatedBorrowCall = findCallTo(
        generatedBorrowWrapper, borrowedBodyFunction);
    auto* generatedOwnedCall = findCallTo(
        generatedOwnedWrapper, owningBodyFunction);
    auto* ownedTake = generatedOwnedCall
        ? llvm::dyn_cast<llvm::LoadInst>(
              generatedOwnedCall->getArgOperand(0)) : nullptr;
    auto* clearedOwner = ownedTake
        ? llvm::dyn_cast<llvm::StoreInst>(ownedTake->getNextNode()) : nullptr;
    if (!generatedBorrowWrapper || !generatedOwnedWrapper ||
        !generatedBorrowWrapper->hasInternalLinkage() ||
        !generatedOwnedWrapper->hasInternalLinkage() ||
        generatedBorrowWrapper->arg_size() != 1 ||
        generatedOwnedWrapper->arg_size() != 1 ||
        !generatedBorrowCall || !generatedOwnedCall ||
        generatedBorrowCall->getArgOperand(0) !=
            generatedBorrowWrapper->getArg(0) ||
        generatedOwnedCall->getArgOperand(0) ==
            generatedOwnedWrapper->getArg(0) ||
        !ownedTake || !clearedOwner ||
        !llvm::isa<llvm::AllocaInst>(ownedTake->getPointerOperand()) ||
        clearedOwner->getPointerOperand() != ownedTake->getPointerOperand() ||
        !llvm::isa<llvm::ConstantPointerNull>(clearedOwner->getValueOperand()))
        return fail("LLVM Ref host wrapper copied an owner or lost its context");

    const auto gatesBodyOnSuccess = [](const llvm::CallInst* status,
                                       const llvm::BasicBlock* body) {
        if (!status) return false;
        const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
            status->getParent()->getTerminator());
        if (!branch || !branch->isConditional() ||
            branch->getSuccessor(0) != body) return false;
        const auto* accepted = llvm::dyn_cast<llvm::ICmpInst>(
            branch->getCondition());
        const auto* failed = llvm::dyn_cast<llvm::ReturnInst>(
            branch->getSuccessor(1)->getTerminator());
        return accepted && accepted->getPredicate() == llvm::CmpInst::ICMP_EQ &&
            accepted->getOperand(0) == status &&
            llvm::isa<llvm::ConstantInt>(accepted->getOperand(1)) &&
            llvm::cast<llvm::ConstantInt>(accepted->getOperand(1))->isZero() &&
            failed && failed->getReturnValue() == status;
    };
    auto* generatedBorrowStatus = findCallTo(
        generatedBorrowWrapper,
        bridgeModule.getFunction("luna_runtime_fragment_ref_check_v1"));
    auto* generatedOwnedStatus = findCallTo(
        generatedOwnedWrapper,
        bridgeModule.getFunction("luna_runtime_fragment_ref_transfer_v1"));
    const auto returnsSuccessAfterBody = [](const llvm::CallInst* call) {
        const auto* returned = call
            ? llvm::dyn_cast_or_null<llvm::ReturnInst>(call->getNextNode())
            : nullptr;
        const auto* status = returned
            ? llvm::dyn_cast<llvm::ConstantInt>(returned->getReturnValue())
            : nullptr;
        return status && status->isZero();
    };
    if (!borrowCheck || !ownedTransfer ||
        !borrowCheck->getCalledFunction() ||
        borrowCheck->getCalledFunction()->getName() !=
            "luna_runtime_fragment_ref_check_v1" ||
        borrowCheck->getArgOperand(0) != borrowed ||
        !carriesIdentity(borrowCheck, 1, target->symbol.value) ||
        !carriesIdentity(borrowCheck, 2, target->contract.value) ||
        !ownedTransfer->getCalledFunction() ||
        ownedTransfer->getCalledFunction()->getName() !=
            "luna_runtime_fragment_ref_transfer_v1" ||
        ownedTransfer->getArgOperand(0) != sourceCell ||
        ownedTransfer->getArgOperand(3) != destinationCell ||
        !carriesIdentity(ownedTransfer, 1, target->symbol.value) ||
        !carriesIdentity(ownedTransfer, 2, target->contract.value) ||
        !gatesBodyOnSuccess(borrowStatus, borrowBody) ||
        !gatesBodyOnSuccess(ownedStatus, ownedBody) ||
        !gatesBodyOnSuccess(generatedBorrowStatus,
            generatedBorrowCall->getParent()) ||
        !gatesBodyOnSuccess(generatedOwnedStatus,
            generatedOwnedCall->getParent()) ||
        !returnsSuccessAfterBody(generatedBorrowCall) ||
        !returnsSuccessAfterBody(generatedOwnedCall) ||
        generatedOwnedStatus->getArgOperand(0) !=
            generatedOwnedWrapper->getArg(0) ||
        generatedOwnedStatus->getArgOperand(3) !=
            ownedTake->getPointerOperand() ||
        !carriesIdentity(generatedBorrowStatus, 1, target->symbol.value) ||
        !carriesIdentity(generatedOwnedStatus, 2, target->contract.value) ||
        ownedStatus->getArgOperand(0) != ownedWrapper->getArg(0) ||
        ownedStatus->getArgOperand(3) != ownedCell ||
        !carriesIdentity(borrowStatus, 1, target->symbol.value) ||
        !carriesIdentity(ownedStatus, 2, target->contract.value) ||
        llvm::verifyModule(bridgeModule))
        return fail("LLVM Ref ingress preparation conflated borrow and owning carrier ABIs");

    // Sema is not the only trust boundary: a structured input may be forged
    // after source analysis. The CFG bridge must reject cyclic static body
    // expansion itself rather than exhausting the compiler's stack.
    auto recursionSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        R"luna(
package canonical.static_recursion;
slot hook();
fragment finite() for hook { resume; }
fn main() -> i32 {
    apply finite { hook() {} }
    return 0;
}
)luna", "<canonical-static-recursion>");
    if (!recursionSnapshot.success())
        return fail("frontend rejected finite static recursion fixture");
    moon::LunaLowerer recursionLowerer;
    auto recursionModule = recursionLowerer.lower(
        *recursionSnapshot.program(), *recursionSnapshot.symbolTable());
    if (!recursionModule || !recursionLowerer.errors().empty())
        return fail("finite static recursion fixture did not lower");
    moon::FunctionDecl* recursionMain = nullptr;
    moon::FragmentDecl* recursionFragment = nullptr;
    moon::SlotDecl* recursionSlot = nullptr;
    for (auto& declaration : recursionModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
            function && function->name == "main")
            recursionMain = function;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(declaration.get()))
            recursionFragment = fragment;
        if (auto* slot = dynamic_cast<moon::SlotDecl*>(declaration.get()))
            recursionSlot = slot;
    }
    if (!recursionMain || !recursionFragment || !recursionSlot)
        return fail("static recursion fixture lost its declarations");
    auto forgedInvocation = std::make_unique<moon::SlotInvokeStmt>();
    forgedInvocation->name = recursionSlot->name;
    forgedInvocation->slotRef = recursionFragment->targetSlot;
    forgedInvocation->structuralType = recursionSlot->structuralType;
    forgedInvocation->continuation = std::make_unique<moon::BlockStmt>();
    recursionFragment->body->stmts.insert(
        recursionFragment->body->stmts.begin(), std::move(forgedInvocation));
    moon::ControlFlowBuilder recursionBuilder;
    auto recursiveCfg = recursionBuilder.build(
        *recursionMain->body, recursionMain->params,
        moon::RegionKind::Function, *recursionModule);
    if (recursiveCfg || !std::any_of(
            recursionBuilder.errors().begin(), recursionBuilder.errors().end(),
            [](const std::string& error) {
                return error.find("recursive static fragment composition") !=
                    std::string::npos;
            }))
        return fail("CFG construction accepted forged recursive static composition");

    auto overrideSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        R"luna(
package canonical.nested_override;
slot hook();
fragment replacement() for hook { resume; print(2); }
fragment wrapper() for hook {
    apply replacement { hook() { print(1); } }
    resume;
}
fn main() -> i32 {
    apply wrapper { hook() {} }
    return 0;
}
)luna", "<canonical-nested-override>");
    if (!overrideSnapshot.success())
        return fail("frontend rejected finite nested Fragment override");
    moon::LunaLowerer overrideLowerer;
    auto overrideModule = overrideLowerer.lower(
        *overrideSnapshot.program(), *overrideSnapshot.symbolTable());
    if (!overrideModule || !overrideLowerer.errors().empty())
        return fail("finite nested Fragment override did not lower");
    moon::FunctionDecl* nestedMain = nullptr;
    for (auto& declaration : overrideModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main") nestedMain = function;
    }
    if (!nestedMain || !nestedMain->body)
        return fail("nested Fragment override lost main");
    moon::ControlFlowBuilder nestedBuilder;
    auto nestedCfg = nestedBuilder.build(
        *nestedMain->body, nestedMain->params,
        moon::RegionKind::Function, *overrideModule);
    if (!nestedCfg || !cfgVerifier.verify(*nestedCfg, *overrideModule))
        return fail("valid nested Fragment entry failed CFG verification");
    const auto enclosingFragment = [&](moon::RegionId start)
        -> const moon::RegionRecord* {
        for (const auto* region = nestedCfg->findRegion(start); region;
             region = nestedCfg->findRegion(region->parent))
            if (region->kind == moon::RegionKind::Fragment) return region;
        return nullptr;
    };
    bool checkedNestedEntry = false;
    for (auto& block : nestedCfg->blocks) {
        if (block.terminator.kind != moon::TerminatorKind::Jump) continue;
        const auto* sourceFragment = enclosingFragment(block.region);
        const auto* target = nestedCfg->findBlock(block.terminator.primary.target);
        const auto* targetFragment = target
            ? enclosingFragment(target->region) : nullptr;
        if (!sourceFragment || !targetFragment ||
            sourceFragment->id == targetFragment->id ||
            enclosingFragment(targetFragment->parent) != sourceFragment ||
            target->id != targetFragment->entry) continue;
        const auto nonEntry = std::find_if(
            nestedCfg->blocks.begin(), nestedCfg->blocks.end(),
            [&](const moon::BasicBlock& candidate) {
                return candidate.region == targetFragment->id &&
                    candidate.id != targetFragment->entry;
            });
        if (nonEntry == nestedCfg->blocks.end())
            return fail("nested Fragment fixture has no non-entry block");
        const auto savedTarget = block.terminator.primary.target;
        block.terminator.primary.target = nonEntry->id;
        if (cfgVerifier.verify(*nestedCfg, *overrideModule) ||
            !std::any_of(cfgVerifier.errors().begin(), cfgVerifier.errors().end(),
                [](const diagnostic::Diagnostic& error) {
                    return error.message.find("jump escapes a fragment through a non-exit edge") !=
                        std::string::npos;
                }))
            return fail("CFG verification allowed a jump into a nested Fragment non-entry");
        block.terminator.primary.target = savedTarget;
        checkedNestedEntry = true;
        break;
    }
    if (!checkedNestedEntry || !cfgVerifier.verify(*nestedCfg, *overrideModule))
        return fail("nested Fragment entry guard did not recover after restoration");

    // Publication must validate the helper's recomputed effect rather than
    // its claimed summary. Static-only handlers and private composition are
    // not subject to the context-free public execute wrapper limitation.
    for (const char* fixture : {
             "exported_fragment_dynamic_body_invalid.luna",
             "exported_fragment_dynamic_call_invalid.luna",
             "exported_fragment_static_body.luna",
             "fragment_static_dynamic_body.luna"}) {
        const auto path = std::filesystem::path(LUNA_TEST_SOURCE_DIR) /
            "tests" / "fixtures" / fixture;
        std::ifstream input(path, std::ios::binary);
        std::ostringstream source;
        source << input.rdbuf();
        if (!input || source.str().empty())
            return fail("could not read Fragment handler context fixture");
        auto snapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
            source.str(), path.generic_string());
        if (!snapshot.success())
            return fail("Fragment handler context fixture failed source analysis");
        moon::LunaLowerer lowerer;
        auto handlerModule = lowerer.lower(
            *snapshot.program(), *snapshot.symbolTable());
        moon::Sealer sealer;
        if (!handlerModule || !lowerer.errors().empty() ||
            !sealer.sealFunctionBodies(*handlerModule))
            return fail("Fragment handler context fixture failed CFG construction");
        const bool rejectedCandidate =
            std::string(fixture).find("_invalid") != std::string::npos;
        const auto hasContextDiagnostic = [&]() {
            return std::any_of(verifier.errors().begin(), verifier.errors().end(),
                [](const diagnostic::Diagnostic& diagnostic) {
                    return diagnostic.message.find(
                        "cannot pass an execution context to its handler body") !=
                        std::string::npos;
                });
        };
        if (verifier.verify(*handlerModule) == rejectedCandidate ||
            (rejectedCandidate && !hasContextDiagnostic()))
            return fail("Fragment publication did not enforce its execution context ABI");
        if (rejectedCandidate) {
            moon::FunctionDecl* helper = nullptr;
            for (const auto& record : handlerModule->declarationTable) {
                if (record.kind != moon::DeclarationKind::Fragment ||
                    record.sourceName != "candidate") continue;
                for (auto& declaration : handlerModule->declarations) {
                    auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
                    if (function && function->symbolId == record.runtimeEntry.symbol &&
                        function->contractId == record.runtimeEntry.contract)
                        helper = function;
                }
            }
            if (!helper || !helper->requiresFragmentContext)
                return fail("published handler lost its transitive context effect");
            helper->requiresFragmentContext = false;
            if (verifier.verify(*handlerModule) || !hasContextDiagnostic())
                return fail("forged helper summary bypassed the Fragment execution ABI check");
            helper->requiresFragmentContext = true;
        } else {
            CodeGenerator codegen("canonical-fragment-handler-context");
            if (!codegen.generate(handlerModule.get()))
                return fail("context-free publication or private context inheritance failed codegen");
        }
    }

    const std::string loweredCompositionSource = R"luna(
package canonical.integration;

slot hook(value: i32);
slot captured();

fragment passthrough(value: i32) for hook {
    resume;
}

fragment lexical_capture[outer: i32] for captured {
    outer;
    resume;
}

fn main() -> i32 {
    let outer = 7;
    apply passthrough {
        hook(outer) {
            outer;
        }
    }
    apply lexical_capture[outer] {
        captured() {
            outer;
        }
        captured() {
            outer;
        }
    }
    return 0;
}
)luna";
    auto compositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredCompositionSource, "<canonical-composition>");
    if (!compositionSnapshot.success()) {
        for (const auto& diagnostic : compositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected canonical fragment integration source");
    }
    moon::LunaLowerer integrationLowerer;
    auto integrationModule = integrationLowerer.lower(
        *compositionSnapshot.program(), *compositionSnapshot.symbolTable());
    if (!integrationLowerer.errors().empty()) {
        for (const auto& diagnostic : integrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected canonical fragment source");
    }
    if (!verifier.verify(*integrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered fragment module failed structured verification");
    }
    moon::FunctionDecl* integrationMain = nullptr;
    const moon::FragmentDecl* integrationFragment = nullptr;
    const moon::FragmentDecl* integrationCaptureFragment = nullptr;
    for (auto& declaration : integrationModule->declarations) {
        if (auto* function = dynamic_cast<moon::FunctionDecl*>(
                declaration.get());
            function && function->name == "main")
            integrationMain = function;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "passthrough")
            integrationFragment = fragment;
        if (auto* fragment = dynamic_cast<moon::FragmentDecl*>(
                declaration.get());
            fragment && fragment->name == "lexical_capture")
            integrationCaptureFragment = fragment;
    }
    if (!integrationMain || !integrationMain->body ||
        !integrationFragment || !integrationFragment->body ||
        !integrationCaptureFragment || !integrationCaptureFragment->body)
        return fail("lowered integration module lost main or its fragment");
    const auto* integrationFragmentBody = integrationFragment->body.get();
    const auto* integrationCaptureBody =
        integrationCaptureFragment->body.get();
    moon::ControlFlowBuilder integrationBuilder;
    auto integrationCfg = integrationBuilder.build(
        std::move(integrationMain->body), integrationMain->params,
        moon::RegionKind::Function, *integrationModule);
    if (!integrationCfg) {
        for (const auto& error : integrationBuilder.errors())
            std::cerr << error << '\n';
        return fail("lowered fragment did not enter canonical CFG construction");
    }
    if (!cfgVerifier.verify(*integrationCfg, *integrationModule)) {
        for (const auto& diagnostic : cfgVerifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("lowered fragment failed canonical CFG verification");
    }
    size_t integrationApplyRegions = 0;
    size_t integrationFragmentRegions = 0;
    size_t integrationContinuationRegions = 0;
    size_t integrationResumeEdges = 0;
    size_t fragmentOuterCaptures = 0;
    size_t continuationOuterCaptures = 0;
    size_t environmentStorageLocals = 0;
    bool environmentEscapedApply = false;
    for (const auto& composedRegion : integrationCfg->regions) {
        integrationApplyRegions +=
            composedRegion.kind == moon::RegionKind::Apply;
        integrationFragmentRegions +=
            composedRegion.kind == moon::RegionKind::Fragment;
        integrationContinuationRegions +=
            composedRegion.kind == moon::RegionKind::Continuation;
    }
    for (const auto& local : integrationCfg->locals)
        environmentStorageLocals +=
            local.name.rfind("$fragment.environment.", 0) == 0;
    for (auto& block : integrationCfg->blocks) {
        integrationResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
        for (auto& operation : block.operations) {
            auto* effect = dynamic_cast<moon::ExprStmt*>(operation.get());
            auto* identifier = effect
                ? dynamic_cast<moon::IdentifierExpr*>(effect->expr.get())
                : nullptr;
            if (!identifier || identifier->name != "outer") continue;
            const auto kind =
                integrationCfg->regions[block.region.value].kind;
            fragmentOuterCaptures += kind == moon::RegionKind::Fragment;
            continuationOuterCaptures +=
                kind == moon::RegionKind::Continuation;
            if (identifier->local.empty()) {
                environmentEscapedApply = true;
                continue;
            }
            const auto& local = integrationCfg->locals[identifier->local.value];
            const auto localRegion = integrationCfg->scopes[local.scope.value].region;
            if (kind == moon::RegionKind::Fragment) {
                if (local.name.rfind("$fragment.environment.", 0) != 0 ||
                    integrationCfg->regions[localRegion.value].kind !=
                        moon::RegionKind::Apply)
                    environmentEscapedApply = true;
            } else if (kind == moon::RegionKind::Continuation &&
                       (local.name != "outer" || local.scope != integrationCfg->rootScope)) {
                environmentEscapedApply = true;
            }
        }
    }
    if (integrationApplyRegions != 2 ||
        integrationFragmentRegions != 3 ||
        integrationContinuationRegions != 3 ||
        integrationResumeEdges != 3 ||
        fragmentOuterCaptures != 2 ||
        continuationOuterCaptures != 3 ||
        environmentStorageLocals != 1 || environmentEscapedApply ||
        integrationFragment->body.get() != integrationFragmentBody ||
        integrationCaptureFragment->body.get() != integrationCaptureBody)
        return fail("frontend-to-CFG composition lost its fragment, environment, or construction body");

    const std::string loweredRuntimeCompositionSource = R"luna(
package canonical.runtime_boundary;

slot pipeline(value: i32);

fragment trace(value: i32) for pipeline {
    value;
    resume;
}

fn stable_entry() -> i32 {
    return 1;
}

fn dynamic_entry() -> i32 {
    apply trace {
        pipeline(1) {
            2;
        }
    }
    return 0;
}
)luna";
    auto runtimeCompositionSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            loweredRuntimeCompositionSource,
            "<canonical-runtime-composition>");
    if (!runtimeCompositionSnapshot.success()) {
        for (const auto& diagnostic : runtimeCompositionSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the canonical runtime-boundary source");
    }
    moon::LunaLowerer runtimeIntegrationLowerer;
    auto runtimeIntegrationModule = runtimeIntegrationLowerer.lower(
        *runtimeCompositionSnapshot.program(),
        *runtimeCompositionSnapshot.symbolTable());
    if (!runtimeIntegrationLowerer.errors().empty()) {
        for (const auto& diagnostic : runtimeIntegrationLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the runtime-boundary source");
    }
    if (!verifier.verify(*runtimeIntegrationModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("runtime-boundary module failed structured verification");
    }
    moon::FunctionDecl* dynamicEntry = nullptr;
    moon::FunctionDecl* stableEntry = nullptr;
    for (auto& declaration : runtimeIntegrationModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "dynamic_entry") {
            dynamicEntry = function;
        } else if (function && function->name == "stable_entry") {
            stableEntry = function;
        }
    }
    if (!dynamicEntry || !dynamicEntry->body ||
        !stableEntry || !stableEntry->body)
        return fail("runtime-boundary module lost its entry body");
    moon::Sealer runtimeBoundarySealer;
    if (!runtimeBoundarySealer.sealFunctionBodies(
            *runtimeIntegrationModule)) {
        for (const auto& diagnostic : runtimeBoundarySealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical function sealing rejected a linked fragment");
    }
    if (dynamicEntry->body || !dynamicEntry->controlFlow ||
        stableEntry->body || !stableEntry->controlFlow)
        return fail("fragment sealing did not atomically consume the function set");
    if (!verifier.verify(*runtimeIntegrationModule))
        return fail("sealed fragment module failed canonical verification");
    size_t runtimeFragmentRegions = 0;
    size_t runtimeContinuationRegions = 0;
    size_t runtimeResumeEdges = 0;
    for (const auto& region : dynamicEntry->controlFlow->regions) {
        runtimeFragmentRegions += region.kind == moon::RegionKind::Fragment;
        runtimeContinuationRegions +=
            region.kind == moon::RegionKind::Continuation;
    }
    for (const auto& block : dynamicEntry->controlFlow->blocks)
        runtimeResumeEdges +=
            block.terminator.kind == moon::TerminatorKind::Resume;
    if (runtimeFragmentRegions != 1 ||
        runtimeContinuationRegions != 1 || runtimeResumeEdges != 1)
        return fail("fragment lost its Fragment/Continuation/resume CFG");

    // Positive: an ordinary lexical fragment apply seals into a verified
    // canonical CFG with one Fragment region and one shared continuation.
    const std::string staticInterceptorSource = R"luna(
package canonical.dynamic_interceptor;

slot pipeline(value: i32);

fragment trace(value: i32) for pipeline {
    print(value + 1);
    resume;
}

fragment audit(value: i32) for pipeline {
    print(value + 2);
    resume;
}

fn main() -> i32 {
    apply trace {
        pipeline(41) {
            print(42);
        }
    }
    return 0;
}
)luna";
    auto interceptorSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            staticInterceptorSource,
            "<canonical-dynamic-interceptor>");
    if (!interceptorSnapshot.success()) {
        for (const auto& diagnostic : interceptorSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the static fragment source");
    }
    moon::LunaLowerer interceptorLowerer;
    auto interceptorModule = interceptorLowerer.lower(
        *interceptorSnapshot.program(),
        *interceptorSnapshot.symbolTable());
    if (!interceptorLowerer.errors().empty()) {
        for (const auto& diagnostic : interceptorLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the static fragment source");
    }
    if (!verifier.verify(*interceptorModule)) {
        for (const auto& diagnostic : verifier.errors())
            std::cerr << diagnostic << '\n';
        return fail("static fragment module failed structured verification");
    }
    moon::FunctionDecl* interceptorMain = nullptr;
    for (auto& declaration : interceptorModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(declaration.get());
        if (function && function->name == "main")
            interceptorMain = function;
    }
    if (!interceptorMain || !interceptorMain->body)
        return fail("static fragment module lost its main body");
    moon::Sealer interceptorSealer;
    if (!interceptorSealer.sealFunctionBodies(*interceptorModule)) {
        for (const auto& diagnostic : interceptorSealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical sealing rejected a valid static fragment apply");
    }
    if (!interceptorMain->controlFlow)
        return fail("static fragment main was not sealed to a canonical CFG");
    size_t interceptorFragmentRegions = 0;
    size_t interceptorContinuationRegions = 0;
    for (const auto& region : interceptorMain->controlFlow->regions) {
        if (region.kind == moon::RegionKind::Fragment) ++interceptorFragmentRegions;
        if (region.kind == moon::RegionKind::Continuation) ++interceptorContinuationRegions;
    }
    if (interceptorFragmentRegions != 1)
        return fail("static fragment apply did not materialize one Fragment region");
    if (interceptorContinuationRegions != 1)
        return fail("static fragment apply did not materialize one shared Continuation region");

    // An unbound exported Slot remains a nominal runtime dispatch boundary.
    // The same-shaped private Slot remains a compile-time identity so static
    // programs do not acquire an execution-context dependency.
    const std::string runtimeSlotSource = R"luna(
package canonical.runtime_slot;

export slot published(value: i32);
slot private_hook(value: i32);

export fragment local_impl(value) for published {
    resume;
}

// Host-only candidate: no source-local apply can supply its ownership checks
// or implicit cleanup. Both sides of resume own independent linear locals.
export fragment runtime_owned(value) for published {
    linear let before = new i32(value);
    free before;
    let cleanup = new i32(value);
    resume;
    linear let after = new i32(value);
    free after;
}

fn dynamic_path() -> i32 {
    let captured = 40;
    published(41) {
        captured += 2;
    }
    return captured;
}

runtime fn transitive_path() -> i32 {
    return dynamic_path();
}

fn static_path() -> i32 {
    private_hook(41) {
        42;
    }
    return 0;
}

fn static_exported_path() -> i32 {
    apply local_impl {
        published(41) {
            42;
        }
    }
    return 0;
}
)luna";
    auto runtimeSlotSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            runtimeSlotSource, "<canonical-runtime-slot>");
    if (!runtimeSlotSnapshot.success()) {
        for (const auto& diagnostic : runtimeSlotSnapshot.errors())
            std::cerr << diagnostic << '\n';
        return fail("frontend rejected the runtime Slot source");
    }
    moon::LunaLowerer runtimeSlotLowerer;
    auto runtimeSlotModule = runtimeSlotLowerer.lower(
        *runtimeSlotSnapshot.program(),
        *runtimeSlotSnapshot.symbolTable());
    if (!runtimeSlotLowerer.errors().empty()) {
        for (const auto& diagnostic : runtimeSlotLowerer.errors())
            std::cerr << diagnostic << '\n';
        return fail("MoonIR lowering rejected the runtime Slot source");
    }
    moon::Sealer runtimeSlotSealer;
    if (!runtimeSlotSealer.sealFunctionBodies(*runtimeSlotModule)) {
        for (const auto& diagnostic : runtimeSlotSealer.errors())
            std::cerr << diagnostic << '\n';
        return fail("canonical sealing rejected the runtime Slot source");
    }
    if (!verifier.verify(*runtimeSlotModule))
        return fail("sealed runtime Slot module failed verification");
    moon::FunctionDecl* dynamicPath = nullptr;
    moon::FunctionDecl* mutableTransitivePath = nullptr;
    const moon::FunctionDecl* transitivePath = nullptr;
    const moon::FunctionDecl* staticPath = nullptr;
    const moon::FunctionDecl* staticExportedPath = nullptr;
    for (auto& declaration : runtimeSlotModule->declarations) {
        auto* function = dynamic_cast<moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "dynamic_path")
            dynamicPath = function;
        else if (function && function->name == "transitive_path") {
            mutableTransitivePath = function;
            transitivePath = function;
        }
        else if (function && function->name == "static_path")
            staticPath = function;
        else if (function && function->name == "static_exported_path")
            staticExportedPath = function;
    }
    if (!dynamicPath || !dynamicPath->controlFlow ||
        !transitivePath || !transitivePath->controlFlow ||
        !staticPath || !staticPath->controlFlow ||
        !staticExportedPath || !staticExportedPath->controlFlow)
        return fail("runtime Slot fixture lost its sealed functions");
    moon::Terminator* runtimeSlot = nullptr;
    size_t privateRuntimeSlots = 0;
    for (auto& block : dynamicPath->controlFlow->blocks)
        if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot)
            runtimeSlot = &block.terminator;
    for (const auto& block : staticPath->controlFlow->blocks)
        privateRuntimeSlots +=
            block.terminator.kind == moon::TerminatorKind::RuntimeSlot;
    for (const auto& block : staticExportedPath->controlFlow->blocks)
        privateRuntimeSlots +=
            block.terminator.kind == moon::TerminatorKind::RuntimeSlot;
    const auto* packedArguments = runtimeSlot
        ? dynamic_cast<const moon::RecordLiteralExpr*>(
              runtimeSlot->operand.get())
        : nullptr;
    if (!runtimeSlot || !runtimeSlot->runtimeSlot.complete() ||
        runtimeSlot->runtimeArgumentsType.empty() || !packedArguments ||
        packedArguments->type != runtimeSlot->runtimeArgumentsType ||
        packedArguments->fields.size() != 1 || privateRuntimeSlots != 0)
        return fail("runtime Slot sealing lost nominal identity or static erasure");
    if (!dynamicPath->requiresFragmentContext ||
        !transitivePath->requiresFragmentContext ||
        staticPath->requiresFragmentContext ||
        staticExportedPath->requiresFragmentContext)
        return fail("runtime Slot context effect did not reach a direct caller fixed point");
    mutableTransitivePath->requiresFragmentContext = false;
    if (verifier.verify(*runtimeSlotModule))
        return fail("verifier accepted a forged fragment-context effect");
    mutableTransitivePath->requiresFragmentContext = true;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored fragment-context effect");
    dynamicPath->isExported = true;
    if (verifier.verify(*runtimeSlotModule))
        return fail("verifier accepted a context-requiring ordinary export");
    dynamicPath->isExported = false;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored internal context ABI");
    const auto* publicSlotRecord = runtimeSlotModule->findDeclaration(
        runtimeSlot->runtimeSlot);
    const auto publicSlotFound = publicSlotRecord
        ? runtimeSlotModule->declarationsById.find(publicSlotRecord->id)
        : runtimeSlotModule->declarationsById.end();
    auto* publishedSlot = publicSlotFound ==
            runtimeSlotModule->declarationsById.end()
        ? nullptr : dynamic_cast<moon::SlotDecl*>(publicSlotFound->second);
    if (!publishedSlot)
        return fail("runtime Slot fixture lost its executable Slot declaration");
    publishedSlot->isExported = false;
    if (verifier.verify(*runtimeSlotModule))
        return fail("forged export row published a private runtime Slot");
    publishedSlot->isExported = true;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored public runtime Slot");
    const auto ownPackage = publishedSlot->packageId;
    publishedSlot->packageId = "canonical.foreign_package";
    runtimeSlotModule->packageUses.push_back({
        runtimeSlotModule->name, publishedSlot->packageId, "foreign"});
    const bool acceptedForeignSlot = verifier.verify(*runtimeSlotModule);
    const bool rejectedRuntimeTarget = std::any_of(
        verifier.errors().begin(), verifier.errors().end(),
        [](const auto& diagnostic) {
            return diagnostic.message.find(
                "runtime Slot target is not an exported control") !=
                std::string::npos;
        });
    if (acceptedForeignSlot || !rejectedRuntimeTarget)
        return fail("forged local export row re-exported a foreign runtime Slot");
    runtimeSlotModule->packageUses.pop_back();
    publishedSlot->packageId = ownPackage;
    if (!verifier.verify(*runtimeSlotModule))
        return fail("verifier rejected the restored owning Slot package");

    // The first dispatch-lowering slice accepts a closed, side-effect-free
    // continuation. This fixture drives the real Runtime ABI rather than
    // replacing the terminator with a backend sentinel.
    CodeGenerator contextAbiCodegen("canonical-runtime-context-abi");
    if (!contextAbiCodegen.generate(runtimeSlotModule.get())) {
        for (const auto& diagnostic : contextAbiCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("hidden fragment-context direct-call ABI is inconsistent");
    }
    const auto contextAbiPath =
        std::filesystem::temp_directory_path() /
        "luna-moonir-fragment-context-abi.ll";
    if (!contextAbiCodegen.emitObjectFile(contextAbiPath.string()))
        return fail("could not inspect the hidden fragment-context ABI");
    std::ifstream contextAbiInput(contextAbiPath, std::ios::binary);
    if (!contextAbiInput.is_open())
        return fail("could not reopen the hidden fragment-context LLVM IR");
    std::ostringstream contextAbiBuffer;
    contextAbiBuffer << contextAbiInput.rdbuf();
    std::error_code removeError;
    std::filesystem::remove(contextAbiPath, removeError);
    const std::string contextAbiIr = contextAbiBuffer.str();
    const auto firstContext = contextAbiIr.find("fragment.context");
    const auto forwardedContext = firstContext == std::string::npos
        ? std::string::npos
        : contextAbiIr.find("fragment.context", firstContext + 1);
    if (firstContext == std::string::npos ||
        forwardedContext == std::string::npos ||
        contextAbiIr.find("luna_runtime_fragment_dispatch_v1") ==
            std::string::npos)
        return fail("LLVM IR did not retain, forward, and dispatch the Fragment context");
    std::string contextAbiError;
    auto contextAbiLease =
        contextAbiCodegen.materializeJitModule(contextAbiError);
    if (!contextAbiLease)
        return fail("could not materialize the context-aware Runtime entry");
    const void* registryAddress = contextAbiLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(runtimeSlotModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView contextAbiRegistry;
    if (!registryAddress ||
        !contextAbiRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                registryAddress),
            contextAbiError))
        return fail("could not bind the context-aware Runtime descriptor");
    const auto* contextEntryDescriptor = contextAbiRegistry.find(
        transitivePath->symbolId.value, transitivePath->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!contextEntryDescriptor || !contextEntryDescriptor->entry)
        return fail("runtime function descriptor lost its Fragment-context ABI flag");
    luna::runtime::RuntimeFragmentBindingSet emptyBindings;
    std::vector<luna::runtime::RuntimeFragmentRef> noFragments;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(noFragments), emptyBindings, contextAbiError))
        return fail("could not construct the empty runtime Fragment policy");
    luna::runtime::RuntimeFragmentExecutionContext executionContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            emptyBindings, executionContext, contextAbiError))
        return fail("could not construct the runtime Fragment execution context");
    using ContextEntry = int32_t (*)(const void*);
    const auto contextEntry = reinterpret_cast<ContextEntry>(
        const_cast<void*>(contextEntryDescriptor->entry));
    if (contextEntry(executionContext.opaque()) != 42)
        return fail("context-aware Runtime entry did not write back its captured frame");

    const auto* slotDeclaration = runtimeSlotModule->findDeclaration(
        runtimeSlot->runtimeSlot);
    const moon::DeclarationRecord* fragmentDeclaration = nullptr;
    for (const auto& declaration : runtimeSlotModule->declarationTable)
        if (declaration.kind == moon::DeclarationKind::Fragment &&
            declaration.sourceName == "runtime_owned")
            fragmentDeclaration = &declaration;
    const auto* fragmentDescriptor = fragmentDeclaration
        ? contextAbiRegistry.find(
              fragmentDeclaration->symbolId.value,
              fragmentDeclaration->contractId.value,
              LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
              LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1 |
                  LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1)
        : nullptr;
    if (!slotDeclaration || !fragmentDescriptor || !fragmentDescriptor->entry)
        return fail("generated runtime Fragment was not discoverable");

    luna::runtime::MoonRuntime fragmentRuntime;
    luna::runtime::GenerationStagingRequest fragmentRequest{
        "canonical.runtime_slot.selected", std::string(64, 'f'),
        contextAbiLease};
    luna::runtime::MoonRuntime::StagedGeneration fragmentStaged;
    if (!fragmentRuntime.stage(
            fragmentRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                bindings.push_back({
                    fragmentDescriptor->symbol_id,
                    fragmentDescriptor->contract_id,
                    fragmentDescriptor->entry,
                    fragmentDescriptor->declaration_kind,
                    luna::runtime::GenerationBindingFragmentExecutable |
                        luna::runtime::GenerationBindingPublicControl});
                return true;
            },
            {}, fragmentStaged, contextAbiError))
        return fail("generated runtime Fragment did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration fragmentGeneration;
    if (!fragmentRuntime.loadOnce(
            fragmentStaged, fragmentGeneration, contextAbiError))
        return fail("generated runtime Fragment did not load");
    const luna::runtime::RuntimeSlotRequirement slotRequirement{
        slotDeclaration->symbolId.value,
        slotDeclaration->contractId.value};
    luna::runtime::RuntimeFragmentCandidateSnapshot candidates;
    if (!luna::runtime::snapshotRuntimeFragmentCandidates(
            fragmentGeneration, slotRequirement, candidates,
            contextAbiError) || candidates.size() != 1)
        return fail("generated runtime Fragment was not an exact Slot candidate");
    luna::runtime::RuntimeFragmentRef selectedFragment;
    const luna::runtime::RuntimeFragmentFactoryArguments noFactory{
        "", nullptr};
    if (!luna::runtime::makeOwnedRuntimeFragmentRef(
            *candidates.at(0), slotRequirement, noFactory,
            selectedFragment, contextAbiError))
        return fail("host could not select the generated runtime Fragment");
    std::vector<luna::runtime::RuntimeFragmentRef> selectedFragments;
    selectedFragments.push_back(std::move(selectedFragment));
    luna::runtime::RuntimeFragmentBindingSet selectedBindings;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(selectedFragments), selectedBindings,
            contextAbiError))
        return fail("host selection did not become an immutable BindingSet");
    luna::runtime::RuntimeFragmentExecutionContext selectedContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            selectedBindings, selectedContext, contextAbiError) ||
        contextEntry(selectedContext.opaque()) != 42)
        return fail("selected runtime Fragment did not resume the outlined continuation");
    for (size_t invocation = 0; invocation < 8; ++invocation)
        if (contextEntry(selectedContext.opaque()) != 42)
            return fail("host-only Fragment ownership/cleanup did not survive repeated dispatch");

    const std::string escapingRuntimeSlotSource = R"luna(
package canonical.runtime_slot_escape;

struct ContinuationResource {
    marker: i32;
}

impl Drop for ContinuationResource {
    fn drop(resource: &mut ContinuationResource) -> unit {
        print(resource.marker);
    }
}

export slot published(value: i32);

fn dynamic_path() -> i32 {
    published(41) {
        let resource = new ContinuationResource(64);
        return 42;
    }
    return 0;
}

runtime fn entry() -> i32 {
    return dynamic_path();
}

fn source_error() -> Result<i32, i32> {
    return Err(7);
}

fn try_path() -> Result<i32, i32> {
    published(41) {
        let resource = new ContinuationResource(65);
        let value = source_error()?;
        print(value);
    }
    return Ok(0);
}

runtime fn try_entry() -> i32 {
    let result = try_path();
    return unwrap_err(move result);
}
)luna";
    auto escapingSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        escapingRuntimeSlotSource, "<canonical-runtime-slot-escape>");
    if (!escapingSnapshot.success())
        return fail("frontend rejected the escaping Runtime Slot fixture");
    moon::LunaLowerer escapingLowerer;
    auto escapingModule = escapingLowerer.lower(
        *escapingSnapshot.program(), *escapingSnapshot.symbolTable());
    moon::Sealer escapingSealer;
    if (!escapingLowerer.errors().empty() ||
        !escapingSealer.sealFunctionBodies(*escapingModule) ||
        !verifier.verify(*escapingModule))
        return fail("escaping Runtime Slot fixture did not reach code generation");
    CodeGenerator escapingCodegen("canonical-runtime-slot-escape");
    if (!escapingCodegen.generate(escapingModule.get()))
        return fail("codegen rejected a resource-cleaning Runtime Slot return escape");
    const moon::FunctionDecl* escapingEntryDeclaration = nullptr;
    const moon::FunctionDecl* tryEntryDeclaration = nullptr;
    for (const auto& declaration : escapingModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            escapingEntryDeclaration = function;
        else if (function && function->name == "try_entry")
            tryEntryDeclaration = function;
    }
    auto escapingLease = escapingCodegen.materializeJitModule(contextAbiError);
    if (!escapingLease || !escapingEntryDeclaration || !tryEntryDeclaration)
        return fail("could not materialize the escaping Runtime entry");
    const void* escapingRegistryAddress = escapingLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(escapingModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView escapingRegistry;
    if (!escapingRegistryAddress ||
        !escapingRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                escapingRegistryAddress),
            contextAbiError))
        return fail("could not bind the escaping Runtime registry");
    const auto* escapingEntry = escapingRegistry.find(
        escapingEntryDeclaration->symbolId.value,
        escapingEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!escapingEntry || !escapingEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(escapingEntry->entry))(
                executionContext.opaque()) != 42)
        return fail("Runtime Slot continuation return did not clean up and escape its entry");
    const auto* tryEntry = escapingRegistry.find(
        tryEntryDeclaration->symbolId.value,
        tryEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!tryEntry || !tryEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(tryEntry->entry))(
                executionContext.opaque()) != 7)
        return fail("Runtime Slot continuation '?' did not clean up and escape its entry");

    const std::string nestedRuntimeSlotSource = R"luna(
package canonical.runtime_slot_nested;

export slot outer_hook(value: i32);
export slot inner_hook(value: i32);

fn inner_path() -> i32 {
    let result = 1;
    inner_hook(2) {
        result += 3;
    }
    return result;
}

fn outer_path() -> i32 {
    let result = 40;
    outer_hook(1) {
        result += inner_path();
    }
    return result;
}

runtime fn entry() -> i32 {
    return outer_path();
}
)luna";
    auto nestedSnapshot = luna::tooling::AnalysisSnapshot::analyzeSource(
        nestedRuntimeSlotSource, "<canonical-runtime-slot-nested>");
    if (!nestedSnapshot.success())
        return fail("frontend rejected nested Runtime Slot calls");
    moon::LunaLowerer nestedLowerer;
    auto nestedModule = nestedLowerer.lower(
        *nestedSnapshot.program(), *nestedSnapshot.symbolTable());
    moon::Sealer nestedSealer;
    if (!nestedLowerer.errors().empty() ||
        !nestedSealer.sealFunctionBodies(*nestedModule) ||
        !verifier.verify(*nestedModule))
        return fail("nested Runtime Slot calls did not verify");
    const moon::FunctionDecl* nestedEntryDeclaration = nullptr;
    for (const auto& declaration : nestedModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            nestedEntryDeclaration = function;
    }
    CodeGenerator nestedCodegen("canonical-runtime-slot-nested");
    if (!nestedCodegen.generate(nestedModule.get())) {
        for (const auto& diagnostic : nestedCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("nested Runtime Slot calls lost the explicit context");
    }
    auto nestedLease = nestedCodegen.materializeJitModule(contextAbiError);
    if (!nestedLease || !nestedEntryDeclaration)
        return fail("could not materialize nested Runtime Slot calls");
    const void* nestedRegistryAddress = nestedLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(nestedModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView nestedRegistry;
    if (!nestedRegistryAddress ||
        !nestedRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                nestedRegistryAddress),
            contextAbiError))
        return fail("could not bind the nested Runtime Slot registry");
    const auto* nestedEntry = nestedRegistry.find(
        nestedEntryDeclaration->symbolId.value,
        nestedEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!nestedEntry || !nestedEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(nestedEntry->entry))(
                executionContext.opaque()) != 44)
        return fail("nested direct call lost its pinned Fragment execution context");

    const std::string lexicalNestedSlotSource = R"luna(
package canonical.runtime_slot_lexical_nested;

export slot outer_hook(value: i32);
export slot inner_hook(value: i32);
export slot deepest_hook(value: i32);

struct CapturedResource {
    marker: i32;
}

impl Drop for CapturedResource {
    fn drop(resource: &mut CapturedResource) -> unit {
        print(resource.marker);
    }
}

fn path() -> i32 {
    let result = 4;
    outer_hook(1) {
        result += 1;
        inner_hook(2) {
            deepest_hook(3) {
                result += 2;
            }
        }
        result += 3;
    }
    return result;
}

fn escape_path() -> i32 {
    let result = 4;
    outer_hook(1) {
        result += 1;
        inner_hook(2) {
            return result + 2;
        }
        result += 100;
    }
    return 0;
}

fn source_error() -> Result<i32, i32> {
    return Err(7);
}

fn try_path() -> Result<i32, i32> {
    outer_hook(1) {
        inner_hook(2) {
            let value = source_error()?;
            print(value);
        }
    }
    return Ok(0);
}

fn resource_path() -> i32 {
    let resource = new CapturedResource(5);
    outer_hook(1) {
        resource.marker += 1;
        inner_hook(2) {
            resource.marker += 2;
        }
    }
    return resource.marker;
}

fn resource_escape_path() -> i32 {
    let outer_resource = new CapturedResource(10);
    outer_hook(1) {
        let local_resource = new CapturedResource(20);
        inner_hook(2) {
            return outer_resource.marker + local_resource.marker;
        }
    }
    return 0;
}

runtime fn entry() -> i32 {
    return path();
}

runtime fn escape_entry() -> i32 {
    return escape_path();
}

runtime fn try_entry() -> i32 {
    let result = try_path();
    return unwrap_err(move result);
}

runtime fn resource_entry() -> i32 {
    return resource_path();
}

runtime fn resource_escape_entry() -> i32 {
    return resource_escape_path();
}

)luna";
    auto lexicalNestedSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            lexicalNestedSlotSource,
            "<canonical-runtime-slot-lexical-nested>");
    if (!lexicalNestedSnapshot.success())
        return fail("frontend rejected the lexical nested Slot fixture");
    moon::LunaLowerer lexicalNestedLowerer;
    auto lexicalNestedModule = lexicalNestedLowerer.lower(
        *lexicalNestedSnapshot.program(),
        *lexicalNestedSnapshot.symbolTable());
    moon::Sealer lexicalNestedSealer;
    const bool lexicalNestedSealed =
        lexicalNestedSealer.sealFunctionBodies(*lexicalNestedModule);
    const bool lexicalNestedVerified = lexicalNestedSealed &&
        verifier.verify(*lexicalNestedModule);
    if (!lexicalNestedLowerer.errors().empty() ||
        !lexicalNestedSealed || !lexicalNestedVerified) {
        for (const auto& diagnostic : lexicalNestedLowerer.errors())
            std::cerr << diagnostic.message << '\n';
        for (const auto& diagnostic : lexicalNestedSealer.errors())
            std::cerr << diagnostic << '\n';
        if (lexicalNestedSealed)
            for (const auto& diagnostic : verifier.errors())
                std::cerr << diagnostic.message << '\n';
        return fail("lexical nested Slot fixture did not reach code generation");
    }
    CodeGenerator lexicalNestedCodegen(
        "canonical-runtime-slot-lexical-nested");
    if (!lexicalNestedCodegen.generate(lexicalNestedModule.get())) {
        for (const auto& diagnostic : lexicalNestedCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("lexical nested Runtime Slot did not recursively outline");
    }
    auto lexicalNestedLease = lexicalNestedCodegen.materializeJitModule(
        contextAbiError);
    if (!lexicalNestedLease)
        return fail("could not materialize lexical nested Runtime Slot calls");
    const moon::FunctionDecl* lexicalNestedEntryDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedEscapeDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedTryDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedResourceDeclaration = nullptr;
    const moon::FunctionDecl* lexicalNestedResourceEscapeDeclaration = nullptr;
    for (const auto& declaration : lexicalNestedModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            lexicalNestedEntryDeclaration = function;
        else if (function && function->name == "escape_entry")
            lexicalNestedEscapeDeclaration = function;
        else if (function && function->name == "try_entry")
            lexicalNestedTryDeclaration = function;
        else if (function && function->name == "resource_entry")
            lexicalNestedResourceDeclaration = function;
        else if (function && function->name == "resource_escape_entry")
            lexicalNestedResourceEscapeDeclaration = function;
    }
    const void* lexicalNestedRegistryAddress = lexicalNestedLease->lookup(
        luna::runtime::runtimeDescriptorRegistrySymbol(
            lexicalNestedModule->name),
        contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView lexicalNestedRegistry;
    if (!lexicalNestedEntryDeclaration ||
        !lexicalNestedEscapeDeclaration ||
        !lexicalNestedTryDeclaration ||
        !lexicalNestedResourceDeclaration ||
        !lexicalNestedResourceEscapeDeclaration ||
        !lexicalNestedRegistryAddress ||
        !lexicalNestedRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                lexicalNestedRegistryAddress),
            contextAbiError))
        return fail("could not bind the lexical nested Runtime registry");
    const auto* lexicalNestedEntry = lexicalNestedRegistry.find(
        lexicalNestedEntryDeclaration->symbolId.value,
        lexicalNestedEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedEntry || !lexicalNestedEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedEntry->entry))(
                executionContext.opaque()) != 10)
        return fail("lexical nested Runtime Slot lost transitive capture writeback");
    const auto* lexicalNestedEscape = lexicalNestedRegistry.find(
        lexicalNestedEscapeDeclaration->symbolId.value,
        lexicalNestedEscapeDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedEscape || !lexicalNestedEscape->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedEscape->entry))(
                executionContext.opaque()) != 7)
        return fail("lexical nested Runtime Slot lost escaped return propagation");
    const auto* lexicalNestedTry = lexicalNestedRegistry.find(
        lexicalNestedTryDeclaration->symbolId.value,
        lexicalNestedTryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedTry || !lexicalNestedTry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedTry->entry))(
                executionContext.opaque()) != 7)
        return fail("lexical nested Runtime Slot lost '?' escape propagation");
    const auto* lexicalNestedResource = lexicalNestedRegistry.find(
        lexicalNestedResourceDeclaration->symbolId.value,
        lexicalNestedResourceDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedResource || !lexicalNestedResource->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedResource->entry))(
                executionContext.opaque()) != 8)
        return fail("lexical nested Runtime Slot lost affine capture writeback");
    const auto* lexicalNestedResourceEscape = lexicalNestedRegistry.find(
        lexicalNestedResourceEscapeDeclaration->symbolId.value,
        lexicalNestedResourceEscapeDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!lexicalNestedResourceEscape ||
        !lexicalNestedResourceEscape->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(lexicalNestedResourceEscape->entry))(
                executionContext.opaque()) != 30)
        return fail("lexical nested Runtime Slot lost outer resource cleanup escape");
    const std::string outerResourceEscapeSource = R"luna(
package canonical.runtime_slot_outer_resource_escape;

struct Resource { marker: i32; }
impl Drop for Resource {
    fn drop(resource: &mut Resource) -> unit {
        print(resource.marker);
    }
}
export slot hook(value: i32);
export fragment skip(value: i32) for hook {
    print(value);
}
fn path() -> i32 {
    let resource = new Resource(9);
    hook(1) {
        return resource.marker;
    }
    return 0;
}
runtime fn entry() -> i32 { return path(); }
)luna";
    auto outerResourceEscapeSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            outerResourceEscapeSource,
            "<canonical-runtime-slot-outer-resource-escape>");
    if (!outerResourceEscapeSnapshot.success())
        return fail("frontend rejected the outer resource escape fixture");
    moon::LunaLowerer outerResourceEscapeLowerer;
    auto outerResourceEscapeModule = outerResourceEscapeLowerer.lower(
        *outerResourceEscapeSnapshot.program(),
        *outerResourceEscapeSnapshot.symbolTable());
    if (!outerResourceEscapeLowerer.errors().empty())
        return fail("outer resource escape failed before sealing");
    moon::Sealer outerResourceEscapeSealer;
    if (!outerResourceEscapeSealer.sealFunctionBodies(
            *outerResourceEscapeModule) ||
        !verifier.verify(*outerResourceEscapeModule))
        return fail("outer resource escape did not preserve exact cleanup edges");
    const moon::FunctionDecl* outerResourceEscapeEntryDeclaration = nullptr;
    for (const auto& declaration : outerResourceEscapeModule->declarations) {
        const auto* function = dynamic_cast<const moon::FunctionDecl*>(
            declaration.get());
        if (function && function->name == "entry")
            outerResourceEscapeEntryDeclaration = function;
    }
    CodeGenerator outerResourceEscapeCodegen(
        "canonical-runtime-slot-outer-resource-escape");
    if (!outerResourceEscapeCodegen.generate(
            outerResourceEscapeModule.get())) {
        for (const auto& diagnostic : outerResourceEscapeCodegen.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("outer resource escape did not outline its cleanup");
    }
    auto outerResourceEscapeLease =
        outerResourceEscapeCodegen.materializeJitModule(contextAbiError);
    if (!outerResourceEscapeLease || !outerResourceEscapeEntryDeclaration)
        return fail("could not materialize the outer resource escape entry");
    const void* outerResourceEscapeRegistryAddress =
        outerResourceEscapeLease->lookup(
            luna::runtime::runtimeDescriptorRegistrySymbol(
                outerResourceEscapeModule->name),
            contextAbiError);
    luna::runtime::RuntimeDescriptorRegistryView outerResourceEscapeRegistry;
    if (!outerResourceEscapeRegistryAddress ||
        !outerResourceEscapeRegistry.bind(
            static_cast<const LunaRuntimeDescriptorRegistryV1*>(
                outerResourceEscapeRegistryAddress),
            contextAbiError))
        return fail("could not bind the outer resource escape registry");
    const auto* outerResourceEscapeEntry = outerResourceEscapeRegistry.find(
        outerResourceEscapeEntryDeclaration->symbolId.value,
        outerResourceEscapeEntryDeclaration->contractId.value,
        LUNA_RUNTIME_DECLARATION_FUNCTION_V1,
        LUNA_RUNTIME_DESCRIPTOR_CALLABLE_V1 |
            LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_CONTEXT_V1);
    if (!outerResourceEscapeEntry || !outerResourceEscapeEntry->entry ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(outerResourceEscapeEntry->entry))(
                executionContext.opaque()) != 9)
        return fail("outer resource cleanup did not run on Slot escape");
    const moon::DeclarationRecord* outerResourceSlot = nullptr;
    const moon::DeclarationRecord* skippingFragment = nullptr;
    for (const auto& declaration : outerResourceEscapeModule->declarationTable) {
        if (declaration.kind == moon::DeclarationKind::Slot &&
            declaration.sourceName == "hook")
            outerResourceSlot = &declaration;
        else if (declaration.kind == moon::DeclarationKind::Fragment &&
                 declaration.sourceName == "skip")
            skippingFragment = &declaration;
    }
    const auto* skippingDescriptor = skippingFragment
        ? outerResourceEscapeRegistry.find(
              skippingFragment->symbolId.value,
              skippingFragment->contractId.value,
              LUNA_RUNTIME_DECLARATION_FRAGMENT_V1,
              LUNA_RUNTIME_DESCRIPTOR_FRAGMENT_EXECUTABLE_V1 |
                  LUNA_RUNTIME_DESCRIPTOR_PUBLIC_CONTROL_V1)
        : nullptr;
    if (!outerResourceSlot || !skippingDescriptor ||
        !skippingDescriptor->entry)
        return fail("skipping Fragment was not published as a Slot candidate");
    luna::runtime::MoonRuntime skippingRuntime;
    luna::runtime::GenerationStagingRequest skippingRequest{
        "canonical.runtime_slot.skipping", std::string(64, 'a'),
        outerResourceEscapeLease};
    luna::runtime::MoonRuntime::StagedGeneration skippingStaged;
    if (!skippingRuntime.stage(
            skippingRequest,
            [](const auto&, std::string&) { return true; },
            [&](const auto&, auto& bindings, std::string&) {
                bindings.push_back({
                    skippingDescriptor->symbol_id,
                    skippingDescriptor->contract_id,
                    skippingDescriptor->entry,
                    skippingDescriptor->declaration_kind,
                    luna::runtime::GenerationBindingFragmentExecutable |
                        luna::runtime::GenerationBindingPublicControl});
                return true;
            },
            {}, skippingStaged, contextAbiError))
        return fail("skipping Fragment did not stage");
    luna::runtime::MoonRuntime::PinnedGeneration skippingGeneration;
    if (!skippingRuntime.loadOnce(
            skippingStaged, skippingGeneration, contextAbiError))
        return fail("skipping Fragment did not load");
    const luna::runtime::RuntimeSlotRequirement skippingSlot{
        outerResourceSlot->symbolId.value,
        outerResourceSlot->contractId.value};
    luna::runtime::RuntimeFragmentCandidateSnapshot skippingCandidates;
    if (!luna::runtime::snapshotRuntimeFragmentCandidates(
            skippingGeneration, skippingSlot, skippingCandidates,
            contextAbiError) || skippingCandidates.size() != 1)
        return fail("skipping Fragment was not an exact Slot candidate");
    luna::runtime::RuntimeFragmentRef skippingRef;
    if (!luna::runtime::makeOwnedRuntimeFragmentRef(
            *skippingCandidates.at(0), skippingSlot, noFactory,
            skippingRef, contextAbiError))
        return fail("host could not select the skipping Fragment");
    std::vector<luna::runtime::RuntimeFragmentRef> skippingFragments;
    skippingFragments.push_back(std::move(skippingRef));
    luna::runtime::RuntimeFragmentBindingSet skippingBindings;
    if (!luna::runtime::makeRuntimeFragmentBindingSet(
            std::move(skippingFragments), skippingBindings, contextAbiError))
        return fail("skipping Fragment did not form an immutable BindingSet");
    luna::runtime::RuntimeFragmentExecutionContext skippingContext;
    if (!luna::runtime::makeRuntimeFragmentExecutionContext(
            skippingBindings, skippingContext, contextAbiError) ||
        reinterpret_cast<ContextEntry>(
            const_cast<void*>(outerResourceEscapeEntry->entry))(
                skippingContext.opaque()) != 0)
        return fail("skipping Fragment did not take the post-Slot cleanup path");
    const std::string divergentSlotOwnershipSource = R"luna(
package canonical.runtime_slot_divergent_ownership;
struct Resource { marker: i32; }
impl Drop for Resource {
    fn drop(resource: &mut Resource) -> unit { print(resource.marker); }
}
export slot hook(value: i32);
fn path() -> i32 {
    let resource = new Resource(1);
    hook(0) { free resource; }
    return 0;
}
)luna";
    auto divergentSlotOwnershipSnapshot =
        luna::tooling::AnalysisSnapshot::analyzeSource(
            divergentSlotOwnershipSource,
            "<canonical-runtime-slot-divergent-ownership>");
    const bool diagnosedDivergentSlotOwnership = std::any_of(
        divergentSlotOwnershipSnapshot.errors().begin(),
        divergentSlotOwnershipSnapshot.errors().end(),
        [](const diagnostic::Diagnostic& diagnostic) {
            return diagnostic.message.find(
                "owned heap value 'resource' is freed or moved on only some paths through `slot`") !=
                std::string::npos;
        });
    if (divergentSlotOwnershipSnapshot.success() ||
        !diagnosedDivergentSlotOwnership) {
        for (const auto& diagnostic : divergentSlotOwnershipSnapshot.errors())
            std::cerr << diagnostic.message << '\n';
        return fail("runtime Slot accepted divergent ownership after a skipped continuation");
    }
    const auto reverseIterator = reverse.typesById.find(shortId.value);
    if (reverseIterator == reverse.typesById.end())
        return fail("sealed type index lost the iterator type");
    reverse.typeTable[reverseIterator->second].sysmeta.resource.usage =
        luna::ownership::Usage::Copy;
    if (verifier.verify(reverse))
        return fail("verifier accepted a forged derived Resource contract");


    return 0;
}

} // namespace canonical_test
