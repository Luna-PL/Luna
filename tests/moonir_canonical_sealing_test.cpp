#include "moonir_canonical_test_support.h"
#include "moonir/ContainerModel.h"
#include "codegen/CGHelpers.h"
#include "core/TypeLayout.h"
#include "core/TypeRelations.h"

#include <algorithm>
#include <llvm/IR/Verifier.h>

namespace canonical_test {

int testRuntimeFragmentRefPreparation() {
    auto slot = Type::makeSlot({TyI32});
    slot->identityMode = luna::types::IdentityMode::Nominal;
    slot->nominalId = "canonical.ref_preparation::checkpoint";
    slot->name = "checkpoint";
    const auto ref = Type::makeRuntimeFragmentRef(slot);
    const auto shape = luna::types::shapeId(ref);
    moon::Module prepared;
    prepared.name = "canonical.ref_preparation";
    const auto id = prepared.registerType(ref);
    const auto slotId = luna::types::typeId(slot);
    const auto aggregateId = prepared.registerType(Type::makeRecord({{"ref", ref}}));
    const auto borrowId = prepared.registerType(Type::makeReference(ref));
    prepared.sealTypeTable();
    // The complete frozen graph must not depend on the frontend object.
    slot->name = "mutated";
    slot->paramTypes = {TyI64};
    moon::TypeMaterializer materializer(prepared);
    const auto restored = materializer.materialize(id);
    const auto* record = prepared.findType(id);
    const auto* aggregate = prepared.findType(aggregateId);
    const auto* borrow = prepared.findType(borrowId);
    if (!restored || !record || !aggregate || !borrow ||
        restored->kind != TypeKind::RuntimeFragmentRef ||
        !luna::types::isWellFormedTypeDomain(restored) ||
        luna::types::typeId(restored) != id || luna::types::shapeId(restored) != shape ||
        restored->inner->name != "checkpoint" || restored->inner->paramTypes[0]->kind != TypeKind::I32 ||
        record->innerTypeId != slotId || record->referencedTypeIds != moon::TypeRefVec{slotId} ||
        record->valueSize != 8 || record->valueAlignment != 8 ||
        !luna::layout::valueLayoutFits(restored) ||
        !record->sysmeta.resource.needsDrop || !record->sysmeta.resource.cleanupRequired ||
        record->sysmeta.resource.usage != luna::ownership::Usage::Affine ||
        record->sysmeta.resource.releaseDomain != luna::sysmeta::ReleaseDomain::Executable ||
        !aggregate->sysmeta.resource.recursiveCleanup ||
        aggregate->sysmeta.resource.usage != luna::ownership::Usage::Affine ||
        borrow->sysmeta.resource.cleanupRequired ||
        borrow->sysmeta.resource.relation != luna::ownership::Relation::SharedBorrow)
        return fail("internal Ref freezing/materialization lost its nominal target or resources");
    moon::Verifier verifier;
    if (verifier.verify(prepared) || !std::any_of(
            verifier.errors().begin(), verifier.errors().end(), [](const auto& error) {
                return error.message.find("RuntimeFragmentRef source import/dropGlue/wire ABI is not implemented") != std::string::npos;
            }))
        return fail("MoonIR publication accepted internal Ref preparation without a source import/dropGlue/wire ABI");
    std::vector<uint8_t> bytes{1, 2, 3};
    std::string error;
    if (moon::ContainerModelCodec::encodeTypes(prepared, bytes, error) || !bytes.empty() ||
        error.find("RuntimeFragmentRef") == std::string::npos)
        return fail("wire type writer emitted the internal, unsupported Ref kind");
    moon::ContainerManifest manifest;
    manifest.packageId = prepared.name;
    manifest.features = prepared.features;
    bytes = {1, 2, 3};
    if (moon::ContainerModelCodec::encodeContainer(manifest, prepared, bytes, error) ||
        !bytes.empty() || error.find("RuntimeFragmentRef") == std::string::npos)
        return fail("container publication bypassed the Ref bridge gate");
    llvm::LLVMContext llvmContext;
    CGHelpers helpers(llvmContext);
    if (helpers.toLLVMType(restored) != helpers.ptrTy())
        return fail("internal Ref has no opaque pointer carrier representation");
    llvm::Module llvmModule("canonical.ref_drop_preparation", llvmContext);
    auto* function = llvm::Function::Create(
        llvm::FunctionType::get(helpers.voidTy(), false),
        llvm::Function::ExternalLinkage, "test_ref_drop", llvmModule);
    auto* entry = llvm::BasicBlock::Create(llvmContext, "entry", function);
    llvm::IRBuilder<> builder(entry);
    auto* carrier = builder.CreateAlloca(helpers.ptrTy(), nullptr, "ref.carrier");
    auto* destination = builder.CreateAlloca(
        helpers.ptrTy(), nullptr, "ref.destination");
    builder.CreateStore(llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(helpers.ptrTy())), carrier);
    builder.CreateStore(llvm::ConstantPointerNull::get(
        llvm::cast<llvm::PointerType>(helpers.ptrTy())), destination);
    auto* taken = llvm::dyn_cast<llvm::LoadInst>(
        helpers.emitRuntimeFragmentRefTake(builder, carrier));
    if (!taken) return fail("internal Ref take did not load its source carrier");
    auto* clear = llvm::dyn_cast<llvm::StoreInst>(taken->getNextNode());
    if (!clear || clear->getPointerOperand() != carrier ||
        !llvm::isa<llvm::ConstantPointerNull>(clear->getValueOperand()))
        return fail("internal Ref take did not clear the source before transfer");
    builder.CreateStore(taken, destination);
    auto* drop = helpers.emitRuntimeFragmentRefDrop(
        builder, llvmModule, destination);
    builder.CreateRetVoid();
    if (!drop->getCalledFunction() ||
        drop->getCalledFunction()->getName() !=
            "luna_runtime_fragment_ref_drop_v1" ||
        drop->getArgOperand(0) != destination ||
        llvmModule.getFunction("rt_dealloc") ||
        llvm::verifyModule(llvmModule))
        return fail("internal Ref Drop did not clear its original carrier via runtime ABI");
    // Forge the new ordinal into an otherwise old, canonical singleton type
    // section. Decoder rejection must preserve the caller's existing table.
    moon::Module old;
    old.name = "canonical.old_types";
    const auto oldId = old.registerType(TyI32);
    old.sealTypeTable();
    if (!moon::ContainerModelCodec::encodeTypes(old, bytes, error))
        return fail("old type writer changed during internal Ref preparation");
    const auto readU32 = [&](size_t offset) {
        return static_cast<uint32_t>(bytes[offset]) |
            (static_cast<uint32_t>(bytes[offset + 1]) << 8) |
            (static_cast<uint32_t>(bytes[offset + 2]) << 16) |
            (static_cast<uint32_t>(bytes[offset + 3]) << 24);
    };
    size_t offset = 4; // Row count, followed by TypeId, ShapeId, AbiLayoutId.
    for (unsigned index = 0; index < 3; ++index) {
        if (offset + 4 > bytes.size()) return fail("old type section string header is truncated");
        const auto length = readU32(offset);
        offset += 4;
        if (length > bytes.size() - offset) return fail("old type section string is truncated");
        offset += length;
    }
    offset += 8; // Domain and identity mode precede kind.
    if (offset + 4 > bytes.size() || readU32(offset) != static_cast<uint32_t>(TypeKind::I32))
        return fail("old type scalar order changed during Ref preparation");
    bytes[offset] = static_cast<uint8_t>(TypeKind::RuntimeFragmentRef);
    if (moon::ContainerModelCodec::decodeTypes(bytes, old, error) || error.empty() ||
        !old.typeTableSealed || old.typeTable.size() != 1 || !old.findType(oldId) ||
        old.findType(oldId)->kind != TypeKind::I32)
        return fail("wire reader accepted an unsupported Ref ordinal or published partial state");
    return 0;
}

int runSealingTests(
    moon::ControlFlowBuilder& cfgBuilder,
    moon::Verifier& cfgVerifier,
    moon::Module& module,
    moon::Module& reverse,
    const moon::TypeRef& shortId,
    const moon::TypeRef& productId) {
    moon::Verifier verifier;
    SealingTestContext context{
        cfgBuilder, cfgVerifier, verifier, module, reverse,
        shortId, productId};
    trace("function sealing tests");
    if (const int result = runFunctionSealingTests(context)) return result;
    trace("composition sealing tests");
    if (const int result = runCompositionSealingTests(context)) return result;
    trace("lowered composition tests");
    if (const int result = runLoweredCompositionTests(context)) return result;
    trace("symbol sealing tests");
    if (const int result = runSymbolSealingTests(context)) return result;
    trace("iterator sealing tests");
    if (const int result = runIteratorSealingTests(context)) return result;
    return testRuntimeFragmentRefPreparation();
}

} // namespace canonical_test
