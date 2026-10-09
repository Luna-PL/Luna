#include "CodeGenerator.h"
#include "NativeOwnedResultFacts.h"
#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/Verifier.h"
#include "runtime/RuntimeFragmentABI.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"
#ifdef LUNA_PRIVATE_REF_JIT_TEST
#include "runtime/RuntimeOwnedResult.h"
#endif

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/IR/Verifier.h>
#include <llvm/MC/TargetRegistry.h>
#include <llvm/Passes/OptimizationLevel.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/CodeGen.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/Target/TargetMachine.h>
#include <llvm/TargetParser/Host.h>

#include <array>
#include <exception>
#include <memory>
#include <optional>
#include <unordered_set>

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

#ifdef LUNA_PRIVATE_REF_JIT_TEST
// The unit Apply JIT ingress is a candidate for a typed host entry. Resolve
// its identity from frozen records instead of inferring a callable ABI from
// the wrapper's raw address. This does not publish or serialize the entry.
struct PrivateRefUnitApplyEntryFacts {
    moon::DeclarationRef target;
    // Module, package, function SymbolId/ContractId/TypeId, source linkage,
    // Ref TypeId, Slot SymbolId/ContractId/TypeId, ingress linkage.
    std::array<std::string, 11> identities;
    uint32_t contextEffect = 0;
};

std::optional<PrivateRefUnitApplyEntryFacts> privateRefUnitApplyEntryFacts(
    const moon::Module& program, const FunctionDecl& function) {
    if (!program.typeTableSealed || !function.controlFlow ||
        !function.controlFlow->sealed || function.controlFlow->runtimeRefApplies.empty() ||
        function.isExtern || function.isKernel || function.isSelector ||
        !function.typeParams.empty() || !function.abi.empty() ||
        !function.linkName.empty() ||
        function.generatedSymbolName.empty() || function.body ||
        !function.requiresFragmentContext ||
        !matchesPrivateRefContextEffect(program, function) ||
        function.params.size() != 1 ||
        function.returnsLinear ||
        function.returnUsage != luna::ownership::Usage::Copy)
        return std::nullopt;
    if (!std::any_of(program.declarations.begin(), program.declarations.end(),
            [&function](const auto& declaration) {
                return declaration.get() == &function;
            }))
        return std::nullopt;
    const auto& parameter = function.params.front();
    const auto* refType = program.findType(parameter.type);
    const auto* returnType = program.findType(function.returnType);
    const auto target = program.resolveRuntimeFragmentRefTarget(parameter.type);
    const auto* slot = target ? program.findDeclaration(*target) : nullptr;
    if (!refType || !returnType || returnType->kind != TypeKind::Unit ||
        !target || !slot || slot->kind != moon::DeclarationKind::Slot ||
        parameter.isLinear ||
        parameter.relation != luna::ownership::Relation::SharedBorrow ||
        parameter.usage != luna::ownership::Usage::Copy)
        return std::nullopt;
    const auto* record = program.findDeclarationById(function.declarationId);
    const auto* callable = record ? program.findType(record->type) : nullptr;
    moon::TypeMaterializer materializer(program);
    const TypePtr restoredCallable = callable
        ? materializer.materialize(record->type) : nullptr;
    if (!record || record->kind != moon::DeclarationKind::Function ||
        record->id != function.declarationId ||
        record->symbolId != function.symbolId ||
        record->symbolId != luna::identity::symbolIdFromCanonical(record->id) ||
        record->contractId != function.contractId ||
        record->canonicalContract != moon::canonicalContract(*record) ||
        record->contractId != luna::identity::contractIdFromCanonical(
            record->canonicalContract) ||
        record->sysmeta.identity.symbol != record->symbolId ||
        record->sysmeta.identity.contract != record->contractId ||
        record->linkageName != function.generatedSymbolName ||
        !callable || callable->kind != TypeKind::Function ||
        !restoredCallable ||
        luna::types::typeId(restoredCallable) != callable->id ||
        luna::types::canonicalType(restoredCallable) != callable->canonicalType ||
        callable->parameterTypeIds.size() != 1 ||
        callable->parameterTypeIds.front() != parameter.type ||
        callable->returnTypeId != function.returnType ||
        callable->parameterContracts.size() != 1 ||
        callable->parameterContracts.front() !=
            luna::ownership::Contract{parameter.relation, parameter.usage} ||
        callable->returnContract != luna::ownership::Contract{
            luna::ownership::Relation::Owned, function.returnUsage} ||
        record->sysmeta.resource.parameters.size() != 1 ||
        record->sysmeta.resource.parameters.front() !=
            callable->parameterContracts.front() ||
        record->sysmeta.resource.result != callable->returnContract)
        return std::nullopt;
    const auto& graph = *function.controlFlow;
    const moon::LocalRecord* parameterLocal = nullptr;
    for (const auto& local : graph.locals)
        if (local.kind == moon::LocalKind::Parameter) {
            if (parameterLocal) return std::nullopt;
            parameterLocal = &local;
        }
    if (!parameterLocal || parameterLocal->scope != graph.rootScope ||
        parameterLocal->name != parameter.name ||
        parameterLocal->type != parameter.type ||
        parameterLocal->relation != parameter.relation ||
        parameterLocal->usage != parameter.usage ||
        graph.runtimeRefApplies.front().reference != parameterLocal->id ||
        graph.runtimeRefApplies.front().slot != *target)
        return std::nullopt;
    PrivateRefUnitApplyEntryFacts facts{
        *target,
        {program.name, function.packageId, record->symbolId.value,
         record->contractId.value, callable->id.value, record->linkageName,
         refType->id.value, target->symbol.value,
         target->contract.value, slot->type.value,
         "__luna_private_ref_apply_ingress_test"},
        static_cast<uint32_t>(function.requiresFragmentContext)};
    if (std::any_of(facts.identities.begin(), facts.identities.end(),
            [](const std::string& value) { return value.empty(); }))
        return std::nullopt;
    return facts;
}

// Pointer-free, test-only candidate row. Its status profile names the private
// JIT result mapping, not a Native or source ABI version.
constexpr std::array<uint8_t, 8> PrivateRefEntryMagic =
    {'L', 'U', 'N', 'A', 'R', 'U', '1', 0};
constexpr std::array<uint32_t, 8> PrivateRefEntryConventions = {
    1, // Function declaration
    1, // One source parameter
    2, // Two machine arguments
    1, // Parent context, then Ref
    1, // Shared borrow
    1, // Unit result
    1, // Private test status profile
    0, // LLVM C calling convention
};
constexpr uint32_t PrivateRefEntryVersion = 1;
constexpr uint32_t PrivateRefEntryMaxString = 4096;

bool validPrivateRefEntryIdentity(const std::string& value) {
    return !value.empty() && value.size() <= PrivateRefEntryMaxString &&
        std::none_of(value.begin(), value.end(), [](char byte) {
            return byte == '\0' || byte == '\r' ||
                   byte == '\n' || byte == '\t';
        });
}

void appendPrivateRefEntryU32(std::vector<uint8_t>& bytes, uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8)
        bytes.push_back(static_cast<uint8_t>(value >> shift));
}

bool readPrivateRefEntryU32(const std::vector<uint8_t>& bytes,
                            size_t& cursor, uint32_t& value) {
    if (cursor > bytes.size() || bytes.size() - cursor < 4) return false;
    value = 0;
    for (unsigned shift = 0; shift < 32; shift += 8)
        value |= static_cast<uint32_t>(bytes[cursor++]) << shift;
    return true;
}

std::optional<std::vector<uint8_t>> encodePrivateRefUnitApplyEntry(
    const PrivateRefUnitApplyEntryFacts& facts) {
    std::vector<uint8_t> bytes(
        PrivateRefEntryMagic.begin(), PrivateRefEntryMagic.end());
    appendPrivateRefEntryU32(bytes, PrivateRefEntryVersion);
    const size_t sizeOffset = bytes.size();
    appendPrivateRefEntryU32(bytes, 0);
    appendPrivateRefEntryU32(bytes, 0); // reserved
    appendPrivateRefEntryU32(bytes,
        static_cast<uint32_t>(facts.identities.size()));
    appendPrivateRefEntryU32(bytes,
        static_cast<uint32_t>(PrivateRefEntryConventions.size() + 1));
    for (const auto& identity : facts.identities) {
        if (!validPrivateRefEntryIdentity(identity))
            return std::nullopt;
        appendPrivateRefEntryU32(bytes,
            static_cast<uint32_t>(identity.size()));
        bytes.insert(bytes.end(), identity.begin(), identity.end());
    }
    for (const auto convention : PrivateRefEntryConventions)
        appendPrivateRefEntryU32(bytes, convention);
    appendPrivateRefEntryU32(bytes, facts.contextEffect);
    const uint32_t encodedSize = static_cast<uint32_t>(bytes.size());
    for (unsigned index = 0; index < 4; ++index)
        bytes[sizeOffset + index] =
            static_cast<uint8_t>(encodedSize >> (index * 8));
    return bytes;
}

bool decodePrivateRefUnitApplyEntry(
    const std::vector<uint8_t>& bytes,
    std::array<std::string, 11>& identities,
    uint32_t& contextEffect) {
    if (bytes.size() < 28 ||
        !std::equal(PrivateRefEntryMagic.begin(), PrivateRefEntryMagic.end(),
                    bytes.begin()))
        return false;
    size_t cursor = PrivateRefEntryMagic.size();
    uint32_t version = 0;
    uint32_t totalSize = 0;
    uint32_t reserved = 0;
    uint32_t identityCount = 0;
    uint32_t conventionCount = 0;
    if (!readPrivateRefEntryU32(bytes, cursor, version) ||
        !readPrivateRefEntryU32(bytes, cursor, totalSize) ||
        !readPrivateRefEntryU32(bytes, cursor, reserved) ||
        !readPrivateRefEntryU32(bytes, cursor, identityCount) ||
        !readPrivateRefEntryU32(bytes, cursor, conventionCount) ||
        version != PrivateRefEntryVersion || totalSize != bytes.size() ||
        reserved != 0 || identityCount != identities.size() ||
        conventionCount != PrivateRefEntryConventions.size() + 1)
        return false;
    for (auto& identity : identities) {
        uint32_t length = 0;
        if (!readPrivateRefEntryU32(bytes, cursor, length) ||
            length == 0 || length > PrivateRefEntryMaxString ||
            cursor > bytes.size() || length > bytes.size() - cursor)
            return false;
        identity.assign(
            reinterpret_cast<const char*>(bytes.data() + cursor), length);
        if (!validPrivateRefEntryIdentity(identity))
            return false;
        cursor += length;
    }
    for (const auto expected : PrivateRefEntryConventions) {
        uint32_t actual = 0;
        if (!readPrivateRefEntryU32(bytes, cursor, actual) || actual != expected)
            return false;
    }
    if (!readPrivateRefEntryU32(bytes, cursor, contextEffect) ||
        contextEffect != 1)
        return false;
    return cursor == bytes.size();
}
#endif

bool isFrameField(const llvm::Value* address, const llvm::Value* frame,
                  unsigned index) {
    const auto* field = llvm::dyn_cast<llvm::GetElementPtrInst>(address);
    const auto* structIndex = field && field->getNumIndices() == 2
        ? llvm::dyn_cast<llvm::ConstantInt>(field->getOperand(1)) : nullptr;
    const auto* memberIndex = structIndex
        ? llvm::dyn_cast<llvm::ConstantInt>(field->getOperand(2)) : nullptr;
    return field && field->getPointerOperand() == frame &&
        structIndex && structIndex->isZero() &&
        memberIndex && memberIndex->equalsInt(index);
}

llvm::LoadInst* outlinedFrameLoad(llvm::Function& callback, unsigned index) {
    if (callback.arg_size() != 1) return nullptr;
    llvm::LoadInst* result = nullptr;
    for (auto& block : callback)
        for (auto& instruction : block)
            if (auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction);
                load && isFrameField(
                    load->getPointerOperand(), callback.getArg(0), index)) {
                if (result) return nullptr;
                result = load;
            }
    return result && result->getParent() == &callback.getEntryBlock()
        ? result : nullptr;
}

bool frameCapturesValue(llvm::Function& parent, llvm::Value* frame,
                        const llvm::CallInst& dispatch, unsigned index,
                        const llvm::Value* value) {
    size_t captures = 0;
    for (auto& block : parent)
        for (auto& instruction : block)
            if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                store && isFrameField(
                    store->getPointerOperand(), frame, index)) {
                if (store->getValueOperand() != value ||
                    store->getParent() != dispatch.getParent() ||
                    !store->comesBefore(&dispatch))
                    return false;
                ++captures;
            }
    return captures == 1;
}

