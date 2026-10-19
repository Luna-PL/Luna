#include "CodeGenerator.h"
#include "NativeOwnedResultFacts.h"
#include "NativeRefUnitFacts.h"
#include "core/TypeLayout.h"
#include "core/TypeRelations.h"
#include "moonir/FragmentContextEffects.h"
#include "moonir/Verifier.h"
#include "runtime/RuntimeFragmentABI.h"
#include "runtime/RuntimeFragmentCompilerBridge.h"
#ifdef LUNA_PRIVATE_REF_JIT_TEST
#include "runtime/RuntimeOwnedResult.h"
#include "runtime/RuntimeFragment.h"
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
// A failed dispatch may be reported after its continuation has run. Admit an
// owned Result status clone only when the outlined continuation cannot write
// an owner into the inherited return cell or leave a live local behind.
struct PrivateOwnedResultStatusPlan {
    std::vector<moon::CleanupId> failureCleanups;
};

std::optional<PrivateOwnedResultStatusPlan> privateOwnedResultStatusCandidate(
    const moon::Module& program, const FunctionDecl& function) {
    const auto* graph = function.controlFlow.get();
    if (!graph || graph->runtimeRefApplies.size() != 1) return std::nullopt;
    moon::Verifier verifier;
    if (!verifier.verify(*graph, program)) return std::nullopt;
    std::string flowError;
    const auto flow = moon::planRuntimeRefApplyFlow(*graph, flowError);
    if (!flow) return std::nullopt;
    const auto* result = program.findType(function.returnType);
    if (!result || result->typeArgumentIds.size() != 2)
        return std::nullopt;
    std::optional<std::vector<moon::CleanupId>> failureCleanups;
    std::optional<std::vector<moon::CleanupId>> orderedFailureCleanups;
    const auto matchFailureCleanups = [&](moon::BlockId block) {
        const auto* source = graph->findBlock(block);
        const auto* active = verifier.activeCleanupsBeforeEdge(block);
        if (!source || !active ||
            (failureCleanups && *failureCleanups != *active))
            return false;
        std::unordered_set<uint32_t> roots;
        std::unordered_set<uint32_t> activeIds;
        for (const auto id : *active) {
            const auto* cleanup = graph->findCleanup(id);
            const auto* local = cleanup
                ? graph->findLocal(cleanup->place.root) : nullptr;
            if (!cleanup || !local ||
                cleanup->scope != local->scope ||
                !cleanup->place.projections.empty() ||
                cleanup->kind != moon::CleanupKind::Value ||
                cleanup->action != luna::ownership::CleanupAction::Drop ||
                !roots.insert(cleanup->place.root.value).second ||
                !activeIds.insert(id.value).second)
                return false;
        }
        std::vector<moon::CleanupId> ordered;
        std::unordered_set<uint32_t> visitedScopes;
        bool reachedRoot = false;
        for (const auto* scope = graph->findScope(source->scope); scope;
             scope = graph->findScope(scope->parent)) {
            if (!visitedScopes.insert(scope->id.value).second) return false;
            for (auto row = scope->cleanups.rbegin();
                 row != scope->cleanups.rend(); ++row)
                if (activeIds.count(row->value)) ordered.push_back(*row);
            if (scope->id == graph->rootScope) {
                reachedRoot = true;
                break;
            }
        }
        if (!reachedRoot || ordered.size() != active->size() ||
            (orderedFailureCleanups && *orderedFailureCleanups != ordered))
            return false;
        failureCleanups = *active;
        orderedFailureCleanups = std::move(ordered);
        return true;
    };
    size_t slotCount = 0;
    for (const auto& block : graph->blocks) {
        bool outlined = false;
        for (auto regionId = block.region; !regionId.empty();) {
            const auto* region = graph->findRegion(regionId);
            if (!region) return std::nullopt;
            if (region->kind == moon::RegionKind::Continuation) {
                outlined = true;
                break;
            }
            regionId = region->parent;
        }
        if (outlined && (!block.operations.empty() ||
                         block.terminator.kind != moon::TerminatorKind::Jump ||
                         !block.terminator.primary.cleanups.empty()))
            return std::nullopt;
        if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot) {
            ++slotCount;
            if (outlined || !matchFailureCleanups(block.id))
                return std::nullopt;
        }
    }
    if (slotCount != 1) return std::nullopt;
    for (const auto& edge : flow->edges)
        if (!edge.enters.empty()) {
            const auto* source = graph->findBlock(edge.source);
            if (!source || source->terminator.kind !=
                    moon::TerminatorKind::Jump ||
                !source->terminator.primary.cleanups.empty() ||
                !matchFailureCleanups(edge.source))
                return std::nullopt;
        }
    if (!orderedFailureCleanups) return std::nullopt;
    return PrivateOwnedResultStatusPlan{std::move(*orderedFailureCleanups)};
}

// A scalar Result can still leave an unrelated resource live on failure.
// Keep this probe limited to empty failure boundaries and inert outlines.
bool privateScalarResultStatusCandidate(
    const moon::Module& program, const FunctionDecl& function) {
    const auto* graph = function.controlFlow.get();
    if (!graph) return false;
    moon::Verifier verifier;
    if (!verifier.verify(*graph, program)) return false;
    std::string flowError;
    const auto flow = moon::planRuntimeRefApplyFlow(*graph, flowError);
    if (!flow) return false;
    size_t slotCount = 0;
    for (const auto& block : graph->blocks) {
        bool outlined = false;
        for (auto regionId = block.region; !regionId.empty();) {
            const auto* region = graph->findRegion(regionId);
            if (!region) return false;
            if (region->kind == moon::RegionKind::Continuation) {
                outlined = true;
                break;
            }
            regionId = region->parent;
        }
        if (outlined) {
            if (!block.operations.empty()) return false;
            if (block.terminator.kind == moon::TerminatorKind::Jump) {
                if (!block.terminator.primary.cleanups.empty()) return false;
            } else if (block.terminator.kind == moon::TerminatorKind::Return) {
                const auto* result = dynamic_cast<const moon::ResultConstructExpr*>(
                    block.terminator.operand.get());
                if (!block.terminator.exitCleanups.empty() || !result ||
                    !dynamic_cast<const moon::IntLiteralExpr*>(
                        result->payload.get()))
                    return false;
            } else return false;
        }
        if (block.terminator.kind == moon::TerminatorKind::RuntimeSlot) {
            ++slotCount;
            const auto* active = verifier.activeCleanupsBeforeEdge(block.id);
            if (outlined || !active || !active->empty()) return false;
        }
    }
    if (!slotCount) return false;
    for (const auto& edge : flow->edges)
        if (!edge.enters.empty()) {
            const auto* source = graph->findBlock(edge.source);
            const auto* active = verifier.activeCleanupsBeforeEdge(edge.source);
            if (!source || source->terminator.kind !=
                    moon::TerminatorKind::Jump ||
                !source->terminator.primary.cleanups.empty() ||
                !active || !active->empty())
                return false;
        }
    return true;
}

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
    luna::codegen::NativeRefUnitSourceFacts source;
    std::string failure;
    if (!luna::codegen::deriveNativeRefUnitSourceFacts(
            program, function, source, failure))
        return std::nullopt;
    const auto target = program.resolveRuntimeFragmentRefTarget(
        function.params.front().type);
    if (!target) return std::nullopt;
    PrivateRefUnitApplyEntryFacts facts;
    facts.target = *target;
    const auto identities = source.identities();
    std::copy(identities.begin(), identities.end(), facts.identities.begin());
    facts.identities.back() = "__luna_private_ref_apply_ingress_test";
    facts.contextEffect = source.contextEffect;
    return facts;
}

// Pointer-free, test-only candidate row. It binds the candidate C entry and
// status profile, but no Native descriptor or source export publishes it.
constexpr std::array<uint8_t, 8> PrivateRefEntryMagic =
    {'L', 'U', 'N', 'A', 'R', 'U', '1', 0};