bool dispatchFailureDropsOwners(
    const llvm::CallInst& dispatch,
    const std::vector<const llvm::Value*>& ownerCells,
    std::unordered_set<const llvm::CallInst*>* accounted = nullptr) {
    const auto matchesStatus = [&dispatch](const llvm::Value* condition,
                                           uint64_t expected) {
        const auto* compare = llvm::dyn_cast<llvm::ICmpInst>(condition);
        const auto* value = compare
            ? llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1))
            : nullptr;
        return compare && compare->getPredicate() == llvm::CmpInst::ICMP_EQ &&
            compare->getOperand(0) == &dispatch && value &&
            value->equalsInt(expected);
    };
    const auto* first = llvm::dyn_cast<llvm::BranchInst>(
        dispatch.getParent()->getTerminator());
    if (!first || !first->isConditional() ||
        !matchesStatus(first->getCondition(),
                       LUNA_RUNTIME_FRAGMENT_DISPATCH_SUCCESS_V1))
        return false;
    const auto* second = llvm::dyn_cast<llvm::BranchInst>(
        first->getSuccessor(1)->getTerminator());
    if (!second || !second->isConditional() ||
        !matchesStatus(second->getCondition(),
                       LUNA_RUNTIME_FRAGMENT_DISPATCH_CONTINUATION_ESCAPED_V1))
        return false;
    const auto* failed = second->getSuccessor(1);
    if (!llvm::isa<llvm::UnreachableInst>(failed->getTerminator()))
        return false;
    std::vector<const llvm::CallInst*> drops;
    const llvm::CallInst* trap = nullptr;
    for (const auto& instruction : *failed)
        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            call && call->getCalledFunction()) {
            const auto name = call->getCalledFunction()->getName();
            if (name == "luna_compiler_fragment_context_drop") {
                if (call->arg_size() != 1)
                    return false;
                drops.push_back(call);
            } else if (name == "llvm.trap") {
                if (trap) return false;
                trap = call;
            }
        }
    if (!trap || drops.size() != ownerCells.size()) return false;
    for (size_t index = 0; index < drops.size(); ++index)
        if (drops[index]->getArgOperand(0) !=
                ownerCells[ownerCells.size() - 1 - index] ||
            !drops[index]->comesBefore(trap))
            return false;
    if (accounted)
        accounted->insert(drops.begin(), drops.end());
    return true;
}

bool overrideFailureDropsOwners(
    const llvm::CallInst& overrideCall,
    const std::vector<const llvm::Value*>& ownerCells,
    std::unordered_set<const llvm::CallInst*>* accounted = nullptr) {
    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
        overrideCall.getParent()->getTerminator());
    const auto* compare = branch && branch->isConditional()
        ? llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition()) : nullptr;
    const auto* success = compare
        ? llvm::dyn_cast<llvm::ConstantInt>(compare->getOperand(1)) : nullptr;
    if (!branch || !branch->isConditional() || !compare || !success ||
        compare->getPredicate() != llvm::CmpInst::ICMP_EQ ||
        compare->getOperand(0) != &overrideCall || !success->isZero())
        return false;
    const auto* failed = branch->getSuccessor(1);
    if (!llvm::isa<llvm::UnreachableInst>(failed->getTerminator()))
        return false;
    std::vector<const llvm::CallInst*> drops;
    const llvm::CallInst* trap = nullptr;
    for (const auto& instruction : *failed)
        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
            call && call->getCalledFunction()) {
            const auto name = call->getCalledFunction()->getName();
            if (name == "luna_compiler_fragment_context_drop") {
                if (call->arg_size() != 1) return false;
                drops.push_back(call);
            } else if (name == "llvm.trap") {
                if (trap) return false;
                trap = call;
            }
        }
    if (!trap || drops.size() != ownerCells.size()) return false;
    for (size_t index = 0; index < drops.size(); ++index)
        if (drops[index]->getArgOperand(0) !=
                ownerCells[ownerCells.size() - 1 - index] ||
            !drops[index]->comesBefore(trap))
            return false;
    if (accounted)
        accounted->insert(drops.begin(), drops.end());
    return true;
}

} // namespace

#ifdef LUNA_PRIVATE_REF_JIT_TEST
bool CodeGenerator::validatePrivateRuntimeFragmentRefApplyEntryRecordForTest(
    const moon::Module& program, const FunctionDecl& function,
    const std::vector<uint8_t>& entryRecord, std::string& failure) {
    moon::Verifier verifier;
    if (!function.controlFlow || !function.controlFlow->sealed ||
        !verifier.verify(*function.controlFlow, program) ||
        !matchesPrivateRefContextEffect(program, function)) {
        failure = "private Ref entry has no verified sealed source CFG";
        return false;
    }
    const auto facts = privateRefUnitApplyEntryFacts(program, function);
    std::array<std::string, 11> encodedIdentities;
    uint32_t encodedContextEffect = 0;
    if (!facts ||
        !decodePrivateRefUnitApplyEntry(
            entryRecord, encodedIdentities, encodedContextEffect) ||
        encodedIdentities != facts->identities ||
        encodedContextEffect != facts->contextEffect) {
        failure = "private Ref entry record differs from its frozen source facts";
        return false;
    }
    failure.clear();
    return true;
}

int32_t LunaPrivateRefUnitApplyLoadedEntry::call(
    const void* parentContext, const void* borrowedRef) const {
    auto keepCodeAlive = lease_;
    if (!keepCodeAlive || !entry_)
        return LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST;
    using Entry = int32_t (*)(const void*, void*);
    const auto entry = reinterpret_cast<Entry>(const_cast<void*>(entry_));
    return entry(parentContext, const_cast<void*>(borrowedRef));
}

int32_t LunaPrivateRefResultLoadedEntry::call(
    const void* parentContext, const void* borrowedRef,
    uint32_t* tagOutput, int32_t* scalarOutput,
    luna::runtime::RuntimeOwnedResultHandle& ownerOutput,
    bool failAdoptionForTest) const {
    auto keepCodeAlive = lease_;
    if (!keepCodeAlive || !entry_ || !drop_)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST;
    if (!tagOutput || !scalarOutput || ownerOutput)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST;
    const auto tagAddress = reinterpret_cast<uintptr_t>(tagOutput);
    const auto scalarAddress = reinterpret_cast<uintptr_t>(scalarOutput);
    const auto separated = [](uintptr_t left, size_t leftSize,
                              uintptr_t right, size_t rightSize) {
        return left < right ? right - left >= leftSize
                            : left - right >= rightSize;
    };
    const auto ownerCell = ownerOutput.cell();
    const auto ownerAddress = reinterpret_cast<uintptr_t>(ownerCell);
    if ((tagAddress & (alignof(uint32_t) - 1)) != 0 ||
        (scalarAddress & (alignof(int32_t) - 1)) != 0 ||
        !separated(tagAddress, sizeof(uint32_t),
                   scalarAddress, sizeof(int32_t)) ||
        (ownerCell &&
            (!separated(tagAddress, sizeof(uint32_t),
                        ownerAddress, sizeof(void*)) ||
             !separated(scalarAddress, sizeof(int32_t),
                        ownerAddress, sizeof(void*)))))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST;
    using Entry = int32_t (*)(const void*, void*, uint32_t*, int32_t*,
                             void**, uint32_t);
    using Drop = int32_t (*)(void**);
    const auto entry = reinterpret_cast<Entry>(const_cast<void*>(entry_));
    const auto drop = reinterpret_cast<Drop>(const_cast<void*>(drop_));
    uint32_t tag = 0;
    int32_t scalar = 0;
    void* rawOwner = nullptr;
    const int32_t status = entry(parentContext,
        const_cast<void*>(borrowedRef), &tag, &scalar, &rawOwner, 0);
    const auto discardOwner = [&] {
        if (rawOwner && (drop(&rawOwner) != 0 || rawOwner))
            std::terminate();
    };
    if (status != LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST) {
        if (rawOwner) discardOwner();
        return status;
    }
    if ((tag == 0 && !rawOwner) || (tag == 1 && rawOwner) || tag > 1) {
        discardOwner();
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_RESOURCE_V1_TEST;
    }
    if (rawOwner) {
        std::string adoptionError;
        if (failAdoptionForTest ||
            !luna::runtime::makeRuntimeOwnedResultHandle(
                rawOwner, drop, keepCodeAlive, ownerOutput, adoptionError)) {
            discardOwner();
            return LUNA_PRIVATE_REF_RESULT_TRANSFER_ADOPTION_FAILURE_V1_TEST;
        }
    } else {
        *scalarOutput = scalar;
    }
    *tagOutput = tag;
    return LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST;
}

std::unique_ptr<LunaPrivateRefUnitApplyLoadedEntry>
CodeGenerator::loadPrivateRuntimeFragmentRefApplyEntryForTest(
    const moon::Module& program, const FunctionDecl& function,
    const std::vector<uint8_t>& entryRecord,
    std::shared_ptr<LunaJitModule> executable, std::string& failure) {
    if (!executable ||
        !validatePrivateRuntimeFragmentRefApplyEntryRecordForTest(
            program, function, entryRecord, failure)) {
        if (!executable)
            failure = "private Ref entry has no retained JIT code lease";
        return {};
    }
    if (executable->mPrivateRefUnitApplyEntryRecord != entryRecord) {
        failure = "private Ref entry record is not bound to this JIT module";
        return {};
    }
    const auto facts = privateRefUnitApplyEntryFacts(program, function);
    if (!facts) {
        failure = "private Ref entry lost its frozen lookup identity";
        return {};
    }
    const void* entry = executable->lookup(facts->identities.back(), failure);
    if (!entry) return {};
    auto loaded = std::make_unique<LunaPrivateRefUnitApplyLoadedEntry>();
    loaded->lease_ = std::move(executable);
    loaded->entry_ = entry;
    loaded->record_ = entryRecord;
    failure.clear();
    return loaded;
}

std::unique_ptr<LunaPrivateRefResultLoadedEntry>
CodeGenerator::loadPrivateRuntimeFragmentRefResultEntryForTest(
    const moon::Module& program, const FunctionDecl& function,
    std::shared_ptr<LunaJitModule> executable, std::string& failure) {
    luna::codegen::NativeOwnedResultSourceFacts facts;
    if (!executable || !executable->mPrivateOwnedResultSourceFacts ||
        !luna::codegen::deriveNativeOwnedResultSourceFacts(
            program, function, facts, failure) ||
        !(facts == *executable->mPrivateOwnedResultSourceFacts)) {
        failure = "private Ref Result entry is not bound to these frozen source facts";
        return {};
    }
    const void* entry = executable->lookup(
        "__luna_private_ref_apply_transfer_test", failure);
    if (!entry) return {};
    const void* drop = executable->lookup(
        "__luna_private_ref_apply_drop_test", failure);
    if (!drop) return {};
    auto loaded = std::make_unique<LunaPrivateRefResultLoadedEntry>();
    loaded->lease_ = std::move(executable);
    loaded->entry_ = entry;
    loaded->drop_ = drop;
    failure.clear();
    return loaded;
}
#endif

bool CodeGenerator::verifyPrivateRuntimeFragmentRefUnitIngress(
    moon::Module& program, FunctionDecl& function, std::string& failure
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    , std::shared_ptr<LunaJitModule>* executable,
    std::vector<uint8_t>* entryRecord
#endif
) {
    // Never reuse the publishing CodeGenerator. Ordinary proofs are destroyed
    // here; only the test-target hook may materialize the verified body behind
    // a private wrapper while the source/container gates remain closed.
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
    const auto* returnType = program.findType(function.returnType);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    // The production ingress proof stays unit-only. The canonical test target
    // may execute scalar Results and narrow resource variants behind a private
    // wrapper that consumes the returned owner after observation.
    const bool resultShape = executable && privateRefApply && returnType &&
        returnType->kind == TypeKind::Result &&
        returnType->typeArgumentIds.size() == 2;
    const auto* okPayload = resultShape
        ? program.findType(returnType->typeArgumentIds[0]) : nullptr;
    const auto* errorPayload = resultShape
        ? program.findType(returnType->typeArgumentIds[1]) : nullptr;
    const bool privateScalarResultJit = okPayload && errorPayload &&
        okPayload->kind == TypeKind::I32 &&
        errorPayload->kind == TypeKind::I32;
    const auto observedResource = [&program](
        const auto& self, const moon::TypeRecord* payload,
        unsigned& nodes) -> bool {
        // Count every owned instance in a fork, not only its maximum depth.
        if (!payload || payload->kind != TypeKind::Struct ||
            ++nodes > 3 ||
            payload->fields.empty() || payload->fields.size() > 3 ||
            !payload->sysmeta.resource.needsDrop ||
            payload->dropGlue.empty())
            return false;
        bool marker = false;
        for (const auto& field : payload->fields) {
            const auto* type = program.findType(field.type);
            if (!type) return false;
            if (field.name == "marker") {
                if (type->kind != TypeKind::I32) return false;
                marker = true;
            } else if (type->kind != TypeKind::I32 &&
                       !self(self, type, nodes)) {
                return false;
            }
        }
        return marker;
    };
    const auto observedResourceShape = [&](const moon::TypeRecord* payload) {
        unsigned nodes = 0;
        return observedResource(observedResource, payload, nodes);
    };
    const bool privateErrResourceResultJit = okPayload && errorPayload &&
        okPayload->kind == TypeKind::I32 &&
        observedResourceShape(errorPayload);
    const bool privateOkResourceResultJit = okPayload && errorPayload &&
        errorPayload->kind == TypeKind::I32 &&
        observedResourceShape(okPayload);
    const bool privateResourceResultJit = privateErrResourceResultJit ||
        privateOkResourceResultJit;
    const auto* resourcePayload = privateErrResourceResultJit
        ? errorPayload : (privateOkResourceResultJit ? okPayload : nullptr);
    std::optional<luna::codegen::NativeOwnedResultSourceFacts>
        privateOwnedResultFacts;
    if (privateErrResourceResultJit && function.params.size() == 1) {
        luna::codegen::NativeOwnedResultSourceFacts facts;
        if (!luna::codegen::deriveNativeOwnedResultSourceFacts(
                program, function, facts, failure))
            return false;
        privateOwnedResultFacts = std::move(facts);
    }
    const bool privateResultJit = privateScalarResultJit ||
        privateResourceResultJit;
    if (resultShape && !privateResultJit) {
        failure = "private Ref Result payload is outside the bounded cleanup proof";
        return false;
    }
#endif
    if (!returnType || (returnType->kind != TypeKind::Unit
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        && !privateResultJit
#endif
        )) {
        failure = "private Ref unit ingress proof requires a unit return";
        return false;
    }
    if (privateRefApply) {
        if (!function.requiresFragmentContext) {
            failure = "private Ref apply proof requires an explicit Fragment context";
            return false;
        }
        if (function.params.empty() || function.params.size() > 2) {
            failure = "private Ref apply proof requires one or two borrowed Ref parameters";
            return false;
        }
        for (const auto& parameter : function.params) {
            const auto* type = program.findType(parameter.type);
            if (!type || type->kind != TypeKind::RuntimeFragmentRef ||
                parameter.relation != luna::ownership::Relation::SharedBorrow) {
                failure = "private Ref apply proof requires borrowed Ref parameters";
                return false;
            }
        }
    }
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (executable && !privateRefApply) {
        failure = "private Ref apply JIT test requires an Apply region";
        return false;
    }
#endif
    if (!matchesPrivateRefContextEffect(program, function)) {
        failure = "Ref entry context effect differs from the sealed CFG fixed point";
        return false;
    }
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    std::optional<PrivateRefUnitApplyEntryFacts> privateUnitApplyFacts;
    if (executable && privateRefApply && returnType->kind == TypeKind::Unit &&
        function.params.size() == 1) {
        privateUnitApplyFacts = privateRefUnitApplyEntryFacts(program, function);
        if (!privateUnitApplyFacts) {
            failure = "private Ref unit Apply entry differs from its frozen signature";
            return false;
        }
    }
    if (entryRecord && !privateUnitApplyFacts) {
        failure = "private Ref entry record requires one borrowed Ref and unit result";
        return false;
    }
#endif
    std::vector<llvm::Type*> parameters;
    if (function.requiresFragmentContext)
        parameters.push_back(proof.mHelpers->ptrTy());
    parameters.insert(parameters.end(),
                      privateRefApply ? function.params.size() : 1,
                      proof.mHelpers->ptrTy());
    auto* body = llvm::Function::Create(
        llvm::FunctionType::get(
#ifdef LUNA_PRIVATE_REF_JIT_TEST
            privateResultJit
                ? proof.mHelpers->toLLVMType(
                    proof.resolveType(function.returnType))
                :
#endif
                  proof.mHelpers->voidTy(), parameters, false),
        llvm::Function::InternalLinkage,
        function.generatedSymbolName, *proof.mModule);
    proof.mFunctions[function.generatedSymbolName] = body;
    proof.mPrivateRefApplyEnabled = privateRefApply;
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (privateResultJit) {
        // Import only frozen auxiliary bodies named by the canonical cleanup
        // table or by an Err return's From conversion.
        std::vector<FunctionDecl*> auxiliaryMethods;
        const auto findImplMethod = [&](moon::DeclarationRef reference,
                                        bool requireFrom) -> FunctionDecl* {
            const auto* frozen = program.findDeclaration(reference);
            if (!frozen) return nullptr;
            for (auto& declaration : program.declarations)
                if (auto* implementation = dynamic_cast<ImplDecl*>(
                        declaration.get())) {
                    const auto* trait = program.findDeclaration(
                        implementation->traitRef);
                    if (requireFrom && (!trait || trait->sourceName != "From" ||
                                        frozen->sourceName != "from"))
                        continue;
                    for (auto& candidate : implementation->methods)
                        if (candidate->symbolId == frozen->symbolId &&
                            candidate->contractId == frozen->contractId)
                            return candidate.get();
                }
            return nullptr;
        };
        const auto importMethod = [&](FunctionDecl* method,
                                      const char* diagnostic) {
            if (!method || !method->controlFlow ||
                !method->controlFlow->sealed || method->requiresFragmentContext ||
                !cfgVerifier.verify(*method->controlFlow, program)) {
                failure = diagnostic;
                return false;
            }
            if (std::find(auxiliaryMethods.begin(), auxiliaryMethods.end(),
                          method) == auxiliaryMethods.end())
                auxiliaryMethods.push_back(method);
            return true;
        };
        for (const auto& cleanup : function.controlFlow->cleanups) {
            const auto* type = program.findType(cleanup.type);
            if (!type || type->dropGlue.empty()) continue;
            if (!importMethod(findImplMethod(type->dropGlue, false),
                              "private Ref Result Drop glue has no verified source body"))
                return false;
        }
        for (const auto& block : function.controlFlow->blocks) {
            if (block.terminator.kind != moon::TerminatorKind::Return)
                continue;
            const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
                block.terminator.operand.get());
            const auto* conversion = result && !result->isOk
                ? dynamic_cast<const moon::CallExpr*>(result->payload.get())
                : nullptr;
            if (!conversion) continue;
            if (!importMethod(findImplMethod(conversion->calleeRef, true),
                              "private Ref Result From conversion has no verified source body"))
                return false;
        }
        const auto importReturnedResource = [&](
            const auto& self, const moon::TypeRecord* type) -> bool {
            if (!importMethod(findImplMethod(type->dropGlue, false),
                              "private Ref Result resource has no verified Drop body"))
                return false;
            for (const auto& field : type->fields) {
                const auto* nested = program.findType(field.type);
                if (nested && nested->kind == TypeKind::Struct &&
                    !self(self, nested))
                    return false;
            }
            return true;
        };
        if (privateResourceResultJit &&
            !importReturnedResource(importReturnedResource, resourcePayload))
            return false;
        // A conversion can consume an affine source value. Follow only Drop
        // glue frozen in each imported method's own cleanup table.
        for (size_t index = 0; index < auxiliaryMethods.size(); ++index)
            for (const auto& cleanup : auxiliaryMethods[index]->controlFlow->cleanups) {
                const auto* type = program.findType(cleanup.type);
                if (!type || type->dropGlue.empty()) continue;
                if (!importMethod(findImplMethod(type->dropGlue, false),
                                  "private Ref Result auxiliary Drop glue has no verified source body"))
                    return false;
            }
        if (!auxiliaryMethods.empty()) {
            const auto declareAuxiliary = [&](FunctionDecl& auxiliary,
                                              bool external) {
                const auto internalName = auxiliary.generatedSymbolName.empty()
                    ? auxiliary.name : auxiliary.generatedSymbolName;
                const auto* frozen = program.findDeclaration(
                    {auxiliary.symbolId, auxiliary.contractId});
                const auto symbolName = frozen
                    ? frozen->linkageName
                    : (auxiliary.linkName.empty()
                        ? internalName : auxiliary.linkName);
                std::vector<llvm::Type*> arguments;
                for (const auto& parameter : auxiliary.params)
                    arguments.push_back(proof.mHelpers->toLLVMType(
                        proof.resolveType(parameter.type)));
                auto* result = proof.mHelpers->toLLVMType(
                    proof.resolveType(auxiliary.returnType));
                auto* declared = llvm::Function::Create(
                    llvm::FunctionType::get(result, arguments, false),
                    external ? llvm::Function::ExternalLinkage
                             : llvm::Function::InternalLinkage,
                    symbolName, *proof.mModule);
                proof.mFunctions[internalName] = declared;
                proof.mFunctions[symbolName] = declared;
            };
            for (auto& declaration : program.declarations)
                if (auto* external = dynamic_cast<FunctionDecl*>(
                        declaration.get()); external && external->isExtern)
                    declareAuxiliary(*external, true);
            for (auto* method : auxiliaryMethods)
                declareAuxiliary(*method, false);
            for (auto* method : auxiliaryMethods)
                proof.generateFunctionBody(method);
            if (!proof.mErrors.empty()) {
                failure = proof.mErrors.front().message;
                return false;
            }
        }
    }