constexpr std::array<uint32_t, 9> PrivateRefEntryConventions = {
    1, // Function declaration
    1, // One source parameter
    2, // Two machine arguments
    1, // Parent context, then Ref
    1, // Shared borrow
    1, // Unit result
    LUNA_NATIVE_ENTRY_ABI_REF_UNIT_V1,
    LUNA_NATIVE_STATUS_DOMAIN_REF_UNIT_V1,
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

bool validPrivateRefResultOutputs(
    uint32_t* tagOutput, int32_t* scalarOutput,
    luna::runtime::RuntimeOwnedResultHandle& ownerOutput) {
    if (!tagOutput || !scalarOutput || ownerOutput) return false;
    const auto tagAddress = reinterpret_cast<uintptr_t>(tagOutput);
    const auto scalarAddress = reinterpret_cast<uintptr_t>(scalarOutput);
    const auto separated = [](uintptr_t left, size_t leftSize,
                              uintptr_t right, size_t rightSize) {
        return left < right ? right - left >= leftSize
                            : left - right >= rightSize;
    };
    const auto ownerCell = ownerOutput.cell();
    const auto ownerAddress = reinterpret_cast<uintptr_t>(ownerCell);
    return (tagAddress & (alignof(uint32_t) - 1)) == 0 &&
        (scalarAddress & (alignof(int32_t) - 1)) == 0 &&
        separated(tagAddress, sizeof(uint32_t),
                  scalarAddress, sizeof(int32_t)) &&
        (!ownerCell ||
         (separated(tagAddress, sizeof(uint32_t),
                    ownerAddress, sizeof(void*)) &&
          separated(scalarAddress, sizeof(int32_t),
                    ownerAddress, sizeof(void*))));
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
    bool continuationFailure,
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
    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
        failed->getTerminator());
    const auto* returnedStatus = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(returned->getReturnValue())
        : nullptr;
    if (continuationFailure
            ? !returnedStatus || returnedStatus->getSExtValue() !=
                LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1
            : !llvm::isa<llvm::UnreachableInst>(failed->getTerminator()))
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
            } else return false;
        }
    if ((continuationFailure ? trap != nullptr : trap == nullptr) ||
        drops.size() != ownerCells.size()) return false;
    for (size_t index = 0; index < drops.size(); ++index)
        if (drops[index]->getArgOperand(0) !=
                ownerCells[ownerCells.size() - 1 - index] ||
            !drops[index]->comesBefore(
                continuationFailure ? failed->getTerminator() : trap))
            return false;
    if (accounted)
        accounted->insert(drops.begin(), drops.end());
    return true;
}

bool overrideFailureDropsOwners(
    const llvm::CallInst& overrideCall,
    const std::vector<const llvm::Value*>& ownerCells,
    bool continuationFailure,
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
    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
        failed->getTerminator());
    const auto* returnedStatus = returned
        ? llvm::dyn_cast_or_null<llvm::ConstantInt>(returned->getReturnValue())
        : nullptr;
    if (continuationFailure
            ? !returnedStatus || returnedStatus->getSExtValue() !=
                LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1
            : !llvm::isa<llvm::UnreachableInst>(failed->getTerminator()))
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
            } else return false;
        }
    if ((continuationFailure ? trap != nullptr : trap == nullptr) ||
        drops.size() != ownerCells.size()) return false;
    for (size_t index = 0; index < drops.size(); ++index)
        if (drops[index]->getArgOperand(0) !=
                ownerCells[ownerCells.size() - 1 - index] ||
            !drops[index]->comesBefore(
                continuationFailure ? failed->getTerminator() : trap))
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
    const auto entry = reinterpret_cast<LunaNativeRefUnitEntryV1>(
        const_cast<void*>(entry_));
    return entry(parentContext, borrowedRef);
}