#endif
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
        const auto& bindings = function.controlFlow->runtimeRefApplies;
        if (bindings.empty() || bindings.size() > 2) {
            failure = "private Ref apply proof supports one or two nested regions";
            return false;
        }
        std::unordered_map<uint32_t, size_t> bindingIndex;
        for (size_t index = 0; index < bindings.size(); ++index) {
            const auto& binding = bindings[index];
            const auto* region = function.controlFlow->findRegion(binding.region);
            if (!region || binding.region.value >= function.controlFlow->regions.size() ||
                !bindingIndex.emplace(binding.region.value, index).second) {
                failure = "nested Ref apply has an invalid or duplicate region";
                return false;
            }
            if (index == 0) {
                if (region->parent != function.controlFlow->rootRegion) {
                    failure = "outer Ref apply region is not at the function root";
                    return false;
                }
            } else {
                bool nested = false;
                for (auto parent = region->parent; !parent.empty();) {
                    if (parent == bindings[index - 1].region) {
                        nested = true;
                        break;
                    }
                    const auto* ancestor = function.controlFlow->findRegion(parent);
                    if (!ancestor)
                        break;
                    parent = ancestor->parent;
                }
                if (!nested) {
                    failure = "nested Ref apply regions are not lexically nested";
                    return false;
                }
            }
        }
        const auto blockCount = function.controlFlow->blocks.size();
        for (const auto& active : flow->activeByBlock) {
            if (active.size() > bindings.size()) {
                failure = "private Ref apply has an unsupported active region depth";
                return false;
            }
            for (size_t index = 0; index < active.size(); ++index)
                if (active[index] != bindings[index].region) {
                    failure = "private Ref apply active regions are not one lexical stack";
                    return false;
                }
        }
        std::vector<const moon::RuntimeRefApplyFlowEdge*> edgeExits(
            blockCount, nullptr);
        std::vector<const moon::RuntimeRefApplyFlowTerminal*> returns(
            blockCount, nullptr);
        size_t expectedDrops = 0;
        std::vector<size_t> entries(function.controlFlow->regions.size(), 0);
        std::vector<size_t> exits(function.controlFlow->regions.size(), 0);
        for (const auto& edge : flow->edges)
            if (!edge.exits.empty() || !edge.enters.empty()) {
                const auto* source = function.controlFlow->findBlock(edge.source);
                if (edge.source.value >= blockCount || !source ||
                    source->terminator.kind != moon::TerminatorKind::Jump ||
                    edge.exits.size() + edge.enters.size() != 1) {
                    failure = "Ref apply has an unsupported non-Jump context transition";
                    return false;
                }
                for (const auto region : edge.enters) {
                    if (region.value >= entries.size()) {
                        failure = "Ref apply entry region is out of range";
                        return false;
                    }
                    ++entries[region.value];
                }
                for (const auto region : edge.exits) {
                    if (region.value >= exits.size()) {
                        failure = "Ref apply exit region is out of range";
                        return false;
                    }
                    ++exits[region.value];
                    ++expectedDrops;
                }
                if (!edge.exits.empty()) {
                    if (edgeExits[edge.source.value]) {
                        failure = "Ref apply has ambiguous normal exit edges";
                        return false;
                    }
                    edgeExits[edge.source.value] = &edge;
                }
            }
        for (const auto& terminal : flow->terminals) {
            if (terminal.kind == moon::TerminatorKind::Unreachable &&
                moon::isExhaustiveResultDefault(
                    *function.controlFlow, program, *flow, terminal.block))
                continue;
            if (terminal.block.value >= blockCount ||
                edgeExits[terminal.block.value] ||
                returns[terminal.block.value] ||
                terminal.kind != moon::TerminatorKind::Return ||
                terminal.exits.empty() ||
                terminal.exits.size() !=
                    flow->activeByBlock[terminal.block.value].size() ||
                !std::equal(terminal.exits.begin(), terminal.exits.end(),
                    flow->activeByBlock[terminal.block.value].rbegin())) {
                failure = "Ref apply has an ambiguous early return proof";
                return false;
            }
            returns[terminal.block.value] = &terminal;
            expectedDrops += terminal.exits.size();
            for (const auto region : terminal.exits) {
                if (region.value >= exits.size()) {
                    failure = "Ref apply return region is out of range";
                    return false;
                }
                ++exits[region.value];
            }
        }
        for (const auto& binding : bindings) {
            if (entries[binding.region.value] != 1 ||
                exits[binding.region.value] == 0) {
                failure = "Ref apply region lacks one entry or a proved exit";
                return false;
            }
        }
        std::vector<const moon::BasicBlock*> slotSites;
        for (const auto& block : function.controlFlow->blocks)
            if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot) {
                if (block.id.value >= flow->activeByBlock.size() ||
                    flow->activeByBlock[block.id.value].empty()) {
                    failure = "private Ref apply has a RuntimeSlot outside its context";
                    return false;
                }
                const auto& active = flow->activeByBlock[block.id.value];
                if (active.size() > bindings.size()) {
                    failure = "private Ref apply Slot has an unknown active region";
                    return false;
                }
                for (size_t index = 0; index < active.size(); ++index)
                    if (active[index] != bindings[index].region) {
                        failure = "private Ref apply Slot has a non-nested active stack";
                        return false;
                    }
                slotSites.push_back(&block);
            }
        std::vector<llvm::CallInst*> deriveCalls;
        std::vector<llvm::CallInst*> dispatchCalls;
        std::vector<llvm::CallInst*> dropCalls;
        std::vector<llvm::CallInst*> refDrops;
        for (auto& block : *body)
            for (auto& instruction : block)
                if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    call && call->getCalledFunction()) {
                    const auto name = call->getCalledFunction()->getName();
                    if (name ==
                        "luna_compiler_fragment_context_override_from_ref") {
                        deriveCalls.push_back(call);
                    } else if (name ==
                        "luna_compiler_fragment_context_drop") {
                        dropCalls.push_back(call);
                    } else if (name ==
                        "luna_runtime_fragment_dispatch_v1") {
                        dispatchCalls.push_back(call);
                    } else if (name ==
                        "luna_runtime_fragment_ref_drop_v1") {
                        refDrops.push_back(call);
                    }
                }
        if (!body->hasInternalLinkage() || deriveCalls.size() != bindings.size() ||
            slotSites.empty() || dispatchCalls.size() != slotSites.size()) {
            failure = "generated Ref apply body lacks connected context lifetimes";
            return false;
        }
        std::unordered_map<uint32_t, llvm::Value*> ownerByRegion;
        std::unordered_map<uint32_t, llvm::CallInst*> deriveByRegion;
        std::unordered_set<const llvm::CallInst*> accountedContextDrops;
        std::unordered_set<const llvm::CallInst*> accountedDerivations;
        size_t expectedDerivationFailureDrops = 0;
        size_t expectedDispatchFailureDrops = 0;
        size_t expectedOutlinedReturnDrops = 0;
        size_t expectedOutlinedJumpDrops = 0;
        size_t expectedOutlinedDerivationFailureDrops = 0;
        for (const auto& bindingRecord : bindings) {
            const auto region = bindingRecord.region;
            const moon::RuntimeRefApplyFlowEdge* entryEdge = nullptr;
            for (const auto& edge : flow->edges)
                if (std::find(edge.enters.begin(), edge.enters.end(), region) !=
                    edge.enters.end()) {
                    if (entryEdge) {
                        failure = "Ref apply region has multiple derivation edges";
                        return false;
                    }
                    entryEdge = &edge;
                }
            if (!entryEdge || entryEdge->enters.size() != 1 ||
                entryEdge->source.value >= blockCount) {
                failure = "Ref apply entry has an ambiguous derivation site";
                return false;
            }
            const auto& sourceActive =
                flow->activeByBlock[entryEdge->source.value];
            const auto binding = bindingIndex.find(region.value);
            if (binding == bindingIndex.end() ||
                binding->second != sourceActive.size()) {
                failure = "Ref apply entry does not extend its active owner stack";
                return false;
            }
            llvm::CallInst* derivation = nullptr;
            const auto sourceName = "cfg." +
                std::to_string(entryEdge->source.value);
            for (auto* call : deriveCalls)
                if (call->getParent()->getName() == sourceName) {
                    if (derivation) {
                        failure = "Ref apply entry emitted duplicate context derivation";
                        return false;
                    }
                    derivation = call;
                }
            if (!derivation || derivation->arg_size() != 5) {
                failure = "Ref apply entry has no unique context derivation";
                return false;
            }
            const auto* borrowed = llvm::dyn_cast<llvm::LoadInst>(
                derivation->getArgOperand(1));
            const auto* local = function.controlFlow->findLocal(
                bindingRecord.reference);
            const auto* carrier = borrowed
                ? llvm::dyn_cast<llvm::AllocaInst>(
                      borrowed->getPointerOperand()) : nullptr;
            size_t parameterPosition = 0;
            for (const auto& candidate : function.controlFlow->locals) {
                if (candidate.kind != moon::LocalKind::Parameter) continue;
                if (candidate.id == bindingRecord.reference) break;
                ++parameterPosition;
            }
            const auto argumentIndex = 1 + parameterPosition;
            bool parameterStore = false;
            if (carrier && argumentIndex < body->arg_size())
                for (const auto& instruction : body->getEntryBlock())
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                            &instruction);
                        store && store->getPointerOperand() == carrier &&
                        store->getValueOperand() == body->getArg(
                            static_cast<unsigned>(argumentIndex)))
                        parameterStore = true;
            llvm::StringRef actualSlot, actualContract;
            if (!local || local->kind != moon::LocalKind::Parameter ||
                parameterPosition >= function.params.size() ||
                !carrier || carrier->getFunction() != body ||
                carrier->getName() !=
                    "local." + std::to_string(local->id.value) + "." +
                        local->name ||
                !parameterStore ||
                !llvm::getConstantStringInfo(
                    derivation->getArgOperand(2), actualSlot) ||
                !llvm::getConstantStringInfo(
                    derivation->getArgOperand(3), actualContract) ||
                actualSlot != bindingRecord.slot.symbol.value ||
                actualContract != bindingRecord.slot.contract.value) {
                failure = "Ref apply derivation changed its verified Ref carrier or exact Slot";
                return false;
            }
            auto* cell = llvm::dyn_cast<llvm::AllocaInst>(
                derivation->getArgOperand(4));
            if (!cell || ownerByRegion.count(region.value)) {
                failure = "Ref apply derivation has no unique owner cell";
                return false;
            }
            const llvm::Value* expectedParent = body->getArg(0);
            if (!sourceActive.empty()) {
                const auto parent = ownerByRegion.find(sourceActive.back().value);
                const auto* loadedParent = llvm::dyn_cast<llvm::LoadInst>(
                    derivation->getArgOperand(0));
                if (parent == ownerByRegion.end() || !loadedParent ||
                    loadedParent->getPointerOperand() != parent->second) {
                    failure = "nested Ref apply derivation lost its parent context";
                    return false;
                }
                expectedParent = loadedParent;
            }
            if (derivation->getArgOperand(0) != expectedParent) {
                failure = "Ref apply derivation has the wrong parent context";
                return false;
            }
            ownerByRegion.emplace(region.value, cell);
            deriveByRegion.emplace(region.value, derivation);
            accountedDerivations.insert(derivation);
            std::vector<const llvm::Value*> parents;
            for (const auto active : sourceActive) {
                const auto found = ownerByRegion.find(active.value);
                if (found == ownerByRegion.end()) {
                    failure = "Ref apply derivation precedes its parent owner";
                    return false;
                }
                parents.push_back(found->second);
            }
            if (!overrideFailureDropsOwners(
                    *derivation, parents, &accountedContextDrops)) {
                failure = "Ref apply derivation failure did not release its parent stack";
                return false;
            }
            expectedDerivationFailureDrops += parents.size();
        }
        if (deriveByRegion.size() != bindings.size()) {
            failure = "private Ref apply left a region without its own derivation";
            return false;
        }
        const auto ownerStackFor = [&flow, &ownerByRegion](
            moon::BlockId block) {
            std::vector<const llvm::Value*> result;
            if (block.value >= flow->activeByBlock.size()) return result;
            for (const auto region : flow->activeByBlock[block.value]) {
                const auto found = ownerByRegion.find(region.value);
                if (found == ownerByRegion.end()) return std::vector<const llvm::Value*>{};
                result.push_back(found->second);
            }
            return result;
        };
        for (const auto* site : slotSites) {
            llvm::CallInst* dispatch = nullptr;
            const auto blockName = "cfg." + std::to_string(site->id.value);
            for (auto* call : dispatchCalls)
                if (call->getParent()->getName() == blockName) {
                    if (dispatch) {
                        failure = "private Ref apply emitted duplicate Slot dispatch";
                        return false;
                    }
                    dispatch = call;
                }
            const auto owners = ownerStackFor(site->id);
            const auto* context = dispatch
                ? llvm::dyn_cast<llvm::LoadInst>(dispatch->getArgOperand(0))
                : nullptr;
            if (owners.empty() || !context || context->getPointerOperand() !=
                    owners.back()) {
                failure = "private Ref apply Slot site lost its derived context";
                return false;
            }
            std::vector<const llvm::Value*> failedOwners = owners;
            if (!dispatchFailureDropsOwners(
                    *dispatch, failedOwners, &accountedContextDrops)) {
                failure = "private Ref apply dispatch failure lost its owner stack";
                return false;
            }
        }
        // An outlined Slot body is emitted both as a canonical (possibly
        // unreachable) source block and as a callable continuation. Prove
        // every generated dispatch, not just the copies in the source body.
        struct ContextFunction {
            llvm::Function* function;
            llvm::Value* outlinedContext;
            std::vector<moon::RegionId> activeRegions;
            std::vector<const llvm::Value*> contextOwnerCells;
            std::unordered_map<uint32_t, const llvm::Value*> ownerByRegion;
            llvm::AllocaInst* incomingFrame;
            llvm::CallInst* incomingDispatch;
        };
        ContextFunction rootContext{body, nullptr, {}, {}, {}, nullptr, nullptr};
        for (const auto& [region, owner] : ownerByRegion)
            rootContext.ownerByRegion.emplace(region, owner);
        std::vector<ContextFunction> pending{std::move(rootContext)};
        std::unordered_set<llvm::Function*> visited;
        size_t connectedDispatches = 0;
        for (size_t index = 0; index < pending.size(); ++index) {
            const auto current = pending[index];
            if (!visited.insert(current.function).second) {
                failure = "private Ref apply reused an outlined Slot callback";
                return false;
            }
            auto functionOwners = current.ownerByRegion;
            if (current.function != body) {
                for (auto& llvmBlock : *current.function) {
                    if (!llvmBlock.getName().starts_with("runtime.slot.cfg."))
                        continue;
                    const auto suffix = llvmBlock.getName().drop_front(
                        llvm::StringRef("runtime.slot.cfg.").size());
                    uint32_t sourceIndex = 0;
                    if (suffix.getAsInteger(10, sourceIndex)) continue;
                    const moon::RuntimeRefApplyFlowEdge* entry = nullptr;
                    moon::RegionId entered;
                    for (const auto& edge : flow->edges)
                        if (edge.source.value == sourceIndex &&
                            edge.enters.size() == 1) {
                            if (entry) {
                                failure = "outlined Ref apply entry has multiple regions";
                                return false;
                            }
                            entry = &edge;
                            entered = edge.enters.front();
                        }
                    if (!entry) continue;
                    llvm::CallInst* derivation = nullptr;
                    for (auto& instruction : llvmBlock)
                        if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                            call && call->getCalledFunction() &&
                            call->getCalledFunction()->getName() ==
                                "luna_compiler_fragment_context_override_from_ref") {
                            if (derivation) {
                                failure = "outlined Ref apply entry emitted duplicate derivations";
                                return false;
                            }
                            derivation = call;
                        }
                    if (!derivation || derivation->arg_size() != 5 ||
                        entered.value >= function.controlFlow->regions.size()) {
                        failure = "outlined Ref apply entry has no unique derivation";
                        return false;
                    }
                    const auto binding = bindingIndex.find(entered.value);
                    if (binding == bindingIndex.end()) {
                        failure = "outlined Ref apply entry has no binding";
                        return false;
                    }
                    const auto* local = function.controlFlow->findLocal(
                        bindings[binding->second].reference);
                    auto* borrowed = llvm::dyn_cast<llvm::LoadInst>(
                        derivation->getArgOperand(1));
                    auto* carrier = borrowed
                        ? llvm::dyn_cast<llvm::AllocaInst>(borrowed->getPointerOperand())
                        : nullptr;
                    llvm::StringRef actualSlot, actualContract;
                    if (binding == bindingIndex.end() || !local || !carrier ||
                        carrier->getFunction() != current.function ||
                        carrier->getName() != "local." +
                            std::to_string(local->id.value) + "." +
                            local->name + ".outlined" ||
                        !llvm::getConstantStringInfo(derivation->getArgOperand(2), actualSlot) ||
                        !llvm::getConstantStringInfo(derivation->getArgOperand(3), actualContract) ||
                        actualSlot != bindings[binding->second].slot.symbol.value ||
                        actualContract != bindings[binding->second].slot.contract.value ||
                        functionOwners.count(entered.value)) {
                        failure = "outlined Ref apply changed its captured carrier or exact Slot";
                        return false;
                    }
                    size_t carrierInitializers = 0;
                    bool capturedCarrier = false;
                    for (auto& instruction : current.function->getEntryBlock())
                        if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction);
                            store && store->getPointerOperand() == carrier) {
                            ++carrierInitializers;
                            const auto* valueLoad = llvm::dyn_cast<llvm::LoadInst>(
                                store->getValueOperand());
                            const auto* captureAddress = valueLoad
                                ? llvm::dyn_cast<llvm::LoadInst>(
                                      valueLoad->getPointerOperand()) : nullptr;
                            const auto* field = captureAddress
                                ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                                      captureAddress->getPointerOperand()) : nullptr;
                            const auto* fieldIndex = field && field->getNumIndices() == 2
                                ? llvm::dyn_cast<llvm::ConstantInt>(field->getOperand(2))
                                : nullptr;
                            if (valueLoad && captureAddress && fieldIndex &&
                                fieldIndex->getZExtValue() >= 2 &&
                                isFrameField(captureAddress->getPointerOperand(),
                                    current.function->getArg(0),
                                    static_cast<unsigned>(fieldIndex->getZExtValue())))
                                capturedCarrier = true;
                        }
                    if (carrierInitializers != 1 || !capturedCarrier) {
                        failure = "outlined Ref apply carrier is not initialized from its callback capture";
                        return false;
                    }
                    const auto* callbackCarrierAddress = llvm::dyn_cast<llvm::LoadInst>(
                        [&]() -> const llvm::Value* {
                            for (auto& instruction : current.function->getEntryBlock())
                                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                                        &instruction);
                                    store && store->getPointerOperand() == carrier)
                                    if (const auto* valueLoad = llvm::dyn_cast<llvm::LoadInst>(
                                            store->getValueOperand()))
                                        return valueLoad->getPointerOperand();
                            return nullptr;
                        }());
                    const auto* captureField = callbackCarrierAddress
                        ? llvm::dyn_cast<llvm::GetElementPtrInst>(
                              callbackCarrierAddress->getPointerOperand()) : nullptr;
                    const auto* captureIndex = captureField &&
                        captureField->getNumIndices() == 2
                        ? llvm::dyn_cast<llvm::ConstantInt>(captureField->getOperand(2))
                        : nullptr;
                    llvm::AllocaInst* parentCarrier = nullptr;
                    size_t parentCarrierCount = 0;
                    if (current.incomingFrame && current.incomingDispatch && captureIndex)
                        for (auto& parentBlock : *current.incomingFrame->getFunction())
                            for (auto& parentInstruction : parentBlock)
                                if (auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(
                                        &parentInstruction);
                                    alloca && (alloca->getName() ==
                                            "local." + std::to_string(local->id.value) +
                                                "." + local->name ||
                                        alloca->getName() == "local." +
                                            std::to_string(local->id.value) + "." +
                                            local->name + ".outlined")) {
                                    parentCarrier = alloca;
                                    ++parentCarrierCount;
                                }
                    if (!current.incomingFrame || !current.incomingDispatch ||
                        !captureIndex || captureIndex->getZExtValue() < 2 ||
                        !parentCarrier || parentCarrierCount != 1 || !frameCapturesValue(
                            *current.incomingFrame->getFunction(),
                            current.incomingFrame, *current.incomingDispatch,
                            static_cast<unsigned>(captureIndex->getZExtValue()),
                            parentCarrier)) {
                        failure = "outlined Ref apply carrier capture has no matching parent local";
                        return false;
                    }
                    const auto& active = flow->activeByBlock[sourceIndex];
                    const llvm::Value* parentContext = current.outlinedContext;
                    std::vector<const llvm::Value*> parents;
                    for (const auto region : active) {
                        const auto owner = functionOwners.find(region.value);
                        if (owner == functionOwners.end()) {
                            failure = "outlined Ref apply derivation lost its parent owner";
                            return false;
                        }
                        parents.push_back(owner->second);
                    }
                    if (active.size() > current.activeRegions.size()) {
                        const auto owner = functionOwners.find(active.back().value);
                        const auto* loaded = llvm::dyn_cast<llvm::LoadInst>(
                            derivation->getArgOperand(0));
                        if (owner == functionOwners.end() || !loaded ||
                            loaded->getPointerOperand() != owner->second) {
                            failure = "outlined Ref apply derivation has the wrong parent context";
                            return false;
                        }
                        parentContext = loaded;
                    }
                    if (derivation->getArgOperand(0) != parentContext) {
                        failure = "outlined Ref apply derivation has the wrong parent context";
                        return false;
                    }
                    auto* cell = llvm::dyn_cast<llvm::AllocaInst>(
                        derivation->getArgOperand(4));
                    if (!cell || !overrideFailureDropsOwners(
                            *derivation, parents, &accountedContextDrops)) {
                        failure = "outlined Ref apply derivation failure lost its parent stack";
                        return false;
                    }
                    accountedDerivations.insert(derivation);
                    functionOwners.emplace(entered.value, cell);
                    expectedOutlinedDerivationFailureDrops += parents.size();
                }
            }
            pending[index].ownerByRegion = functionOwners;
            for (auto& llvmBlock : *current.function)
                for (auto& instruction : llvmBlock) {
                    auto* dispatch = llvm::dyn_cast<llvm::CallInst>(&instruction);
                    if (!dispatch || !dispatch->getCalledFunction() ||
                        dispatch->getCalledFunction()->getName() !=
                            "luna_runtime_fragment_dispatch_v1")
                        continue;
                    ++connectedDispatches;
                    const moon::BasicBlock* site = nullptr;
                    for (const auto* candidate : slotSites)
                        if (llvmBlock.getName() ==
                            (current.function == body ? "cfg." :
                                "runtime.slot.cfg.") +
                                std::to_string(candidate->id.value)) {
                            site = candidate;
                            break;
                        }
                    if (!site) {
                        failure = "outlined Ref apply dispatch has no canonical Slot site";
                        return false;
                    }
                    if (dispatch->arg_size() != 9) {
                        failure = "outlined Ref apply dispatch has invalid ABI arity";
                        return false;
                    }
                    const auto* argumentRecord = program.findType(
                        site->terminator.runtimeArgumentsType);
                    const auto* argumentSize = llvm::dyn_cast<llvm::ConstantInt>(
                        dispatch->getArgOperand(4));
                    const auto* argumentAlignment =
                        llvm::dyn_cast<llvm::ConstantInt>(
                            dispatch->getArgOperand(5));
                    llvm::StringRef dispatchedSlot, dispatchedContract,
                        dispatchedLayout;
                    if (!argumentRecord || !argumentSize || !argumentAlignment ||
                        !llvm::getConstantStringInfo(
                            dispatch->getArgOperand(1), dispatchedSlot) ||
                        !llvm::getConstantStringInfo(
                            dispatch->getArgOperand(2), dispatchedContract) ||
                        !llvm::getConstantStringInfo(
                            dispatch->getArgOperand(3), dispatchedLayout) ||
                        dispatchedSlot !=
                            site->terminator.runtimeSlot.symbol.value ||
                        dispatchedContract !=
                            site->terminator.runtimeSlot.contract.value ||
                        dispatchedLayout != argumentRecord->abiLayoutId.value ||
                        argumentSize->getZExtValue() != argumentRecord->valueSize ||
                        argumentAlignment->getZExtValue() !=
                            argumentRecord->valueAlignment) {
                        failure = "outlined Ref apply dispatch changed its exact Slot or argument layout";
                        return false;
                    }
                    const auto siteRegions =
                        flow->activeByBlock[site->id.value];
                    const auto rootOwners = ownerStackFor(site->id);
                    std::vector<const llvm::Value*> callbackSiteOwners;
                    if (current.function != body)
                        for (const auto region : siteRegions) {
                            const auto found = functionOwners.find(region.value);
                            if (found == functionOwners.end()) {
                                failure = "outlined Ref apply dispatch lost a derived owner";
                                return false;
                            }
                            callbackSiteOwners.push_back(found->second);
                        }
                    const auto& owners = current.function == body
                        ? rootOwners : callbackSiteOwners;
                    if (siteRegions.empty() || owners.size() != siteRegions.size()) {
                        failure = "outlined Ref apply dispatch has no active owner stack";
                        return false;
                    }
                    auto* context = dispatch->getArgOperand(0);
                    const auto* rootContext = llvm::dyn_cast<llvm::LoadInst>(context);
                    const bool callbackBaseDispatch =
                        current.function != body &&
                        current.activeRegions == siteRegions;
                    if (callbackBaseDispatch
                            ? context != current.outlinedContext
                            : !rootContext || rootContext->getPointerOperand() !=
                                  owners.back()) {
                        failure = "outlined Ref apply dispatch lost its Slot or context";
                        return false;
                    }
                    if (!dispatchFailureDropsOwners(
                            *dispatch, owners, &accountedContextDrops)) {
                        failure = "private Ref apply dispatch failure lost its owner stack";
                        return false;
                    }
                    expectedDispatchFailureDrops += owners.size();
                    auto* callback = llvm::dyn_cast<llvm::Function>(
                        dispatch->getArgOperand(7)->stripPointerCasts());
                    auto* frame = llvm::dyn_cast<llvm::AllocaInst>(
                        dispatch->getArgOperand(8));
                    const auto* frameType = frame
                        ? llvm::dyn_cast<llvm::StructType>(
                              frame->getAllocatedType()) : nullptr;
                    const auto ownerCount = owners.size();
                    const auto ownerStart = frameType &&
                        frameType->getNumElements() >= ownerCount + 2
                        ? frameType->getNumElements() - ownerCount : 0;
                    auto* callbackContext = callback
                        ? outlinedFrameLoad(*callback, 0) : nullptr;
                    if (!callback || !callback->hasInternalLinkage() ||
                        !frame || frame->getFunction() != current.function ||
                        !callbackContext || !ownerStart ||
                        ownerStart != frameType->getNumElements() - ownerCount ||
                        !frameCapturesValue(
                            *current.function, frame, *dispatch, 0, context)) {
                        failure = "outlined Ref apply callback has an invalid frame";
                        return false;
                    }
                    std::vector<const llvm::Value*> callbackOwners;
                    for (size_t owner = 0; owner < ownerCount; ++owner) {
                        const auto field = static_cast<unsigned>(ownerStart + owner);
                        auto* loaded = outlinedFrameLoad(*callback, field);
                        if (!loaded || !frameCapturesValue(
                                *current.function, frame, *dispatch, field,
                                owners[owner])) {
                            failure = "outlined Ref apply callback did not inherit its full owner stack";
                            return false;
                        }
                        callbackOwners.push_back(loaded);
                    }
                    std::unordered_map<uint32_t, const llvm::Value*> callbackOwnerMap;
                    for (size_t owner = 0; owner < siteRegions.size(); ++owner)
                        callbackOwnerMap.emplace(siteRegions[owner].value,
                                                 callbackOwners[owner]);
                    pending.push_back({callback, callbackContext, siteRegions,
                                       std::move(callbackOwners),
                                       std::move(callbackOwnerMap), frame, dispatch});
                }
        }
        // Continuation copies must release owners on every CFG-proved Jump
        // exit and Return, including regions entered inside the callback.
        for (size_t index = 1; index < pending.size(); ++index) {
            const auto current = pending[index];
            for (auto& llvmBlock : *current.function) {
                if (!llvmBlock.getName().starts_with("runtime.slot.cfg.")) continue;
                const auto suffix = llvmBlock.getName().drop_front(
                    llvm::StringRef("runtime.slot.cfg.").size());
                uint32_t blockIndex = 0;
                if (suffix.getAsInteger(10, blockIndex) || blockIndex >= blockCount)
                    continue;
                const auto* edge = edgeExits[blockIndex];
                const auto* terminal = returns[blockIndex];
                if (!edge && !terminal) continue;
                std::vector<llvm::CallInst*> contextDrops;
                for (auto& instruction : llvmBlock)
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() ==
                            "luna_compiler_fragment_context_drop") {
                        contextDrops.push_back(call);
                    }
                const auto& exited = edge ? edge->exits : terminal->exits;
                if (contextDrops.size() != exited.size()) {
                    failure = "outlined Ref apply exit has an incomplete context Drop stack";
                    return false;
                }
                for (size_t dropIndex = 0; dropIndex < exited.size(); ++dropIndex) {
                    const auto owner = current.ownerByRegion.find(exited[dropIndex].value);
                    if (owner == current.ownerByRegion.end() ||
                        contextDrops[dropIndex]->getArgOperand(0) != owner->second) {
                        failure = "outlined Ref apply exit released the wrong owner";
                        return false;
                    }
                    accountedContextDrops.insert(contextDrops[dropIndex]);
                }
                if (edge) {
                    const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
                        llvmBlock.getTerminator());
                    if (!branch || !branch->isUnconditional() ||
                        std::any_of(contextDrops.begin(), contextDrops.end(),
                            [branch](const auto* drop) {
                                return !drop->comesBefore(branch);
                            })) {
                        failure = "outlined Ref apply Jump exit does not release before its branch";
                        return false;
                    }
                    const bool targetOutlined = std::any_of(
                        current.function->begin(), current.function->end(),
                        [edge](const llvm::BasicBlock& candidate) {
                            return candidate.getName() == "runtime.slot.cfg." +
                                std::to_string(edge->target.value);
                        });
                    const auto expectedTarget = targetOutlined
                        ? "runtime.slot.cfg." + std::to_string(edge->target.value)
                        : "runtime.slot.completed";
                    if (branch->getSuccessor(0)->getName() != expectedTarget) {
                        failure = "outlined Ref apply Jump exit targets the wrong continuation";
                        return false;
                    }
                    expectedOutlinedJumpDrops += exited.size();
                } else {
                    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                        llvmBlock.getTerminator());
                    if (!returned || std::any_of(contextDrops.begin(), contextDrops.end(),
                            [returned](const auto* drop) {
                                return !drop->comesBefore(returned);
                            })) {
                        failure = "outlined Ref apply return does not release before return";
                        return false;
                    }
                    for (const auto* refDrop : refDrops)
                        if (refDrop->getParent() == &llvmBlock &&
                            std::any_of(contextDrops.begin(), contextDrops.end(),
                                [refDrop](const auto* drop) {
                                    return !drop->comesBefore(refDrop);
                                })) {
                            failure = "outlined Ref apply return drops Ref before context";
                            return false;
                        }
                    expectedOutlinedReturnDrops += exited.size();
                }
            }
        }
        size_t generatedDispatches = 0;
        size_t generatedDerivations = 0;
        for (auto& generated : *proof.mModule)
            for (auto& llvmBlock : generated)
                for (auto& instruction : llvmBlock)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() ==
                            "luna_runtime_fragment_dispatch_v1")
                        ++generatedDispatches;
                    else if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() ==
                            "luna_compiler_fragment_context_override_from_ref")
                        ++generatedDerivations;
        if (generatedDerivations != accountedDerivations.size()) {
            failure = "private Ref apply left an unverified context derivation";
            return false;
        }
        if (connectedDispatches != generatedDispatches) {
            failure = "private Ref apply left an unverified outlined dispatch";
            return false;
        }
        for (size_t index = 0; index < blockCount; ++index) {
            if (!edgeExits[index] && !returns[index]) continue;
            llvm::BasicBlock* exitBlock = nullptr;
            const auto blockName = "cfg." + std::to_string(index);
            for (auto& block : *body)
                if (block.getName() == blockName) exitBlock = &block;
            const std::vector<moon::RegionId>& exited = edgeExits[index]
                ? edgeExits[index]->exits : returns[index]->exits;
            std::vector<llvm::CallInst*> contextDrops;
            for (auto* call : dropCalls)
                if (exitBlock && call->getParent() == exitBlock) {
                    contextDrops.push_back(call);
                }
            if (contextDrops.size() != exited.size()) {
                failure = "Ref apply exit has an incomplete owner Drop stack";
                return false;
            }
            for (size_t dropIndex = 0; dropIndex < exited.size(); ++dropIndex) {
                const auto owner = ownerByRegion.find(exited[dropIndex].value);
                if (owner == ownerByRegion.end() ||
                    contextDrops[dropIndex]->getArgOperand(0) != owner->second) {
                    failure = "Ref apply exit released the wrong context owner";
                    return false;
                }
                accountedContextDrops.insert(contextDrops[dropIndex]);
            }
            if (const auto* edge = edgeExits[index]) {
                const auto* branch = llvm::dyn_cast<llvm::BranchInst>(
                    exitBlock->getTerminator());
                if (!branch || !branch->isUnconditional() ||
                    branch->getSuccessor(0)->getName() !=
                        "cfg." + std::to_string(edge->target.value) ||
                    std::any_of(contextDrops.begin(), contextDrops.end(),
                        [branch](const auto* drop) {
                            return !drop->comesBefore(branch);
                        })) {
                    failure = "normal Ref apply exit does not release before its Jump";
                    return false;
                }
                continue;
            }
            const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                exitBlock->getTerminator());
            if (!returned || std::any_of(contextDrops.begin(), contextDrops.end(),
                    [returned](const auto* drop) {
                        return !drop->comesBefore(returned);
                    })) {
                failure = "early Ref apply exit does not release before return";
                return false;
            }
            // A Return created by `?` may also clean apply-local resources.
            // Match each direct local's Drop/deallocation before context Drop.
            const auto* canonicalReturn = function.controlFlow->findBlock(
                returns[index]->block);
#ifdef LUNA_PRIVATE_REF_JIT_TEST
            if (privateResultJit && canonicalReturn) {
                const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
                    canonicalReturn->terminator.operand.get());
                const auto* conversion = result && !result->isOk
                    ? dynamic_cast<const moon::CallExpr*>(result->payload.get())
                    : nullptr;
                if (conversion) {
                    const auto* frozen = program.findDeclaration(
                        conversion->calleeRef);
                    size_t calls = 0;
                    for (const auto& instruction : *exitBlock)
                        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                                &instruction);
                            call && frozen && call->getCalledFunction() &&
                            call->getCalledFunction()->getName() ==
                                frozen->linkageName &&
                            call->comesBefore(contextDrops.front()))
                            ++calls;
                    if (calls != 1) {
                        failure = "Ref apply Err conversion did not run exactly once before context Drop";
                        return false;
                    }
                }
            }
#endif
            struct LocalCleanupProof {
                std::string storage;
                std::string dropGlue;
            };
            std::vector<LocalCleanupProof> localCleanups;
            if (canonicalReturn)
                for (const auto cleanupId :
                     canonicalReturn->terminator.exitCleanups) {
                    const auto* cleanup =
                        function.controlFlow->findCleanup(cleanupId);
                    const auto* scope = cleanup
                        ? function.controlFlow->findScope(cleanup->scope)
                        : nullptr;
                    if (!cleanup || !scope || cleanup->guard ||
                        !cleanup->place.projections.empty())
                        continue;
                    const auto* cleanupType = program.findType(cleanup->type);
                    const bool plainDeallocate = cleanup->action ==
                        luna::ownership::CleanupAction::Deallocate;
                    const bool customDrop = cleanup->action ==
                            luna::ownership::CleanupAction::Drop &&
                        cleanupType && cleanupType->kind == TypeKind::Struct &&
                        !cleanupType->dropGlue.empty();
                    if (!plainDeallocate && !customDrop) continue;
                    for (auto region = scope->region; !region.empty();) {
                        if (std::find(
                                flow->activeByBlock[index].begin(),
                                flow->activeByBlock[index].end(), region) !=
                            flow->activeByBlock[index].end()) {
                            const auto* local = function.controlFlow->findLocal(
                                cleanup->place.root);
                            if (!local) {
                                failure = "Ref apply return cleanup has no local storage";
                                return false;
                            }
                            const auto* glue = customDrop
                                ? program.findDeclaration(cleanupType->dropGlue)
                                : nullptr;
                            if (customDrop && !glue) {
                                failure = "Ref apply local Drop has no frozen glue";
                                return false;
                            }
                            localCleanups.push_back({
                                "local." + std::to_string(local->id.value) +
                                    "." + local->name,
                                glue ? glue->linkageName : ""});
                            break;
                        }
                        const auto* record =
                            function.controlFlow->findRegion(region);
                        if (!record) break;
                        region = record->parent;
                    }
                }
            for (const auto& expected : localCleanups) {
                const llvm::CallInst* deallocation = nullptr;
                const llvm::CallInst* drop = nullptr;
                for (const auto& instruction : *exitBlock) {
                    const auto* call = llvm::dyn_cast<llvm::CallInst>(
                        &instruction);
                    if (!call || !call->getCalledFunction() ||
                        call->arg_size() == 0 ||
                        !call->comesBefore(contextDrops.front()))
                        continue;
                    const auto* loaded = llvm::dyn_cast<llvm::LoadInst>(
                        call->getArgOperand(0)->stripPointerCasts());
                    const auto* storage = loaded
                        ? llvm::dyn_cast<llvm::AllocaInst>(
                            loaded->getPointerOperand())
                        : nullptr;
                    if (!storage || storage->getName() != expected.storage)
                        continue;
                    const auto callee = call->getCalledFunction()->getName();
                    if (callee == "rt_dealloc") deallocation = call;
                    if (!expected.dropGlue.empty() &&
                        callee == expected.dropGlue)
                        drop = call;
                }
                if (!deallocation ||
                    (!expected.dropGlue.empty() &&
                     (!drop || !drop->comesBefore(deallocation)))) {
                    failure = "Ref apply return lost apply-local Drop/deallocation order before context Drop";
                    return false;
                }
            }
            for (const auto* refDrop : refDrops)
                if (refDrop->getParent() == exitBlock &&
                    std::any_of(contextDrops.begin(), contextDrops.end(),
                        [refDrop](const auto* drop) {
                            return !drop->comesBefore(refDrop);
                        })) {
                    failure = "early Ref apply released its Ref before its context";
                    return false;
                }
        }
        size_t generatedContextDrops = 0;
        for (auto& generated : *proof.mModule)
            for (auto& llvmBlock : generated)
                for (auto& instruction : llvmBlock)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() ==
                            "luna_compiler_fragment_context_drop") {
                        ++generatedContextDrops;
                        if (!accountedContextDrops.count(call)) {
                            failure = "private Ref apply left an unverified context Drop";
                            return false;
                        }
                    }
        if (generatedContextDrops != accountedContextDrops.size() ||
            accountedContextDrops.size() != expectedDrops +
                expectedDerivationFailureDrops +
                expectedDispatchFailureDrops + expectedOutlinedReturnDrops +
                expectedOutlinedJumpDrops +
                expectedOutlinedDerivationFailureDrops) {
            failure = "private Ref apply context Drop count differs from its CFG proof";
            return false;
        }
        std::string invalidIR;
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        if (executable) {
            auto* entryType = privateResultJit
                ? llvm::FunctionType::get(
                    proof.mHelpers->i64Ty(),
                    body->getFunctionType()->params(), false)
                : body->getFunctionType();
            auto* entry = llvm::Function::Create(
                entryType, llvm::Function::ExternalLinkage,
                "__luna_private_ref_apply_jit_test", *proof.mModule);
            auto* entryBlock = llvm::BasicBlock::Create(
                *proof.mCtx, "entry", entry);
            llvm::IRBuilder<> builder(entryBlock);
            std::vector<llvm::Value*> arguments;
            for (auto& argument : entry->args())
                arguments.push_back(&argument);
            auto* returned = builder.CreateCall(body, arguments);
            if (privateScalarResultJit) {
                // Private test observation only: tag in bit 32, i32 payload
                // in bits 0..31. This is not a source or host ABI.
                auto* tag = builder.CreateZExt(
                    builder.CreateExtractValue(returned, {0}),
                    proof.mHelpers->i64Ty());
                auto* payload = builder.CreateExtractValue(returned, {1, 0});
                auto* packed = builder.CreateOr(
                    builder.CreateShl(tag, 32),
                    builder.CreateAnd(payload,
                        llvm::ConstantInt::get(proof.mHelpers->i64Ty(),
                                               0xffffffffu)));
                builder.CreateRet(packed);
            } else if (privateResourceResultJit) {
                // The test wrapper owns one resource variant returned by the
                // body. Observe its marker, then finalize and deallocate it.
                // No pointer or ownership carrier escapes this JIT module.
                auto* resourceBlock = llvm::BasicBlock::Create(
                    *proof.mCtx, "result.resource", entry);
                auto* scalarBlock = llvm::BasicBlock::Create(
                    *proof.mCtx, "result.scalar", entry);
                builder.CreateCondBr(
                    builder.CreateExtractValue(returned, {0}),
                    privateOkResourceResultJit ? resourceBlock : scalarBlock,
                    privateOkResourceResultJit ? scalarBlock : resourceBlock);
                builder.SetInsertPoint(scalarBlock);
                auto* scalarValue = builder.CreateExtractValue(returned, {1, 0});
                builder.CreateRet(builder.CreateOr(
                    llvm::ConstantInt::get(proof.mHelpers->i64Ty(),
                        privateErrResourceResultJit ? uint64_t{1} << 32 : 0),
                    builder.CreateAnd(scalarValue,
                        llvm::ConstantInt::get(proof.mHelpers->i64Ty(),
                                               0xffffffffu))));
                builder.SetInsertPoint(resourceBlock);
                const auto* frozenDrop = program.findDeclaration(
                    resourcePayload->dropGlue);
                auto* drop = frozenDrop
                    ? proof.mModule->getFunction(frozenDrop->linkageName)
                    : nullptr;
                if (!drop) {
                    failure = "private Ref Result return lost its frozen Drop glue";
                    return false;
                }
                auto* ownerBits = builder.CreateExtractValue(
                    returned, {1, 0});
                auto* owner = builder.CreateIntToPtr(
                    ownerBits, proof.mHelpers->ptrTy(), "result.owner");
                const auto resourceType = proof.resolveType(
                    privateErrResourceResultJit
                        ? returnType->typeArgumentIds[1]
                        : returnType->typeArgumentIds[0]);
                const auto markerField = std::find_if(
                    resourcePayload->fields.begin(),
                    resourcePayload->fields.end(),
                    [](const auto& field) { return field.name == "marker"; });
                if (!resourceType ||
                    markerField == resourcePayload->fields.end()) {
                    failure = "private Ref Result return has no frozen marker layout";
                    return false;
                }
                const auto markerIndex = static_cast<size_t>(
                    markerField - resourcePayload->fields.begin());
                const auto markerOffset = luna::layout::productFieldOffset(
                    resourceType, markerIndex);
                if (typeSize(resourceType) < sizeof(int32_t) ||
                    markerOffset > typeSize(resourceType) - sizeof(int32_t)) {
                    failure = "private Ref Result marker escapes its resource layout";
                    return false;
                }
                auto* markerAddress = markerOffset == 0 ? owner
                    : builder.CreateGEP(
                        llvm::Type::getInt8Ty(*proof.mCtx), owner,
                        llvm::ConstantInt::get(proof.mHelpers->sizeTy(),
                                               markerOffset),
                        "result.marker.address");
                auto* marker = builder.CreateLoad(
                    proof.mHelpers->i32Ty(), markerAddress, "result.marker");
                const auto savedIP = proof.mBuilder->saveIP();
                auto* savedFunction = proof.mCurrentFunc;
                auto* savedContext = proof.mCurrentFragmentContext;
                const bool savedKernel = proof.mCurrentFunctionIsKernel;
                proof.mBuilder->SetInsertPoint(resourceBlock);
                proof.mCurrentFunc = entry;
                proof.mCurrentFragmentContext = nullptr;
                proof.mCurrentFunctionIsKernel = false;
                proof.emitOwnedPayloadCleanup(
                    owner, resourceType, "result.owner");
                auto* cleanupEnd = proof.mBuilder->GetInsertBlock();
                proof.mBuilder->restoreIP(savedIP);
                proof.mCurrentFunc = savedFunction;
                proof.mCurrentFragmentContext = savedContext;
                proof.mCurrentFunctionIsKernel = savedKernel;
                if (!proof.mErrors.empty()) {
                    failure = proof.mErrors.front().message;
                    return false;
                }
                if (cleanupEnd != resourceBlock ||
                    resourceBlock->getTerminator()) {
                    failure = "private Ref Result resource cleanup left its admitted shape";
                    return false;
                }
                std::vector<std::string> expectedCleanupCalls;
                const auto collectCleanupCalls = [&](
                    const auto& self, const moon::TypeRecord* type) -> bool {
                    const auto* frozen = program.findDeclaration(type->dropGlue);
                    if (!frozen) return false;
                    expectedCleanupCalls.push_back(frozen->linkageName);
                    for (const auto& field : type->fields) {
                        const auto* nested = program.findType(field.type);
                        if (nested && nested->kind == TypeKind::Struct &&
                            !self(self, nested))
                            return false;
                    }
                    expectedCleanupCalls.push_back("rt_dealloc");
                    return true;
                };
                if (!collectCleanupCalls(collectCleanupCalls,
                                         resourcePayload)) {
                    failure = "private Ref Result nested Drop lost its frozen reference";
                    return false;
                }
                std::vector<std::string> actualCleanupCalls;
                for (const auto& instruction : *resourceBlock)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction)) {
                        const auto* callee = call->getCalledFunction();
                        if (!callee) {
                            failure = "private Ref Result resource cleanup has an indirect call";
                            return false;
                        }
                        actualCleanupCalls.push_back(callee->getName().str());
                    }
                if (actualCleanupCalls != expectedCleanupCalls) {
                    failure = "private Ref Result resource cleanup lost Drop/deallocation order";
                    return false;
                }
                builder.SetInsertPoint(cleanupEnd);
                auto* packedMarker = builder.CreateZExt(
                    marker, proof.mHelpers->i64Ty());
                builder.CreateRet(builder.CreateOr(
                    llvm::ConstantInt::get(proof.mHelpers->i64Ty(),
                        privateOkResourceResultJit ? uint64_t{1} << 32 : 0),
                    packedMarker));
            } else {
                builder.CreateRetVoid();
            }
            if (privateResourceResultJit && function.params.size() == 1) {
                // Private host-transfer experiment. The owner cell is the
                // commit point; the JIT module must remain live until Drop.
                const auto resourceType = proof.resolveType(
                    privateErrResourceResultJit
                        ? returnType->typeArgumentIds[1]
                        : returnType->typeArgumentIds[0]);
                const auto transferTarget =
                    program.resolveRuntimeFragmentRefTarget(
                        function.params.front().type);
                if (!resourceType || !transferTarget ||
                    (privateOwnedResultFacts &&
                     (privateOwnedResultFacts->refSlotSymbolId !=
                          transferTarget->symbol.value ||
                      privateOwnedResultFacts->refSlotContractId !=
                          transferTarget->contract.value))) {
                    failure = "private Ref Result transfer lost its frozen resource or Ref target";
                    return false;
                }
                auto* ptrTy = proof.mHelpers->ptrTy();
                auto* i32Ty = proof.mHelpers->i32Ty();
                std::vector<llvm::Type*> transferParams(
                    body->getFunctionType()->params().begin(),
                    body->getFunctionType()->params().end());
                transferParams.insert(transferParams.end(),
                                      {ptrTy, ptrTy, ptrTy, i32Ty});
                auto* transfer = llvm::Function::Create(
                    llvm::FunctionType::get(i32Ty, transferParams, false),
                    llvm::Function::ExternalLinkage,
                    "__luna_private_ref_apply_transfer_test", *proof.mModule);
                if (privateOwnedResultFacts &&
                    (body->getName() !=
                        privateOwnedResultFacts->sourceLinkageName ||
                     resourcePayload->id.value !=
                        privateOwnedResultFacts->errorTypeId ||
                     transfer->getCallingConv() != llvm::CallingConv::C ||
                     transfer->arg_size() != 6 ||
                     transfer->getReturnType() != i32Ty)) {
                    failure = "private Ref Result transfer differs from its frozen source facts";
                    return false;
                }
                auto* transferEntry = llvm::BasicBlock::Create(
                    *proof.mCtx, "entry", transfer);
                auto* checkOwner = llvm::BasicBlock::Create(
                    *proof.mCtx, "check.owner", transfer);
                auto* checkContext = llvm::BasicBlock::Create(
                    *proof.mCtx, "check.context", transfer);
                auto* checkedContext = llvm::BasicBlock::Create(
                    *proof.mCtx, "context.accepted", transfer);
                auto* invalidContext = llvm::BasicBlock::Create(
                    *proof.mCtx, "context.invalid", transfer);
                auto* invalidHandle = llvm::BasicBlock::Create(
                    *proof.mCtx, "ref.invalid.handle", transfer);
                auto* invalidTarget = llvm::BasicBlock::Create(
                    *proof.mCtx, "ref.invalid.target", transfer);
                auto* unexpectedCheck = llvm::BasicBlock::Create(
                    *proof.mCtx, "check.unexpected", transfer);
                auto* callBody = llvm::BasicBlock::Create(
                    *proof.mCtx, "call.body", transfer);
                auto* invalid = llvm::BasicBlock::Create(
                    *proof.mCtx, "invalid", transfer);
                auto* resource = llvm::BasicBlock::Create(
                    *proof.mCtx, "resource", transfer);
                auto* scalar = llvm::BasicBlock::Create(
                    *proof.mCtx, "scalar", transfer);
                auto* scalarCommit = llvm::BasicBlock::Create(
                    *proof.mCtx, "scalar.commit", transfer);
                auto* injectedScalarFailure = llvm::BasicBlock::Create(
                    *proof.mCtx, "scalar.injected.failure", transfer);
                auto* nullResource = llvm::BasicBlock::Create(
                    *proof.mCtx, "null.resource", transfer);
                auto* resourceValid = llvm::BasicBlock::Create(
                    *proof.mCtx, "resource.valid", transfer);
                auto* injectedResourceFailure = llvm::BasicBlock::Create(
                    *proof.mCtx, "resource.injected.failure", transfer);
                llvm::IRBuilder<> transferBuilder(transferEntry);
                auto argument = transfer->arg_begin();
                std::vector<llvm::Value*> bodyArguments;
                for (size_t i = 0; i < function.params.size() + 1; ++i)
                    bodyArguments.push_back(&*argument++);
                auto* tagOut = &*argument++;
                auto* scalarOut = &*argument++;
                auto* ownerOut = &*argument++;
                auto* failAfterBody = &*argument;
                const auto disjoint = [&](llvm::Value* left, size_t leftSize,
                                          llvm::Value* right, size_t rightSize) {
                    auto* leftAddress = transferBuilder.CreatePtrToInt(
                        left, proof.mHelpers->sizeTy());
                    auto* rightAddress = transferBuilder.CreatePtrToInt(
                        right, proof.mHelpers->sizeTy());
                    auto* leftBeforeRight = transferBuilder.CreateICmpULT(
                        leftAddress, rightAddress);
                    auto* gap = transferBuilder.CreateSelect(leftBeforeRight,
                        transferBuilder.CreateSub(rightAddress, leftAddress),
                        transferBuilder.CreateSub(leftAddress, rightAddress));
                    return transferBuilder.CreateSelect(leftBeforeRight,
                        transferBuilder.CreateICmpUGE(gap,
                            llvm::ConstantInt::get(proof.mHelpers->sizeTy(),
                                                   leftSize)),
                        transferBuilder.CreateICmpUGE(gap,
                            llvm::ConstantInt::get(proof.mHelpers->sizeTy(),
                                                   rightSize)));
                };
                auto* validPointers = transferBuilder.CreateAnd(
                    transferBuilder.CreateAnd(
                        transferBuilder.CreateIsNotNull(tagOut),
                        transferBuilder.CreateIsNotNull(scalarOut)),
                    transferBuilder.CreateAnd(
                        transferBuilder.CreateIsNotNull(ownerOut),
                        transferBuilder.CreateAnd(
                            disjoint(tagOut, sizeof(uint32_t),
                                     scalarOut, sizeof(int32_t)),
                            transferBuilder.CreateAnd(
                                disjoint(tagOut, sizeof(uint32_t),
                                         ownerOut, sizeof(void*)),
                                disjoint(scalarOut, sizeof(int32_t),
                                         ownerOut, sizeof(void*))))));
                const auto aligned = [&](llvm::Value* pointer, size_t alignment) {
                    auto* address = transferBuilder.CreatePtrToInt(
                        pointer, proof.mHelpers->sizeTy());
                    return transferBuilder.CreateICmpEQ(
                        transferBuilder.CreateAnd(address,
                            llvm::ConstantInt::get(proof.mHelpers->sizeTy(),
                                                   alignment - 1)),
                        llvm::ConstantInt::get(proof.mHelpers->sizeTy(), 0));
                };
                validPointers = transferBuilder.CreateAnd(validPointers,
                    transferBuilder.CreateAnd(
                        aligned(tagOut, alignof(uint32_t)),
                        transferBuilder.CreateAnd(
                            aligned(scalarOut, alignof(int32_t)),
                            aligned(ownerOut, alignof(void*)))));
                transferBuilder.CreateCondBr(validPointers, checkOwner, invalid);
                transferBuilder.SetInsertPoint(checkOwner);
                transferBuilder.CreateCondBr(
                    transferBuilder.CreateIsNull(
                        transferBuilder.CreateLoad(ptrTy, ownerOut)),
                    checkContext, invalid);
                transferBuilder.SetInsertPoint(invalid);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST));
                transferBuilder.SetInsertPoint(checkContext);
                auto contextCheck = proof.mModule->getOrInsertFunction(
                    "luna_compiler_fragment_context_check", i32Ty, ptrTy);
                auto* contextCall = transferBuilder.CreateCall(
                    contextCheck, {bodyArguments.front()});
                auto* statusType = llvm::cast<llvm::IntegerType>(i32Ty);
                auto* contextBranch = transferBuilder.CreateSwitch(
                    contextCall, unexpectedCheck, 2);
                contextBranch->addCase(llvm::ConstantInt::getSigned(statusType,
                    LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS), checkedContext);
                contextBranch->addCase(llvm::ConstantInt::getSigned(statusType,
                    LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT),
                    invalidContext);
                transferBuilder.SetInsertPoint(invalidContext);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_CONTEXT_V1_TEST));
                transferBuilder.SetInsertPoint(checkedContext);
                auto* refCheck =
                    proof.mHelpers->emitRuntimeFragmentRefBorrowCheck(
                        transferBuilder, *proof.mModule, bodyArguments[1],
                        *transferTarget);
                if (!refCheck) {
                    failure = "private Ref Result transfer lost its exact borrowed check";
                    return false;
                }
                auto* refBranch = transferBuilder.CreateSwitch(
                    refCheck, unexpectedCheck, 3);
                refBranch->addCase(llvm::ConstantInt::getSigned(statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1), callBody);
                refBranch->addCase(llvm::ConstantInt::getSigned(statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1),
                    invalidHandle);
                refBranch->addCase(llvm::ConstantInt::getSigned(statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1),
                    invalidTarget);
                transferBuilder.SetInsertPoint(invalidHandle);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_HANDLE_V1_TEST));
                transferBuilder.SetInsertPoint(invalidTarget);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_TARGET_V1_TEST));
                transferBuilder.SetInsertPoint(unexpectedCheck);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST));
                transferBuilder.SetInsertPoint(callBody);
                auto* transferred = transferBuilder.CreateCall(body, bodyArguments);
                transferBuilder.CreateCondBr(
                    transferBuilder.CreateExtractValue(transferred, {0}),
                    privateOkResourceResultJit ? resource : scalar,
                    privateOkResourceResultJit ? scalar : resource);
                transferBuilder.SetInsertPoint(scalar);
                transferBuilder.CreateCondBr(
                    transferBuilder.CreateICmpNE(failAfterBody,
                        llvm::ConstantInt::get(i32Ty, 0)),
                    injectedScalarFailure, scalarCommit);
                transferBuilder.SetInsertPoint(injectedScalarFailure);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INJECTED_FAILURE_V1_TEST));
                transferBuilder.SetInsertPoint(scalarCommit);
                transferBuilder.CreateStore(llvm::ConstantInt::get(i32Ty,
                    privateErrResourceResultJit ? 1 : 0), tagOut);
                transferBuilder.CreateStore(
                    transferBuilder.CreateIntCast(
                        transferBuilder.CreateExtractValue(transferred, {1, 0}),
                        i32Ty, true),
                    scalarOut);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST));
                transferBuilder.SetInsertPoint(resource);
                auto* transferredOwner = transferBuilder.CreateIntToPtr(
                    transferBuilder.CreateExtractValue(transferred, {1, 0}),
                    ptrTy);
                auto* commit = llvm::BasicBlock::Create(
                    *proof.mCtx, "commit", transfer);
                transferBuilder.CreateCondBr(
                    transferBuilder.CreateIsNotNull(transferredOwner),
                    resourceValid, nullResource);
                transferBuilder.SetInsertPoint(nullResource);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_RESOURCE_V1_TEST));
                transferBuilder.SetInsertPoint(resourceValid);
                transferBuilder.CreateCondBr(
                    transferBuilder.CreateICmpNE(failAfterBody,
                        llvm::ConstantInt::get(i32Ty, 0)),
                    injectedResourceFailure, commit);
                transferBuilder.SetInsertPoint(commit);
                transferBuilder.CreateStore(llvm::ConstantInt::get(i32Ty,
                    privateOkResourceResultJit ? 1 : 0), tagOut);
                transferBuilder.CreateStore(transferredOwner, ownerOut);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST));

                const auto failureSavedIP = proof.mBuilder->saveIP();
                auto* failureSavedFunction = proof.mCurrentFunc;
                auto* failureSavedContext = proof.mCurrentFragmentContext;
                const bool failureSavedKernel = proof.mCurrentFunctionIsKernel;
                proof.mBuilder->SetInsertPoint(injectedResourceFailure);
                proof.mCurrentFunc = transfer;
                proof.mCurrentFragmentContext = nullptr;
                proof.mCurrentFunctionIsKernel = false;
                proof.emitOwnedPayloadCleanup(transferredOwner, resourceType,
                                              "result.uncommitted.owner");
                auto* failureCleanupEnd = proof.mBuilder->GetInsertBlock();
                proof.mBuilder->restoreIP(failureSavedIP);
                proof.mCurrentFunc = failureSavedFunction;
                proof.mCurrentFragmentContext = failureSavedContext;
                proof.mCurrentFunctionIsKernel = failureSavedKernel;
                if (!proof.mErrors.empty()) {
                    failure = proof.mErrors.front().message;
                    return false;
                }
                if (failureCleanupEnd != injectedResourceFailure ||
                    injectedResourceFailure->getTerminator()) {
                    failure = "private Ref Result failed transfer left its admitted shape";
                    return false;
                }
                transferBuilder.SetInsertPoint(failureCleanupEnd);
                transferBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                    LUNA_PRIVATE_REF_RESULT_TRANSFER_INJECTED_FAILURE_V1_TEST));

                auto* dropEntry = llvm::Function::Create(
                    llvm::FunctionType::get(i32Ty, {ptrTy}, false),
                    llvm::Function::ExternalLinkage,
                    "__luna_private_ref_apply_drop_test", *proof.mModule);
                auto* dropStart = llvm::BasicBlock::Create(
                    *proof.mCtx, "entry", dropEntry);
                auto* dropCheck = llvm::BasicBlock::Create(
                    *proof.mCtx, "check.owner", dropEntry);
                auto* dropInvalid = llvm::BasicBlock::Create(
                    *proof.mCtx, "invalid", dropEntry);
                auto* dropOwned = llvm::BasicBlock::Create(
                    *proof.mCtx, "drop.owner", dropEntry);
                llvm::IRBuilder<> dropBuilder(dropStart);
                auto* ownerCell = &*dropEntry->arg_begin();
                dropBuilder.CreateCondBr(
                    dropBuilder.CreateIsNotNull(ownerCell),
                    dropCheck, dropInvalid);
                dropBuilder.SetInsertPoint(dropCheck);
                auto* owned = dropBuilder.CreateLoad(ptrTy, ownerCell);
                dropBuilder.CreateCondBr(
                    dropBuilder.CreateIsNotNull(owned), dropOwned, dropInvalid);
                dropBuilder.SetInsertPoint(dropInvalid);
                dropBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 1));
                dropBuilder.SetInsertPoint(dropOwned);
                dropBuilder.CreateStore(
                    llvm::Constant::getNullValue(ptrTy), ownerCell);
                const auto savedIP = proof.mBuilder->saveIP();
                auto* savedFunction = proof.mCurrentFunc;
                auto* savedContext = proof.mCurrentFragmentContext;
                const bool savedKernel = proof.mCurrentFunctionIsKernel;
                proof.mBuilder->SetInsertPoint(dropOwned);
                proof.mCurrentFunc = dropEntry;
                proof.mCurrentFragmentContext = nullptr;
                proof.mCurrentFunctionIsKernel = false;
                proof.emitOwnedPayloadCleanup(
                    owned, resourceType, "result.transferred.owner");
                auto* cleanupEnd = proof.mBuilder->GetInsertBlock();
                proof.mBuilder->restoreIP(savedIP);
                proof.mCurrentFunc = savedFunction;
                proof.mCurrentFragmentContext = savedContext;
                proof.mCurrentFunctionIsKernel = savedKernel;
                if (!proof.mErrors.empty()) {
                    failure = proof.mErrors.front().message;
                    return false;
                }
                if (cleanupEnd != dropOwned || dropOwned->getTerminator()) {
                    failure = "private Ref Result transferred Drop left its admitted shape";
                    return false;
                }
                std::vector<std::string> expectedDropCalls;
                const auto collectDropCalls = [&](const auto& self,
                    const moon::TypeRecord* type) -> bool {
                    const auto* frozen = program.findDeclaration(type->dropGlue);
                    if (!frozen) return false;
                    expectedDropCalls.push_back(frozen->linkageName);
                    for (const auto& field : type->fields) {
                        const auto* nested = program.findType(field.type);
                        if (nested && nested->kind == TypeKind::Struct &&
                            !self(self, nested))
                            return false;
                    }
                    expectedDropCalls.push_back("rt_dealloc");
                    return true;
                };
                if (!collectDropCalls(collectDropCalls, resourcePayload)) {
                    failure = "private Ref Result transferred Drop lost its frozen reference";
                    return false;
                }
                std::vector<std::string> actualDropCalls;
                for (const auto& instruction : *dropOwned)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction)) {
                        const auto* callee = call->getCalledFunction();
                        if (!callee) {
                            failure = "private Ref Result transferred Drop has an indirect call";
                            return false;
                        }
                        actualDropCalls.push_back(callee->getName().str());
                    }
                if (actualDropCalls != expectedDropCalls) {
                    failure = "private Ref Result transferred Drop lost cleanup order";
                    return false;
                }
                std::vector<std::string> failureCleanupCalls;
                for (const auto& instruction : *injectedResourceFailure)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction)) {
                        const auto* callee = call->getCalledFunction();
                        if (!callee) {
                            failure = "private Ref Result failed transfer has an indirect call";
                            return false;
                        }
                        failureCleanupCalls.push_back(callee->getName().str());
                    }
                if (failureCleanupCalls != expectedDropCalls) {
                    failure = "private Ref Result failed transfer lost cleanup order";
                    return false;
                }
                size_t transferBodyCalls = 0;
                size_t contextChecks = 0;
                size_t refChecks = 0;
                for (const auto& block : *transfer)
                    for (const auto& instruction : block)
                        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                                &instruction)) {
                            if (&block == injectedResourceFailure) continue;
                            if (call == contextCall) {
                                ++contextChecks;
                            } else if (call == refCheck) {
                                ++refChecks;
                            } else if (call->getCalledFunction() == body) {
                                ++transferBodyCalls;
                            } else {
                                failure = "private Ref Result transfer has an unexpected call";
                                return false;
                            }
                        }
                if (transferBodyCalls != 1 || contextChecks != 1 ||
                    refChecks != 1 || contextCall->getParent() != checkContext ||
                    refCheck->getParent() != checkedContext ||
                    contextCall->getArgOperand(0) != bodyArguments.front() ||
                    refCheck->getArgOperand(0) != bodyArguments[1]) {
                    failure = "private Ref Result transfer lost preflight or one body call";
                    return false;
                }
                dropBuilder.SetInsertPoint(cleanupEnd);
                dropBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
            }
            if (returnType->kind == TypeKind::Unit &&
                function.params.size() == 1) {
                // Apply CFGs are intentionally excluded from the generic
                // ingress wrapper. Compose its exact borrow gate here only
                // for the private executable proof.
                if (!privateUnitApplyFacts) {
                    failure = "private Ref apply lost its frozen ingress target";
                    return false;
                }
                auto* ingressEntry = llvm::Function::Create(
                    llvm::FunctionType::get(proof.mHelpers->i32Ty(),
                        {proof.mHelpers->ptrTy(), proof.mHelpers->ptrTy()},
                        false),
                    llvm::Function::ExternalLinkage,
                    "__luna_private_ref_apply_ingress_test",
                    *proof.mModule);
                if (ingressEntry->getName() !=
                        privateUnitApplyFacts->identities.back() ||
                    ingressEntry->getCallingConv() != llvm::CallingConv::C ||
                    ingressEntry->arg_size() != 2 ||
                    ingressEntry->getReturnType() != proof.mHelpers->i32Ty() ||
                    body->getCallingConv() != llvm::CallingConv::C ||
                    body->arg_size() != 2 ||
                    !body->getReturnType()->isVoidTy()) {
                    failure = "private Ref entry record differs from generated LLVM ABI";
                    return false;
                }
                auto* ingressStart = llvm::BasicBlock::Create(
                    *proof.mCtx, "entry", ingressEntry);
                auto* checkedContext = llvm::BasicBlock::Create(
                    *proof.mCtx, "context.accepted", ingressEntry);
                auto* invalidContext = llvm::BasicBlock::Create(
                    *proof.mCtx, "context.invalid", ingressEntry);
                auto* invalidHandle = llvm::BasicBlock::Create(
                    *proof.mCtx, "ref.invalid.handle", ingressEntry);
                auto* invalidTarget = llvm::BasicBlock::Create(
                    *proof.mCtx, "ref.invalid.target", ingressEntry);
                auto* unexpectedCheck = llvm::BasicBlock::Create(
                    *proof.mCtx, "check.unexpected", ingressEntry);
                auto* ingressBody = llvm::BasicBlock::Create(
                    *proof.mCtx, "ref.body", ingressEntry);
                llvm::IRBuilder<> ingressBuilder(ingressStart);
                auto contextCheck = proof.mModule->getOrInsertFunction(
                    "luna_compiler_fragment_context_check",
                    proof.mHelpers->i32Ty(), proof.mHelpers->ptrTy());
                auto* contextStatus = ingressBuilder.CreateCall(contextCheck,
                    {ingressEntry->getArg(0)});
                auto* statusType = llvm::cast<llvm::IntegerType>(
                    proof.mHelpers->i32Ty());
                auto* contextBranch = ingressBuilder.CreateSwitch(
                    contextStatus, unexpectedCheck, 2);
                contextBranch->addCase(llvm::ConstantInt::getSigned(
                    statusType,
                    LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS), checkedContext);
                contextBranch->addCase(llvm::ConstantInt::getSigned(
                    statusType,
                    LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT),
                    invalidContext);
                ingressBuilder.SetInsertPoint(invalidContext);
                ingressBuilder.CreateRet(llvm::ConstantInt::getSigned(
                    proof.mHelpers->i32Ty(),
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST));
                ingressBuilder.SetInsertPoint(checkedContext);
                auto* check =
                    proof.mHelpers->emitRuntimeFragmentRefBorrowCheck(
                        ingressBuilder, *proof.mModule,
                        ingressEntry->getArg(1), privateUnitApplyFacts->target);
                if (!check) {
                    failure = "private Ref apply lost its exact borrowed check";
                    return false;
                }
                auto* refBranch = ingressBuilder.CreateSwitch(
                    check, unexpectedCheck, 3);
                refBranch->addCase(llvm::ConstantInt::getSigned(
                    statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1), ingressBody);
                refBranch->addCase(llvm::ConstantInt::getSigned(
                    statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1),
                    invalidHandle);
                refBranch->addCase(llvm::ConstantInt::getSigned(
                    statusType,
                    LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1),
                    invalidTarget);
                ingressBuilder.SetInsertPoint(invalidHandle);
                ingressBuilder.CreateRet(llvm::ConstantInt::get(
                    proof.mHelpers->i32Ty(),
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST));
                ingressBuilder.SetInsertPoint(invalidTarget);
                ingressBuilder.CreateRet(llvm::ConstantInt::get(
                    proof.mHelpers->i32Ty(),
                    LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST));
                ingressBuilder.SetInsertPoint(unexpectedCheck);
                ingressBuilder.CreateRet(llvm::ConstantInt::get(
                    proof.mHelpers->i32Ty(),
                    LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST));
                ingressBuilder.SetInsertPoint(ingressBody);
                auto* ingressCall = ingressBuilder.CreateCall(body,
                    {ingressEntry->getArg(0), ingressEntry->getArg(1)});
                ingressCall->setCallingConv(body->getCallingConv());
                ingressBuilder.CreateRet(llvm::ConstantInt::get(
                    proof.mHelpers->i32Ty(),
                    LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST));
                const auto switchDestination = [](llvm::SwitchInst* branch,
                    int32_t status) -> llvm::BasicBlock* {
                    for (const auto choice : branch->cases())
                        if (choice.getCaseValue()->getSExtValue() == status)
                            return choice.getCaseSuccessor();
                    return nullptr;
                };
                const auto returnsStatus = [](llvm::BasicBlock* block,
                    int32_t expected) {
                    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                        block->getTerminator());
                    const auto* status = returned
                        ? llvm::dyn_cast<llvm::ConstantInt>(
                            returned->getReturnValue()) : nullptr;
                    return status && status->getSExtValue() == expected;
                };
                if (contextStatus->getParent() != ingressStart ||
                    contextBranch->getParent() != ingressStart ||
                    contextBranch->getDefaultDest() != unexpectedCheck ||
                    contextBranch->getNumCases() != 2 ||
                    switchDestination(contextBranch,
                        LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS) !=
                        checkedContext ||
                    switchDestination(contextBranch,
                        LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT) !=
                        invalidContext ||
                    checkedContext->getSinglePredecessor() != ingressStart ||
                    check->getParent() != checkedContext ||
                    refBranch->getParent() != checkedContext ||
                    refBranch->getDefaultDest() != unexpectedCheck ||
                    refBranch->getNumCases() != 3 ||
                    switchDestination(refBranch,
                        LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1) != ingressBody ||
                    switchDestination(refBranch,
                        LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1) !=
                        invalidHandle ||
                    switchDestination(refBranch,
                        LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1) !=
                        invalidTarget ||
                    ingressBody->getSinglePredecessor() != checkedContext ||
                    ingressCall->getParent() != ingressBody ||
                    !returnsStatus(invalidContext,
                        LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST) ||
                    !returnsStatus(invalidHandle,
                        LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST) ||
                    !returnsStatus(invalidTarget,
                        LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST) ||
                    !returnsStatus(unexpectedCheck,
                        LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST) ||
                    !returnsStatus(ingressBody,
                        LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)) {
                    failure = "private Ref ingress status profile lost its exact branches";
                    return false;
                }
                size_t contextCheckCalls = 0;
                size_t refCheckCalls = 0;
                size_t bodyCalls = 0;
                for (const auto& block : *ingressEntry)
                    for (const auto& instruction : block)
                        if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                                &instruction)) {
                            if (call->getCalledFunction() == body)
                                ++bodyCalls;
                            else if (call->getCalledFunction() &&
                                     call->getCalledFunction()->getName() ==
                                         "luna_compiler_fragment_context_check")
                                ++contextCheckCalls;
                            else if (call->getCalledFunction() &&
                                     call->getCalledFunction()->getName() ==
                                         "luna_runtime_fragment_ref_check_v1")
                                ++refCheckCalls;
                            else {
                                failure = "private Ref ingress has an unexpected call";
                                return false;
                            }
                        }
                if (contextCheckCalls != 1 || refCheckCalls != 1 ||
                    bodyCalls != 1) {
                    failure = "private Ref ingress lost context/ref/body call pairing";
                    return false;
                }
            }
        }