int32_t LunaPrivateRefPairUnitLoadedEntry::call(
    const luna::runtime::RuntimeFragmentExecutionContext& parent,
    const luna::runtime::RuntimeFragmentRefHandle& first,
    const luna::runtime::RuntimeFragmentRefHandle& second) const {
    auto keepCodeAlive = lease_;
    if (!keepCodeAlive || !entry_)
        return LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST;
    const auto pinnedParent = parent;
    const auto checkedContext =
        luna_compiler_fragment_context_check(pinnedParent.opaque());
    if (checkedContext == LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT)
        return LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_CONTEXT_V1_TEST;
    if (checkedContext != LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS)
        return LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST;
    luna::runtime::RuntimeFragmentRefHandle pinnedFirst;
    luna::runtime::RuntimeFragmentRefHandle pinnedSecond;
    std::string pinError;
    // The outer handler can release caller handles before the inner Apply.
    const auto pinAndCheck = [&](const auto& source, auto& pinned) {
        if (!luna::runtime::pinRuntimeFragmentRefHandle(
                source, pinned, pinError))
            return LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST;
        const auto checked = luna_runtime_fragment_ref_check_v1(
            pinned.opaque(), slotSymbolId_.c_str(),
            slotContractId_.c_str());
        if (checked == LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1)
            return LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_HANDLE_V1_TEST;
        if (checked == LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1)
            return LUNA_PRIVATE_REF_UNIT_APPLY_INVALID_TARGET_V1_TEST;
        if (checked != LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1)
            return LUNA_PRIVATE_REF_UNIT_APPLY_UNEXPECTED_CHECK_V1_TEST;
        return LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST;
    };
    const auto firstCheck = pinAndCheck(first, pinnedFirst);
    if (firstCheck != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
        return firstCheck;
    const auto secondCheck = pinAndCheck(second, pinnedSecond);
    if (secondCheck != LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST)
        return secondCheck;
    using Entry = int32_t (*)(const void*, const void*, const void*);
    const auto entry = reinterpret_cast<Entry>(const_cast<void*>(entry_));
    return entry(pinnedParent.opaque(), pinnedFirst.opaque(),
                 pinnedSecond.opaque()) == 0
        ? LUNA_PRIVATE_REF_UNIT_APPLY_SUCCESS_V1_TEST
        : LUNA_PRIVATE_REF_UNIT_APPLY_EXECUTION_FAILURE_V1_TEST;
}

int32_t LunaPrivateRefResultLoadedEntry::call(
    const luna::runtime::RuntimeFragmentExecutionContext& parent,
    const luna::runtime::RuntimeFragmentRefHandle& borrowedRef,
    uint32_t* tagOutput, int32_t* scalarOutput,
    luna::runtime::RuntimeOwnedResultHandle& ownerOutput,
    bool failAdoptionForTest) const {
    auto keepCodeAlive = lease_;
    if (!keepCodeAlive || !entry_ || !injectionEntry_ || !drop_)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST;
    if (!validPrivateRefResultOutputs(
            tagOutput, scalarOutput, ownerOutput))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST;
    const auto pinnedParent = parent;
    const auto checkedContext =
        luna_compiler_fragment_context_check(pinnedParent.opaque());
    if (checkedContext == LUNA_COMPILER_FRAGMENT_OVERRIDE_INVALID_CONTEXT)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_CONTEXT_V1_TEST;
    if (checkedContext != LUNA_COMPILER_FRAGMENT_OVERRIDE_SUCCESS)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST;
    luna::runtime::RuntimeFragmentRefHandle pinnedReference;
    std::string pinError;
    if (!luna::runtime::pinRuntimeFragmentRefHandle(
            borrowedRef, pinnedReference, pinError))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_HANDLE_V1_TEST;
    const auto checkedRef = luna_runtime_fragment_ref_check_v1(
        pinnedReference.opaque(), refSlotSymbolId_.c_str(),
        refSlotContractId_.c_str());
    if (checkedRef == LUNA_RUNTIME_FRAGMENT_REF_INVALID_HANDLE_V1)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_HANDLE_V1_TEST;
    if (checkedRef == LUNA_RUNTIME_FRAGMENT_REF_INVALID_TARGET_V1)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_TARGET_V1_TEST;
    if (checkedRef != LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST;
    return call(pinnedParent.opaque(), pinnedReference.opaque(), tagOutput,
                scalarOutput, ownerOutput, failAdoptionForTest);
}

int32_t LunaPrivateRefResultLoadedEntry::call(
    const void* parentContext, const void* borrowedRef,
    uint32_t* tagOutput, int32_t* scalarOutput,
    luna::runtime::RuntimeOwnedResultHandle& ownerOutput,
    bool failAdoptionForTest) const {
    auto keepCodeAlive = lease_;
    if (!keepCodeAlive || !entry_ || !injectionEntry_ || !drop_)
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST;
    if (!validPrivateRefResultOutputs(
            tagOutput, scalarOutput, ownerOutput))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST;
    using InjectionEntry = int32_t (*)(
        const void*, void*, uint32_t*, int32_t*, void**,
        const void*, uint32_t);
    const auto entry = reinterpret_cast<LunaNativeRefResultOwnerEntryV1>(
        const_cast<void*>(entry_));
    const auto injectionEntry = reinterpret_cast<InjectionEntry>(
        const_cast<void*>(injectionEntry_));
    luna::runtime::RuntimeOwnedResultHandle stagedOwner;
    std::string adoptionError;
    if (!stagedOwner.prepareEmptyCell(adoptionError))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_ADOPTION_FAILURE_V1_TEST;
    luna::runtime::RuntimeOwnedResultCodeLease codeLease;
    if (!codeLease.prepare(keepCodeAlive, adoptionError))
        return LUNA_PRIVATE_REF_RESULT_TRANSFER_ADOPTION_FAILURE_V1_TEST;
    const int32_t status = failAdoptionForTest
        ? injectionEntry(parentContext, const_cast<void*>(borrowedRef),
            tagOutput, scalarOutput, stagedOwner.cell(),
            codeLease.opaque(), 1u)
        : entry(parentContext, borrowedRef, tagOutput, scalarOutput,
            stagedOwner.cell(), codeLease.opaque());
    if (status == LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST &&
        stagedOwner)
        ownerOutput = std::move(stagedOwner);
    return status;
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
    if (!executable || !executable->mPrivateOwnedResultEntryProof ||
        !luna::codegen::deriveNativeOwnedResultSourceFacts(
            program, function, facts, failure) ||
        !(facts == executable->mPrivateOwnedResultEntryProof->source)) {
        failure = "private Ref Result entry is not bound to these frozen source facts";
        return {};
    }
    const auto& proof = *executable->mPrivateOwnedResultEntryProof;
    if (proof.entryAbi != LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1 ||
        proof.statusDomain !=
            luna::codegen::NativeOwnedResultPrivateStatusDomainV1 ||
        proof.entryLinkageName !=
            "__luna_private_ref_apply_entry_v1_test" ||
        proof.injectionLinkageName !=
            "__luna_private_ref_apply_host_transfer_test" ||
        proof.dropLinkageName !=
            "__luna_private_ref_apply_drop_test") {
        failure = "private Ref Result entry lost its verified profile or linkage";
        return {};
    }
    const void* entry = executable->lookup(
        proof.entryLinkageName, failure);
    if (!entry) return {};
    const void* injectionEntry = executable->lookup(
        proof.injectionLinkageName, failure);
    if (!injectionEntry) return {};
    const void* drop = executable->lookup(proof.dropLinkageName, failure);
    if (!drop) return {};
    auto loaded = std::make_unique<LunaPrivateRefResultLoadedEntry>();
    loaded->lease_ = std::move(executable);
    loaded->entry_ = entry;
    loaded->injectionEntry_ = injectionEntry;
    loaded->drop_ = drop;
    loaded->refSlotSymbolId_ = proof.source.refSlotSymbolId;
    loaded->refSlotContractId_ = proof.source.refSlotContractId;
    failure.clear();
    return loaded;
}
#endif

bool CodeGenerator::verifyPrivateRuntimeFragmentRefUnitIngress(
    moon::Module& program, FunctionDecl& function, std::string& failure,
    const luna::driver::NativeProofSpec* nativeProof,
    const std::vector<luna::driver::NativeExportSpec>* nativeV1Exports,
    CodeGenerator* preparedModule,
    std::shared_ptr<LunaJitModule>* preparedExecutable
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    , std::shared_ptr<LunaJitModule>* executable,
    std::vector<uint8_t>* entryRecord
#endif
) {
    // Lower in isolation first. Only a proof-bound Native library source may
    // transfer the verified module into ordinary codegen; the test target can
    // instead materialize its own private JIT lease.
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (preparedModule && executable) {
        failure = "Native Ref/unit module preparation cannot retain a JIT lease";
        return false;
    }
#endif
    if (preparedModule && preparedExecutable) {
        failure = "Native Ref/unit preparation cannot retain two code outputs";
        return false;
    }
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (preparedExecutable && executable) {
        failure = "Native Ref/unit preparation cannot mix JIT entry modes";
        return false;
    }
#endif
    if (preparedExecutable && *preparedExecutable) {
        failure = "Native Ref/unit preparation output already owns JIT code";
        return false;
    }
    if (preparedModule && !preparedModule->mModule) {
        failure = "Native Ref/unit module preparation has no destination";
        return false;
    }
    CodeGenerator proof("private.ref.ingress.proof");
    proof.mProgram = &program;
    proof.mTypeMaterializer =
        std::make_unique<moon::TypeMaterializer>(program);
    if (function.generatedSymbolName.empty()) {
        failure = "function has no generated symbol";
        return false;
    }
    // Native library preparation can now admit one source Ref candidate.
    // Validate its sealed CFG independently before codegen traverses it.
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
    // may execute scalar Results, narrow resource variants, and a two-Ref
    // branch selector behind private wrappers.
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
    std::optional<luna::codegen::NativeOwnedResultEntryProof>
        privateOwnedResultEntryProof;
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
    const bool privateOwnedPair = !privateRefApply &&
        !function.requiresFragmentContext && function.params.size() == 2 &&
        std::all_of(function.params.begin(), function.params.end(),
            [&program](const auto& parameter) {
                const auto* type = program.findType(parameter.type);
                return type && type->kind == TypeKind::RuntimeFragmentRef &&
                    parameter.relation == luna::ownership::Relation::Owned;
            });
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    const bool privateRefApplySelector = executable && privateRefApply &&
        function.params.size() == 2 &&
        [&program, &function] {
            const auto& reference = function.params.front();
            const auto& selector = function.params.back();
            const auto* refType = program.findType(reference.type);
            const auto* selectorType = program.findType(selector.type);
            return refType && refType->kind == TypeKind::RuntimeFragmentRef &&
                reference.relation == luna::ownership::Relation::SharedBorrow &&
                selectorType && selectorType->kind == TypeKind::Bool &&
                selector.relation == luna::ownership::Relation::Owned &&
                selector.usage == luna::ownership::Usage::Copy;
        }();
    // A disposable test entry may add one Copy selector to the two owned
    // Refs. The ordinary proof and publication paths do not accept it.
    const bool privateOwnedPairSelector = executable && !privateRefApply &&
        !function.requiresFragmentContext && function.params.size() == 3 &&
        std::all_of(function.params.begin(), function.params.begin() + 2,
            [&program](const auto& parameter) {
                const auto* type = program.findType(parameter.type);
                return type && type->kind == TypeKind::RuntimeFragmentRef &&
                    parameter.relation == luna::ownership::Relation::Owned;
            }) &&
        [&program, &function] {
            const auto& selector = function.params[2];
            const auto* type = program.findType(selector.type);
            return type && type->kind == TypeKind::Bool &&
                selector.relation == luna::ownership::Relation::Owned &&
                selector.usage == luna::ownership::Usage::Copy;
        }();
#else
    const bool privateRefApplySelector = false;
    const bool privateOwnedPairSelector = false;
#endif
    const bool privateOwnedRefProbe =
        privateOwnedPair || privateOwnedPairSelector;
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    const bool privateSelectorScalarResult = privateOwnedPairSelector &&
        returnType && returnType->kind == TypeKind::I32;
#else
    const bool privateSelectorScalarResult = false;
#endif
    if (!returnType || (returnType->kind != TypeKind::Unit
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        && !privateResultJit
#endif
        && !privateSelectorScalarResult)) {
        failure = "private Ref unit ingress proof requires a unit return";
        return false;
    }
    if (!privateRefApply && function.params.size() != 1 &&
        !privateOwnedRefProbe) {
        failure = "private Ref unit ingress proof requires one Ref or two owned Refs";
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
        const auto borrowedCount = function.params.size() -
            static_cast<size_t>(privateRefApplySelector);
        for (size_t index = 0; index < borrowedCount; ++index) {
            const auto& parameter = function.params[index];
            const auto* type = program.findType(parameter.type);
            if (!type || type->kind != TypeKind::RuntimeFragmentRef ||
                parameter.relation != luna::ownership::Relation::SharedBorrow) {
                failure = "private Ref apply proof requires borrowed Ref parameters";
                return false;
            }
        }
    }
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (executable && !privateRefApply && !privateOwnedRefProbe) {
        failure = "private Ref JIT test requires Apply or two owned Refs";
        return false;
    }
#endif
    if (!matchesPrivateRefContextEffect(program, function)) {
        failure = "Ref entry context effect differs from the sealed CFG fixed point";
        return false;
    }
    if (privateRefApply && returnType->kind == TypeKind::Unit &&
        function.params.size() == 1) {
        luna::codegen::NativeRefUnitSourceFacts source;
        if (!luna::codegen::deriveNativeRefUnitSourceFacts(
                program, function, source, failure))
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
    parameters.insert(parameters.end(), function.params.size(),
                      proof.mHelpers->ptrTy());
    if (privateOwnedPairSelector || privateRefApplySelector)
        parameters.back() = proof.mHelpers->boolTy();
    const bool privateUnitStatusBody = privateRefApply &&
        returnType->kind == TypeKind::Unit &&
        function.params.size() <= 2;
    auto* body = llvm::Function::Create(
        llvm::FunctionType::get(
#ifdef LUNA_PRIVATE_REF_JIT_TEST
            privateResultJit
                ? proof.mHelpers->toLLVMType(
                    proof.resolveType(function.returnType))
                :
#endif
                  ((privateUnitStatusBody || privateSelectorScalarResult)
                      ? proof.mHelpers->i32Ty()
                      : proof.mHelpers->voidTy()),
            parameters, false),
        llvm::Function::InternalLinkage,
        function.generatedSymbolName, *proof.mModule);
    proof.mFunctions[function.generatedSymbolName] = body;
    proof.mPrivateRefApplyEnabled = privateRefApply;
    proof.mPrivateRefUnitStatusBody = privateUnitStatusBody;
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
                for (auto ancestor = region->parent;
                     ancestor != function.controlFlow->rootRegion;) {
                    const auto* enclosing =
                        function.controlFlow->findRegion(ancestor);
                    if (!enclosing ||
                        enclosing->kind != moon::RegionKind::Lexical ||
                        enclosing->parent.empty()) {
                        failure = "outer Ref apply has non-lexical ancestors";
                        return false;
                    }
                    ancestor = enclosing->parent;
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
                    *derivation, parents, privateUnitStatusBody,
                    &accountedContextDrops)) {
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
                    *dispatch, failedOwners, privateUnitStatusBody,
                    &accountedContextDrops)) {
                failure = "private Ref apply dispatch failure lost its owner stack";
                return false;
            }
        }
        if (privateUnitStatusBody) {
            size_t succeeded = 0;
            size_t failed = 0;
            for (const auto& block : *body) {
                for (const auto& instruction : block)
                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction); call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() == "llvm.trap") {
                        failure = "private Ref unit status body retained a trap";
                        return false;
                    }
                const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                    block.getTerminator());
                if (!returned) continue;
                const auto* status = llvm::dyn_cast_or_null<llvm::ConstantInt>(
                    returned->getReturnValue());
                if (!status) {
                    failure = "private Ref unit body returned a dynamic status";
                    return false;
                }
                if (status->isZero()) ++succeeded;
                else if (status->getSExtValue() ==
                        LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1 &&
                    (block.getName().starts_with("ref.apply.failed") ||
                     block.getName().starts_with("runtime.slot.failed")))
                    ++failed;
                else {
                    failure = "private Ref unit body returned an unproved status";
                    return false;
                }
            }
            if (!succeeded || failed != bindings.size() + slotSites.size()) {
                failure = "private Ref unit body lost a recoverable failure exit";
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
                            *derivation, parents, true,
                            &accountedContextDrops)) {
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
                            *dispatch, owners,
                            current.function != body || privateUnitStatusBody,
                            &accountedContextDrops)) {
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
            std::optional<PrivateOwnedResultStatusPlan> ownedStatusPlan;
            if (privateErrResourceResultJit && function.params.size() == 1)
                ownedStatusPlan = privateOwnedResultStatusCandidate(
                    program, function);
            const bool scalarStatusCandidate = privateScalarResultJit &&
                function.params.size() == 1 &&
                privateScalarResultStatusCandidate(program, function);
            if (function.params.size() == 1 &&
                (scalarStatusCandidate || ownedStatusPlan)) {
                // The bounded owned Result clone releases its proven local
                // owners when derivation or dispatch reports failure.
                // A trailing cell separates failure from the Result tag.
                std::vector<llvm::Type*> statusParameters(
                    body->getFunctionType()->params().begin(),
                    body->getFunctionType()->params().end());
                statusParameters.push_back(proof.mHelpers->ptrTy());
                auto* statusBody = llvm::Function::Create(
                    llvm::FunctionType::get(body->getReturnType(),
                                            statusParameters, false),
                    llvm::Function::InternalLinkage,
                    "__luna_private_ref_result_status_body_test",
                    *proof.mModule);
                proof.mFunctions[function.generatedSymbolName] = statusBody;
                proof.mPrivateRefResultStatusBody = true;
                proof.mPrivateRefResultFailureCleanups = ownedStatusPlan
                    ? ownedStatusPlan->failureCleanups
                    : std::vector<moon::CleanupId>{};
                proof.generateFunctionBody(&function);
                proof.mPrivateRefResultStatusBody = false;
                proof.mPrivateRefResultFailureCleanups.clear();
                proof.mFunctions[function.generatedSymbolName] = body;
                if (!proof.mErrors.empty() || statusBody->empty() ||
                    statusBody->arg_size() != 3) {
                    failure = proof.mErrors.empty()
                        ? "private Ref Result status body lost its source signature"
                        : proof.mErrors.front().message;
                    return false;
                }
                size_t derivationFailures = 0;
                size_t dispatchFailures = 0;
                size_t statusInitializations = 0;
                const auto* statusArgument = statusBody->getArg(2);
                for (const auto& block : *statusBody) {
                    const bool derivationFailure =
                        block.getName().starts_with("ref.apply.failed");
                    const bool dispatchFailure =
                        block.getName().starts_with("runtime.slot.failed");
                    for (const auto& instruction : block) {
                        const auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        if (call && call->getCalledFunction() &&
                            call->getCalledFunction()->getName() == "llvm.trap") {
                            failure = "private Ref Result status body retained a trap";
                            return false;
                        }
                        const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                            &instruction);
                        if (!store || store->getPointerOperand() !=
                                statusArgument)
                            continue;
                        const auto* value = llvm::dyn_cast<llvm::ConstantInt>(
                            store->getValueOperand());
                        if (value && value->isZero() &&
                            &block == &statusBody->getEntryBlock())
                            ++statusInitializations;
                        else if (!value || value->getSExtValue() !=
                                LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1 ||
                                 (!derivationFailure && !dispatchFailure)) {
                            failure = "private Ref Result body wrote an unproved status";
                            return false;
                        }
                    }
                    if (!derivationFailure && !dispatchFailure) continue;
                    const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                        block.getTerminator());
                    const auto* nullResult = returned
                        ? llvm::dyn_cast_or_null<llvm::Constant>(
                            returned->getReturnValue())
                        : nullptr;
                    const llvm::StoreInst* failureStore = nullptr;
                    for (const auto& instruction : block) {
                        const auto* store = llvm::dyn_cast<llvm::StoreInst>(
                            &instruction);
                        if (!store || store->getPointerOperand() !=
                                statusArgument)
                            continue;
                        const auto* value = llvm::dyn_cast<llvm::ConstantInt>(
                            store->getValueOperand());
                        if (failureStore || !value || value->getSExtValue() !=
                                LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1) {
                            failure = "private Ref Result failure returned an unproved status";
                            return false;
                        }
                        failureStore = store;
                    }
                    if (!nullResult || !nullResult->isNullValue() ||
                        !failureStore) {
                        failure = "private Ref Result failure lost its status return";
                        return false;
                    }
                    if (derivationFailure) ++derivationFailures;
                    else ++dispatchFailures;
                }
                if (statusInitializations != 1 ||
                    derivationFailures != bindings.size() ||
                    dispatchFailures != slotSites.size()) {
                    failure = "private Ref Result status body lost a failure exit";
                    return false;
                }
                std::vector<llvm::Function*> statusGraph{statusBody};
                std::unordered_set<llvm::Function*> statusVisited;
                size_t statusDispatches = 0;
                for (size_t index = 0; index < statusGraph.size(); ++index) {
                    auto* current = statusGraph[index];
                    if (!statusVisited.insert(current).second) {
                        failure = "private Ref Result status graph reused a callback";
                        return false;
                    }
                    for (const auto& block : *current) {
                        for (const auto& instruction : block) {
                            const auto* call = llvm::dyn_cast<llvm::CallInst>(
                                &instruction);
                            if (!call || !call->getCalledFunction()) continue;
                            const auto name = call->getCalledFunction()->getName();
                            if (name == "llvm.trap") {
                                failure = "private Ref Result status graph retained a trap";
                                return false;
                            }
                            if (name != "luna_runtime_fragment_dispatch_v1")
                                continue;
                            ++statusDispatches;
                            auto* callback = llvm::dyn_cast<llvm::Function>(
                                call->getArgOperand(7)->stripPointerCasts());
                            if (!callback || !callback->hasInternalLinkage() ||
                                !callback->getName().starts_with(
                                    statusBody->getName().str() +
                                    ".runtime_slot.") ||
                                callback->arg_size() != 1 ||
                                callback->getReturnType() !=
                                    proof.mHelpers->i32Ty()) {
                                failure = "private Ref Result status dispatch lost its callback";
                                return false;
                            }
                            statusGraph.push_back(callback);
                        }
                        if (current == statusBody ||
                            (!block.getName().starts_with("ref.apply.failed") &&
                             !block.getName().starts_with("runtime.slot.failed")))
                            continue;
                        const auto* returned = llvm::dyn_cast<llvm::ReturnInst>(
                            block.getTerminator());
                        const auto* value = returned
                            ? llvm::dyn_cast_or_null<llvm::ConstantInt>(
                                returned->getReturnValue())
                            : nullptr;
                        if (!value || value->getSExtValue() !=
                                LUNA_RUNTIME_FRAGMENT_CONTINUATION_FAILED_V1) {
                            failure = "private Ref Result outlined failure lost its status";
                            return false;
                        }
                    }
                }
                if (statusDispatches != connectedDispatches ||
                    statusGraph.size() != pending.size()) {
                    failure = "private Ref Result status graph changed its Slot topology";
                    return false;
                }
                auto* statusProbe = llvm::Function::Create(
                    llvm::FunctionType::get(proof.mHelpers->i32Ty(),
                        {proof.mHelpers->ptrTy(), proof.mHelpers->ptrTy()},
                        false),
                    llvm::Function::ExternalLinkage,
                    "__luna_private_ref_result_status_failure_probe_test",
                    *proof.mModule);
                llvm::IRBuilder<> statusBuilder(llvm::BasicBlock::Create(
                    *proof.mCtx, "entry", statusProbe));
                auto* statusCell = statusBuilder.CreateAlloca(
                    proof.mHelpers->i32Ty());
                statusBuilder.CreateCall(statusBody,
                    {statusProbe->getArg(0), statusProbe->getArg(1),
                     statusCell});
                statusBuilder.CreateRet(statusBuilder.CreateLoad(
                    proof.mHelpers->i32Ty(), statusCell));
            }
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
            } else if (privateUnitStatusBody) {
                builder.CreateRet(returned);
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
                std::vector<luna::codegen::NativeOwnedResultCleanupNode>
                    cleanupNodes;
                std::vector<luna::codegen::NativeOwnedResultCleanupStep>
                    cleanupSteps;
                const auto collectDropCalls = [&](const auto& self,
                    const moon::TypeRecord* type, const TypePtr& resolved,
                    uint32_t parentIndex, uint64_t fieldOffset) -> bool {
                    if (!resolved || resolved->kind != TypeKind::Struct ||
                        resolved->fields.size() != type->fields.size())
                        return false;
                    const auto* frozen = program.findDeclaration(type->dropGlue);
                    if (!frozen) return false;
                    const auto nodeIndex = static_cast<uint32_t>(
                        cleanupNodes.size());
                    cleanupNodes.push_back({frozen->linkageName,
                        parentIndex, fieldOffset, typeSize(resolved),
                        typeAlignment(resolved)});
                    cleanupSteps.push_back({
                        luna::codegen::NativeOwnedResultCleanupStepKind::Drop,
                        nodeIndex, 0, 0});
                    expectedDropCalls.push_back(frozen->linkageName);
                    for (size_t index = 0; index < type->fields.size(); ++index) {
                        const auto& field = type->fields[index];
                        if (resolved->fields[index].name != field.name ||
                            luna::types::typeId(
                                resolved->fields[index].type) != field.type)
                            return false;
                        const auto* nested = program.findType(field.type);
                        if (!nested) return false;
                        const auto offset = luna::layout::productFieldOffset(
                            resolved, index);
                        if (nested->kind == TypeKind::Struct) {
                            cleanupSteps.push_back({
                                luna::codegen::NativeOwnedResultCleanupStepKind::ReadOwner,
                                nodeIndex, offset,
                                static_cast<uint32_t>(cleanupNodes.size())});
                            if (!self(self, nested,
                                    resolved->fields[index].type,
                                    nodeIndex, offset))
                                return false;
                        } else if (nested->kind == TypeKind::I32) {
                            cleanupSteps.push_back({
                                luna::codegen::NativeOwnedResultCleanupStepKind::ReadI32,
                                nodeIndex, offset, 0});
                        } else {
                            return false;
                        }
                    }
                    cleanupSteps.push_back({
                        luna::codegen::NativeOwnedResultCleanupStepKind::Deallocate,
                        nodeIndex, 0, 0});
                    expectedDropCalls.push_back("rt_dealloc");
                    return true;
                };
                if (!collectDropCalls(collectDropCalls, resourcePayload,
                        resourceType, 0, 0)) {
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
                const luna::codegen::NativeOwnedResultTransferStatuses
                    transferStatuses = {
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_SUCCESS_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_RESOURCE_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INJECTED_FAILURE_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_CONTEXT_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_HANDLE_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_TARGET_V1_TEST,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_UNEXPECTED_CHECK_V1_TEST};
                dropBuilder.SetInsertPoint(cleanupEnd);
                dropBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
                if (privateOwnedResultFacts &&
                    !luna::codegen::verifyNativeOwnedResultDropClosedGraph(
                        *proof.mModule, dropEntry->getName().str(),
                        transfer->getName().str(), *privateOwnedResultFacts,
                        expectedDropCalls, cleanupNodes, cleanupSteps,
                        transferStatuses,
                        failure))
                    return false;
                if (privateOwnedResultFacts) {
                    // The private C host boundary owns the complete return
                    // transaction. The loader supplies a live code lease and
                    // a stable, empty Runtime owner cell.
                    auto* hostEntry = llvm::Function::Create(
                        llvm::FunctionType::get(i32Ty,
                            {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, i32Ty},
                            false),
                        llvm::Function::ExternalLinkage,
                        "__luna_private_ref_apply_host_transfer_test",
                        *proof.mModule);
                    if (hostEntry->getCallingConv() != llvm::CallingConv::C ||
                        hostEntry->arg_size() != 7 ||
                        hostEntry->getReturnType() != i32Ty) {
                        failure = "private Ref Result host transfer lost its C contract";
                        return false;
                    }
                    auto* hostStart = llvm::BasicBlock::Create(
                        *proof.mCtx, "entry", hostEntry);
                    auto* hostCheckOwner = llvm::BasicBlock::Create(
                        *proof.mCtx, "check.owner", hostEntry);
                    auto* hostCheckLease = llvm::BasicBlock::Create(
                        *proof.mCtx, "check.lease", hostEntry);
                    auto* hostTransfer = llvm::BasicBlock::Create(
                        *proof.mCtx, "transfer", hostEntry);
                    auto* hostTransferred = llvm::BasicBlock::Create(
                        *proof.mCtx, "transferred", hostEntry);
                    auto* hostScalar = llvm::BasicBlock::Create(
                        *proof.mCtx, "scalar", hostEntry);
                    auto* hostScalarCommit = llvm::BasicBlock::Create(
                        *proof.mCtx, "scalar.commit", hostEntry);
                    auto* hostResource = llvm::BasicBlock::Create(
                        *proof.mCtx, "resource", hostEntry);
                    auto* hostAdopt = llvm::BasicBlock::Create(
                        *proof.mCtx, "adopt", hostEntry);
                    auto* hostAdoptCheck = llvm::BasicBlock::Create(
                        *proof.mCtx, "adopt.check", hostEntry);
                    auto* hostResourceCommit = llvm::BasicBlock::Create(
                        *proof.mCtx, "resource.commit", hostEntry);
                    auto* hostInvalidResourceCheck = llvm::BasicBlock::Create(
                        *proof.mCtx, "resource.invalid.check", hostEntry);
                    auto* hostInvalidResourceDrop = llvm::BasicBlock::Create(
                        *proof.mCtx, "resource.invalid.drop", hostEntry);
                    auto* hostAdoptionDrop = llvm::BasicBlock::Create(
                        *proof.mCtx, "adoption.drop", hostEntry);
                    auto* hostInvalidOutput = llvm::BasicBlock::Create(
                        *proof.mCtx, "output.invalid", hostEntry);
                    auto* hostInvalidLease = llvm::BasicBlock::Create(
                        *proof.mCtx, "lease.invalid", hostEntry);
                    auto* hostInvalidResource = llvm::BasicBlock::Create(
                        *proof.mCtx, "resource.invalid", hostEntry);
                    llvm::IRBuilder<> hostBuilder(hostStart);
                    auto hostArg = hostEntry->arg_begin();
                    auto* hostParent = &*hostArg++;
                    auto* hostRef = &*hostArg++;
                    auto* hostTagOut = &*hostArg++;
                    auto* hostScalarOut = &*hostArg++;
                    auto* hostOwnerOut = &*hostArg++;
                    auto* hostLease = &*hostArg++;
                    auto* hostFailAdoption = &*hostArg;
                    auto* localTag = hostBuilder.CreateAlloca(i32Ty);
                    auto* localScalar = hostBuilder.CreateAlloca(i32Ty);
                    auto* localOwner = hostBuilder.CreateAlloca(ptrTy);
                    const auto hostAligned = [&](llvm::Value* pointer,
                                                 size_t alignment) {
                        auto* address = hostBuilder.CreatePtrToInt(
                            pointer, proof.mHelpers->sizeTy());
                        return hostBuilder.CreateICmpEQ(
                            hostBuilder.CreateAnd(address,
                                llvm::ConstantInt::get(
                                    proof.mHelpers->sizeTy(), alignment - 1)),
                            llvm::ConstantInt::get(proof.mHelpers->sizeTy(), 0));
                    };
                    const auto hostDisjoint = [&](llvm::Value* left,
                                                   size_t leftSize,
                                                   llvm::Value* right,
                                                   size_t rightSize) {
                        auto* leftAddress = hostBuilder.CreatePtrToInt(
                            left, proof.mHelpers->sizeTy());
                        auto* rightAddress = hostBuilder.CreatePtrToInt(
                            right, proof.mHelpers->sizeTy());
                        auto* leftBeforeRight = hostBuilder.CreateICmpULT(
                            leftAddress, rightAddress);
                        auto* gap = hostBuilder.CreateSelect(leftBeforeRight,
                            hostBuilder.CreateSub(rightAddress, leftAddress),
                            hostBuilder.CreateSub(leftAddress, rightAddress));
                        return hostBuilder.CreateSelect(leftBeforeRight,
                            hostBuilder.CreateICmpUGE(gap,
                                llvm::ConstantInt::get(
                                    proof.mHelpers->sizeTy(), leftSize)),
                            hostBuilder.CreateICmpUGE(gap,
                                llvm::ConstantInt::get(
                                    proof.mHelpers->sizeTy(), rightSize)));
                    };
                    auto* outputsValid = hostBuilder.CreateAnd(
                        hostBuilder.CreateIsNotNull(hostTagOut),
                        hostBuilder.CreateAnd(
                            hostBuilder.CreateIsNotNull(hostScalarOut),
                            hostBuilder.CreateIsNotNull(hostOwnerOut)));
                    outputsValid = hostBuilder.CreateAnd(outputsValid,
                        hostBuilder.CreateAnd(
                            hostAligned(hostTagOut, alignof(uint32_t)),
                            hostBuilder.CreateAnd(
                                hostAligned(hostScalarOut, alignof(int32_t)),
                                hostAligned(hostOwnerOut, alignof(void*)))));
                    outputsValid = hostBuilder.CreateAnd(outputsValid,
                        hostBuilder.CreateAnd(
                            hostDisjoint(hostTagOut, sizeof(uint32_t),
                                         hostScalarOut, sizeof(int32_t)),
                            hostBuilder.CreateAnd(
                                hostDisjoint(hostTagOut, sizeof(uint32_t),
                                             hostOwnerOut, sizeof(void*)),
                                hostDisjoint(hostScalarOut, sizeof(int32_t),
                                             hostOwnerOut, sizeof(void*)))));
                    hostBuilder.CreateCondBr(outputsValid, hostCheckOwner,
                                             hostInvalidOutput);
                    hostBuilder.SetInsertPoint(hostCheckOwner);
                    hostBuilder.CreateCondBr(hostBuilder.CreateIsNull(
                        hostBuilder.CreateLoad(ptrTy, hostOwnerOut)),
                        hostCheckLease, hostInvalidOutput);
                    hostBuilder.SetInsertPoint(hostCheckLease);
                    auto leaseCheck = proof.mModule->getOrInsertFunction(
                        "luna_runtime_owned_result_lease_check_v1", i32Ty,
                        ptrTy);
                    auto* leaseStatus = hostBuilder.CreateCall(leaseCheck,
                        {hostLease});
                    hostBuilder.CreateCondBr(hostBuilder.CreateICmpEQ(
                        leaseStatus, llvm::ConstantInt::get(i32Ty,
                            LUNA_RUNTIME_OWNED_RESULT_LEASE_LIVE_V1)),
                        hostTransfer, hostInvalidLease);
                    hostBuilder.SetInsertPoint(hostInvalidOutput);
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_OUTPUT_V1_TEST));
                    hostBuilder.SetInsertPoint(hostInvalidLease);
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_LEASE_V1_TEST));
                    hostBuilder.SetInsertPoint(hostTransfer);
                    hostBuilder.CreateStore(
                        llvm::Constant::getNullValue(ptrTy), localOwner);
                    auto* transferredStatus = hostBuilder.CreateCall(transfer,
                        {hostParent, hostRef, localTag, localScalar, localOwner,
                         llvm::ConstantInt::get(i32Ty, 0)});
                    auto* hostTransferFailed = llvm::BasicBlock::Create(
                        *proof.mCtx, "transfer.failed", hostEntry);
                    hostBuilder.CreateCondBr(hostBuilder.CreateICmpEQ(
                        transferredStatus, llvm::ConstantInt::get(i32Ty, 0)),
                        hostTransferred, hostTransferFailed);
                    hostBuilder.SetInsertPoint(hostTransferFailed);
                    hostBuilder.CreateRet(transferredStatus);
                    hostBuilder.SetInsertPoint(hostTransferred);
                    auto* returnedTag = hostBuilder.CreateLoad(i32Ty, localTag);
                    auto* returnedOwner = hostBuilder.CreateLoad(ptrTy, localOwner);
                    hostBuilder.CreateCondBr(hostBuilder.CreateICmpEQ(
                        returnedTag, llvm::ConstantInt::get(i32Ty, 0)),
                        hostResource, hostScalar);
                    hostBuilder.SetInsertPoint(hostScalar);
                    hostBuilder.CreateCondBr(hostBuilder.CreateAnd(
                        hostBuilder.CreateICmpEQ(returnedTag,
                            llvm::ConstantInt::get(i32Ty, 1)),
                        hostBuilder.CreateIsNull(returnedOwner)),
                        hostScalarCommit, hostInvalidResourceCheck);
                    hostBuilder.SetInsertPoint(hostScalarCommit);
                    hostBuilder.CreateStore(
                        hostBuilder.CreateLoad(i32Ty, localScalar), hostScalarOut);
                    hostBuilder.CreateStore(returnedTag, hostTagOut);
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
                    hostBuilder.SetInsertPoint(hostResource);
                    hostBuilder.CreateCondBr(
                        hostBuilder.CreateIsNotNull(returnedOwner),
                        hostAdopt, hostInvalidResource);
                    hostBuilder.SetInsertPoint(hostAdopt);
                    hostBuilder.CreateCondBr(hostBuilder.CreateICmpEQ(
                        hostFailAdoption, llvm::ConstantInt::get(i32Ty, 0)),
                        hostAdoptCheck, hostAdoptionDrop);
                    auto adoption = proof.mModule->getOrInsertFunction(
                        "luna_runtime_owned_result_adopt_v1", i32Ty,
                        ptrTy, ptrTy, ptrTy, ptrTy);
                    hostBuilder.SetInsertPoint(hostAdoptCheck);
                    auto* adoptStatus = hostBuilder.CreateCall(adoption,
                        {returnedOwner, dropEntry, hostLease, hostOwnerOut});
                    hostBuilder.CreateCondBr(hostBuilder.CreateICmpEQ(
                        adoptStatus, llvm::ConstantInt::get(i32Ty,
                            LUNA_RUNTIME_OWNED_RESULT_ADOPT_SUCCESS_V1)),
                        hostResourceCommit, hostAdoptionDrop);
                    hostBuilder.SetInsertPoint(hostResourceCommit);
                    hostBuilder.CreateStore(returnedTag, hostTagOut);
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty, 0));
                    hostBuilder.SetInsertPoint(hostInvalidResourceCheck);
                    hostBuilder.CreateCondBr(
                        hostBuilder.CreateIsNotNull(returnedOwner),
                        hostInvalidResourceDrop, hostInvalidResource);
                    hostBuilder.SetInsertPoint(hostInvalidResourceDrop);
                    hostBuilder.CreateCall(dropEntry, {localOwner});
                    hostBuilder.CreateBr(hostInvalidResource);
                    hostBuilder.SetInsertPoint(hostInvalidResource);
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_INVALID_RESOURCE_V1_TEST));
                    hostBuilder.SetInsertPoint(hostAdoptionDrop);
                    hostBuilder.CreateCall(dropEntry, {localOwner});
                    hostBuilder.CreateRet(llvm::ConstantInt::get(i32Ty,
                        LUNA_PRIVATE_REF_RESULT_TRANSFER_ADOPTION_FAILURE_V1_TEST));
                    size_t hostTransferCalls = 0;
                    size_t hostAdoptCalls = 0;
                    size_t hostDropCalls = 0;
                    size_t hostLeaseChecks = 0;
                    for (const auto& block : *hostEntry)
                        for (const auto& instruction : block)
                            if (const auto* call = llvm::dyn_cast<llvm::CallInst>(
                                    &instruction)) {
                                const auto* callee = call->getCalledFunction();
                                if (callee == transfer) ++hostTransferCalls;
                                else if (callee == dropEntry) ++hostDropCalls;
                                else if (callee && callee->getName() ==
                                    "luna_runtime_owned_result_adopt_v1")
                                    ++hostAdoptCalls;
                                else if (callee && callee->getName() ==
                                    "luna_runtime_owned_result_lease_check_v1")
                                    ++hostLeaseChecks;
                                else {
                                    failure = "private Ref Result host transfer has an unexpected call";
                                    return false;
                                }
                            }
                    if (hostTransferCalls != 1 || hostAdoptCalls != 1 ||
                        hostDropCalls != 2 || hostLeaseChecks != 1 ||
                        leaseStatus->getParent() != hostCheckLease) {
                        failure = "private Ref Result host transfer lost its commit or cleanup path";
                        return false;
                    }
                    if (!luna::codegen::verifyNativeOwnedResultHostClosedGraph(
                            *proof.mModule, hostEntry->getName().str(),
                            transfer->getName().str(),
                            dropEntry->getName().str(), failure))
                        return false;
                    // The candidate callable profile has no test injection
                    // argument. Its one call fixes that argument to zero.
                    auto* candidateEntry = llvm::Function::Create(
                        llvm::FunctionType::get(i32Ty,
                            {ptrTy, ptrTy, ptrTy, ptrTy, ptrTy, ptrTy},
                            false),
                        llvm::Function::ExternalLinkage,
                        "__luna_private_ref_apply_entry_v1_test",
                        *proof.mModule);
                    if (candidateEntry->getCallingConv() !=
                            llvm::CallingConv::C ||
                        candidateEntry->arg_size() != 6 ||
                        candidateEntry->getReturnType() != i32Ty ||
                        candidateEntry->getName() !=
                            "__luna_private_ref_apply_entry_v1_test") {
                        failure = "private Ref Result candidate entry lost its fixed C prototype";
                        return false;
                    }
                    auto* candidateStart = llvm::BasicBlock::Create(
                        *proof.mCtx, "entry", candidateEntry);
                    llvm::IRBuilder<> candidateBuilder(candidateStart);
                    std::vector<llvm::Value*> candidateArguments;
                    for (auto& argument : candidateEntry->args()) {
                        if (argument.getType() != ptrTy) {
                            failure = "private Ref Result candidate entry changed an argument type";
                            return false;
                        }
                        candidateArguments.push_back(&argument);
                    }
                    candidateArguments.push_back(
                        llvm::ConstantInt::get(i32Ty, 0));
                    auto* candidateCall = candidateBuilder.CreateCall(
                        hostEntry, candidateArguments);
                    candidateBuilder.CreateRet(candidateCall);
                    if (candidateCall->getCalledFunction() != hostEntry ||
                        candidateCall->arg_size() != 7 ||
                        candidateCall->getArgOperand(6) !=
                            candidateArguments.back()) {
                        failure = "private Ref Result candidate entry lost its host transfer binding";
                        return false;
                    }
                    luna::codegen::NativeOwnedResultDescriptorV3Candidate paired;
                    if (!luna::codegen::derivePairedNativeOwnedResultDescriptorV3Candidate(
                            program, function, *proof.mModule,
                            candidateEntry->getName().str(),
                            hostEntry->getName().str(), paired, failure) ||
                        !(paired.source == *privateOwnedResultFacts)) {
                        if (failure.empty())
                            failure = "private Ref Result entry changed its frozen v3 row";
                        return false;
                    }
                    privateOwnedResultEntryProof =
                        luna::codegen::NativeOwnedResultEntryProof{
                            *privateOwnedResultFacts,
                            candidateEntry->getName().str(),
                            hostEntry->getName().str(),
                            dropEntry->getName().str(),
                            LUNA_NATIVE_ENTRY_ABI_REF_RESULT_OWNER_V1,
                            luna::codegen::NativeOwnedResultPrivateStatusDomainV1};
                }
            }
            if (returnType->kind == TypeKind::Unit &&
                function.params.size() == 1) {
                // Apply CFGs are intentionally excluded from the generic
                // ingress wrapper. This executable proof also emits the
                // reusable descriptor candidate for its private JIT module.
                if (!privateUnitApplyFacts) {
                    failure = "private Ref apply lost its frozen ingress target";
                    return false;
                }
                const std::string ingressName =
                    "__luna_private_ref_apply_ingress_test";
                if (ingressName !=
                        privateUnitApplyFacts->identities.back() ||
                    !luna::codegen::emitNativeRefUnitStatusWrapper(
                        program, function, *proof.mModule,
                        ingressName, failure))
                    return false;
                if (!luna::codegen::emitNativeRefUnitDescriptorV4Candidate(
                        program, function, *proof.mModule,
                        ingressName, "private-test",
                        llvm::sys::getProcessTriple(), "luna/private-test",
                        failure))
                    return false;
            }
        }
#endif
        bool publicUnitPreflight = privateUnitStatusBody &&
            function.isExported && function.packageId == program.name;
#ifdef LUNA_PRIVATE_REF_JIT_TEST
        if (executable) publicUnitPreflight = false;
#endif
        std::optional<std::string> preparedClaim;
        if (publicUnitPreflight) {
            const std::string wrapperName =
                "__luna_native_ref_unit_entry_v1_" + function.symbolId.value;
            if (!luna::codegen::emitNativeRefUnitStatusWrapper(
                    program, function, *proof.mModule,
                    wrapperName, failure))
                return false;
            if (nativeProof && nativeV1Exports) {
                auto claimedProof = *nativeProof;
                if (!luna::codegen::emitNativeRefUnitV4ClaimedDescriptorCandidate(
                        program, function, *proof.mModule, wrapperName,
                        *nativeV1Exports, claimedProof, failure))
                    return false;
                if (claimedProof.exportedDescriptors.size() !=
                        nativeProof->exportedDescriptors.size() + 1) {
                    failure = "Native Ref/unit descriptor produced no single proof claim";
                    return false;
                }
                preparedClaim = claimedProof.exportedDescriptors.back();
            }
        }
        llvm::raw_string_ostream stream(invalidIR);
        if (llvm::verifyModule(*proof.mModule, &stream)) {
            stream.flush();
            failure = "generated Ref apply LLVM IR is invalid: " + invalidIR;
            return false;
        }
        if (preparedModule) {
            if (!publicUnitPreflight || !nativeProof || !nativeV1Exports ||
                !preparedClaim) {
                failure = "Native Ref/unit module preparation has no proof-bound source";
                return false;
            }
            const auto moduleName =
                preparedModule->mModule->getModuleIdentifier();
            preparedModule->mBuilder.reset();
            preparedModule->mHelpers.reset();
            preparedModule->mModule.reset();
            preparedModule->mCtx.reset();
            preparedModule->mCtx = std::move(proof.mCtx);
            preparedModule->mModule = std::move(proof.mModule);
            preparedModule->mBuilder = std::move(proof.mBuilder);
            preparedModule->mHelpers = std::move(proof.mHelpers);
            preparedModule->mModule->setModuleIdentifier(moduleName);
            preparedModule->mNativeRefUnitV4Claim = std::move(preparedClaim);
        }
        if (preparedExecutable) {
            if (!publicUnitPreflight || nativeProof || nativeV1Exports) {
                failure = "isolated Ref/unit preparation has no public source preflight";
                return false;
            }
            auto materialized = proof.materializeJitModule(failure);
            if (!materialized) return false;
            const std::string wrapperName =
                "__luna_native_ref_unit_entry_v1_" + function.symbolId.value;
            if (!materialized->lookup(wrapperName, failure)) return false;
            *preparedExecutable = std::move(materialized);
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
            if (privateOwnedResultFacts) {
                if (!privateOwnedResultEntryProof) {
                    failure = "private Ref Result lost its generated entry proof";
                    executable->reset();
                    return false;
                }
                (*executable)->mPrivateOwnedResultEntryProof =
                    *privateOwnedResultEntryProof;
            }
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
    llvm::Function* wrapper = nullptr;
    if (privateOwnedRefProbe) {
        // This disposable pair proves the two-owner body shape. A test-only
        // selector wrapper returns the scalar branch marker for observation.
        // Neither wrapper becomes a published host ingress ABI.
        std::vector<llvm::Type*> wrapperParameters{
            proof.mHelpers->ptrTy(), proof.mHelpers->ptrTy()};
        if (privateOwnedPairSelector)
            wrapperParameters.push_back(llvm::Type::getInt8Ty(*proof.mCtx));
        wrapper = llvm::Function::Create(
            llvm::FunctionType::get(
                proof.mHelpers->i32Ty(), wrapperParameters, false),
            llvm::Function::InternalLinkage,
            "__private_ref_pair_proof", *proof.mModule);
        auto* entry = llvm::BasicBlock::Create(
            *proof.mCtx, "entry", wrapper);
        llvm::IRBuilder<> builder(entry);
        std::vector<llvm::Value*> bodyArguments{
            wrapper->getArg(0), wrapper->getArg(1)};
        if (privateOwnedPairSelector)
            bodyArguments.push_back(builder.CreateTrunc(
                wrapper->getArg(2), proof.mHelpers->boolTy(),
                "private.ref.selector"));
        auto* result = builder.CreateCall(body, bodyArguments);
        if (privateSelectorScalarResult)
            builder.CreateRet(result);
        else
            builder.CreateRet(llvm::ConstantInt::get(
                proof.mHelpers->i32Ty(), LUNA_RUNTIME_FRAGMENT_REF_SUCCESS_V1));
    } else {
        wrapper = proof.mHelpers->emitRuntimeFragmentRefUnitIngressWrapper(
            *proof.mModule, *body, program, function,
            "__private_ref_ingress_proof");
    }
    if (!wrapper || body->empty() || !body->hasInternalLinkage() ||
        !wrapper->hasInternalLinkage()) {
        failure = "frozen signature/CFG did not pair with the generated body";
        return false;
    }
    size_t bodyDropCalls = 0;
    size_t closureDropCalls = 0;
    size_t wrapperBodyCalls = 0;
    for (auto& block : *body)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                call && call->getCalledFunction() &&
                call->getCalledFunction()->getName() ==
                    "luna_runtime_fragment_ref_drop_v1")
                ++bodyDropCalls;
    // An invoked affine closure transfers its owner to the generated lambda;
    // its Ref Drop lives there rather than in the caller's body.
    if (privateOwnedRefProbe)
        for (auto& generated : *proof.mModule) {
            if (!generated.hasInternalLinkage() ||
                !generated.getName().starts_with("__lambda_"))
                continue;
            for (auto& block : generated)
                for (auto& instruction : block)
                    if (auto* call = llvm::dyn_cast<llvm::CallInst>(
                            &instruction);
                        call && call->getCalledFunction() &&
                        call->getCalledFunction()->getName() ==
                            "luna_runtime_fragment_ref_drop_v1")
                        ++closureDropCalls;
        }
    for (auto& block : *wrapper)
        for (auto& instruction : block)
            if (auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction);
                call && call->getCalledFunction() == body)
                ++wrapperBodyCalls;
    const bool owned = function.params.front().relation ==
        luna::ownership::Relation::Owned;
    const bool wrongDropCount = privateOwnedRefProbe
        ? bodyDropCalls + closureDropCalls < 2
        : (owned ? bodyDropCalls == 0 : bodyDropCalls != 0);
    if (wrapperBodyCalls != 1 || wrongDropCount) {
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
#ifdef LUNA_PRIVATE_REF_JIT_TEST
    if (executable) {
        // Only the test target exports a callable trampoline. The proven
        // body and pair wrapper stay internal to this disposable JIT module.
        auto* entry = llvm::Function::Create(
            wrapper->getFunctionType(), llvm::Function::ExternalLinkage,
            "__luna_private_ref_pair_jit_test", *proof.mModule);
        llvm::IRBuilder<> builder(
            llvm::BasicBlock::Create(*proof.mCtx, "entry", entry));
        std::vector<llvm::Value*> arguments{
            entry->getArg(0), entry->getArg(1)};
        if (privateOwnedPairSelector)
            arguments.push_back(entry->getArg(2));
        builder.CreateRet(builder.CreateCall(wrapper, arguments));
        std::string invalidEntry;
        llvm::raw_string_ostream entryStream(invalidEntry);
        if (llvm::verifyModule(*proof.mModule, &entryStream)) {
            entryStream.flush();
            failure = "private Ref pair JIT entry is invalid: " + invalidEntry;
            return false;
        }
        *executable = proof.materializeJitModule(failure);
        return static_cast<bool>(*executable);
    }
#endif
    failure.clear();
    return true;
}