#endif
        llvm::raw_string_ostream stream(invalidIR);
        if (llvm::verifyModule(*proof.mModule, &stream)) {
            stream.flush();
            failure = "generated Ref apply LLVM IR is invalid: " + invalidIR;
            return false;
        }
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        if (executable) {
            std::optional<std::vector<uint8_t>> encodedEntry;
            if (entryRecord) {
                encodedEntry = encodePrivateRefUnitApplyEntry(
                    *privateUnitApplyFacts);
                if (!encodedEntry ||
                    !validatePrivateRuntimeFragmentRefApplyEntryRecordForTest(
                        program, function, *encodedEntry, failure))
                    return false;
            }
            *executable = proof.materializeJitModule(failure);
            if (!*executable) return false;
            if (privateOwnedResultFacts)
                (*executable)->mPrivateOwnedResultSourceFacts =
                    *privateOwnedResultFacts;
            if (entryRecord) {
                (*executable)->mPrivateRefUnitApplyEntryRecord = *encodedEntry;
                *entryRecord = std::move(*encodedEntry);
            }
            return true;
        }
#endif
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

#ifdef LUNA_PRIVATE_REF_JIT_TEST
std::shared_ptr<LunaJitModule>
CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
    moon::Module& program, FunctionDecl& function, std::string& failure,
    std::vector<uint8_t>* entryRecord) {
    std::shared_ptr<LunaJitModule> executable;
    if (!verifyPrivateRuntimeFragmentRefUnitIngress(
            program, function, failure, &executable, entryRecord))
        return {};
    return executable;
}
#endif

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