bool CodeGenerator::verifyNativeRefUnitPreparationLowering(
    moon::Module& program, FunctionDecl& function, std::string& failure) {
    luna::codegen::NativeRefUnitSourceFacts facts;
    if (!luna::codegen::deriveNativeRefUnitSourceFacts(
            program, function, facts, failure))
        return false;
    return verifyPrivateRuntimeFragmentRefUnitIngress(
        program, function, failure);
}

std::shared_ptr<LunaJitModule>
CodeGenerator::materializeNativeRefUnitPreparationLowering(
    moon::Module& program, FunctionDecl& function,
    const void*& entry, std::string& wrapperLinkageName,
    std::string& failure) {
    entry = nullptr;
    wrapperLinkageName.clear();
    luna::codegen::NativeRefUnitSourceFacts facts;
    if (!luna::codegen::deriveNativeRefUnitSourceFacts(
            program, function, facts, failure))
        return {};
    std::shared_ptr<LunaJitModule> executable;
    if (!verifyPrivateRuntimeFragmentRefUnitIngress(
            program, function, failure, nullptr, nullptr, nullptr,
            &executable))
        return {};
    if (!executable) {
        failure = "isolated Ref/unit preparation lost its JIT code lease";
        return {};
    }
    const std::string wrapperName =
        "__luna_native_ref_unit_entry_v1_" + function.symbolId.value;
    entry = executable->lookup(wrapperName, failure);
    if (!entry) return {};
    wrapperLinkageName = wrapperName;
    return executable;
}

#ifdef LUNA_PRIVATE_REF_JIT_TEST
std::shared_ptr<LunaJitModule>
CodeGenerator::materializePrivateRuntimeFragmentRefApplyForTest(
    moon::Module& program, FunctionDecl& function, std::string& failure,
    std::vector<uint8_t>* entryRecord) {
    std::shared_ptr<LunaJitModule> executable;
    if (!verifyPrivateRuntimeFragmentRefUnitIngress(
            program, function, failure, nullptr, nullptr, nullptr,
            nullptr, &executable, entryRecord))
        return {};
    return executable;
}

std::unique_ptr<LunaPrivateRefPairUnitLoadedEntry>
CodeGenerator::materializePrivateNativeRefPairUnitPreparationForTest(
    moon::Module& program, FunctionDecl& function, std::string& failure) {
    luna::codegen::NativeRefPairUnitSourceFacts facts;
    if (!luna::codegen::deriveNativeRefPairUnitSourceFacts(
            program, function, facts, failure))
        return {};
    auto executable = materializePrivateRuntimeFragmentRefApplyForTest(
        program, function, failure);
    if (!executable) return {};
    const void* entry = executable->lookup(
        "__luna_private_ref_apply_jit_test", failure);
    if (!entry) return {};
    auto loaded = std::make_unique<LunaPrivateRefPairUnitLoadedEntry>();
    loaded->lease_ = std::move(executable);
    loaded->entry_ = entry;
    loaded->slotSymbolId_ = std::move(facts.slotSymbolId);
    loaded->slotContractId_ = std::move(facts.slotContractId);
    failure.clear();
    return loaded;
}

std::shared_ptr<LunaJitModule>
CodeGenerator::materializePrivateRuntimeFragmentRefOwnedPairForTest(
    moon::Module& program, FunctionDecl& function, std::string& failure) {
    std::shared_ptr<LunaJitModule> executable;
    if (function.controlFlow &&
        !function.controlFlow->runtimeRefApplies.empty()) {
        failure = "private owned Ref pair test cannot execute Apply";
        return {};
    }
    if (!verifyPrivateRuntimeFragmentRefUnitIngress(
            program, function, failure, nullptr, nullptr, nullptr,
            nullptr, &executable, nullptr))
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

bool CodeGenerator::generate(
    moon::Module* program,
    const luna::driver::NativeProofSpec* nativeProof,
    const std::vector<luna::driver::NativeExportSpec>* nativeV1Exports) {
    mNativeRefUnitV4Claim.reset();
    if (!program) {
        error("code generation has no MoonIR module");
        return false;
    }
    if (static_cast<bool>(nativeProof) !=
        static_cast<bool>(nativeV1Exports)) {
        error("Native Ref/unit preflight has an incomplete proof surface");
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
    FunctionDecl* preparedRefSource = nullptr;
    if (containsRef) {
        if (nativeProof) {
            std::string preparationError;
            if (!luna::codegen::verifyNativeRefUnitModulePreparation(
                    *program, preparationError)) {
                error(preparationError);
                return false;
            }
        }
        size_t provenEntries = 0;
        size_t provenReturns = 0;
        size_t provenApplies = 0;
        size_t provenPublicWrappers = 0;
        size_t provenPublicClaims = 0;
        for (auto& declaration : program->declarations) {
            auto* function = dynamic_cast<FunctionDecl*>(declaration.get());
            if (!function || function->params.empty() ||
                function->params.size() > 2) continue;
            const bool privateOwnedPair = function->params.size() == 2 &&
                !function->requiresFragmentContext &&
                function->controlFlow &&
                function->controlFlow->runtimeRefApplies.empty() &&
                std::all_of(function->params.begin(), function->params.end(),
                    [program](const auto& parameter) {
                        const auto* type = program->findType(parameter.type);
                        return type &&
                            type->kind == TypeKind::RuntimeFragmentRef &&
                            parameter.relation ==
                                luna::ownership::Relation::Owned;
                    });
            if (function->params.size() == 2 && !privateOwnedPair) continue;
            const auto* parameter = program->findType(
                function->params.front().type);
            const auto* result = program->findType(function->returnType);
            if (!parameter || !result ||
                parameter->kind != TypeKind::RuntimeFragmentRef) continue;
            std::string failure;
            if (result->kind == TypeKind::Unit) {
                if (verifyPrivateRuntimeFragmentRefUnitIngress(
                        *program, *function, failure,
                        nativeProof, nativeV1Exports,
                        nativeProof ? this : nullptr)) {
                    if (function->controlFlow &&
                        !function->controlFlow->runtimeRefApplies.empty()) {
                        ++provenApplies;
                        if (function->isExported &&
                            function->packageId == program->name) {
                            ++provenPublicWrappers;
                            if (nativeProof) {
                                ++provenPublicClaims;
                                preparedRefSource = function;
                            }
                        }
                    } else
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
        if (!preparedRefSource || provenPublicClaims != 1 ||
            provenApplies != 1 || !mErrors.empty()) {
            error("RuntimeFragmentRef host ingress/return ABI is not implemented; "
                  "raw-pointer function publication is blocked; " +
                  std::to_string(provenEntries) +
                  " private unit body/wrapper pair(s) and " +
                  std::to_string(provenReturns) +
                  " private owned return body/wrapper pair(s) verified and discarded" +
                  (provenApplies ? "; " + std::to_string(provenApplies) +
                      " private Ref apply body(s) verified and discarded" : "") +
                  (provenPublicWrappers ? "; " +
                      std::to_string(provenPublicWrappers) +
                      " public Native Ref/unit preflight wrapper(s) verified and discarded" : "") +
                  (provenPublicClaims ? "; " +
                      std::to_string(provenPublicClaims) +
                      " public Native Ref/unit v4 descriptor/proof claim(s) verified and discarded" : ""));
            return false;
        }
        // Continue in the proof-bound module; artifact publication has a
        // separate gate until the linked image and loader validate the v4 entry.
    }
    mProgram = program;
    mHostTargetMachine.reset();
    mTypeMaterializer = std::make_unique<moon::TypeMaterializer>(*program);
    mFunctions.clear();
    mDropCallbacks.clear();
    mKernelPTX.clear();
    mKernelHSACO.clear();

    auto declareFunc = [&](FunctionDecl* f) {
        if (f == preparedRefSource) return;
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
        if (preparedRefSource && mModule->getFunction(symbolName)) {
            error("Native Ref/unit module has a generated symbol collision: " +
                  symbolName);
            return;
        }
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
                if (function != preparedRefSource && !function->isSelector &&
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
    if (!mErrors.empty()) return false;

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
